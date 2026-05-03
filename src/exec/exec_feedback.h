#pragma once

#include <cstdint>
#include "common/types.h"

namespace lt {

enum class ExecFeedbackKind : uint8_t {
    REQUEST_SENT = 0,
    ORDER_ACCEPTED,
    ORDER_REJECTED,
    CANCEL_CONFIRMED,
    RATE_LIMITED,
    EXCHANGE_UNAVAILABLE,
    TIMEOUT,
    HEARTBEAT_OK,
    HEARTBEAT_FAILED,
    GATEWAY_DEGRADED,
    GATEWAY_RECOVERED,
};

// Feedback event published T3 -> T2 via exec_to_strategy queue
struct ExecFeedback {
    ExecFeedbackKind kind = ExecFeedbackKind::REQUEST_SENT;
    uint32_t intent_id = 0;         // references the original ExecIntent
    OrderId client_order_id;        // stable correlation ID
    OrderId exchange_order_id;      // assigned by exchange on placement
    int http_status = 0;
    Timestamp_ns latency_ns = 0;    // REST RTT
    Timestamp_ns created_ts = 0;
    char error_msg[128]{};          // fixed-size error message (truncated)

    void set_error(const char* msg) {
        size_t len = 0;
        while (msg[len] && len < sizeof(error_msg) - 1) {
            error_msg[len] = msg[len];
            ++len;
        }
        error_msg[len] = '\0';
    }
};

}  // namespace lt
