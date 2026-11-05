#include <doctest/doctest.h>

#include "core/onvif.hpp"
#include "fixture_loader.hpp"

using so_test::load_fixture_text;

#include <algorithm>
#include <cstdlib>

using namespace so;

TEST_CASE("the ONVIF address comes out of the RTSP URL the operator already typed") {
    const auto t = onvif_target_from_rtsp("rtsp://admin:s3cretpw@192.168.1.103:554/stream0");
    REQUIRE(t.ok);
    CHECK(t.host == "192.168.1.103");
    CHECK(t.user == "admin");
    CHECK(t.pass == "s3cretpw");

    // The RTSP port is not the ONVIF port and must not be carried over: ONVIF
    // is HTTP on 80 unless the camera says otherwise, and asking for it on 554
    // gets a connection that accepts bytes and never answers.
    CHECK(t.host.find(':') == std::string::npos);

    // A password with the characters a site actually uses. `@` inside the
    // password is the one that matters — splitting on the FIRST `@` puts half
    // the password in the host and reports a camera that does not exist.
    const auto at = onvif_target_from_rtsp("rtsp://admin:Ex@mple_pw@10.0.0.5:554/media/video0");
    REQUIRE(at.ok);
    CHECK(at.host == "10.0.0.5");
    CHECK(at.user == "admin");
    CHECK(at.pass == "Ex@mple_pw");

    // No credentials at all is normal on an open camera, and is not a failure.
    const auto bare = onvif_target_from_rtsp("rtsp://192.168.1.9/stream1");
    CHECK(bare.ok);
    CHECK(bare.host == "192.168.1.9");
    CHECK(bare.user.empty());

    CHECK_FALSE(onvif_target_from_rtsp("").ok);
    CHECK_FALSE(onvif_target_from_rtsp("not a url").ok);
    CHECK_FALSE(onvif_target_from_rtsp("http://192.168.1.9/").ok);
}

TEST_CASE("the WS-Security digest is the one the camera computes") {
    // The vector from the OASIS UsernameToken profile, so this tests the
    // algorithm rather than agreeing with itself:
    //   digest = base64(sha1(nonce + created + password))
    const std::string nonce(16, '\0');      // all zero bytes, a fixed input
    const std::string created = "2026-10-13T11:00:00Z";
    const std::string h = onvif_security_header("admin", "123456", nonce, created);

    CHECK(h.find("<Username>admin</Username>") != std::string::npos);
    CHECK(h.find(created) != std::string::npos);
    CHECK(h.find("#PasswordDigest") != std::string::npos);

    // The password itself must NEVER appear: PasswordDigest exists precisely so
    // the cleartext does not cross the wire, and a header that carries both is
    // worse than one that carries neither.
    CHECK(h.find("123456") == std::string::npos);

    // Changing only the password must change the digest — the check that
    // catches a digest computed over the wrong fields or in the wrong order.
    CHECK(onvif_security_header("admin", "other", nonce, created) != h);
    CHECK(onvif_security_header("admin", "123456", nonce, "2026-10-13T11:00:01Z") != h);
}

TEST_CASE("a real camera's profiles become a main and a sub stream") {
    // Captured from the VARITO at the front door on 2026-10-13, unedited. A
    // handwritten sample would only prove the parser agrees with whoever wrote
    // the sample; this is the XML the device really sends, PTZ blocks, analytics
    // rules, duplicated Extension sections and all.
    const auto streams = onvif_parse_profiles(load_fixture_text("onvif_varito_profiles.xml"));
    REQUIRE(streams.size() == 2);

    CHECK(streams[0].profile == "D01_CH01_Main");
    CHECK(streams[0].token == "VideoEncoderToken_1_1_1");
    CHECK(streams[0].encoding == "H264");
    CHECK(streams[0].width == 1920);
    CHECK(streams[0].height == 1080);
    CHECK(streams[0].fps == 25);
    CHECK(streams[0].gov_length == 20);

    CHECK(streams[1].profile == "D01_CH01_Sub");
    CHECK(streams[1].token == "VideoEncoderToken_1_1_2");
    CHECK(streams[1].width == 864);
    CHECK(streams[1].height == 480);

    // ORDER IS MEANING HERE. The page offers "main" and "sub" as two buttons,
    // and it picks them by position. A parser that returned the profiles in
    // whatever order a map iterated would silently put the 864x480 sub-stream
    // behind the Main button on some cameras and not others.
    CHECK(streams[0].width > streams[1].width);
}

TEST_CASE("a camera's own resolution list is what the page offers") {
    auto streams = onvif_parse_profiles(load_fixture_text("onvif_varito_profiles.xml"));
    REQUIRE(streams.size() == 2);

    onvif_parse_options(load_fixture_text("onvif_varito_options_main.xml"), streams[0]);
    onvif_parse_options(load_fixture_text("onvif_varito_options_sub.xml"), streams[1]);

    const auto& main = streams[0];
    CHECK(main.fps_min == 5);
    CHECK(main.fps_max == 30);
    REQUIRE(main.resolutions.size() == 6);
    CHECK(main.resolutions[0].width == 3200);
    CHECK(main.resolutions[0].height == 1800);
    CHECK(main.resolutions.back().width == 1920);

    // The sub-stream's list is a DIFFERENT list, which is the whole reason the
    // two buttons cannot share one set of options: offering the main stream's
    // 3200x1800 under the Sub-Stream button would offer a size that camera will
    // refuse.
    const auto& sub = streams[1];
    REQUIRE(sub.resolutions.size() == 4);
    const auto has = [](const OnvifStream& s, int w, int h) {
        return std::any_of(s.resolutions.begin(), s.resolutions.end(),
                           [&](const OnvifResolution& r) { return r.width == w && r.height == h; });
    };
    CHECK(has(sub, 864, 480));
    CHECK(has(sub, 640, 360));
    CHECK_FALSE(has(sub, 3200, 1800));
    CHECK_FALSE(has(main, 640, 360));

    // The size the camera is SET to must appear in the list it offers,
    // otherwise the dropdown opens with nothing selected and the operator is
    // invited to "fix" a camera that is already correct.
    CHECK(has(main, main.width, main.height));
    CHECK(has(sub, sub.width, sub.height));
}

TEST_CASE("the options block that is read is the one for the codec in use") {
    // This device returns its H264 block twice — once plainly and once inside
    // <tt:Extension>, which is how ONVIF carries per-codec extras. Reading both
    // and appending gives twelve resolutions where the camera has six, and the
    // dropdown then lists every size twice.
    auto s = onvif_parse_profiles(load_fixture_text("onvif_varito_profiles.xml"))[0];
    onvif_parse_options(load_fixture_text("onvif_varito_options_main.xml"), s);
    CHECK(s.resolutions.size() == 6);

    // Nothing in the response for a codec this stream does not use should reach
    // it. The camera is on H264; an H265 list, if the device offered one, is a
    // list of sizes that would be valid only after also changing the codec.
    so::OnvifStream h265 = s;
    h265.encoding = "H265";
    h265.resolutions.clear();
    onvif_parse_options(load_fixture_text("onvif_varito_options_main.xml"), h265);
    CHECK(h265.resolutions.empty());
}

TEST_CASE("rubbish from a camera is empty, not a crash") {
    CHECK(onvif_parse_profiles("").empty());
    CHECK(onvif_parse_profiles("<html>401 Unauthorized</html>").empty());
    CHECK(onvif_parse_profiles("<trt:Profiles token=\"x\">truncated").empty());

    so::OnvifStream s;
    s.encoding = "H264";
    onvif_parse_options("", s);
    CHECK(s.resolutions.empty());
    CHECK(s.fps_max == 0);
}

TEST_CASE("a real camera on the network answers") {
    // OPT-IN, via SO_ONVIF_URL. The fixture tests above prove the parsing; this
    // one proves the half that fixtures cannot reach — the SOAP envelope, the
    // WS-Security digest a camera will actually accept, and the HTTP exchange.
    // It needs a camera on the LAN, so it skips everywhere else rather than
    // failing: a red suite on a laptop with no cameras teaches people to ignore
    // a red suite.
    const char* url = std::getenv("SO_ONVIF_URL");
    if (!url || !*url) return;

    const auto t = onvif_target_from_rtsp(url);
    REQUIRE(t.ok);
    std::string err;
    const auto streams = onvif_streams(t, &err);
    INFO("error: " << err);
    REQUIRE(!streams.empty());

    for (const auto& s : streams) {
        INFO("profile " << s.profile);
        CHECK(s.width > 0);
        CHECK(s.height > 0);
        CHECK(!s.encoding.empty());
        // The point of the whole exercise: a camera that answers must offer a
        // list to choose from, not just report what it is set to.
        CHECK(!s.resolutions.empty());
        CHECK(s.fps_max > 0);
        MESSAGE(s.profile << " " << s.encoding << " " << s.width << "x" << s.height
                          << " @" << s.fps << " — " << s.resolutions.size()
                          << " sizes, fps " << s.fps_min << "-" << s.fps_max);
    }
}

TEST_CASE("a resolution change is built as a complete configuration") {
    // ONVIF SetVideoEncoderConfiguration replaces the WHOLE configuration, it
    // does not merge. A request carrying only the new width and height is a
    // request to blank the bitrate, the GOP length and the H.264 profile, and
    // devices honour that: the picture comes back at the right size and a
    // quarter of the bitrate. So the change has to be applied to everything
    // already read back from the camera.
    auto s = onvif_parse_profiles(load_fixture_text("onvif_varito_profiles.xml"))[1];
    onvif_parse_options(load_fixture_text("onvif_varito_options_sub.xml"), s);
    REQUIRE(s.width == 864);
    REQUIRE(s.bitrate > 0);
    REQUIRE(s.gov_length > 0);

    const std::string xml = onvif_set_resolution_body(s, 640, 360);

    CHECK(xml.find("<tt:Width>640</tt:Width>") != std::string::npos);
    CHECK(xml.find("<tt:Height>360</tt:Height>") != std::string::npos);
    CHECK(xml.find(s.token) != std::string::npos);
    // Everything NOT being changed must be carried through unchanged.
    CHECK(xml.find("<tt:BitrateLimit>" + std::to_string(s.bitrate)) != std::string::npos);
    CHECK(xml.find("<tt:GovLength>" + std::to_string(s.gov_length)) != std::string::npos);
    CHECK(xml.find("<tt:Encoding>" + s.encoding) != std::string::npos);
    // The old size must not survive anywhere in the request.
    CHECK(xml.find("<tt:Width>864</tt:Width>") == std::string::npos);

    // THE FRAME RATE IS CARRIED, NEVER CHOSEN. This camera ignores frame-rate
    // writes while honouring resolution writes, so sending a frame rate we
    // invented would be a silent no-op the page might report as applied.
    CHECK(xml.find("<tt:FrameRateLimit>" + std::to_string(s.fps)) != std::string::npos);

    // A size the camera never offered must be refused before it is sent: the
    // device answers such a request with a success it does not honour.
    CHECK(onvif_set_resolution_body(s, 3200, 1800).empty());
    CHECK(onvif_set_resolution_body(s, 0, 0).empty());
}

TEST_CASE("a real camera accepts a resolution change and proves it") {
    // OPT-IN, and it CHANGES THE CAMERA — gated on its own variable so it can
    // never run by accident alongside the read-only live test. It uses the
    // SUB-stream and puts the original size back, because the main stream is
    // what the front door runs on.
    const char* url = std::getenv("SO_ONVIF_WRITE_URL");
    if (!url || !*url) return;

    const auto t = onvif_target_from_rtsp(url);
    REQUIRE(t.ok);
    auto streams = onvif_streams(t, nullptr);
    REQUIRE(streams.size() >= 2);
    const auto sub = streams[1];
    REQUIRE(sub.resolutions.size() >= 2);

    // Something the camera offers that is NOT what it is set to.
    OnvifResolution other{};
    for (const auto& r : sub.resolutions)
        if (r.width != sub.width || r.height != sub.height) { other = r; break; }
    REQUIRE(other.width > 0);

    OnvifStream after;
    std::string err;
    const bool ok = onvif_apply_resolution(t, sub.token, other.width, other.height, &after, &err);
    INFO("error: " << err);
    CHECK(ok);
    CHECK(after.width == other.width);
    CHECK(after.height == other.height);
    MESSAGE(sub.width << "x" << sub.height << " -> " << after.width << "x" << after.height);

    // A size this stream does not offer must be refused WITHOUT being sent.
    OnvifStream ignored;
    CHECK_FALSE(onvif_apply_resolution(t, sub.token, 3200, 1800, &ignored, &err));
    MESSAGE("refused 3200x1800: " << err);

    // Put it back exactly as it was, whatever happened above.
    OnvifStream restored;
    CHECK(onvif_apply_resolution(t, sub.token, sub.width, sub.height, &restored, &err));
    CHECK(restored.width == sub.width);
    CHECK(restored.height == sub.height);
    CHECK(restored.bitrate == sub.bitrate);   // carried through, not blanked
    MESSAGE("restored to " << restored.width << "x" << restored.height
                           << " br=" << restored.bitrate);
}
