#include "modules/module.hpp"

namespace so {

const char* to_string(Severity s) {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Critical: return "critical";
    }
    return "info";
}

}  // namespace so
