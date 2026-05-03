#pragma once

#include <string_view>
#include "common/error.h"
#include "rest/rest_types.h"

namespace lt {

// Parse POST /order response JSON
Expected<OrderResponse> parse_order_response(std::string_view body);

// Parse DELETE /order response JSON
Expected<CancelResponse> parse_cancel_response(std::string_view body);

// Parse POST /heartbeat response JSON
Expected<HeartbeatResponse> parse_heartbeat_response(std::string_view body);

// Parse POST /orders batch response JSON (array of order results)
Expected<BatchOrderResponse> parse_batch_order_response(std::string_view body);

}  // namespace lt
