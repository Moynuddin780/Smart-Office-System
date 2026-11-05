# Models

The compiled NPU models are **not stored in this repository** (large binaries, separate licences). `./setup.sh`
checks for them and can copy them from a folder: `MODEL_SOURCE_DIR=/path/to/models ./setup.sh`.

| File | Input | Used for |
|---|---|---|
| `yolo26s_pose.dxnn` | `[1,640,640,3]` uint8 | person boxes and 17 keypoints (one class) |
| `scrfd_10g.dxnn` | `[1,640,640,3]` uint8 | face boxes + 5 landmarks |
| `ArcFace_MobileFaceNet.dxnn` | `[1,112,112,3]` uint8 | 512-D face embedding |

Compile them from the ONNX models with the DEEPX compiler (DX-COM). The decoders in `src/vision` are verified against
captured ground truth — see `tools/dump_fixtures.py` and the README.
