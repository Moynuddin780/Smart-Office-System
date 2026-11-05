#include "api/server.hpp"

#include "core/log.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/WebSocketController.h>

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace so::api {
namespace {

Context* g_ctx = nullptr;

constexpr const char* kBoundary = "smartofficeframe";

/**
 * One viewer's MJPEG stream.
 *
 * The camera only ANNOTATES frames while somebody is watching — the worker's
 * live-view flag is turned on here and off when the last viewer goes away, so
 * an unwatched camera spends nothing on drawing or JPEG encoding. That is the
 * difference between a live wall costing nothing when the tab is closed and
 * costing a core per camera forever.
 */
class MjpegStream : public std::enable_shared_from_this<MjpegStream> {
public:
    MjpegStream(Context* ctx, std::int64_t camera_id, int fps)
        : ctx_(ctx), camera_id_(camera_id),
          interval_(std::chrono::milliseconds(1000 / std::max(1, fps))) {
        ctx_->engine->set_live_view(camera_id_, true);
    }

    ~MjpegStream() {
        // The last viewer leaving turns annotation back off.
        ctx_->engine->set_live_view(camera_id_, false);
    }

    /// Fills `buf` with the next slice of the multipart body. Returning 0 ends
    /// the response, which is how a disconnected browser stops the work.
    /// THIS RUNS ON A DROGON EVENT LOOP, AND THAT GOVERNS EVERYTHING BELOW.
    ///
    /// Drogon serves every connection on one of a handful of loops. Whatever
    /// this function does, no API request assigned to the same loop is being
    /// answered. The first version slept here for a whole frame interval (125 ms
    /// at 8 fps) AND JPEG-encoded the frame, per viewer. Four tiles on the
    /// dashboard were therefore enough to occupy every loop essentially all the
    /// time, and the symptom was the whole UI sitting on "Loading…" — measured
    /// at 11 s for a request that takes 2 ms with the tiles closed.
    ///
    /// So: the encode is gone (the camera worker does it once per frame for all
    /// viewers), and the wait is capped at a few milliseconds. When no new frame
    /// is ready this returns a tiny keep-alive instead of holding the loop —
    /// pacing comes from the camera's own frame rate, which is the honest source
    /// for it anyway.
    static constexpr std::chrono::milliseconds kMaxLoopHold{4};

    std::size_t read(char* buf, std::size_t size) {
        if (size == 0) return 0;
        if (pending_.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (now < next_) {
                const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
                    next_ - now);
                std::this_thread::sleep_for(std::min(wait, kMaxLoopHold));
                if (wait > kMaxLoopHold) return fill_keepalive(buf, size);
            }

            // Renew the lease; a viewer that is still reading is still watching.
            ctx_->engine->set_live_view(camera_id_, true);

            std::uint64_t seq = 0;
            auto jpg = ctx_->engine->annotated_jpeg(camera_id_, &seq);
            if (!jpg || jpg->empty()) {
                // Give up on ELAPSED TIME, never on a call count: without a
                // frame this function returns immediately, so a counter is
                // exhausted in milliseconds and the stream dies before the
                // camera has produced its first picture.
                if (last_seen_ == std::chrono::steady_clock::time_point{})
                    last_seen_ = std::chrono::steady_clock::now();
                if (std::chrono::steady_clock::now() - last_seen_ > std::chrono::seconds(60))
                    return 0;
                return fill_keepalive(buf, size);
            }
            last_seen_ = std::chrono::steady_clock::now();
            // Nothing new to send yet — do not spend the viewer's bandwidth or
            // this loop's time resending a frame they already have.
            if (seq == last_seq_) return fill_keepalive(buf, size);

            empty_ = 0;
            last_seq_ = seq;
            next_ = std::chrono::steady_clock::now() + interval_;

            pending_ = "--" + std::string(kBoundary) + "\r\n" +
                       "Content-Type: image/jpeg\r\n" +
                       "Content-Length: " + std::to_string(jpg->size()) + "\r\n\r\n";
            pending_ += *jpg;
            pending_ += "\r\n";
            offset_ = 0;
        }

        const std::size_t left = pending_.size() - offset_;
        const std::size_t take = std::min(size, left);
        std::memcpy(buf, pending_.data() + offset_, take);
        offset_ += take;
        if (offset_ >= pending_.size()) {
            pending_.clear();
            offset_ = 0;
        }
        return take;
    }

private:
    std::size_t fill_keepalive(char* buf, std::size_t size) {
        // A comment line keeps the connection alive while a camera reconnects,
        // so the browser does not tear the <img> down and retry in a loop.
        static const std::string ka = "\r\n";
        const std::size_t take = std::min(size, ka.size());
        std::memcpy(buf, ka.data(), take);
        return take;
    }

    Context* ctx_;
    std::int64_t camera_id_;
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point next_{};
    std::uint64_t last_seq_{0};
    std::chrono::steady_clock::time_point last_seen_{};
    std::string pending_;
    std::size_t offset_{0};
    int empty_{0};
};

/// Live event feed. Tenant-filtered at the point of delivery, so a socket can
/// only ever receive its own tenant's events.
class EventSocket : public drogon::WebSocketController<EventSocket> {
public:
    void handleNewMessage(const drogon::WebSocketConnectionPtr&, std::string&&,
                          const drogon::WebSocketMessageType&) override {}

    void handleNewConnection(const HttpRequestPtr& req,
                             const drogon::WebSocketConnectionPtr& conn) override {
        if (!g_ctx) {
            conn->shutdown();
            return;
        }
        const auto claims = authenticate(*g_ctx, req);
        if (!claims || claims->tenant_id == 0) {
            // A live feed with no authentication is a live feed of somebody
            // else's building.
            conn->shutdown();
            return;
        }
        std::lock_guard<std::mutex> lk(mtx());
        conns()[conn] = claims->tenant_id;
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override {
        std::lock_guard<std::mutex> lk(mtx());
        conns().erase(conn);
    }

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/events");
    WS_PATH_LIST_END

    static void broadcast(std::int64_t tenant_id, const std::string& json) {
        std::lock_guard<std::mutex> lk(mtx());
        for (auto& [conn, tid] : conns())
            if (tid == tenant_id && conn->connected()) conn->send(json);
    }

private:
    static std::mutex& mtx() {
        static std::mutex m;
        return m;
    }
    static std::map<drogon::WebSocketConnectionPtr, std::int64_t>& conns() {
        static std::map<drogon::WebSocketConnectionPtr, std::int64_t> c;
        return c;
    }
};

}  // namespace

void register_stream(Context& ctx) {
    g_ctx = &ctx;

    ctx.engine->on_live_event([](const app::LiveEvent& e) {
        EventSocket::broadcast(e.tenant_id, e.json);
    });

    // ── live MJPEG ─────────────────────────────────────────────────────────
    drogon::app().registerHandler(
        "/api/cameras/{id}/live.mjpg",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::int64_t id = path_id(req, 1);
                  if (!ctx.repos->camera(t, id)) {
                      cb(json_error(drogon::k404NotFound, "no such camera"));
                      return;
                  }
                  int fps = 10;
                  try {
                      fps = std::stoi(query(req, "fps", "10"));
                  } catch (const std::exception&) {
                  }
                  auto stream = std::make_shared<MjpegStream>(&ctx, id,
                                                              std::clamp(fps, 1, 25));
                  auto r = drogon::HttpResponse::newStreamResponse(
                      [stream](char* buf, std::size_t size) -> std::size_t {
                          return stream->read(buf, size);
                      },
                      "", drogon::CT_CUSTOM,
                      std::string("multipart/x-mixed-replace; boundary=") + kBoundary);
                  cb(r);
              }),
        {drogon::Get});

    // ── a still frame, for zone drawing ────────────────────────────────────
    drogon::app().registerHandler(
        "/api/cameras/{id}/snapshot.jpg",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::int64_t id = path_id(req, 1);
                  if (!ctx.repos->camera(t, id)) {
                      cb(json_error(drogon::k404NotFound, "no such camera"));
                      return;
                  }
                  // Annotation has to be on for a frame to exist at all; turn it
                  // on briefly so the drawing page can get its background.
                  ctx.engine->set_live_view(id, true);
                  // ?clean=1 returns the frame with NOTHING drawn on it. Guided
                  // enrolment uses that: the preview shows boxes so the person
                  // can see a face is being found, but a box baked into the
                  // pixels would become part of the embedding.
                  const bool clean = query(req, "clean") == "1";
                  // 12 s, not 3. A camera is asked for a still right after its
                  // live view is switched on, so the wait has to cover a decode
                  // on the slowest stream an operator might point at us — a 4 MP
                  // H.265 main stream software-decodes far slower than a frame a
                  // second on this board.
                  //
                  // The annotated picture is preferred but NOT required: it needs
                  // a detection as well as a decode, and a page that only wants a
                  // background to draw on should not wait for the NPU.
                  // 2.5 s, not 12. The worker now publishes a frame as soon
                  // as one is DECODED rather than after inference, so a camera
                  // that is delivering has a picture ready almost immediately —
                  // and a request that waits longer than this is holding an
                  // event loop that the rest of the UI needs. A camera still
                  // warming up gets a 503 and the page retries.
                  // FAST PATH, and the one the live tiles use: the camera
                  // worker already holds this frame encoded, so serving it is a
                  // copy. Nothing is decoded, nothing is drawn and nothing is
                  // compressed on this thread — which is what makes it safe to
                  // ask for at 8 frames a second from several tiles at once.
                  // A frame older than this is NOT served. When a camera is
                  // unplugged — moved to another wall, swapped for a different
                  // unit — the worker just stops producing and its last frame
                  // would otherwise be handed out indefinitely, looking live.
                  // Refusing is the honest answer; the tile then says so.
                  constexpr std::int64_t kFrameMaxAgeMs = 5000;

                  if (!clean) {
                      std::uint64_t seq = 0;
                      std::int64_t age_ms = -1;
                      auto jpg = ctx.engine->annotated_jpeg(id, &seq, &age_ms);
                      // A frame exists but has gone stale: refuse HERE rather
                      // than falling through, because the fallback below serves
                      // the very same picture from cv::Mat and would hand out
                      // exactly what this check just rejected.
                      if (jpg && !jpg->empty() && age_ms > kFrameMaxAgeMs) {
                          cb(json_error(drogon::k503ServiceUnavailable,
                                        "no current picture from this camera — its last "
                                        "frame is " + std::to_string(age_ms / 1000) +
                                        " s old, so it has stopped delivering"));
                          return;
                      }
                      if (jpg && !jpg->empty() && age_ms >= 0 && age_ms <= kFrameMaxAgeMs) {
                          auto r = drogon::HttpResponse::newHttpResponse();
                          r->setContentTypeCode(drogon::CT_IMAGE_JPG);
                          r->addHeader("Cache-Control", "no-store");
                          r->addHeader("X-Frame-Seq", std::to_string(seq));
                          r->addHeader("X-Frame-Age-Ms", std::to_string(age_ms));
                          r->setBody(*jpg);
                          cb(r);
                          return;
                      }
                  }

                  cv::Mat frame;
                  for (int i = 0; i < 25 && frame.empty(); ++i) {
                      frame = clean ? ctx.engine->raw_frame(id) : ctx.engine->annotated(id);
                      if (frame.empty() && !clean) frame = ctx.engine->raw_frame(id);
                      if (frame.empty())
                          std::this_thread::sleep_for(std::chrono::milliseconds(100));
                  }
                  if (frame.empty()) {
                      cb(json_error(drogon::k503ServiceUnavailable,
                                    "no current picture from this camera — it is "
                                    "online but decoding too slowly, or it has just started"));
                      return;
                  }
                  std::vector<uchar> jpg;
                  const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, 88};
                  cv::imencode(".jpg", frame, jpg, params);
                  auto r = drogon::HttpResponse::newHttpResponse();
                  r->setContentTypeCode(drogon::CT_IMAGE_JPG);
                  r->setBody(std::string(reinterpret_cast<const char*>(jpg.data()),
                                         jpg.size()));
                  cb(r);
              }),
        {drogon::Get});

    // ── alert images ───────────────────────────────────────────────────────
    drogon::app().registerHandler(
        "/api/snapshots",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::string path = query(req, "path");
                  // The stored path always begins with the tenant's own slug, so
                  // requiring that prefix is what stops one client reading
                  // another's alert images by pasting a path.
                  const auto tenants = ctx.repos->all_tenants();
                  std::string slug;
                  for (const auto& x : tenants)
                      if (x.id == t.value) slug = x.slug;
                  if (path.empty() || slug.empty() || path.rfind(slug + "/", 0) != 0) {
                      cb(json_error(drogon::k404NotFound, "no such image"));
                      return;
                  }
                  const std::string abs = ctx.snapshots->resolve(path);
                  if (abs.empty()) {
                      cb(json_error(drogon::k404NotFound, "no such image"));
                      return;
                  }
                  std::ifstream f(abs, std::ios::binary);
                  if (!f) {
                      cb(json_error(drogon::k404NotFound, "no such image"));
                      return;
                  }
                  const std::string data((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());
                  auto r = drogon::HttpResponse::newHttpResponse();
                  r->setContentTypeCode(drogon::CT_IMAGE_JPG);
                  r->setBody(data);
                  cb(r);
              }),
        {drogon::Get});
}

}  // namespace so::api
