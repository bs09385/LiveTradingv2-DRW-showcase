#pragma once

#include <cstdint>

namespace lt {

// Execution mode controlling what intents pass through to the real exchange.
// Default is DRY_RUN (safe by construction — no real orders).
enum class ExecutionMode : uint8_t {
    DRY_RUN = 0,       // Log "would send", never forward
    LIVE = 1,           // Forward everything unconditionally
};

inline const char* execution_mode_name(ExecutionMode mode) {
    switch (mode) {
        case ExecutionMode::DRY_RUN:      return "DRY_RUN";
        case ExecutionMode::LIVE:         return "LIVE";
    }
    return "UNKNOWN";
}

inline constexpr int kExecutionModeCount = 2;

}  // namespace lt
