#pragma once

#include "identity/gallery.hpp"
#include "vision/scrfd.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace so {

/// Head pose derived from the five landmarks alone.
struct FacePose {
    float yaw{0};     ///< sideways turn, in eye-widths. 0 = square on.
    float pitch{0};   ///< where the nose sits between the eye and mouth lines
    float roll{0};    ///< in-plane tilt of the eye line, degrees
};

/**
 * Yaw / pitch / roll from five landmarks.
 *
 * Measured in the EYE-LINE frame, so head roll does not leak into yaw — a
 * person with their head tilted is not a person facing sideways, and conflating
 * them throws away usable frames.
 */
FacePose face_pose(const std::array<Landmark, kNumLandmarks>& lm);

struct IdentifierConfig {
    /// A frame may contribute to an identity only below this yaw.
    ///
    /// Not a tunable preference: beyond roughly 0.35 eye-widths the best WRONG
    /// identity outscores the right one, so such frames cannot be allowed to
    /// name anybody at ANY threshold. 0.22 keeps a margin below that.
    float max_yaw = 0.22f;
    /// Frames count in proportion to exp(-(yaw/sigma)^2), so a square-on frame
    /// outweighs a half-turned one rather than merely outvoting it.
    float yaw_sigma = 0.12f;
    /// Face width at which a frame carries full weight; below it, linearly less.
    float full_px = 96.0f;
    /// Below this a face is not matched at all.
    ///
    /// This was 60 px, which threw away the one mechanism that already handles
    /// small faces properly. A frame's weight is min(1, face_px/full_px), so a
    /// 27 px face is worth 0.28 and needs five square-on looks to reach
    /// min_weight, where a 100 px face needs one. Small faces are therefore
    /// admitted on EVIDENCE rather than on a size rule — and a hard floor at 60
    /// px meant they were never weighed in the first place.
    ///
    /// Measured on this site's own footage, at the sub-stream sizes these
    /// cameras actually deliver: the deployed model named 25-27 px faces
    /// correctly at scores of 0.42-0.62, well clear of the 0.28 floor. Those
    /// faces were being refused with "0.00" — not "no match", but "never
    /// looked at", which is the worst kind of silent failure.
    ///
    /// 24 px is where SCRFD's own landmarks stop being reliable enough for the
    /// alignment to mean anything, so matching below it would be guesswork.
    float min_face_px = 24.0f;
    /// Below this a detection is not weighed at all.
    ///
    /// Matched to the DETECTOR's own threshold, which is 0.50. It was 0.70, a
    /// second bar nothing explained: a face the detector accepted and the live
    /// view drew a box around was then silently never compared to anybody. A
    /// clearly recognisable 34 px face on this site was refused that way, and
    /// the picture said only "faint".
    ///
    /// Confidence still counts — it scales the frame's weight (see observe())
    /// instead of deciding it. A hesitant detection contributes less and needs
    /// company; it is not thrown away.
    float min_det_score = 0.50f;
    /// How many of a track's best frames the template pools. Bounded because a
    /// person who glances at the camera once must be identified from that
    /// glance, not from the minutes of profile view around it.
    int template_frames = 16;
    /// Total quality weight before a name may be proposed. One square-on,
    /// well-detected, close face is worth ~0.8, so this is "one good look plus
    /// a little confirmation" — deliberately not a frame count, so a 6 fps live
    /// wall and a 30 fps clip behave the same.
    float min_weight = 1.2f;
    /// Score floor and the margin over the runner-up. Both must hold: a high
    /// score that ties with somebody else is not an identification.
    ///
    /// These were 0.28 and 0.06, and measured against the people actually
    /// enrolled here they were far too loose: FOURTEEN pairs of different
    /// people scored above 0.28 against each other, the worst at 0.435. A
    /// 32 px face, whose embedding is noisy anyway, could clear that floor
    /// against the wrong person — and did: a man was labelled "Eshita" at 0.46.
    ///
    /// 0.38 sits above all but two impostor pairs and below the 0.42-0.84 that
    /// genuine matches score on this site. The margin matters more than the
    /// floor, though: the failure is rarely a high score, it is two similar
    /// scores where the noise decided which came first. 0.12 is wide enough
    /// that a near-tie is refused and narrow enough that a real match, which
    /// typically leads its runner-up by 0.2 or more, still passes.
    float min_score = 0.38f;
    float min_margin = 0.12f;
    /// How much a new frame may disagree with what a track has pooled before
    /// it is treated as a DIFFERENT PERSON rather than a poor look.
    ///
    /// Boxes change hands when people cross, and the name follows the box. The
    /// faces are what give it away: the same person's frames agree at about 0.6
    /// and up here, while different people average 0.18 and never exceed 0.44.
    /// 0.30 sits in the empty space between, so a genuine bad angle survives
    /// and a swapped identity does not.
    float same_face_min = 0.30f;
    /// Consecutive disagreeing frames before a track is treated as having
    /// changed person. One is noise — a blur, a half-turn, a hand across the
    /// face. Four in a row is somebody else.
    int swap_frames = 4;

    /// Once settled, a name is only replaced by a clearly better one.
    float rename_margin = 0.15f;
    /// Tracks idle longer than this are forgotten.
    std::chrono::seconds track_ttl{60};
};

struct Identification {
    std::int64_t employee_id{0};    ///< 0 = not identified
    std::string name;
    float score{0};
    float margin{0};
    float weight{0};                ///< evidence accumulated so far
    bool settled{false};
};

/**
 * Decides who a tracked face belongs to.
 *
 * A tracked face keeps a QUALITY-WEIGHTED TEMPLATE of its most frontal frames,
 * that template is scored against the CENTRED gallery, and a name appears only
 * once the evidence clears a floor AND beats the runner-up by a margin.
 *
 * Why not a per-frame argmax: two embeddings of the same face in consecutive
 * frames agree at only about 0.61 cosine at these resolutions, so one frame is
 * not evidence. Why not average the whole track: that averages the turned
 * frames back in — pooling a real track over all its frames gave a margin of
 * 0.069 over the next identity, pooling only its most frontal frames gave 0.204.
 */
class FaceIdentifier {
public:
    FaceIdentifier(const Gallery& gallery, IdentifierConfig cfg = {});

    /// Feed one observation of a tracked face. Returns the current verdict.
    Identification observe(int track_id, const std::vector<float>& embedding,
                           float yaw, float face_px, float det_score,
                           std::chrono::steady_clock::time_point now =
                               std::chrono::steady_clock::now());

    /// Current verdict without adding evidence.
    Identification current(int track_id) const;

    /// WHO THE POOLED EVIDENCE MOST RESEMBLES, whatever the gates say.
    ///
    /// `current()` answers "who is this", and correctly says nobody until the
    /// evidence is strong enough. That is right for naming somebody on screen
    /// and wrong for the one case where there will BE no more evidence: a
    /// person walking out gets a look or two, the pooled weight never reaches
    /// the bar, and rescore() returns before it has even compared them to
    /// anybody. The frames were captured, embedded and then dropped.
    ///
    /// This compares anyway and reports the top match with its score, margin
    /// and weight, so a caller can keep the evidence for a human to confirm.
    /// `settled` is always false: it is NOT an identification and must never
    /// be written to attendance as one.
    Identification best_guess(int track_id) const;

    /// Did this track ever produce a face good enough to judge? This is what
    /// separates "we saw them and they are not enrolled" from "we never got a
    /// look" — the difference between an unauthorised-person alert and silence.
    bool was_judged(int track_id) const;

    /// Does this track still need looks before it can be named?
    ///
    /// This is what decides who gets the camera's limited face budget when
    /// more people are in frame than it can crop in one cycle, and it is NOT
    /// the same question as `was_judged`. A track is judged after ONE usable
    /// frame, but naming needs `min_weight` of accumulated evidence — about
    /// five square-on looks at a 27 px face. Ordering by "judged" therefore
    /// demoted everybody as soon as each had been seen once, after which the
    /// budget went to whoever was CLOSEST, every cycle. Three people walking
    /// in together would each get one look and then the nearest would take
    /// the rest, so the person at the back was never named.
    ///
    /// Bounded on purpose: once a track has enough evidence it stops asking,
    /// whether or not that evidence produced a name. Otherwise somebody who
    /// is not enrolled — and never can be named — would hold the budget for
    /// as long as they stood there.
    bool needs_more_looks(int track_id) const;

    void forget(int track_id);
    void prune(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    std::size_t tracked() const { return tracks_.size(); }

private:
    struct TrackState {
        std::vector<std::pair<float, std::vector<float>>> best;  ///< (weight, embedding)
        float weight{0};
        bool judged{false};
        /// Consecutive frames that did not match what this track has pooled.
        int disagreements{0};
        Identification id;
        std::chrono::steady_clock::time_point last_seen{};
    };

    void rescore(int track_id, TrackState& st, const GallerySnapshot& g);
    /// Pool a track's frames and compare the result to the gallery. Shared by
    /// rescore() and best_guess() so the two can never drift into answering
    /// the same question differently.
    static Identification match(const TrackState& st, const GallerySnapshot& g);
    /// The other live track holding this identity, if any.
    TrackState* other_holder(int track_id, std::int64_t employee_id);

    const Gallery& gallery_;
    IdentifierConfig cfg_;
    std::map<int, TrackState> tracks_;
    mutable std::uint64_t seen_version_{0};
};

}  // namespace so
