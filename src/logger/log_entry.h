#pragma once

#include <cstdint>
#include <cstring>

#include "common/types.h"

namespace lt {

enum class LogLevel : uint8_t { TRACE = 0, DEBUG, INFO, WARN, ERROR, FATAL };

inline const char* log_level_str(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARN: return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        default: return "???";
    }
}

inline LogLevel parse_log_level(std::string_view sv) {
    if (sv == "TRACE") return LogLevel::TRACE;
    if (sv == "DEBUG") return LogLevel::DEBUG;
    if (sv == "INFO") return LogLevel::INFO;
    if (sv == "WARN") return LogLevel::WARN;
    if (sv == "ERROR") return LogLevel::ERROR;
    if (sv == "FATAL") return LogLevel::FATAL;
    return LogLevel::INFO;
}

// Fixed-size log entry: no heap allocation
struct LogEntry {
    static constexpr int kMaxMsg = 240;

    Timestamp_ns timestamp = 0;
    LogLevel level = LogLevel::INFO;
    uint8_t producer_id = 0;
    char message[kMaxMsg]{};

    void set_message(std::string_view sv) {
        auto len = std::min(sv.size(), static_cast<std::size_t>(kMaxMsg - 1));
        std::memcpy(message, sv.data(), len);
        message[len] = '\0';
    }
};

}  // namespace lt
