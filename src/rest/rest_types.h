#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "common/types.h"

namespace lt {

enum class HttpMethod : uint8_t {
    GET = 0,
    POST,
    DELETE_METHOD,
};

inline const char* http_method_str(HttpMethod m) {
    switch (m) {
        case HttpMethod::GET: return "GET";
        case HttpMethod::POST: return "POST";
        case HttpMethod::DELETE_METHOD: return "DELETE";
    }
    return "GET";
}

struct RestRequest {
    HttpMethod method = HttpMethod::GET;
    std::string path;
    std::string body;
    Timestamp_ns created_ts = 0;
};

struct RestResponse {
    int http_status = 0;
    std::string body;
    Timestamp_ns latency_ns = 0;
    bool timed_out = false;
};

using RestCallback = std::function<void(RestResponse)>;

// L2 authentication headers for Polymarket CLOB API
struct L2Headers {
    std::string api_key;
    std::string signature;
    std::string timestamp;
    std::string passphrase;
    std::string address;
};

// Parsed responses from REST API
struct OrderResponse {
    bool success = false;
    std::string error_msg;
    std::string order_id;
    std::string status;
};

struct CancelResponse {
    std::vector<std::string> canceled;
    // not_canceled is a map: orderID -> failure reason
    std::vector<std::pair<std::string, std::string>> not_canceled;
};

struct HeartbeatResponse {
    std::string heartbeat_id;
};

struct BatchOrderResponse {
    struct Item {
        bool success = false;
        std::string error_msg;
        std::string order_id;
        std::string status;
    };
    std::vector<Item> items;
};

}  // namespace lt
