// Smart Office — entry point.
//
// Startup order matters and is deliberate: configuration, then the database and
// its migrations, then the NPU engines and cameras, then the HTTP server. The
// server comes last so that by the time it answers /healthz the answer is true.
//
// It FAILS FAST. A missing model or an unreachable NPU stops the process with a
// message rather than starting an application that looks healthy and detects
// nothing — which is the harder failure to notice and the more expensive one.

#include "core/config.hpp"
#include "core/camera_reader.hpp"
#include "core/camera_worker.hpp"
#include "core/engine_pool.hpp"
#include "core/preprocess.hpp"
#include "identity/face_identifier.hpp"
#include "vision/arcface.hpp"
#include "vision/scrfd.hpp"
#include "vision/tensor_view.hpp"
#include "vision/yolo26_pose.hpp"

#include <opencv2/imgcodecs.hpp>

#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include "core/log.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#ifdef SO_WITH_API
#include "api/auth.hpp"
#include "api/server.hpp"
#include "app/engine.hpp"
#include "db/repos.hpp"
#endif

namespace {

#ifdef SO_WITH_API
/// Create the first SuperAdmin on an empty database, from the environment.
///
/// Runs only when there is no superadmin at all, so the credentials in the
/// environment cannot be used to reset or take over an existing deployment.
void seed_first_admin(so::db::Repos& repos) {
    if (repos.any_superadmin()) return;

    const char* email = std::getenv("SEED_ADMIN_EMAIL");
    const char* password = std::getenv("SEED_ADMIN_PASSWORD");
    if (!email || !*email || !password || !*password) {
        SO_LOG_WARN("[seed] no superadmin exists and SEED_ADMIN_EMAIL/"
                 "SEED_ADMIN_PASSWORD are not set — nobody can log in. Set them "
                 "and restart.");
        return;
    }
    if (std::strlen(password) < 10) {
        SO_LOG_ERROR("[seed] SEED_ADMIN_PASSWORD is shorter than 10 characters; "
                  "refusing to create the account");
        return;
    }
    repos.create_user(std::nullopt, email, so::hash_password(password), "Administrator",
                      "superadmin");
    SO_LOG_INFO("[seed] created the first superadmin: %s", email);
}
#endif

}  // namespace

/// `smart_office --probe <url>` — open one camera URL and say what happened.
///
/// WHY THIS IS A FIRST-CLASS MODE AND NOT A THROWAWAY SCRIPT: when an operator
/// pastes an RTSP URL into Add Camera and no picture appears, the question is
/// always the same — is it the URL, the password, the codec, the transport or
/// the network? Answering it used to mean reading application logs and guessing.
/// This opens exactly the pipeline the product uses and reports the answer.
///
/// Run it with GST_DEBUG set for GStreamer's own trace:
///     docker compose exec -e GST_DEBUG=rtspsrc:5,decodebin:4 app \
///         smart_office --probe 'rtsp://user:pass@host:554/path'
int probe_camera(const std::string& url, const std::string& want_path, int seconds) {
    so::CameraReaderConfig rc;
    rc.url = url;
    rc.max_width = 0;        // probe the stream as it really is, uncapped
    rc.max_height = 0;
    rc.reconnect_min_ms = 500;

    // Which decoder to measure. Without this the probe could only ever report
    // the default, which is no use for the one question worth asking of a board
    // with a video engine: is the hardware path actually faster HERE?
    if (want_path == "hw") {
        rc.prefer_hardware = true;
        rc.allow_software = false;      // do not quietly fall back and flatter it
    } else if (want_path == "sw") {
        rc.prefer_hardware = false;
    }

    SO_LOG_INFO("[probe] opening %s (path: %s, %d s)", so::redact_url(url).c_str(),
                want_path.empty() ? "default" : want_path.c_str(), seconds);
    so::CameraReader r(rc);
    if (!r.start()) {
        SO_LOG_ERROR("[probe] the reader refused to start");
        return 2;
    }

    so::Frame f;
    bool got = false;
    for (int i = 0; i < 50 && !got; ++i) got = r.read(f, std::chrono::milliseconds(500));
    if (!got) {
        r.stop();
        SO_LOG_ERROR("[probe] NO FRAME after 25 s — see the gstreamer lines above "
                     "for the reason. Common causes: wrong password, the camera "
                     "refuses TCP interleaved RTP, or the path is wrong.");
        return 1;
    }

    // THROUGHPUT, measured after the first frame rather than including it: the
    // RTSP handshake and the wait for a keyframe are startup cost, not decode
    // speed, and folding them in would understate every path equally but
    // uselessly.
    const auto t0 = std::chrono::steady_clock::now();
    const std::int64_t base = r.frames_decoded();
    const auto until = t0 + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < until) r.read(f, std::chrono::milliseconds(200));
    const double secs = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - t0).count();
    const double decoded = static_cast<double>(r.frames_decoded() - base);
    const double stream_fps = r.stream_fps();
    const auto path = r.decode_path();
    const int w = f.image.cols, h = f.image.rows;
    r.stop();

    const double fps = secs > 0.1 ? decoded / secs : 0.0;
    SO_LOG_INFO("[probe] RESULT  %dx%d  path=%s  decoded=%.1f fps  stream sends=%.0f fps  %s",
                w, h, so::to_string(path), fps, stream_fps,
                (stream_fps > 1.0 && fps < stream_fps * 0.9)
                    ? "<-- BEHIND: delay will build up"
                    : "<-- keeps up");
    return 0;
}

/// `smart_office --collect-faces <url> <seconds> <out-dir>`
///
/// Writes out the face crops this build would actually hand to the recogniser,
/// for a stream, for a while. It exists so an engineer can look at the input
/// rather than argue about the output: "recognition is poor" and "the crops are
/// 20 px of blur" are the same sentence, but only one of them can be checked.
///
/// It deliberately runs THE SAME two stages the live worker runs — person
/// detection on the whole frame, then a head crop from the full-resolution
/// image through SCRFD — and reuses CameraWorker::head_region for the geometry,
/// so what lands on disk is not an approximation of the production path.
///
/// Each file is named with what matters for judging it:
///     0007_px83_yaw-0.12_det0.91.jpg
/// face_px is measured in SOURCE pixels, the honest count of what the camera
/// captured; identification needs roughly 60.
int collect_faces(const std::string& url, int seconds, const std::string& out_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        SO_LOG_ERROR("[faces] cannot create %s: %s", out_dir.c_str(), ec.message().c_str());
        return 2;
    }

    so::Config cfg = so::Config::from_env();
    const auto model = [&](const char* n) { return cfg.models_dir + "/" + n; };

    // One engine each: a collector is not a throughput test, and leaving the
    // other NPU cores alone keeps it usable while the appliance is running.
    so::EnginePool pose(model("yolo26s_pose.dxnn"), 1);
    so::EnginePool scrfd(model("scrfd_10g.dxnn"), 1);
    so::Yolo26PoseDecoder pose_dec(640, 0.45f, 0.35f);
    so::ScrfdDecoder scrfd_dec(640, 0.5f, 0.4f);

    so::CameraReaderConfig rc;
    rc.url = url;
    rc.max_width = 0;      // a face camera is never downscaled; nor is this
    rc.max_height = 0;
    so::CameraReader reader(rc);
    if (!reader.start()) {
        SO_LOG_ERROR("[faces] the reader refused to start");
        return 2;
    }

    std::vector<std::uint8_t> frame_in(static_cast<std::size_t>(pose.input_size()));
    std::vector<std::uint8_t> frame_out(static_cast<std::size_t>(pose.output_size()));
    std::vector<std::uint8_t> head_in(static_cast<std::size_t>(scrfd.input_size()));
    std::vector<std::uint8_t> head_out(static_cast<std::size_t>(scrfd.output_size()));

    SO_LOG_INFO("[faces] %s -> %s for %d s", so::redact_url(url).c_str(),
                out_dir.c_str(), seconds);

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    int saved = 0, frames = 0, people = 0;
    double px_sum = 0.0, px_max = 0.0;
    so::Frame f;

    while (std::chrono::steady_clock::now() < until) {
        if (!reader.read(f, std::chrono::milliseconds(400)) || f.image.empty()) continue;
        ++frames;

        so::LetterboxMap map;
        const cv::Mat canvas = so::letterbox(f.image, 640, map);
        if (canvas.total() * canvas.elemSize() != frame_in.size()) continue;
        std::memcpy(frame_in.data(), canvas.data, frame_in.size());

        std::vector<so::PoseDetection> dets;
        try {
            dets = pose_dec.decode(so::views_of(pose.run_sync(frame_in.data(), frame_out.data())));
        } catch (const std::exception& e) {
            SO_LOG_WARN("[faces] person stage failed: %s", e.what());
            continue;
        }

        int per_frame = 0;
        for (auto& d : dets) {
            if (per_frame >= 3) break;      // the live worker's own budget
            so::Box pb = d.box;
            so::box_to_source(map, f.image.cols, f.image.rows, pb.data());
            const cv::Rect head = so::CameraWorker::head_region(pb, f.image.cols, f.image.rows);
            if (head.width < 24 || head.height < 24) continue;
            ++per_frame; ++people;

            so::LetterboxMap cmap;
            const cv::Mat hc = so::letterbox(f.image(head), 640, cmap);
            if (hc.total() * hc.elemSize() != head_in.size()) continue;
            std::memcpy(head_in.data(), hc.data, head_in.size());

            std::vector<so::FaceDetection> faces;
            try {
                faces = scrfd_dec.decode(so::views_of(scrfd.run_sync(head_in.data(), head_out.data())));
            } catch (const std::exception& e) {
                SO_LOG_WARN("[faces] face stage failed: %s", e.what());
                continue;
            }
            if (faces.empty()) continue;

            const auto best = std::max_element(faces.begin(), faces.end(),
                [](const so::FaceDetection& a, const so::FaceDetection& b) { return a.score < b.score; });

            so::Box sb = best->box;
            so::box_to_source(cmap, head.width, head.height, sb.data());
            const double face_px = sb[2] - sb[0];
            const auto pose_of = so::face_pose(best->lm);

            // The aligned 112x112 is what ArcFace is given. Saving the wider
            // head crop beside it shows WHY a bad alignment went wrong.
            const cv::Mat aligned = so::align_face(hc, best->lm);
            if (aligned.empty()) continue;

            std::ostringstream name;
            name << out_dir << "/" << std::setfill('0') << std::setw(4) << saved
                 << "_px" << static_cast<int>(face_px + 0.5)
                 << "_yaw" << std::fixed << std::setprecision(2) << pose_of.yaw
                 << "_det" << std::setprecision(2) << best->score;
            cv::imwrite(name.str() + ".jpg", aligned);
            cv::imwrite(name.str() + "_head.jpg", hc);

            px_sum += face_px;
            if (face_px > px_max) px_max = face_px;
            ++saved;
        }
    }

    reader.stop();
    SO_LOG_INFO("[faces] DONE  frames=%d  people=%d  faces=%d  face_px mean=%.1f max=%.1f  -> %s",
                frames, people, saved, saved ? px_sum / saved : 0.0, px_max, out_dir.c_str());
    return 0;
}

#ifdef SO_WITH_API
/**
 * Recompute every stored face embedding with the CURRENT recognition model.
 *
 * A change of recognition model silently invalidates the whole gallery.
 * Embeddings from two models are not merely incomparable — a cross-model
 * cosine returns confident WRONG matches rather than no match — so a profile
 * left holding a mixture of old and new vectors is worse than an empty one.
 *
 * This reads each enrolment photo back off disk and puts it through the SAME
 * path enrolment uses (detect, align, embed), so the new vectors are made
 * exactly as freshly enrolled ones would be. Photos that no longer pass the
 * enrolment gates are reported and their rows are REMOVED rather than left
 * carrying a stale vector.
 */
int reembed(so::Config& cfg) {
    so::db::Database database(cfg.db_url, 4);
    so::db::Repos repos(database);
    so::app::Engine engine(cfg, repos);
    engine.start();

    SO_LOG_INFO("[reembed] recognition model: %s", cfg.rec_model.c_str());
    const std::filesystem::path root(cfg.snapshot_dir);

    int people = 0, done = 0, failed = 0;
    for (const auto& t : repos.all_tenants()) {
        const so::db::TenantId tid{t.id};
        for (const auto& e : repos.employees(tid)) {
            const auto imgs = repos.face_images(tid, e.id);
            if (imgs.empty()) continue;
            ++people;
            int ok = 0;
            for (const auto& f : imgs) {
                const cv::Mat img = cv::imread((root / f.image_path).string());
                if (img.empty()) {
                    SO_LOG_WARN("[reembed] %s: cannot read %s — removing that row",
                                e.name.c_str(), f.image_path.c_str());
                    repos.delete_face_image(tid, f.id);
                    ++failed;
                    continue;
                }
                const auto r = engine.enrol(img);
                if (!r.ok || r.embedding.empty()) {
                    SO_LOG_WARN("[reembed] %s: %s rejected (%s) — removing that row",
                                e.name.c_str(), f.filename.c_str(), r.reason.c_str());
                    repos.delete_face_image(tid, f.id);
                    ++failed;
                    continue;
                }
                repos.update_face_embedding(tid, f.id, r.embedding);
                ++ok; ++done;
            }
            const auto p = repos.recompute_profile(tid, e.id);
            SO_LOG_INFO("[reembed] %-24s %2d/%zu image(s)  coherence %.3f",
                        e.name.c_str(), ok, imgs.size(), p.coherence);
        }
    }
    SO_LOG_INFO("[reembed] DONE  %d people, %d embedding(s) rebuilt, %d row(s) dropped",
                people, done, failed);
    engine.stop();
    return failed > 0 && done == 0 ? 1 : 0;
}
#endif

int main(int argc, char** argv) {
    const bool check_only = (argc > 1 && std::strcmp(argv[1], "--check") == 0);
    if (argc > 4 && std::strcmp(argv[1], "--collect-faces") == 0)
        return collect_faces(argv[2], std::atoi(argv[3]), argv[4]);

#ifdef SO_WITH_API
    if (argc > 1 && std::strcmp(argv[1], "--reembed") == 0) {
        try {
            so::Config cfg = so::Config::from_env();
            cfg.validate();
            return reembed(cfg);
        } catch (const std::exception& e) {
            SO_LOG_ERROR("reembed failed: %s", e.what());
            return 1;
        }
    }
#endif

    if (argc > 2 && std::strcmp(argv[1], "--probe") == 0) {
        // --probe <url> [hw|sw] [seconds]
        const std::string want = argc > 3 ? argv[3] : "";
        const int secs = argc > 4 ? std::atoi(argv[4]) : 10;
        return probe_camera(argv[2], want, secs > 0 ? secs : 10);
    }

    try {
        so::Config cfg = so::Config::from_env();

        if (check_only) {
            // --check reports readiness without requiring a full production
            // configuration, so it is usable in a build or a smoke test.
            SO_LOG_INFO("smart_office --check");
            SO_LOG_INFO("  models      : %s", cfg.models_dir.c_str());
            SO_LOG_INFO("  npu cores   : %d", cfg.npu_cores);
            SO_LOG_INFO("  workers     : %d", cfg.postprocess_workers);
            SO_LOG_INFO("  http port   : %d", static_cast<int>(cfg.http_port));
#ifdef SO_WITH_API
            SO_LOG_INFO("  api layer   : enabled");
#else
            SO_LOG_INFO("  api layer   : NOT COMPILED (Drogon/libpq/libcurl were absent)");
#endif
            return 0;
        }

        cfg.validate();

#ifdef SO_WITH_API
        so::db::Database database(cfg.db_url, 8);
        const char* migrations = std::getenv("MIGRATIONS_DIR");
        database.migrate(migrations && *migrations ? migrations : "migrations");

        so::db::Repos repos(database);
        seed_first_admin(repos);

        so::app::Engine engine(cfg, repos);
        engine.start();

        so::notify::SnapshotStore snapshots(cfg.snapshot_dir, cfg.jpeg_quality);

        so::api::Context ctx;
        ctx.cfg = &cfg;
        ctx.repos = &repos;
        ctx.engine = &engine;
        ctx.snapshots = &snapshots;

        so::api::run_server(ctx);   // blocks

        engine.stop();
#else
        SO_LOG_ERROR("this binary was built without the API layer "
                  "(Drogon, libpq and libcurl were not found at configure time)");
        return 1;
#endif
    } catch (const std::exception& e) {
        SO_LOG_ERROR("startup failed: %s", e.what());
        return 1;
    }
    return 0;
}
