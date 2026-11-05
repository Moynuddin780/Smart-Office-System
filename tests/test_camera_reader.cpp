#include "doctest/doctest.h"

#include "device.hpp"

#include "core/camera_reader.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

namespace {

const char* kClip = "/tmp/so_test_clip.mp4";

// Generated rather than committed: a test video blob in the repo would be
// several MB of fixture for something GStreamer can synthesise in a second,
// and generating it also proves the encoder half of the stack is present.
bool ensure_clip() {
    struct stat st{};
    if (stat(kClip, &st) == 0 && st.st_size > 1000) return true;

    // Encoder availability varies by board. This one has Rockchip's HARDWARE
    // H.264 encoder (mpph264enc) but not x264enc, so try in order of what the
    // deployment would actually prefer rather than assuming the usual one.
    const char* encoders[] = {"mpph264enc", "openh264enc", "avenc_mpeg4"};
    for (const char* enc : encoders) {
        if (std::system((std::string("gst-inspect-1.0 ") + enc +
                         " >/dev/null 2>&1").c_str()) != 0)
            continue;  // NOLINT
        const std::string cmd =
            std::string("gst-launch-1.0 -q videotestsrc num-buffers=90 pattern=ball ! ") +
            "video/x-raw,width=640,height=360,framerate=30/1 ! videoconvert ! " + enc +
            // h264parse is not optional here: an mp4 muxed straight from an
            // encoder can carry no in-band SPS/PPS, which mppvideodec tolerates
            // and avdec_h264 rightly refuses. Without it the fixture decodes on
            // one path and not the other, which looks like a reader bug.
            (std::string(enc) == "avenc_mpeg4" ? " ! " : " ! h264parse ! ") +
            "mp4mux ! filesink location=" + kClip + " >/dev/null 2>&1";
        const int rc = std::system(cmd.c_str());
        (void)rc;
        if (stat(kClip, &st) == 0 && st.st_size > 1000) return true;
        std::remove(kClip);
    }
    return false;
}

}  // namespace

TEST_CASE("the RTSP pipeline asks for hardware decode and refuses to buffer") {
    so::CameraReaderConfig cfg;
    cfg.url = "rtsp://cam.example/stream1";
    so::CameraReader r(cfg);

    const std::string hw = r.pipeline_for(so::DecodePath::HardwareRkmpp);
    CHECK(hw.find("rtspsrc") != std::string::npos);
    // The hardware path asks the Rockchip decoder for BGR at the final size and
    // lets its 2D engine do both. THE ASSERTION THAT MATTERS IS THE ABSENCE OF
    // videoconvert: putting a software colour conversion behind this decoder is
    // what made an earlier attempt slower than decoding in software altogether,
    // because it reads back out of uncached DMA memory.
    CHECK(hw.find("mppvideodec") != std::string::npos);
    CHECK(hw.find("format=BGR") != std::string::npos);
    CHECK(hw.find("videoconvert") == std::string::npos);
    CHECK(hw.find("videoscale") == std::string::npos);

    // See the dedicated case below for why this must be false.
    CHECK(hw.find("drop-on-latency=false") != std::string::npos);
    // appsink must drop too, or the ring buffer's drop-oldest policy is
    // defeated by a queue that formed earlier in the pipeline.
    CHECK(hw.find("drop=true") != std::string::npos);
    CHECK(hw.find("max-buffers=1") != std::string::npos);
}

TEST_CASE("the software path converts in software and says so") {
    so::CameraReaderConfig cfg;
    cfg.url = "rtsp://cam.example/stream1";
    so::CameraReader r(cfg);
    const std::string sw = r.pipeline_for(so::DecodePath::Software);

    CHECK(sw.find("force-sw-decoders=true") != std::string::npos);
    CHECK(sw.find("videoconvert") != std::string::npos);
    // It must never reach for the hardware decoder: this is the path that has
    // to work on a board that has none.
    CHECK(sw.find("mppvideodec") == std::string::npos);

    // The two paths are genuinely different pipelines now, not one switch.
    CHECK(sw != r.pipeline_for(so::DecodePath::HardwareRkmpp));
}

TEST_CASE("the hardware path scales on the decoder, not afterwards") {
    so::CameraReaderConfig cfg;
    cfg.url = "rtsp://cam.example/stream1";
    cfg.max_width = 960;
    cfg.max_height = 540;
    // A source bigger than the cap, so there is something to scale. Without a
    // known source size the decoder is deliberately asked for no size at all —
    // see "the hardware decoder is never asked to enlarge a stream".
    cfg.src_width = 1920;
    cfg.src_height = 1080;
    so::CameraReader r(cfg);
    const std::string hw = r.pipeline_for(so::DecodePath::HardwareRkmpp);
    // The size goes to the decoder itself, which resizes on the RGA. A
    // videoscale here would be the CPU redoing work the hardware already did.
    CHECK(hw.find("width=960") != std::string::npos);
    CHECK(hw.find("height=540") != std::string::npos);
    CHECK(hw.find("videoscale") == std::string::npos);

    // A face camera asks for no cap, and then nothing must constrain the size.
    cfg.max_width = 0;
    cfg.max_height = 0;
    so::CameraReader uncapped(cfg);
    const std::string u = uncapped.pipeline_for(so::DecodePath::HardwareRkmpp);
    CHECK(u.find("width=") == std::string::npos);
}

TEST_CASE("the pipeline names no codec, so an H.265 camera decodes too") {
    // THIS IS THE REGRESSION TEST FOR A REAL FIELD FAILURE.
    //
    // The reader used to hardcode `avdec_h264`. The H.264 test footage decoded
    // perfectly, so every test passed — and the first camera on site reported
    // "no usable decode path" forever, because its sub-stream advertises
    //     a=rtpmap:108 H265/90000
    // Naming ANY codec-specific element here reintroduces that bug for
    // whichever codec is not named, so the assertion is about absence.
    so::CameraReaderConfig cfg;
    cfg.url = "rtsp://cam.example/stream1";
    so::CameraReader r(cfg);

    for (so::DecodePath path : {so::DecodePath::Software,
                               so::DecodePath::HardwareRkmpp}) {
        const std::string p = r.pipeline_for(path);
        // NOTE mppvideodec is deliberately NOT in this list. The rule is "no
        // element that handles only ONE codec"; mppvideodec handles H.264,
        // H.265, VP8, VP9 and MPEG, so naming it does not reintroduce the bug
        // this test exists for.
        for (const char* codec_specific : {"avdec_h264", "avdec_h265",
                                           "h264parse", "h265parse",
                                           "mpph264dec", "mpph265dec",
                                           "rtph264depay", "rtph265depay"}) {
            INFO("path=" << so::to_string(path) << " must not name "
                         << codec_specific << ": " << p);
            CHECK(p.find(codec_specific) == std::string::npos);
        }
    }
}

TEST_CASE("a logged camera URL never carries the password") {
    // The reader logs the URL on every reconnect, so this is the difference
    // between an operator reading `docker compose logs` and that operator
    // reading the site's camera credentials.
    CHECK(so::redact_url("rtsp://admin:s3cr3t@10.0.0.9:554/media/video1") ==
          "rtsp://admin:***@10.0.0.9:554/media/video1");

    // A password is not required to be absent-safe: no credentials, no change.
    CHECK(so::redact_url("rtsp://10.0.0.9:554/sub") == "rtsp://10.0.0.9:554/sub");
    CHECK(so::redact_url("/app/sample/corridor.mp4") == "/app/sample/corridor.mp4");

    // Username with no password still must not leak whatever follows the colon
    // of the PORT rather than of a credential.
    CHECK(so::redact_url("rtsp://admin@10.0.0.9:554/sub") ==
          "rtsp://***@10.0.0.9:554/sub");

    // A realistic one, checked literally: a ':' inside the path must not
    // confuse the scan, and the secret must be gone.
    const std::string red =
        so::redact_url("rtsp://admin:Ex4mple_pw@192.168.1.102:554/media/video1");
    CHECK(red.find("Ex4mple_pw") == std::string::npos);
    CHECK(red.find("admin") != std::string::npos);
    CHECK(red.find("192.168.1.102:554/media/video1") != std::string::npos);
}

TEST_CASE("the RTSP pipeline takes the video stream, not the ONVIF metadata") {
    // The camera on site advertises two media in its SDP:
    //     m=video       ... H265/90000
    //     m=application ... vnd.onvif.metadata/90000
    // gst_parse_launch links the first pad that will link, so without a caps
    // filter the decoder can be wired to the metadata track and the pipeline
    // produces nothing, silently, forever.
    so::CameraReaderConfig cfg;
    cfg.url = "rtsp://cam.example/stream1";
    so::CameraReader r(cfg);
    const std::string p = r.pipeline_for(so::DecodePath::Software);
    CHECK(p.find("application/x-rtp,media=video") != std::string::npos);

    // A local file has no RTP at all and must not acquire that filter.
    so::CameraReaderConfig fcfg;
    fcfg.url = "/tmp/clip.mp4";
    so::CameraReader fr(fcfg);
    CHECK(fr.pipeline_for(so::DecodePath::Software).find("x-rtp") ==
          std::string::npos);
}

TEST_CASE("the jitter buffer is big enough and patient enough for a keyframe") {
    // REGRESSION TEST FOR A SILENT, TOTAL FAILURE.
    //
    // The reader shipped with latency=100 AND drop-on-latency=true. An H.265
    // camera sends a large IDR every few seconds; over interleaved TCP that
    // keyframe needs well over 100 ms to arrive. A 100 ms buffer told never to
    // exceed its latency drops part of EVERY keyframe, so the depayloader never
    // assembles one and the decoder never emits a frame — with no error on the
    // bus, no warning, and a perfectly healthy RTSP session. The camera simply
    // showed black forever.
    //
    // Both halves of that pairing are asserted here because either one alone
    // brings the failure back.
    so::CameraReaderConfig cfg;
    CHECK(cfg.rtsp_latency_ms >= 300);
    CHECK(cfg.rtsp_drop_on_latency == false);

    cfg.url = "rtsp://cam.example/stream1";
    so::CameraReader r(cfg);
    const std::string p = r.pipeline_for(so::DecodePath::Software);
    CHECK(p.find("drop-on-latency=false") != std::string::npos);
    CHECK(p.find("latency=" + std::to_string(cfg.rtsp_latency_ms)) != std::string::npos);
}

TEST_CASE("the RTSP transport is the one measured to work on site") {
    // UDP delivered 0 bytes from the camera on site while interleaved TCP
    // delivered 220 KB in 6 s, so TCP is the default — but it stays a setting,
    // because the opposite is true on plenty of networks.
    so::CameraReaderConfig cfg;
    cfg.url = "rtsp://cam.example/stream1";
    so::CameraReader r(cfg);
    CHECK(r.pipeline_for(so::DecodePath::Software).find("protocols=tcp") != std::string::npos);

    cfg.rtsp_protocols = "udp+tcp";
    so::CameraReader r2(cfg);
    CHECK(r2.pipeline_for(so::DecodePath::Software).find("protocols=udp+tcp") != std::string::npos);
}

TEST_CASE("a local file uses filesrc, not rtspsrc") {
    so::CameraReaderConfig cfg;
    cfg.url = "/tmp/clip.mp4";
    so::CameraReader r(cfg);
    const std::string p = r.pipeline_for(so::DecodePath::HardwareRkmpp);
    CHECK(p.find("filesrc") != std::string::npos);
    CHECK(p.find("rtspsrc") == std::string::npos);
    CHECK(p.find("mppvideodec") != std::string::npos);
}

TEST_CASE("the reader actually decodes a real clip") {
    SO_REQUIRE_DECODER();
    REQUIRE(ensure_clip());

    so::CameraReaderConfig cfg;
    cfg.url = kClip;
    so::CameraReader r(cfg);
    REQUIRE(r.start());

    so::Frame f;
    bool got = false;
    for (int i = 0; i < 40 && !got; ++i)
        got = r.read(f, std::chrono::milliseconds(500));

    REQUIRE(got);
    CHECK(f.image.cols == 640);
    CHECK(f.image.rows == 360);
    CHECK(f.image.type() == CV_8UC3);
    CHECK(f.seq > 0);
    CHECK(r.frames_decoded() > 0);
    CHECK(r.decode_path() != so::DecodePath::None);

    // Which path won is reported, not guessed at. A silent software fallback is
    // a performance regression nobody notices until the box is saturated.
    MESSAGE("decode path: " << std::string(so::to_string(r.decode_path())));

    r.stop();
    CHECK_FALSE(r.online());
}

TEST_CASE("the ring drops old frames rather than growing") {
    SO_REQUIRE_DECODER();
    REQUIRE(ensure_clip());

    so::CameraReaderConfig cfg;
    cfg.url = kClip;
    cfg.ring_size = 2;
    so::CameraReader r(cfg);
    REQUIRE(r.start());

    // Decode for a while without reading: the producer must shed frames, not
    // accumulate them. A frame from four seconds ago cannot raise a useful
    // intrusion alert, so latency is the thing worth protecting.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const auto decoded = r.frames_decoded();
    r.stop();

    if (decoded > 2) CHECK(r.frames_dropped() > 0);
    CHECK(decoded >= 1);
}

TEST_CASE("a camera that cannot be opened reports offline instead of blocking") {
    so::CameraReaderConfig cfg;
    cfg.url = "/nonexistent/path/to/nothing.mp4";
    cfg.reconnect_min_ms = 200;
    so::CameraReader r(cfg);
    REQUIRE(r.start());

    so::Frame f;
    CHECK_FALSE(r.read(f, std::chrono::milliseconds(300)));
    CHECK_FALSE(r.online());
    r.stop();
}

TEST_CASE("the hardware decoder is never asked to enlarge a stream") {
    // REGRESSION TEST FOR A SILENT TWELVE-SECOND FAILURE.
    //
    // mppvideodec takes an EXACT output size, not a range. Told to turn a
    // 704x576 camera into 1920x1080 it emits nothing at all — no error on the
    // bus, no warning — and the reader gives up and decodes in software. On
    // this site that put four of five cameras on the CPU while the fifth, whose
    // stream was already 1920x1080, ran on the video engine perfectly. The CPU
    // then had no room left to keep up, so the pictures ran minutes behind.
    so::CameraReaderConfig cfg;
    cfg.url = "rtsp://cam.example/stream1";
    cfg.max_width = 1920;
    cfg.max_height = 1080;

    SUBCASE("a stream smaller than the cap is not resized at all") {
        cfg.src_width = 704;
        cfg.src_height = 576;
        so::CameraReader r(cfg);
        const std::string p = r.pipeline_for(so::DecodePath::HardwareRkmpp);
        CHECK(p.find("mppvideodec") != std::string::npos);
        CHECK(p.find("width=") == std::string::npos);
    }

    SUBCASE("a larger stream is scaled, keeping its shape") {
        cfg.src_width = 3200;
        cfg.src_height = 1800;
        so::CameraReader r(cfg);
        const std::string p = r.pipeline_for(so::DecodePath::HardwareRkmpp);
        CHECK(p.find("width=1920") != std::string::npos);
        CHECK(p.find("height=1080") != std::string::npos);   // 16:9 preserved
    }

    SUBCASE("an unprobed camera is not resized either") {
        // The remembered size may decide WHETHER to shrink. It may never decide
        // the shape, and when it is missing there is nothing to decide with.
        cfg.src_width = 0;
        cfg.src_height = 0;
        so::CameraReader r(cfg);
        const std::string p = r.pipeline_for(so::DecodePath::HardwareRkmpp);
        CHECK(p.find("width=") == std::string::npos);
    }

    SUBCASE("a tall stream fits by its own limiting side") {
        cfg.src_width = 2048;
        cfg.src_height = 1536;                       // 4:3
        so::CameraReader r(cfg);
        const std::string p = r.pipeline_for(so::DecodePath::HardwareRkmpp);
        CHECK(p.find("height=1080") != std::string::npos);
        CHECK(p.find("width=1440") != std::string::npos);    // not 1920: shape kept
    }
}

TEST_CASE("the software path never changes the shape of the picture") {
    // REGRESSION TEST FOR A SILENT GEOMETRY BUG.
    //
    // The cap used to be a RANGE on each axis — width=[1,W] height=[1,H] — and
    // videoscale takes each axis to its own maximum independently. A 3200x1800
    // stream under a 1920x1920 cap came out 1920x1800: squashed by a third. No
    // error, no warning, a picture that still looks like the room — and every
    // facial landmark displaced, which is precisely what the alignment and
    // therefore the whole identification rests on.
    so::CameraReaderConfig cfg;
    cfg.url = "rtsp://cam.example/stream1";
    cfg.max_width = 1000;
    cfg.max_height = 1000;

    SUBCASE("a 16:9 stream keeps 16:9") {
        cfg.src_width = 2688;
        cfg.src_height = 1520;
        so::CameraReader r(cfg);
        const std::string p = r.pipeline_for(so::DecodePath::Software);
        CHECK(p.find("width=1000") != std::string::npos);
        CHECK(p.find("height=564") != std::string::npos);   // 1520 * 1000/2688
        CHECK(p.find("[1,") == std::string::npos);          // no range left
    }

    SUBCASE("a 4:3 stream keeps 4:3 and is not stretched to the box") {
        cfg.src_width = 1600;
        cfg.src_height = 1200;
        so::CameraReader r(cfg);
        const std::string p = r.pipeline_for(so::DecodePath::Software);
        CHECK(p.find("width=1000") != std::string::npos);
        CHECK(p.find("height=750") != std::string::npos);   // NOT 1000
    }

    SUBCASE("a stream already under the cap gets no videoscale at all") {
        cfg.src_width = 704;
        cfg.src_height = 576;
        so::CameraReader r(cfg);
        const std::string p = r.pipeline_for(so::DecodePath::Software);
        CHECK(p.find("videoscale") == std::string::npos);
    }

    SUBCASE("no camera is ever handed a frame wider or taller than the cap") {
        for (auto [w, h] : {std::pair{3200, 1800}, std::pair{2688, 1520},
                            std::pair{1600, 1200}, std::pair{1920, 1080}}) {
            cfg.src_width = w;
            cfg.src_height = h;
            so::CameraReader r(cfg);
            const auto fit = r.fitted_size();
            INFO("source " << w << "x" << h);
            CHECK(fit.first <= 1000);
            CHECK(fit.second <= 1000);
        }
    }
}

TEST_CASE("a remembered size never decides the shape of the picture") {
    // REGRESSION TEST FOR A DISTORTION THE PICTURE DOES NOT ADMIT TO.
    //
    // cameras.probed_width was written once, when the camera was added, and
    // never corrected. A camera recorded as 720x576 was really streaming
    // 864x480; sizing the frame from the record squeezed 16:9 into 5:4. The
    // room still looked like the room — and every facial landmark had moved,
    // which is the one thing identification cannot survive.
    //
    // So a remembered size may say WHETHER to shrink, never what shape to be.
    // When it is missing, nothing is imposed: an uncapped frame costs CPU, a
    // misshapen one costs identifications.
    so::CameraReaderConfig cfg;
    cfg.url = "rtsp://cam.example/stream1";
    cfg.max_width = 1000;
    cfg.max_height = 1000;
    cfg.src_width = 0;
    cfg.src_height = 0;

    so::CameraReader r(cfg);
    for (auto path : {so::DecodePath::Software, so::DecodePath::HardwareRkmpp}) {
        const std::string p = r.pipeline_for(path);
        CHECK(p.find("videoscale") == std::string::npos);
        CHECK(p.find("width=[1,") == std::string::npos);
        CHECK(p.find("pixel-aspect-ratio") == std::string::npos);
    }
}

TEST_CASE("decoding runs on its own thread, not the network thread") {
    // REGRESSION TEST FOR A LOAD-BALANCING FAILURE THAT LOOKS LIKE AN IDLE BOARD.
    //
    // A GStreamer pad task runs everything downstream inline, so with nothing
    // between the RTP source and the decoder, one thread does jitter
    // buffering, depayloading, parsing and decoding for a camera. Measured
    // here: 91% of one core on that single thread, six cores below 9%, three
    // cameras behind. Raising the decoder's thread count cannot fix it — the
    // work has no second thread to spread into.
    so::CameraReaderConfig cfg;
    cfg.url = "rtsp://cam.example/stream1";
    so::CameraReader r(cfg);

    for (auto path : {so::DecodePath::Software, so::DecodePath::HardwareRkmpp}) {
        const std::string p = r.pipeline_for(path);
        const auto q = p.find("queue");
        const auto dec = p.find("decodebin") != std::string::npos
                             ? p.find("decodebin") : p.find("parsebin");
        REQUIRE(q != std::string::npos);
        CHECK(q < dec);                                   // before the decoder
        CHECK(p.find("max-size-time=") != std::string::npos);   // bounded
    }
}

TEST_CASE("encoded data is never dropped on the way to the decoder") {
    // REGRESSION TEST FOR A CHANGE THAT BLINDED THREE CAMERAS OF FOUR.
    //
    // To bound a growing lag, a leaky queue was put between the parser and the
    // decoder — the safe place in principle, because a buffer there is a whole
    // encoded frame. Getting a seam there meant naming `parsebin` instead of
    // letting `decodebin` do depayloading as well, and parsebin does NOT
    // depayload RTP for every codec: .101, .102 and .104 went to zero frames on
    // BOTH paths within a minute, with nothing on the bus to say why. Only the
    // H.265 camera survived.
    //
    // That also explains the older mystery of hardware decode working on one
    // camera in five: the hardware path has always named parsebin.
    //
    // So decodebin keeps the whole job, and nothing between the network and the
    // decoder may leak. Lag is dealt with by making the decode keep up —
    // a lower frame limit, a lower camera frame rate — not by discarding
    // fragments of a stream the decoder then cannot reassemble.
    so::CameraReaderConfig cfg;
    cfg.url = "rtsp://cam.example/stream1";
    so::CameraReader r(cfg);

    const std::string sw = r.pipeline_for(so::DecodePath::Software);
    CHECK(sw.find("leaky") == std::string::npos);
    CHECK(sw.find("decodebin") != std::string::npos);
    CHECK(sw.find("parsebin") == std::string::npos);   // decodebin depayloads too
    CHECK(r.pipeline_for(so::DecodePath::HardwareRkmpp).find("leaky") == std::string::npos);
}

TEST_CASE("the lag watchdog is set where it cannot thrash") {
    // A stream the board cannot keep up with queues rather than drops, and the
    // picture falls further behind every second. The backlog cannot be thrown
    // away inside the pipeline — dropping RTP packets or half-parsed frames
    // leaves the decoder nothing it can assemble, which has gone wrong here
    // twice — so the reader reopens the stream instead, which discards the
    // backlog with the old pipeline.
    //
    // The number has to sit between two failures. Too high and the live wall is
    // stale, which is the problem being solved. Too low and the reader spends
    // its time reconnecting: each reopen costs an RTSP handshake plus the wait
    // for a keyframe, and on these cameras that is well over a second, so a
    // threshold near or below it would never finish catching up before tripping
    // again.
    so::CameraReaderConfig cfg;
    CHECK(cfg.max_behind_s >= 1.0);     // above the cost of one reconnect
    CHECK(cfg.max_behind_s <= 5.0);     // below "nobody would call this live"

    // The jitter buffer must stay well under it, or the reader would be
    // reopening a stream that is merely buffering normally.
    CHECK(cfg.rtsp_latency_ms / 1000.0 < cfg.max_behind_s);
}

TEST_CASE("frames are thrown away BEFORE the colour conversion, not after") {
    // A camera that will not be told to send less.
    //
    // The VARITO on site advertises `FrameRateRange 5-30` over ONVIF, accepts
    // `SetVideoEncoderConfiguration` with FrameRateLimit=10 and returns no
    // fault — and then keeps sending 25. Its web UI reverts the same field the
    // same way. The frame rate control in that firmware is cosmetic, so
    // "configure the camera properly" is not an available fix.
    //
    // What is available: the pipeline ended in `appsink max-buffers=1
    // drop=true`, so the excess frames WERE already being discarded — but only
    // after videoconvert had turned each of them into 2 MP of BGR. At 25 fps
    // into a worker that consumes ten, three frames in five were fully
    // converted and then dropped. The conversion is the most expensive step in
    // the whole software path (the measurements in the header put 720p at
    // 16.0 ms/frame, and 5.9 ms once the pixel count falls), so where the drop
    // happens decides whether it costs anything.
    so::CameraReaderConfig cfg;
    cfg.url = "rtsp://cam/stream0";
    cfg.src_width = 1920;
    cfg.src_height = 1080;
    cfg.max_fps = 10;

    const std::string p = so::CameraReader(cfg).pipeline_for(so::DecodePath::Software);
    const auto rate = p.find("videorate");
    const auto convert = p.find("videoconvert");
    REQUIRE(rate != std::string::npos);
    REQUIRE(convert != std::string::npos);
    CHECK(rate < convert);
    CHECK(p.find("max-rate=10") != std::string::npos);

    // drop-only, so this can only ever reduce the rate. Without it videorate
    // DUPLICATES buffers to reach a target rate, which on a camera sending
    // fewer frames than the cap would invent work instead of saving it — and
    // every duplicate costs a full conversion.
    CHECK(p.find("drop-only=true") != std::string::npos);

    // Zero means "take every frame", and must add nothing at all: a pipeline
    // that always carries a videorate is a pipeline where a camera running
    // under the cap pays for an element that can never fire.
    so::CameraReaderConfig off = cfg;
    off.max_fps = 0;
    CHECK(so::CameraReader(off).pipeline_for(so::DecodePath::Software)
              .find("videorate") == std::string::npos);
}
