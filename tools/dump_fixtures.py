#!/usr/bin/env python3
"""Generate the C++ decoder test fixtures from the real NPU.

Every decoder in `src/vision/` reproduces, in C++, a tensor layout that only
the device can tell us the truth about. Rather than trust a formula written
from memory, this script captures BOTH sides of the same inference:

    use_ort=True   the fused, already-decoded tensor DX-RT produces by running
                   the model's tail on the CPU through ONNX Runtime.  This is
                   the ground truth.
    use_ort=False  the raw head the NPU emits, which is what the C++ decoders
                   actually consume at run time.

The C++ tests then decode the raw tensors and assert they match the ORT rows.
That is how the keypoint formula was settled: three plausible candidates were
wrong by 22-40 px and the fourth by 0.000 px.

Run it whenever a model is recompiled:

    source <dx-runtime venv>/bin/activate
    python3 tools/dump_fixtures.py
"""
from __future__ import annotations

import argparse
import math
import pathlib
import sys

import cv2
import numpy as np

try:
    from dx_engine import InferenceEngine, InferenceOption
except ImportError:
    sys.exit("dx_engine not importable — activate the dx-runtime venv first")

CANVAS = 640
PAD = 114


def letterbox(frame: np.ndarray, size: int = CANVAS):
    """Byte-for-byte the same transform as src/core/preprocess.cpp."""
    h, w = frame.shape[:2]
    scale = min(size / w, size / h)
    nw, nh = int(round(w * scale)), int(round(h * scale))
    canvas = np.full((size, size, 3), PAD, np.uint8)
    dx, dy = (size - nw) // 2, (size - nh) // 2
    canvas[dy:dy + nh, dx:dx + nw] = cv2.resize(frame, (nw, nh), interpolation=cv2.INTER_LINEAR)
    return canvas, scale, dx, dy


def run(model: str, canvas_bgr: np.ndarray, use_ort: bool):
    opt = InferenceOption()
    opt.devices = [0]
    # Per-core binding, never NPU_ALL — the same rule the C++ EnginePool keeps.
    opt.bound_option = InferenceOption.BOUND_OPTION.NPU_0
    opt.use_ort = use_ort
    ie = InferenceEngine(model, opt)
    inp = np.ascontiguousarray(cv2.cvtColor(canvas_bgr, cv2.COLOR_BGR2RGB)[None, ...])
    out = ie.run(inp)
    arrs = [np.asarray(t) for t in (out if isinstance(out, (list, tuple)) else [out])]
    del ie
    return arrs


def write_tensors(out_dir: pathlib.Path, prefix: str, arrs: list[np.ndarray]) -> None:
    """One .bin per tensor plus a plain-text manifest.

    Deliberately not JSON: the C++ tests then need no JSON parser, which keeps
    the vision-core test binary dependent on nothing but OpenCV and DXRT.
    Format, one line per tensor:  <index> <ndim> <d0..dn> <filename>
    """
    lines = []
    for i, a in enumerate(arrs):
        a = np.ascontiguousarray(a.astype(np.float32))
        name = f"{prefix}_{i}.bin"
        (out_dir / name).write_bytes(a.tobytes())
        lines.append(f"{i} {a.ndim} {' '.join(str(d) for d in a.shape)} {name}")
    (out_dir / f"{prefix}.manifest").write_text("\n".join(lines) + "\n")
    print(f"  wrote {len(arrs)} tensors -> {prefix}.manifest")


def pick_frame(videos: list[str], model: str, want: int = 2):
    """First frame in the corpus with at least `want` confident people."""
    opt = InferenceOption()
    opt.devices = [0]
    opt.bound_option = InferenceOption.BOUND_OPTION.NPU_0
    opt.use_ort = True
    ie = InferenceEngine(model, opt)
    best = None
    try:
        for clip in videos:
            cap = cv2.VideoCapture(clip)
            for idx in range(0, 400, 20):
                cap.set(cv2.CAP_PROP_POS_FRAMES, idx)
                ok, fr = cap.read()
                if not ok:
                    break
                canvas, *_ = letterbox(fr)
                inp = np.ascontiguousarray(cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)[None, ...])
                rows = np.asarray(ie.run(inp)[0]).reshape(-1, 57)
                n = int((rows[:, 4] >= 0.5).sum())
                if best is None or n > best[0]:
                    best = (n, clip, idx, fr)
                if n >= want:
                    cap.release()
                    return best
            cap.release()
    finally:
        del ie
    return best



# ── SCRFD reference decode ──────────────────────────────────────────────────
# Ported from the VisionX pipeline that has been running on this hardware, with
# one deliberate change: it consumes tensors selected BY SHAPE rather than by
# position.  VisionX indexes outputs[i], outputs[i+3], outputs[i+6], which is
# only correct for ONNX Runtime's canonical output order.  Probed natively on
# the NPU the same model returns them as bbox80, kps80, score80, score40,
# bbox40, kps40, bbox20, kps20, score20 - so an order-based decoder silently
# reads the wrong tensor.  Grouping by channel count (2 = score, 8 = bbox,
# 20 = keypoints) is order-independent and is what the C++ decoder does too.

SCRFD_STRIDES = (8, 16, 32)


def _sigmoid(x):
    return 1.0 / (1.0 + np.exp(-np.clip(x, -30, 30)))


def _nms(boxes, scores, thr):
    if len(boxes) == 0:
        return []
    x1, y1, x2, y2 = boxes[:, 0], boxes[:, 1], boxes[:, 2], boxes[:, 3]
    areas = (x2 - x1 + 1) * (y2 - y1 + 1)
    order = scores.argsort()[::-1]
    keep = []
    while order.size > 0:
        i = order[0]
        keep.append(i)
        xx1 = np.maximum(x1[i], x1[order[1:]]); yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]]); yy2 = np.minimum(y2[i], y2[order[1:]])
        inter = np.maximum(0.0, xx2 - xx1 + 1) * np.maximum(0.0, yy2 - yy1 + 1)
        ovr = inter / (areas[i] + areas[order[1:]] - inter)
        order = order[np.where(ovr <= thr)[0] + 1]
    return keep


def scrfd_group(arrs):
    """{stride: (score[A], bbox[A,4], kps[A,10])}, selected by channel count.

    Handles BOTH layouts this model can present, because the two paths do not
    agree and neither the order nor the shape is safe to assume:

        native NPU   (1, S, S, 2 / 8 / 20)   score is a RAW LOGIT
        ONNX Runtime (1, A, 1 / 4 / 10)      score is ALREADY sigmoid'd,
                                             A = S*S*2 (2 anchors per cell)

    Measured on this board: bbox and keypoint values are numerically identical
    between the two paths (e.g. stride-8 bbox spans 0.016..3.573 either way),
    so only the score needs activating, and only on the native path.
    """
    out = {}
    for a in arrs:
        a = np.squeeze(np.asarray(a))
        if a.ndim == 3:                       # (S, S, C) native
            grid, chan = a.shape[0], a.shape[2]
            n = grid * grid * 2
            per = {2: ("score", 1), 8: ("bbox", 4), 20: ("kps", 10)}
        elif a.ndim == 2:                     # (A, C) from ORT
            grid = int(round(math.sqrt(a.shape[0] / 2)))
            chan = a.shape[1]
            n = a.shape[0]
            per = {1: ("score", 1), 4: ("bbox", 4), 10: ("kps", 10)}
        elif a.ndim == 1:                     # (A,) — an ORT score tensor
            grid = int(round(math.sqrt(a.shape[0] / 2)))
            chan = 1
            n = a.shape[0]
            per = {1: ("score", 1)}
        else:
            raise ValueError(f"unexpected SCRFD tensor shape {a.shape}")
        if chan not in per:
            raise ValueError(f"unexpected SCRFD channel count {chan} in {a.shape}")
        kind, width = per[chan]
        flat = a.reshape(n, width)
        out.setdefault(CANVAS // grid, {})[kind] = flat[:, 0] if kind == "score" else flat

    groups = {}
    for stride, d in out.items():
        missing = {"score", "bbox", "kps"} - set(d)
        if missing:
            raise ValueError(f"stride {stride} is missing {sorted(missing)}")
        groups[stride] = (d["score"], d["bbox"], d["kps"])
    return groups


def scrfd_decode(arrs, conf=0.5, nms_thr=0.4, size=CANVAS, cell_offset=0.0):
    """-> list of [x1, y1, x2, y2, score, 5x(x, y)] in CANVAS coordinates.

    `cell_offset` places the anchor within its grid cell. Upstream insightface
    uses the cell ORIGIN (0.0); the VisionX pipeline on this board uses the cell
    CENTRE (0.5). Half a stride is 4 px at stride 8 but 16 px at stride 32, so
    it is not a rounding detail — at the wrong offset the nose landmark lands
    about one eye-width off, which corrupts the Umeyama alignment and therefore
    every embedding computed from it.

    SETTLED BY MEASUREMENT (2026-09-24, 96 paired samples across the whole clip
    corpus). The discriminator is point-to-point and independent: SCRFD's own
    nose LANDMARK against the pose model's nose KEYPOINT on the same frame,
    normalised by the subject's eye separation.

        cell_offset = 0.0    median error 0.2364 eye-widths
        cell_offset = 0.5    median error 0.9737 eye-widths   (4.1x worse)

    So 0.0 it is. An earlier, weaker check — nose-to-face-box-CENTRE distance on
    a single frame — preferred 0.5, which is why it was rerun properly: the nose
    is not at the centre of a face box, so that distance barely discriminates.
    """
    groups = scrfd_group(arrs)
    S, B, K = [], [], []
    for stride in SCRFD_STRIDES:
        if stride not in groups:
            continue
        score, bbox, kps = groups[stride]
        feat = size // stride
        if score.max() > 1.0 or score.min() < 0.0:
            score = _sigmoid(score)
        yy, xx = np.mgrid[:feat, :feat]
        centers = (np.vstack([xx.ravel(), yy.ravel()]).T + cell_offset) * stride
        centers = np.repeat(centers, 2, axis=0)        # 2 anchors per cell
        m = score > conf
        if not m.any():
            continue
        sc, bb, kk, cc = score[m], bbox[m], kps[m], centers[m]
        db = np.stack([cc[:, 0] - bb[:, 0] * stride,
                       cc[:, 1] - bb[:, 1] * stride,
                       cc[:, 0] + bb[:, 2] * stride,
                       cc[:, 1] + bb[:, 3] * stride], axis=1)
        dk = np.empty_like(kk)
        for k in range(5):
            dk[:, k * 2]     = cc[:, 0] + kk[:, k * 2] * stride
            dk[:, k * 2 + 1] = cc[:, 1] + kk[:, k * 2 + 1] * stride
        S.append(sc); B.append(db); K.append(dk)
    if not S:
        return []
    score = np.concatenate(S); bbox = np.concatenate(B); kps = np.concatenate(K)
    keep = _nms(bbox, score, nms_thr)
    order = sorted(keep, key=lambda i: -score[i])
    return [np.concatenate([bbox[i], [score[i]], kps[i]]) for i in order]



# ── ArcFace alignment ───────────────────────────────────────────────────────
# The canonical 5-point template ArcFace was trained against, in its 112x112
# input frame.
ARCFACE_REF = np.array([
    [38.2946, 51.6963], [73.5318, 51.5014], [56.0252, 71.7366],
    [41.5493, 92.3655], [70.7299, 92.2041],
], dtype=np.float64)


def umeyama(src, dst):
    """Least-squares similarity transform (rotation + uniform scale + shift).

    Umeyama 1991 — the closed form behind skimage's SimilarityTransform and what
    the ArcFace reference alignment uses. Deterministic, and it uses all five
    landmarks, unlike a RANSAC/LMEDS fit over a 2-point minimal subset.
    """
    src = np.asarray(src, np.float64)
    dst = np.asarray(dst, np.float64)
    n = src.shape[0]
    src_mean, dst_mean = src.mean(0), dst.mean(0)
    src_c, dst_c = src - src_mean, dst - dst_mean
    U, S, Vt = np.linalg.svd(dst_c.T @ src_c / n)
    d = np.ones(2)
    if np.linalg.det(U) * np.linalg.det(Vt) < 0:
        d[-1] = -1
    R = U @ np.diag(d) @ Vt
    var_src = (src_c ** 2).sum() / n
    scale = 1.0 if var_src < 1e-12 else float((S * d).sum() / var_src)
    M = np.zeros((2, 3), np.float64)
    M[:, :2] = scale * R
    M[:, 2] = dst_mean - scale * R @ src_mean
    return M


def face_yaw(lm):
    """Sideways head turn, in eye-widths, from the five landmarks alone.

    0 is square on to the camera. Measured in the eye-line frame so head ROLL
    does not leak into the number. This is the quality signal the recognition
    gate uses (|yaw| <= 0.22), so the fixture face should be one that would
    actually pass it — otherwise the alignment fixture exercises a case
    production never embeds.
    """
    lm = np.asarray(lm, np.float64).reshape(5, 2)
    le, re, nose = lm[0], lm[1], lm[2]
    eye_mid = (le + re) / 2.0
    ev = re - le
    ed = float(np.linalg.norm(ev)) or 1e-6
    ang = float(np.arctan2(ev[1], ev[0]))
    rot = np.array([[np.cos(-ang), -np.sin(-ang)], [np.sin(-ang), np.cos(-ang)]])
    n = rot @ (nose - eye_mid)
    return float(n[0] / ed)


def align_face(canvas_bgr, landmarks_xy, size=112):
    M = umeyama(np.asarray(landmarks_xy, np.float64).reshape(5, 2), ARCFACE_REF)
    return cv2.warpAffine(canvas_bgr, M, (size, size), borderValue=0.0)


def arcface_embed(model, crop_bgr):
    opt = InferenceOption(); opt.devices = [0]
    opt.bound_option = InferenceOption.BOUND_OPTION.NPU_0; opt.use_ort = False
    ie = InferenceEngine(model, opt)
    inp = np.ascontiguousarray(cv2.cvtColor(crop_bgr, cv2.COLOR_BGR2RGB)[None, ...])
    v = np.asarray(ie.run(inp)[0]).reshape(-1).astype(np.float32)
    del ie
    n = float(np.linalg.norm(v))
    return v / n if n > 1e-8 else v


def pick_face_frame(videos, model, conf=0.5):
    """First frame in the corpus where SCRFD finds a face - the pose fixture
    frame is a corridor at 1920x1080, where a face is ~14 px on the canvas and
    genuinely undetectable, so the two fixtures need different frames."""
    opt = InferenceOption(); opt.devices = [0]
    opt.bound_option = InferenceOption.BOUND_OPTION.NPU_0; opt.use_ort = True
    ie = InferenceEngine(model, opt)
    best = None
    try:
        for clip in videos:
            cap = cv2.VideoCapture(clip)
            for idx in range(0, 400, 25):
                cap.set(cv2.CAP_PROP_POS_FRAMES, idx)
                ok, fr = cap.read()
                if not ok:
                    break
                canvas, *_ = letterbox(fr)
                inp = np.ascontiguousarray(cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)[None, ...])
                dets = scrfd_decode([np.asarray(t) for t in ie.run(inp)], conf=conf)
                big = [d for d in dets if (d[2] - d[0]) >= 40]
                # Rank by face count first, then by the largest face: a fixture
                # with several well-resolved faces exercises the decode (and the
                # NMS) far harder than one distant face does.
                widest = max((d[2] - d[0] for d in big), default=0.0)
                key = (len(big), widest)
                if best is None or key > best[0]:
                    best = (key, clip, idx, fr, dets)
            cap.release()
    finally:
        del ie
    return best


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", default="models")
    ap.add_argument("--out", default="tests/fixtures")
    ap.add_argument("--videos", nargs="*", default=None,
                    help="clips to search for a frame containing people")
    ap.add_argument("--conf", type=float, default=0.5)
    args = ap.parse_args()

    models = pathlib.Path(args.models)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    pose_model = str(models / "yolo26s_pose.dxnn")
    scrfd_model = str(models / "scrfd_10g.dxnn")

    videos = args.videos
    if not videos:
        import glob
        videos = sorted(glob.glob(
            "/home/deepx1/dx-all-suite/aslam_workspace/VisionX/server/"
            "edge-videos/main_stream/*.mp4"))
    if not videos:
        return exit_with("no source videos found; pass --videos")

    print("selecting a frame with people...")
    picked = pick_frame(videos, pose_model)
    if picked is None or picked[0] == 0:
        return exit_with("no frame with a confident person detection was found")
    n, clip, idx, frame = picked
    print(f"  {n} people in {pathlib.Path(clip).name} frame {idx} ({frame.shape[1]}x{frame.shape[0]})")

    canvas, scale, dx, dy = letterbox(frame)
    cv2.imwrite(str(out / "pose_canvas.png"), canvas)
    (out / "pose_canvas.meta").write_text(
        f"{frame.shape[1]} {frame.shape[0]} {scale:.9f} {dx} {dy}\n")

    # ---- pose ------------------------------------------------------------
    print("pose: capturing ORT ground truth and the raw NPU head")
    ort_rows = np.asarray(run(pose_model, canvas, True)[0]).reshape(-1, 57)
    keep = ort_rows[ort_rows[:, 4] >= args.conf]
    keep = keep[np.argsort(-keep[:, 4])]
    np.savetxt(out / "pose_expected.csv", keep, delimiter=",", fmt="%.6f")
    print(f"  {len(keep)} reference detections >= {args.conf}")
    write_tensors(out, "pose_raw", run(pose_model, canvas, False))

    # ---- scrfd -----------------------------------------------------------
    # A different frame: the pose fixture is a wide corridor where a face is
    # ~14 px on the canvas, below anything SCRFD can resolve.
    print("scrfd: selecting a frame with a resolvable face...")
    picked_f = pick_face_frame(videos, scrfd_model)
    if picked_f is None or picked_f[0][0] == 0:
        return exit_with("no frame with a face >= 40 px was found in the corpus")

    (nf, widest), fclip, fidx, fframe, _ = picked_f
    print(f"  {nf} face(s), widest {widest:.0f} px, in "
          f"{pathlib.Path(fclip).name} frame {fidx}")
    fcanvas, fscale, fdx, fdy = letterbox(fframe)
    cv2.imwrite(str(out / "scrfd_canvas.png"), fcanvas)
    (out / "scrfd_canvas.meta").write_text(
        f"{fframe.shape[1]} {fframe.shape[0]} {fscale:.9f} {fdx} {fdy}\n")

    ort_arrs = [np.asarray(a) for a in run(scrfd_model, fcanvas, True)]

    # Settle the anchor cell offset by measurement, not by citation. The pose
    # model's nose keypoints on the SAME frame are an entirely independent
    # signal: whichever offset puts more noses inside a face box is the one the
    # model was actually exported with.
    pose_rows = np.asarray(run(pose_model, fcanvas, True)[0]).reshape(-1, 57)
    pose_rows = pose_rows[pose_rows[:, 4] >= 0.5]
    print(f"  cross-check against {len(pose_rows)} pose detection(s):")

    scored = {}
    for offset in (0.0, 0.5):
        dets = scrfd_decode(ort_arrs, cell_offset=offset)
        hits = 0
        dist = []
        for pr in pose_rows:
            nose = pr[6:8]
            for fb in dets:
                if fb[0] <= nose[0] <= fb[2] and fb[1] <= nose[1] <= fb[3]:
                    hits += 1
                    cx, cy = (fb[0] + fb[2]) / 2, (fb[1] + fb[3]) / 2
                    dist.append(float(np.hypot(cx - nose[0], cy - nose[1])))
                    break
        mean_d = float(np.mean(dist)) if dist else float("inf")
        scored[offset] = (hits, mean_d, dets)
        print(f"    cell_offset={offset}: {len(dets)} face(s), "
              f"{hits}/{len(pose_rows)} noses inside, "
              f"mean nose-to-face-centre {mean_d:.2f} px")

    best_offset = max(scored, key=lambda o: (scored[o][0], -scored[o][1]))
    hits, mean_d, ref = scored[best_offset]
    print(f"  -> cell_offset = {best_offset} (chosen by measurement)")
    (out / "scrfd_cell_offset.txt").write_text(f"{best_offset}\n")

    np.savetxt(out / "scrfd_expected.csv", np.asarray(ref), delimiter=",", fmt="%.6f")
    np.savetxt(out / "scrfd_pose_expected.csv", pose_rows, delimiter=",", fmt="%.6f")
    print(f"  {len(ref)} reference faces written")
    write_tensors(out, "scrfd_raw", run(scrfd_model, fcanvas, False))

    # ---- arcface ---------------------------------------------------------
    # Align the widest reference face and embed it. The C++ side must reproduce
    # both halves: the same 112x112 crop from the same landmarks, and an
    # embedding that agrees with this one.
    arc_model = str(models / "ArcFace_MobileFaceNet.dxnn")
    # Prefer the most FRONTAL face of a usable size over the merely widest one:
    # the recognition gate only ever embeds faces at |yaw| <= 0.22, so a fixture
    # built from a turned face would exercise a path production never takes.
    usable = [d for d in ref if (d[2] - d[0]) >= 32]
    if not usable:
        usable = ref
    ranked = sorted(usable, key=lambda d: abs(face_yaw(d[5:])))
    chosen = ranked[0]
    chosen_yaw = face_yaw(chosen[5:])
    print(f"  arcface: chose the most frontal usable face — "
          f"yaw {chosen_yaw:+.3f} eye-widths, {chosen[2] - chosen[0]:.0f} px wide "
          f"(of {len(usable)} candidates)")
    widest_face = chosen
    lm = np.asarray(widest_face[5:]).reshape(5, 2)
    np.savetxt(out / "arcface_landmarks.csv", lm.reshape(1, -1), delimiter=",", fmt="%.6f")

    crop = align_face(fcanvas, lm)
    cv2.imwrite(str(out / "arcface_aligned.png"), crop)

    M = umeyama(lm, ARCFACE_REF)
    proj = (M[:, :2] @ lm.T).T + M[:, 2]
    residuals = np.linalg.norm(proj - ARCFACE_REF, axis=1)
    np.savetxt(out / "arcface_residuals.csv", residuals.reshape(1, -1),
               delimiter=",", fmt="%.6f")
    print(f"  arcface: alignment residuals max {residuals.max():.3f} px, "
          f"mean {residuals.mean():.3f} px")

    emb = arcface_embed(arc_model, crop)
    (out / "arcface_expected.f32").write_bytes(emb.astype(np.float32).tobytes())
    print(f"  arcface: aligned a {widest_face[2] - widest_face[0]:.0f} px face, "
          f"embedding dim {emb.size}, |v| = {np.linalg.norm(emb):.6f}")

    # A second embedding of the SAME crop, to record how much of any C++/Python
    # difference is just device nondeterminism rather than a real mismatch.
    emb2 = arcface_embed(arc_model, crop)
    print(f"  arcface: self-consistency cosine = {float(emb @ emb2):.6f}")

    print("done ->", out)
    return 0


def exit_with(msg: str) -> int:
    print("ERROR:", msg, file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
