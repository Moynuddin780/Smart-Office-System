#include "core/camera_worker.hpp"

#include "core/log.hpp"
#include "core/preprocess.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace so {
namespace {

// In-flight jobs per camera. Four is two per NPU core plus a little slack, so a
// single camera can keep the pool fed without being able to monopolise it.
constexpr int kJobsPerCamera = 4;
constexpr int kCanvas = 640;

}  // namespace

CameraWorker::CameraWorker(CameraConfig cfg, CameraReaderConfig reader_cfg,
                           WorkerDeps deps)
    : cfg_(std::move(cfg)),
      deps_(deps),
      reader_(std::move(reader_cfg)),
      pose_decoder_(kCanvas, 0.45f, 0.35f),
      scrfd_decoder_(kCanvas, 0.5f, 0.4f) {
    jpeg_quality_ = deps.jpeg_quality;
    if (!deps_.pose) throw std::invalid_argument("CameraWorker needs a pose engine pool");
    if (!deps_.pool) throw std::invalid_argument("CameraWorker needs a postprocess pool");

    const bool faces = cfg_.face_enabled && deps_.scrfd && deps_.arcface && deps_.gallery;
    if (cfg_.face_enabled && !faces)
        SO_LOG_WARN("[worker] camera %lld asks for face recognition but the face "
                 "engines are not available; it will run body modules only",
                 static_cast<long long>(cfg_.id));
    if (faces) {
        arcface_ = std::make_unique<ArcFace>(*deps_.arcface);
        identifier_ = std::make_unique<FaceIdentifier>(*deps_.gallery);
        scrfd_out_.resize(static_cast<std::size_t>(deps_.scrfd->output_size()));
    }

    jobs_.reserve(kJobsPerCamera);
    for (int i = 0; i < kJobsPerCamera; ++i) {
        auto j = std::make_unique<Job>();
        j->owner = this;
        j->input.resize(static_cast<std::size_t>(deps_.pose->input_size()));
        j->output.resize(static_cast<std::size_t>(deps_.pose->output_size()));
        jobs_.push_back(std::move(j));
    }
}

CameraWorker::~CameraWorker() { stop(); }

void CameraWorker::set_modules(std::vector<ModulePtr> modules) {
    std::lock_guard<std::mutex> lk(modules_mtx_);
    modules_ = std::move(modules);
}

void CameraWorker::reconfigure(const CameraConfig& cfg) {
    std::lock_guard<std::mutex> lk(modules_mtx_);
    cfg_ = cfg;
    for (auto& m : modules_) m->reconfigure(cfg_);
}

bool CameraWorker::start() {
    if (thread_.joinable()) return true;
    if (!reader_.start()) return false;
    stop_.store(false);
    started_ = std::chrono::steady_clock::now();
    thread_ = std::thread(&CameraWorker::dispatch_loop, this);
    return true;
}

void CameraWorker::stop() {
    // 1. No new submissions.
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    reader_.stop();

    // 2. Wait for every job ALREADY WITH DX-RT to land.
    //
    // This is not belt and braces. A job submitted just before the flag was set
    // is still inside DX-RT; its completion callback will dereference
    // `job->owner` and then the postprocess worker will touch this object's
    // decoder, tracker and module list. Draining the postprocess pool is not
    // sufficient, because the job has not REACHED the pool yet — it is still on
    // the NPU. Found by an intermittent SIGSEGV in the worker teardown test:
    // one run in several would crash here, which is exactly how a shutdown race
    // presents.
    //
    // `busy` is cleared at the very end of postprocess(), so all-clear means
    // both the callback and the postprocess have finished with this object.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        bool any = false;
        for (auto& j : jobs_)
            if (j->busy.load()) { any = true; break; }
        if (!any) break;
        if (std::chrono::steady_clock::now() > deadline) {
            // Leaking the jobs is strictly better than freeing memory DX-RT is
            // about to write to, so say so and leave them.
            SO_LOG_ERROR("[worker] camera %lld: jobs still in flight after 10s; "
                         "leaking them rather than freeing memory the NPU may "
                         "still write to",
                         static_cast<long long>(cfg_.id));
            for (auto& j : jobs_) j.release();
            jobs_.clear();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // 3. Only now is the postprocess queue safe to drain for anything else.
    if (deps_.pool) deps_.pool->drain();
}

CameraWorker::Job* CameraWorker::acquire_job() {
    for (auto& j : jobs_) {
        bool expected = false;
        if (j->busy.compare_exchange_strong(expected, true)) return j.get();
    }
    return nullptr;
}

namespace {

/// The same colours the configuration page draws these shapes in.
///
/// Matching them is not decoration: somebody draws a red perimeter on the setup
/// page and then looks at the dashboard, and a different colour there would read
/// as a different thing.
cv::Scalar zone_colour(ZoneType t) {
    switch (t) {
        case ZoneType::Desk:       return {255, 141, 76};    // #4c8dff
        case ZoneType::Restricted: return {0, 165, 240};     // #f0a500
        default:                   return {75, 75, 239};     // #ef4b4b
    }
}

/// How big to draw overlays on THIS picture.
///
/// Everything here was first written against a 700-px-wide sub-stream, where a
/// 0.45-point label reads fine. A face camera runs uncapped — 2688 px wide and
/// more — and at that width the same label is four times smaller on screen and
/// cannot be read at all. The annotation has to be legible on both, so it is
/// sized from the frame rather than fixed.
///
/// 960 is the reference width, not a magic number: it is the widest capped
/// stream in this deployment, so capped cameras keep exactly the size they had
/// and only the big ones grow.
double ui_scale(const cv::Mat& img) {
    return std::clamp(static_cast<double>(img.cols) / 960.0, 1.0, 3.2);
}

/// Text with a solid plate behind it, because a label drawn straight onto a
/// camera image is unreadable over anything bright.
void plate_label(cv::Mat& img, const std::string& text, cv::Point at,
                 const cv::Scalar& colour) {
    if (text.empty()) return;
    const double ui = ui_scale(img);
    const double kScale = 0.45 * ui;
    const int kThick = std::max(1, static_cast<int>(std::lround(ui)));
    const int pad = std::max(4, static_cast<int>(std::lround(4 * ui)));
    int base = 0;
    const cv::Size sz = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, kScale, kThick, &base);

    cv::Point tl(at.x, at.y - sz.height - pad - 2);
    tl.x = std::clamp(tl.x, 0, std::max(0, img.cols - sz.width - 2 * pad));
    tl.y = std::clamp(tl.y, 0, std::max(0, img.rows - sz.height - 2 * pad));

    const cv::Rect plate(tl.x, tl.y, sz.width + 2 * pad, sz.height + 2 * pad - 1);
    cv::rectangle(img, plate & cv::Rect(0, 0, img.cols, img.rows), colour, cv::FILLED);
    // Dark text, not white. These plates are drawn in the shape's own colour and
    // the face plates are light green or light grey, on which white text is
    // barely there. Dark ink reads on every plate colour used here.
    cv::putText(img, text, cv::Point(tl.x + pad, tl.y + sz.height + pad - 2),
                cv::FONT_HERSHEY_SIMPLEX, kScale, cv::Scalar(25, 25, 25), kThick,
                cv::LINE_AA);
}

/// What a face box says: who, how sure, and how big the face was.
///
/// The size is the point of the third field. A face under the identifier's 60 px
/// gate is never scored at all, so "no name" on a 23 px face and "no name" on a
/// 90 px face mean entirely different things — the first is the camera being too
/// far away, the second is the model failing. Without the number on the picture
/// those two look identical, and the wrong one gets fixed.
///
/// The score is shown for refusals too, for the same reason: 0.11 against the
/// best candidate is a stranger, 0.26 is somebody the system nearly named.
///
/// Only the first word of the name is drawn. A full name on a 23 px box covers
/// the face it is labelling, and the people watching a live wall know their own
/// colleagues by first name.
std::string face_label(const FaceObs& f) {
    std::ostringstream s;
    if (f.employee_id != 0 && !f.employee_name.empty()) {
        const auto cut = f.employee_name.find(' ');
        s << f.employee_name.substr(0, cut)
          << ' ' << std::fixed << std::setprecision(2) << f.match_score;
    } else if (f.match_score > 0.0f) {
        // Weighed against the gallery and refused: the score says how close it
        // came, which is the difference between a stranger and a near miss.
        s << "? " << std::fixed << std::setprecision(2) << f.match_score;
    } else {
        // Never weighed at all. Saying "0.00" here reads as "no resemblance",
        // which is a lie — the face failed a gate before any comparison. Which
        // gate it failed is the whole diagnosis: "small" means move the camera,
        // "turned" means wait for a better frame, "faint" means the detector
        // itself was unsure. All three used to look identical on the picture.
        const IdentifierConfig g;
        if (f.face_px < g.min_face_px)                 s << "? small";
        else if (std::abs(f.yaw) > g.max_yaw)          s << "? turned";
        else if (f.score < g.min_det_score)            s << "? faint";
        else                                           s << "? ...";
    }
    s << "  " << static_cast<int>(f.face_px) << "px";
    return s.str();
}

/// Shade and outline one zone.
///
/// The fill is composited over the zone's BOUNDING BOX rather than the whole
/// frame, so the cost tracks the size of the zone instead of the size of the
/// picture — this runs on every annotated frame of every watched camera.
void draw_zone(cv::Mat& vis, const Zone& z) {
    if (z.polygon.size() < 3) return;

    std::vector<cv::Point> pts;
    pts.reserve(z.polygon.size());
    for (const auto& p : z.polygon)
        pts.emplace_back(static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y)));

    const cv::Scalar col = zone_colour(z.type);
    const cv::Rect frame_rect(0, 0, vis.cols, vis.rows);
    const cv::Rect box = cv::boundingRect(pts) & frame_rect;
    if (box.width > 1 && box.height > 1) {
        cv::Mat roi = vis(box);
        cv::Mat shade = roi.clone();
        std::vector<cv::Point> local;
        local.reserve(pts.size());
        for (const auto& p : pts) local.emplace_back(p.x - box.x, p.y - box.y);
        cv::fillPoly(shade, std::vector<std::vector<cv::Point>>{local}, col, cv::LINE_AA);
        cv::addWeighted(shade, 0.25, roi, 0.75, 0.0, roi);
    }
    cv::polylines(vis, std::vector<std::vector<cv::Point>>{pts}, true, col, 2, cv::LINE_AA);

    // A desk is only meaningful with the person it belongs to on it.
    std::string text = z.name;
    // ASCII only. OpenCV's Hershey fonts draw one glyph per byte, so a UTF-8
    // separator came out as a pair of question marks on every desk label —
    // "R ?? Ramim" — which reads like a fault in the recognition rather than a
    // font limitation.
    if (z.type == ZoneType::Desk && !z.employee_name.empty()) text += " - " + z.employee_name;
    const auto top = std::min_element(pts.begin(), pts.end(),
                                      [](const cv::Point& a, const cv::Point& b) {
                                          return a.y < b.y;
                                      });
    plate_label(vis, text, *top, col);
}

/// The gate line, with the direction that counts as IN.
///
/// The arrow is derived from the same sign convention the attendance module
/// uses — crossing from side<0 to side>0 is In, and for a line a->b that is the
/// (-dy, dx) normal — so the picture cannot drift away from the behaviour. An
/// arrow drawn by eye would be a guess that silently goes stale.
void draw_gate_line(cv::Mat& vis, const GateLineCfg& g) {
    const cv::Point2f a = g.line.a, b = g.line.b;
    const cv::Point2f d = b - a;
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1.0f) return;

    const cv::Scalar col(113, 191, 47);   // #2fbf71
    cv::line(vis, a, b, cv::Scalar(0, 0, 0), 5, cv::LINE_AA);   // a dark rim, for contrast
    cv::line(vis, a, b, col, 3, cv::LINE_AA);
    cv::circle(vis, a, 5, col, cv::FILLED, cv::LINE_AA);
    cv::circle(vis, b, 5, col, cv::FILLED, cv::LINE_AA);

    const cv::Point2f mid = (a + b) * 0.5f;
    const cv::Point2f in_dir(-d.y / len, d.x / len);
    const float reach = std::min(46.0f, len * 0.35f);
    cv::arrowedLine(vis, mid, mid + in_dir * reach, col, 3, cv::LINE_AA, 0, 0.35);

    plate_label(vis, g.name.empty() ? std::string("gate") : g.name,
                cv::Point(static_cast<int>(mid.x), static_cast<int>(mid.y)), col);
    plate_label(vis, "IN",
                cv::Point(static_cast<int>(mid.x + in_dir.x * (reach + 20)),
                          static_cast<int>(mid.y + in_dir.y * (reach + 20)) + 16), col);
}

}  // namespace

void CameraWorker::release_job(Job* j) { j->busy.store(false); }

void CameraWorker::dispatch_loop() {
    const auto detect_period =
        std::chrono::milliseconds(1000 / std::max(1, cfg_.detect_hz));
    const auto face_period =
        std::chrono::milliseconds(1000 / std::max(1, cfg_.face_hz));

    auto next_detect = std::chrono::steady_clock::now();
    auto next_face = next_detect;

    Frame frame;
    while (!stop_.load()) {
        if (!reader_.read(frame, std::chrono::milliseconds(500))) continue;
        frames_read_.fetch_add(1);

        // Publish the undrawn frame the moment it is DECODED, not when the NPU
        // finishes with it.
        //
        // Both pictures used to be produced only in the inference callback, so
        // "give me a still of this camera" waited on a decode AND a detection.
        // On a 2880x1620 stream that this board software-decodes at well under
        // a frame a second, that is many seconds — long enough that the zone
        // drawing page gave up and said the camera had delivered nothing, while
        // the live tile beside it was playing. Drawing a zone needs a picture,
        // not a detection.
        if (live_view()) {
            std::lock_guard<std::mutex> lk(annotated_mtx_);
            raw_ = frame.image;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < next_detect) continue;         // decoded, deliberately not inferred
        next_detect = now + detect_period;

        Job* job = acquire_job();
        if (!job) {
            // Every job outstanding means the NPU is already the constraint.
            // Queueing another buffer would add latency, not throughput.
            skipped_busy_.fetch_add(1);
            continue;
        }

        job->frame = frame.image;               // cv::Mat is refcounted
        job->ts = frame.ts;
        job->face_due = identifier_ != nullptr && now >= next_face;
        if (job->face_due) next_face = now + face_period;

        const cv::Mat canvas = letterbox(job->frame, kCanvas, job->map);
        std::memcpy(job->input.data(), canvas.data, job->input.size());

        try {
            deps_.pose->submit(job->input.data(), job, job->output.data());
            submitted_.fetch_add(1);
        } catch (const std::exception& e) {
            SO_LOG_ERROR("[worker] camera %lld submit failed: %s",
                      static_cast<long long>(cfg_.id), e.what());
            release_job(job);
        }
    }
}

void CameraWorker::on_inference_done(dxrt::TensorPtrs& outputs, void* user_arg) {
    auto* job = static_cast<Job*>(user_arg);
    if (!job || !job->owner) return;
    CameraWorker* self = job->owner;

    // Decode here would stall the NPU queue; the callback only hands work on.
    std::vector<PoseDetection> dets;
    try {
        dets = self->pose_decoder_.decode(views_of(outputs));
    } catch (const std::exception& e) {
        SO_LOG_ERROR("[worker] pose decode failed: %s", e.what());
        self->release_job(job);
        return;
    }
    self->inferred_.fetch_add(1);

    if (!self->deps_.pool->post([self, job, dets = std::move(dets)]() mutable {
            self->postprocess(job, std::move(dets));
        })) {
        // Shed rather than queue — see PostprocessPool.
        self->release_job(job);
    }
}

cv::Rect CameraWorker::head_region(const Box& person, int frame_w, int frame_h) {
    // Faces sit in the top of a standing person, and SCRFD wants context around
    // them, so this takes the upper part of the box and widens it. Square,
    // because the canvas is square and a tall sliver would waste most of it on
    // padding — which is the very resolution this whole stage exists to win.
    const float px1 = person[0], py1 = person[1], px2 = person[2], py2 = person[3];
    const float pw = px2 - px1, ph = py2 - py1;
    if (pw <= 1.0f || ph <= 1.0f) return {};

    const float cx = (px1 + px2) * 0.5f;
    const float cy = py1 + ph * 0.18f;              // roughly head height
    const float side = std::max(pw * 1.25f, ph * 0.42f);

    int x = static_cast<int>(std::lround(cx - side * 0.5f));
    int y = static_cast<int>(std::lround(cy - side * 0.5f));
    int w = static_cast<int>(std::lround(side));
    int h = w;

    // Clamp without changing the size where possible, so the aspect stays 1:1.
    x = std::clamp(x, 0, std::max(0, frame_w - 1));
    y = std::clamp(y, 0, std::max(0, frame_h - 1));
    w = std::min(w, frame_w - x);
    h = std::min(h, frame_h - y);
    return cv::Rect(x, y, std::max(0, w), std::max(0, h));
}

/**
 * Faces, found on a crop of each person rather than on the whole frame.
 *
 * WHY THIS IS TWO STAGES
 * ----------------------
 * SCRFD sees a 640x640 canvas. Letterboxing a WHOLE frame onto it is what the
 * first version did, and that makes the detector's view of a face depend on how
 * much of the picture the face occupies — not on how many pixels the camera
 * actually captured of it. Measured on the door camera: 26,419 faces seen at a
 * mean of 17 canvas px against a 60 px gate, so nobody was identified until
 * they were almost touching the lens. Pointing the same camera at its 3200x1800
 * main stream made it WORSE, not better: the extra pixels existed, but
 * squeezing 3200 down to 640 shrank every face by a further factor of four and
 * the detector stopped finding them at all (mean 13.9, 294 faces in a minute).
 *
 * So: the person detector finds people — it works fine at 640, a person is a
 * large object — and then each person's head region is cropped from the FULL
 * resolution frame and letterboxed on its own. The face then fills the canvas
 * it is detected on, and the pixels the camera captured are actually used.
 *
 * The budget is bounded: at most kMaxFaceCrops people per face cycle, and
 * people nobody has identified yet go first, so a room that is already known
 * does not crowd out the stranger who just walked in.
 */
std::vector<FaceObs> CameraWorker::run_face_stage(Job* j,
                                                  const std::vector<Track>& tracks) {
    std::vector<FaceObs> faces;
    if (!identifier_ || !deps_.scrfd || !arcface_) return faces;
    if (tracks.empty() || j->frame.empty()) return faces;
    if (face_input_.empty())
        face_input_.resize(static_cast<std::size_t>(deps_.scrfd->input_size()));

    // SOMEBODY STANDING IN AN EXIT ZONE IS ABOUT TO BE UNREADABLE.
    //
    // Every other person on this camera can be looked at again next second, and
    // the second after that. A person in the exit zone cannot: they are a step
    // from the door, and once through it there is no second chance — whatever
    // was not read by then is lost, and with it the record of who left.
    //
    // So they take the face budget first. It costs the room nothing it cannot
    // recover: a colleague at a desk who waits one cycle is still sitting there
    // on the next one.
    const auto in_exit_zone = [this](const Track* t) {
        if (cfg_.zones.empty()) return false;
        const cv::Point2f foot = foot_point(t->box);
        for (const auto& z : cfg_.zones) {
            if (!z.enabled || z.type != ZoneType::Exit) continue;
            if (point_in_polygon(foot, z.polygon)) return true;
        }
        return false;
    };

    std::vector<const Track*> order;
    order.reserve(tracks.size());
    for (const auto& t : tracks) order.push_back(&t);
    std::stable_sort(order.begin(), order.end(),
                     [this, &in_exit_zone](const Track* a, const Track* b) {
                         const bool ea = in_exit_zone(a);
                         const bool eb = in_exit_zone(b);
                         if (ea != eb) return ea;           // about to leave, first
                         // Then whoever still lacks the evidence to be named
                         // — NOT merely whoever has not been looked at once.
                         // See FaceIdentifier::needs_more_looks.
                         const bool na = identifier_->needs_more_looks(a->id);
                         const bool nb = identifier_->needs_more_looks(b->id);
                         if (na != nb) return na;
                         const float aa = (a->box[2] - a->box[0]) * (a->box[3] - a->box[1]);
                         const float ab = (b->box[2] - b->box[0]) * (b->box[3] - b->box[1]);
                         return aa > ab;                    // then the nearest
                     });
    if (order.size() > kMaxFaceCrops) order.resize(kMaxFaceCrops);

    for (const Track* t : order) {
        const cv::Rect head = head_region(t->box, j->frame.cols, j->frame.rows);
        if (head.width < 24 || head.height < 24) continue;

        LetterboxMap cmap;
        const cv::Mat canvas = letterbox(j->frame(head), kCanvas, cmap);
        if (canvas.total() * canvas.elemSize() != face_input_.size()) continue;
        std::memcpy(face_input_.data(), canvas.data, face_input_.size());

        std::vector<FaceDetection> raw;
        try {
            const dxrt::TensorPtrs out =
                deps_.scrfd->run_sync(face_input_.data(), scrfd_out_.data());
            raw = scrfd_decoder_.decode(views_of(out));
        } catch (const std::exception& e) {
            SO_LOG_ERROR("[worker] SCRFD failed on camera %lld: %s",
                      static_cast<long long>(cfg_.id), e.what());
            continue;
        }
        if (raw.empty()) continue;

        // One face per person. More than one detection inside a single person's
        // head region is a passer-by behind them or a false positive, and
        // binding either to this track would be a mis-identification.
        const auto best = std::max_element(raw.begin(), raw.end(),
                                           [](const FaceDetection& a, const FaceDetection& b) {
                                               return a.score < b.score;
                                           });
        const FaceDetection& f = *best;

        FaceObs obs;
        obs.score = f.score;
        const auto pose = face_pose(f.lm);
        obs.yaw = pose.yaw;

        // Crop canvas -> crop source -> full frame.
        Box src_box = f.box;
        box_to_source(cmap, head.width, head.height, src_box.data());
        src_box[0] += static_cast<float>(head.x);
        src_box[1] += static_cast<float>(head.y);
        src_box[2] += static_cast<float>(head.x);
        src_box[3] += static_cast<float>(head.y);
        obs.box = src_box;

        // MEASURED IN SOURCE PIXELS, NOT CANVAS PIXELS.
        //
        // This used to be the canvas width, which was fine while every face was
        // detected on one whole-frame canvas. It is not fine now: a head crop is
        // magnified onto the canvas, so a face the camera captured at 20 px
        // would report 64 px and sail through a quality gate meant to keep
        // under-resolved faces out of the gallery. Source pixels are the honest
        // measure of how much the camera actually saw.
        obs.face_px = src_box[2] - src_box[0];

        for (int i = 0; i < kNumLandmarks; ++i) {
            const cv::Point2f p = cmap.to_source(f.lm[static_cast<std::size_t>(i)].x,
                                                 f.lm[static_cast<std::size_t>(i)].y);
            obs.lm[static_cast<std::size_t>(i)] =
                Landmark{p.x + static_cast<float>(head.x), p.y + static_cast<float>(head.y)};
        }

        // By construction: the crop came from this person's box.
        obs.track_id = t->id;

        faces_seen_.fetch_add(1);
        // Three postprocess workers share this, and a plain load-add-store
        // loses updates under contention — which would quietly undercount the
        // very number an operator is being asked to trust.
        for (double cur = face_px_sum_.load();
             !face_px_sum_.compare_exchange_weak(cur, cur + obs.face_px);) {}
        for (double cur = face_px_max_.load();
             obs.face_px > cur && !face_px_max_.compare_exchange_weak(cur, obs.face_px);) {}

        if (obs.track_id >= 0) {
            faces_bound_.fetch_add(1);
            try {
                // Aligned from the HEAD CANVAS, where the face is largest —
                // the same pixels ArcFace was trained to see rather than a
                // handful of them stretched up afterwards.
                const cv::Mat crop = align_face(canvas, f.lm);
                const auto emb = arcface_->embed(crop);
                const auto ident = identifier_->observe(obs.track_id, emb, obs.yaw,
                                                        obs.face_px, obs.score);
                if (identifier_->was_judged(obs.track_id)) faces_judged_.fetch_add(1);
                obs.employee_id = ident.employee_id;
                obs.employee_name = ident.name;
                obs.match_score = ident.score;
                // WHO IT MOST LOOKS LIKE, even when that is not enough to say
                // so. Carried on every observation because the one person who
                // needs it — somebody about to walk out — is also the one who
                // will not be here for another look.
                const auto guess = identifier_->best_guess(obs.track_id);
                obs.guess_employee_id = guess.employee_id;
                obs.guess_score = guess.score;
                obs.guess_margin = guess.margin;
                obs.guess_weight = guess.weight;
                if (ident.employee_id != 0) identified_.fetch_add(1);
            } catch (const std::exception& e) {
                SO_LOG_WARN("[worker] embedding failed: %s", e.what());
            }
        }
        faces.push_back(std::move(obs));
    }
    return faces;
}

void CameraWorker::postprocess(Job* j, std::vector<PoseDetection> dets) {
    // Map detections into the source frame and hand them to the tracker.
    std::vector<TrackInput> inputs;
    inputs.reserve(dets.size());
    for (auto& d : dets) {
        TrackInput ti;
        ti.score = d.score;
        ti.box = d.box;
        box_to_source(j->map, j->frame.cols, j->frame.rows, ti.box.data());
        for (int k = 0; k < kNumKeypoints; ++k) {
            const auto& src = d.kps[static_cast<std::size_t>(k)];
            const cv::Point2f p = j->map.to_source(src.x, src.y);
            ti.kps[static_cast<std::size_t>(k)] = Keypoint{p.x, p.y, src.conf};
        }
        inputs.push_back(ti);
    }

    std::vector<Track> tracks = tracker_.update(inputs);
    tracks_seen_.fetch_add(static_cast<std::int64_t>(tracks.size()));

    std::vector<FaceObs> faces;
    if (j->face_due) faces = run_face_stage(j, tracks);

    FrameContext ctx;
    ctx.tenant_id = cfg_.tenant_id;
    ctx.camera_id = cfg_.id;
    ctx.ts = j->ts;
    ctx.frame = &j->frame;
    ctx.tracks = std::move(tracks);
    ctx.faces = std::move(faces);
    ctx.cfg = &cfg_;
    ctx.presence = deps_.presence;

    {
        std::lock_guard<std::mutex> lk(modules_mtx_);
        for (auto& m : modules_) {
            try {
                m->on_frame(ctx, *deps_.sink);
            } catch (const std::exception& e) {
                // One module misbehaving must not silence the others on this
                // camera, let alone the other cameras sharing the pool.
                SO_LOG_ERROR("[worker] module %s threw on camera %lld: %s",
                          m->id(), static_cast<long long>(cfg_.id), e.what());
            }
        }
    }

    if (live_view()) {
        // Copy the shapes under the lock, draw outside it.
        //
        // reconfigure() replaces cfg_ from an HTTP thread the moment somebody
        // saves a zone, and this runs on the postprocess thread. Iterating the
        // live vectors would be a race that shows up as a crash exactly when a
        // zone is being drawn — the one moment the operator is watching. The
        // copy is a handful of small structs; the drawing, which is the
        // expensive part, then holds nothing.
        std::vector<Zone> zones;
        std::vector<GateLineCfg> gate_lines;
        {
            std::lock_guard<std::mutex> lk(modules_mtx_);
            zones = cfg_.zones;
            gate_lines = cfg_.gate_lines;
        }

        cv::Mat vis = j->frame.clone();

        // What the operator CONFIGURED, drawn first so live detections sit on
        // top of it. Without this the dashboard showed boxes around people and
        // no sign of the zone or the gate line they were being judged against,
        // which makes an alert impossible to sanity-check from the picture.
        for (const auto& z : zones)
            if (z.enabled) draw_zone(vis, z);
        // Gate lines are NOT drawn any more. Attendance counted crossings once
        // and does not any more — it records a person when their face is
        // recognised — so a line across the picture would announce a rule that
        // no longer exists, on a camera whose behaviour it no longer affects.
        (void)gate_lines;

        const int line = std::max(2, static_cast<int>(std::lround(2 * ui_scale(vis))));
        for (const auto& t : ctx.tracks) {
            cv::rectangle(vis, cv::Point(static_cast<int>(t.box[0]), static_cast<int>(t.box[1])),
                          cv::Point(static_cast<int>(t.box[2]), static_cast<int>(t.box[3])),
                          cv::Scalar(0, 200, 0), line);
        }
        for (const auto& f : ctx.faces) {
            const bool known = f.employee_id != 0;
            const cv::Scalar col = known ? cv::Scalar(90, 200, 90)
                                         : cv::Scalar(150, 150, 150);
            cv::rectangle(vis, cv::Point(static_cast<int>(f.box[0]), static_cast<int>(f.box[1])),
                          cv::Point(static_cast<int>(f.box[2]), static_cast<int>(f.box[3])),
                          col, line);
            plate_label(vis, face_label(f),
                        cv::Point(static_cast<int>(f.box[0]), static_cast<int>(f.box[1])),
                        col);
        }
        // Encode before taking the lock: imencode is the expensive part and no
        // reader needs to be blocked while it runs.
        std::vector<uchar> jpg;
        const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, jpeg_quality_};
        cv::imencode(".jpg", vis, jpg, params);

        auto encoded = std::make_shared<const std::string>(
            reinterpret_cast<const char*>(jpg.data()), jpg.size());

        std::lock_guard<std::mutex> lk(annotated_mtx_);
        annotated_jpeg_ = std::move(encoded);
        annotated_at_ = now_ms();
        ++annotated_seq_;
        annotated_ = std::move(vis);
        // Kept alongside, undrawn, for enrolment — and refreshed here as well as
        // at decode time so the clean frame matches what the boxes were drawn on.
        raw_ = j->frame;
    }

    if (identifier_) identifier_->prune();
    release_job(j);
}

cv::Mat CameraWorker::latest_annotated() {
    std::lock_guard<std::mutex> lk(annotated_mtx_);
    return annotated_.clone();
}

std::shared_ptr<const std::string> CameraWorker::latest_jpeg(std::uint64_t* seq,
                                                             std::int64_t* age_ms) {
    std::lock_guard<std::mutex> lk(annotated_mtx_);
    if (seq) *seq = annotated_seq_;
    if (age_ms) *age_ms = annotated_at_ ? now_ms() - annotated_at_ : -1;
    // A pointer, not the bytes. Every viewer of this camera reads the same
    // buffer, and the lock is held for a pointer copy rather than for ~100 KB.
    return annotated_jpeg_;
}

cv::Mat CameraWorker::latest_raw() {
    std::lock_guard<std::mutex> lk(annotated_mtx_);
    return raw_.clone();
}

WorkerStats CameraWorker::stats() const {
    WorkerStats s;
    s.camera_id = cfg_.id;
    s.camera_name = cfg_.name;
    s.frames_read = frames_read_.load();
    s.submitted = submitted_.load();
    s.inferred = inferred_.load();
    s.skipped_busy = skipped_busy_.load();
    s.faces_seen = faces_seen_.load();
    s.faces_bound = faces_bound_.load();
    s.faces_judged = faces_judged_.load();
    s.tracks_seen = tracks_seen_.load();
    s.face_px_mean = s.faces_seen ? face_px_sum_.load() / static_cast<double>(s.faces_seen) : 0.0;
    s.face_px_max = face_px_max_.load();
    s.identified = identified_.load();
    s.online = reader_.online();
    s.decode_path = to_string(reader_.decode_path());
    s.decode_fps = reader_.fps();
    s.stream_fps = reader_.stream_fps();
    s.width = reader_.width();
    s.height = reader_.height();
    s.native_size = reader_.native_size();
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    s.detect_hz = secs > 0.5 ? static_cast<double>(s.inferred) / secs : 0.0;
    return s;
}

}  // namespace so
