#include "core/config.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace so {
namespace {

std::string env_str(const char* key, const std::string& dflt) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : dflt;
}

int env_int(const char* key, int dflt) {
    const char* v = std::getenv(key);
    if (!v || !*v) return dflt;
    try {
        return std::stoi(v);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string(key) + " is not an integer: " + v);
    }
}

bool env_bool(const char* key, bool dflt) {
    const char* v = std::getenv(key);
    if (!v || !*v) return dflt;
    const std::string s(v);
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

}  // namespace

Config Config::from_env() {
    Config c;
    c.http_port           = static_cast<std::uint16_t>(env_int("HTTP_PORT", c.http_port));
    c.db_url              = env_str("DATABASE_URL", c.db_url);
    c.jwt_secret          = env_str("JWT_SECRET", c.jwt_secret);
    c.models_dir          = env_str("MODELS_DIR", c.models_dir);
    c.rec_model           = env_str("REC_MODEL", c.rec_model);
    c.snapshot_dir        = env_str("SNAPSHOT_DIR", c.snapshot_dir);
    c.frontend_dir        = env_str("FRONTEND_DIR", c.frontend_dir);
    c.npu_cores           = env_int("NPU_CORES", c.npu_cores);
    c.use_ort             = env_bool("USE_ORT", c.use_ort);
    c.buffer_count        = env_int("NPU_BUFFER_COUNT", c.buffer_count);
    c.postprocess_workers = env_int("POSTPROCESS_WORKERS", c.postprocess_workers);
    c.frame_ring          = env_int("FRAME_RING", c.frame_ring);
    c.detect_hz           = env_int("DETECT_HZ", c.detect_hz);
    c.face_hz             = env_int("FACE_HZ", c.face_hz);
    c.jpeg_quality        = env_int("JPEG_QUALITY", c.jpeg_quality);
    return c;
}

void Config::validate() const {
    if (npu_cores < 1 || npu_cores > 3)
        throw std::runtime_error("NPU_CORES must be 1..3 (the DX-M1 has three cores); got " +
                                 std::to_string(npu_cores));
    if (postprocess_workers < 1)
        throw std::runtime_error("POSTPROCESS_WORKERS must be >= 1");
    if (frame_ring < 1)
        throw std::runtime_error("FRAME_RING must be >= 1");
    if (detect_hz < 1 || detect_hz > 60)
        throw std::runtime_error("DETECT_HZ must be 1..60");
    if (face_hz < 1 || face_hz > 60)
        throw std::runtime_error("FACE_HZ must be 1..60");
    if (jpeg_quality < 1 || jpeg_quality > 100)
        throw std::runtime_error("JPEG_QUALITY must be 1..100");
    // Deliberately NOT defaulted: a deployment that silently runs on a
    // hardcoded signing key is one where every tenant's tokens are forgeable.
    if (jwt_secret.size() < 16)
        throw std::runtime_error("JWT_SECRET must be set and at least 16 characters");
}

}  // namespace so
