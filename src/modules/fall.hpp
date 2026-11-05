#pragma once

#include "modules/module.hpp"

#include <chrono>
#include <map>

namespace so {

enum class Posture { Up, Down, Unknown };

struct PostureVerdict {
    Posture verdict{Posture::Unknown};
    const char* why{""};
    float torso_angle{0};    ///< degrees away from vertical; 0 = upright
    bool head_low{false};
    bool knees_high{false};
};

struct FallConfig {
    /// Torso angle past which the body reads as horizontal. Two standing people
    /// on this hardware measured 0.9 and 7.5 degrees, so the gap to 55 is wide.
    float torso_down_deg = 55.0f;
    /// Head-to-ankle span as a fraction of the person's own body span. Catches
    /// a fall ALONG the camera axis, where the torso foreshortens and its angle
    /// stays deceptively small.
    float head_low_frac = 0.55f;
    /// A seated person's knees sit near hip level. This is the test that keeps
    /// chairs and low sofas from reading as collapses — the one posture both
    /// signals above get wrong.
    float knee_hip_frac = 0.25f;
    float kp_conf = 0.35f;
    /// Fallback when the keypoints are unusable: a box wider than it is tall.
    float box_aspect_down = 1.25f;

    std::chrono::milliseconds confirm{1200};   ///< held this long -> FALLEN
    std::chrono::milliseconds help{10000};     ///< still down this much later -> NEED HELP
    /// Movement below this fraction of body height counts as "not getting up".
    float still_frac = 0.12f;
    std::chrono::milliseconds cooldown{120000};
};

enum class FallState { None, Fallen, NeedHelp };

/// Posture from the 17 keypoints. Unknown matters as much as the verdicts: it
/// is how the caller knows to fall back to box geometry rather than trust a
/// guess built on keypoints the model was unsure about.
PostureVerdict posture(const std::array<Keypoint, kNumKeypoints>& kps,
                       const FallConfig& cfg = {});

/**
 * Fall detection, in two stages.
 *
 * A bounding box cannot express posture: someone crouching to pick something up
 * and someone lying on the floor both produce a wide box, and someone falling
 * straight towards the camera produces a tall one. So the body is read directly
 * from the keypoints.
 *
 * Either posture signal is sufficient on its own, because they fail in
 * different situations. What prevents false alarms is not stacking signals, it
 * is PERSISTENCE: somebody who crouches is upright again within a second,
 * somebody who fell is not.
 *
 *   FALLEN     the posture has held for `confirm`. Worth showing.
 *   NEED HELP  still down and barely moving `help` later. Worth interrupting
 *              a human for. Separating the two is what stops the module crying
 *              wolf every time somebody kneels.
 */
class FallTracker {
public:
    explicit FallTracker(FallConfig cfg = {});

    /// Feed one tracked person. Returns their current state.
    FallState feed(int track_id, const Track& track,
                   std::chrono::steady_clock::time_point now);

    FallState state(int track_id) const;
    void forget(int track_id);
    void prune(std::chrono::steady_clock::time_point now);

private:
    struct State {
        FallState state{FallState::None};
        std::chrono::steady_clock::time_point down_since{};
        std::chrono::steady_clock::time_point last_seen{};
        bool down{false};
        float last_cx{0}, last_cy{0}, moved{0};
        float height{1};
    };
    FallConfig cfg_;
    std::map<int, State> tracks_;
};

/// The module wrapper: turns state transitions into events.
class FallModule : public IModule {
public:
    explicit FallModule(FallConfig cfg = {});
    const char* id() const override { return "fall"; }
    void on_frame(const FrameContext& ctx, EventSink& sink) override;

private:
    FallConfig cfg_;
    FallTracker tracker_;
    std::map<int, std::chrono::steady_clock::time_point> last_alert_;
};

}  // namespace so
