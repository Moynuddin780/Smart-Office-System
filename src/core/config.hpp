#pragma once
#include <cstdint>
#include <string>

namespace so {

// Every tunable in one place, loaded once from the environment at startup.
// Defaults are the values the design spec settled on, so an empty environment
// produces a working single-tenant deployment.
struct Config {
    // HTTP / storage
    std::uint16_t http_port      = 8080;
    std::string   db_url         = "postgresql://smartoffice:smartoffice@postgres:5432/smartoffice";
    std::string   jwt_secret     = "";            // empty => refuse to start
    std::string   models_dir     = "models";
    /// The recognition model, by filename inside `models_dir`.
    ///
    /// Named rather than hardcoded so a candidate recogniser can be put in
    /// front of the live system and taken away again with one environment
    /// variable, instead of a rebuild. Switching it INVALIDATES every stored
    /// embedding — see `--reembed`.
    std::string   rec_model      = "ArcFace_MobileFaceNet.dxnn";
    std::string   snapshot_dir   = "/data/snapshots";
    std::string   frontend_dir   = "frontend/dist";

    // NPU
    int npu_cores                = 3;             // 1..3; 3 = the whole DX-M1
    bool use_ort                 = false;         // native NPU; keeps the tail off the CPU
    int buffer_count             = 4;

    // Pipeline
    int postprocess_workers      = 3;
    int frame_ring               = 2;             // drop-oldest; never queue
    int detect_hz                = 10;
    int face_hz                  = 6;
    int jpeg_quality             = 75;

    static Config from_env();
    // Throws std::runtime_error describing the first problem found. Called
    // before anything is started so a misconfiguration fails at boot rather
    // than on the first frame.
    void validate() const;
};

}  // namespace so
