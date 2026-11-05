#include "doctest/doctest.h"

#include "api/camera_rules.hpp"

using so::api::CameraDraft;
using so::api::camera_problem;

namespace {

/// A camera that is valid, so each test can break exactly one thing.
CameraDraft good() {
    CameraDraft c;
    c.name = "Reception";
    c.rtsp_sub = "rtsp://admin:pw@192.168.1.64:554/Streaming/Channels/102";
    c.rtsp_main = "rtsp://admin:pw@192.168.1.64:554/Streaming/Channels/101";
    c.stream_pref = "sub";
    c.role = "area";
    return c;
}

}  // namespace

TEST_CASE("a valid live camera is accepted") {
    CHECK(camera_problem(good()) == "");
}

TEST_CASE("a recorded file cannot be used as a camera") {
    // THE REASON THIS RULE EXISTS: demo clips were accepted during development
    // and ended up looping on the live wall beside real cameras, which reads as
    // a room being watched when nobody is watching it.
    for (const char* not_a_camera : {"/app/sample/corridor.mp4",
                                     "file:///app/sample/corridor.mp4",
                                     "./clip.mp4",
                                     "http://192.168.1.64/video.mjpg",
                                     "C:\\videos\\clip.mp4"}) {
        CameraDraft c = good();
        c.rtsp_sub = not_a_camera;
        INFO("must be refused: " << not_a_camera);
        CHECK(camera_problem(c) != "");
    }
    // rtsps:// is a live camera too, and must NOT be caught by the same rule.
    CameraDraft tls = good();
    tls.rtsp_sub = "rtsps://admin:pw@192.168.1.64:322/Streaming/Channels/102";
    CHECK(camera_problem(tls) == "");
}

TEST_CASE("an RTSP URL with no host is refused") {
    CameraDraft c = good();
    c.rtsp_sub = "rtsp:///Streaming/Channels/102";
    CHECK(camera_problem(c) != "");
}

TEST_CASE("the stream the camera is set to use must exist") {
    // A camera saved with only a main URL but set to "sub" is configured,
    // enabled, and permanently blank — the failure nobody can diagnose.
    CameraDraft sub_missing = good();
    sub_missing.rtsp_sub = "";
    sub_missing.stream_pref = "sub";
    CHECK(camera_problem(sub_missing) != "");

    CameraDraft main_missing = good();
    main_missing.rtsp_main = "";
    main_missing.stream_pref = "main";
    CHECK(camera_problem(main_missing) != "");

    // ...but the OTHER URL being empty is fine.
    CameraDraft only_sub = good();
    only_sub.rtsp_main = "";
    only_sub.stream_pref = "sub";
    CHECK(camera_problem(only_sub) == "");
}

TEST_CASE("a site may name a camera's role whatever it likes") {
    // The engine does not branch on the role — behaviour comes from the modules
    // enabled on the camera — so refusing an unfamiliar name buys nothing and
    // stops an operator describing their own building.
    for (const char* role : {"reception", "warehouse gate", "server room",
                             "Ground floor lobby", "gate", "area"}) {
        CameraDraft c = good();
        c.role = role;
        INFO("role must be allowed: " << role);
        CHECK(camera_problem(c) == "");
    }
    CameraDraft blank = good();
    blank.role = "";
    CHECK(camera_problem(blank) != "");   // but a blank label is still refused
}

TEST_CASE("a nameless camera is refused") {
    CameraDraft c = good();
    c.name = "";
    CHECK(camera_problem(c) != "");
}

TEST_CASE("rates outside what the engine will schedule are refused") {
    for (int hz : {0, -1, 61, 1000}) {
        CameraDraft c = good();
        c.detect_hz = hz;
        INFO("detect_hz " << hz);
        CHECK(camera_problem(c) != "");
    }
    for (int hz : {1, 10, 60}) {
        CameraDraft c = good();
        c.detect_hz = hz;
        INFO("detect_hz " << hz);
        CHECK(camera_problem(c) == "");
    }
}

TEST_CASE("a sub-stream too small to recognise anybody at any distance is refused") {
    CameraDraft c = good();
    c.role = "gate";
    c.face_enabled = true;
    c.probed_width = 320;
    c.probed_height = 240;
    const std::string p = camera_problem(c);
    REQUIRE(p != "");
    CHECK(p.find("320x240") != std::string::npos);   // says WHICH size, not just "too small"

    // 640x360 is accepted now. It used to be refused on the arithmetic for a
    // 60 px face; the identifier admits small faces on evidence instead, so a
    // camera people walk close to does identify them at this size. Whether it
    // actually does is reported later from MEASURED face sizes, which beats
    // guessing from the resolution.
    c.probed_width = 640;
    c.probed_height = 360;
    CHECK(camera_problem(c) == "");

    // The rule is about the pixels, not the label on the camera.
    c.role = "reception";
    c.probed_width = 320;
    c.probed_height = 240;
    CHECK(camera_problem(c) != "");

    // Choosing the main stream is the documented way out.
    c.stream_pref = "main";
    CHECK(camera_problem(c) == "");

    // Not yet probed: warn later, do not block the operator now.
    c.stream_pref = "sub";
    c.probed_width = 0;
    c.probed_height = 0;
    CHECK(camera_problem(c) == "");

    // The same small sub-stream is fine when the camera is not identifying anybody.
    c.probed_width = 640;
    c.probed_height = 360;
    c.face_enabled = false;
    CHECK(camera_problem(c) == "");
}
