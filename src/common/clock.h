#pragma once

#include <chrono>

#include "common/types.h"

namespace lt {

struct SteadyClock {
    static Timestamp_ns now() {
        auto tp = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    }
};

struct SystemClock {
    static Timestamp_ns now() {
        auto tp = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    }
};

inline int64_t ns_to_us(Timestamp_ns ns) { return ns / 1000; }
inline int64_t ns_to_ms(Timestamp_ns ns) { return ns / 1'000'000; }
inline double ns_to_sec(Timestamp_ns ns) { return static_cast<double>(ns) / 1'000'000'000.0; }

}  // namespace lt
