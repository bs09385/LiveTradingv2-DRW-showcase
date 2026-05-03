#include "rest/rest_response_parser.h"

#include <simdjson.h>
#include <string>

namespace lt {

Expected<OrderResponse> parse_order_response(std::string_view body) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(body.data(), body.size()).get(doc);
    if (err) {
        return ErrorCode::JSON_PARSE_ERROR;
    }

    OrderResponse resp;

    bool success_val;
    if (doc["success"].get_bool().get(success_val) == simdjson::SUCCESS) {
        resp.success = success_val;
    }

    std::string_view sv;
    if (doc["errorMsg"].get_string().get(sv) == simdjson::SUCCESS) {
        resp.error_msg = std::string(sv);
    }

    if (doc["orderID"].get_string().get(sv) == simdjson::SUCCESS) {
        resp.order_id = std::string(sv);
    }

    if (doc["status"].get_string().get(sv) == simdjson::SUCCESS) {
        resp.status = std::string(sv);
    }

    return Expected<OrderResponse>(std::move(resp));
}

Expected<CancelResponse> parse_cancel_response(std::string_view body) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(body.data(), body.size()).get(doc);
    if (err) {
        return ErrorCode::JSON_PARSE_ERROR;
    }

    CancelResponse resp;
    bool has_canceled_field = false;
    bool has_not_canceled_field = false;

    simdjson::dom::element canceled_elem;
    auto canceled_err = doc["canceled"].get(canceled_elem);
    if (canceled_err == simdjson::SUCCESS) {
        has_canceled_field = true;
        simdjson::dom::array arr;
        if (canceled_elem.get_array().get(arr) != simdjson::SUCCESS) {
            return ErrorCode::JSON_TYPE_ERROR;
        }
        for (auto elem : arr) {
            std::string_view sv;
            if (elem.get_string().get(sv) != simdjson::SUCCESS) {
                return ErrorCode::JSON_TYPE_ERROR;
            }
            resp.canceled.emplace_back(sv);
        }
    } else if (canceled_err != simdjson::NO_SUCH_FIELD) {
        return ErrorCode::JSON_TYPE_ERROR;
    }

    // not_canceled is an object: { orderID: "reason", ... }
    simdjson::dom::element not_canceled_elem;
    auto not_canceled_err = doc["not_canceled"].get(not_canceled_elem);
    if (not_canceled_err == simdjson::SUCCESS) {
        has_not_canceled_field = true;
        simdjson::dom::object obj;
        if (not_canceled_elem.get_object().get(obj) != simdjson::SUCCESS) {
            return ErrorCode::JSON_TYPE_ERROR;
        }
        for (auto field : obj) {
            std::string_view reason_sv;
            if (field.value.get_string().get(reason_sv) != simdjson::SUCCESS) {
                return ErrorCode::JSON_TYPE_ERROR;
            }
            resp.not_canceled.emplace_back(
                std::string(field.key), std::string(reason_sv));
        }
    } else if (not_canceled_err != simdjson::NO_SUCH_FIELD) {
        return ErrorCode::JSON_TYPE_ERROR;
    }

    // Ambiguous/unknown 2xx bodies must not be treated as cancel success.
    if (!has_canceled_field && !has_not_canceled_field) {
        return ErrorCode::JSON_MISSING_FIELD;
    }

    return Expected<CancelResponse>(std::move(resp));
}

Expected<HeartbeatResponse> parse_heartbeat_response(std::string_view body) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(body.data(), body.size()).get(doc);
    if (err) {
        return ErrorCode::JSON_PARSE_ERROR;
    }

    HeartbeatResponse resp;
    std::string_view sv;
    if (doc["heartbeat_id"].get_string().get(sv) == simdjson::SUCCESS) {
        resp.heartbeat_id = std::string(sv);
    }

    return Expected<HeartbeatResponse>(std::move(resp));
}

Expected<BatchOrderResponse> parse_batch_order_response(std::string_view body) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(body.data(), body.size()).get(doc);
    if (err) {
        return ErrorCode::JSON_PARSE_ERROR;
    }

    // Response should be a JSON array. Fallback: try {"orders": [...]} wrapper.
    // If neither, check for top-level error object {"error": "..."}.
    simdjson::dom::array arr;
    if (doc.get_array().get(arr) != simdjson::SUCCESS) {
        // Try wrapped format: {"orders": [...]}
        simdjson::dom::element orders_elem;
        if (doc["orders"].get(orders_elem) == simdjson::SUCCESS &&
            orders_elem.get_array().get(arr) == simdjson::SUCCESS) {
            // Successfully unwrapped
        } else {
            // Check for top-level error: {"error": "..."}
            std::string_view error_sv;
            if (doc["error"].get_string().get(error_sv) == simdjson::SUCCESS) {
                BatchOrderResponse resp;
                BatchOrderResponse::Item item;
                item.success = false;
                item.error_msg = std::string(error_sv);
                resp.items.push_back(std::move(item));
                return Expected<BatchOrderResponse>(std::move(resp));
            }
            return ErrorCode::JSON_TYPE_ERROR;
        }
    }

    BatchOrderResponse resp;
    for (auto elem : arr) {
        BatchOrderResponse::Item item;

        bool success_val;
        if (elem["success"].get_bool().get(success_val) == simdjson::SUCCESS) {
            item.success = success_val;
        }

        std::string_view sv;
        if (elem["errorMsg"].get_string().get(sv) == simdjson::SUCCESS) {
            item.error_msg = std::string(sv);
        }

        // Try "orderID" first, then "id" as fallback
        if (elem["orderID"].get_string().get(sv) == simdjson::SUCCESS && sv.size() > 0) {
            item.order_id = std::string(sv);
        } else if (elem["id"].get_string().get(sv) == simdjson::SUCCESS && sv.size() > 0) {
            item.order_id = std::string(sv);
        }

        if (elem["status"].get_string().get(sv) == simdjson::SUCCESS) {
            item.status = std::string(sv);
        }

        // Diagnostic: capture raw JSON when success=true but no orderID
        if (item.success && item.order_id.empty() && item.error_msg.empty()) {
            item.error_msg = simdjson::minify(elem);
        }

        resp.items.push_back(std::move(item));
    }

    return Expected<BatchOrderResponse>(std::move(resp));
}

}  // namespace lt
