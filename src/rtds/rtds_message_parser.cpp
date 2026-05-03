#include "rtds/rtds_message_parser.h"

#include "simdjson.h"

namespace lt {

struct RtdsMessageParser::Impl {
    simdjson::dom::parser parser;
};

RtdsMessageParser::RtdsMessageParser() : impl_(std::make_unique<Impl>()) {}
RtdsMessageParser::~RtdsMessageParser() = default;

ErrorCode RtdsMessageParser::parse(std::string_view payload, Timestamp_ns recv_ts,
                                    CryptoPriceUpdate& out) {
    // Check for PONG
    if (payload == "PONG" || payload == "pong") {
        return ErrorCode::PONG_MESSAGE;
    }

    simdjson::dom::element doc;
    auto err = impl_->parser.parse(payload.data(), payload.size()).get(doc);
    if (err) {
        return ErrorCode::JSON_PARSE_ERROR;
    }

    // Check topic
    std::string_view topic;
    if (doc["topic"].get_string().get(topic) != simdjson::SUCCESS) {
        return ErrorCode::UNKNOWN_EVENT_TYPE;
    }
    if (topic != "crypto_prices") {
        return ErrorCode::UNKNOWN_EVENT_TYPE;
    }

    // Check type
    std::string_view type;
    if (doc["type"].get_string().get(type) != simdjson::SUCCESS) {
        return ErrorCode::UNKNOWN_EVENT_TYPE;
    }
    if (type != "update") {
        return ErrorCode::UNKNOWN_EVENT_TYPE;
    }

    // Parse payload object
    simdjson::dom::element payload_obj;
    if (doc["payload"].get(payload_obj) != simdjson::SUCCESS) {
        return ErrorCode::JSON_PARSE_ERROR;
    }

    // Extract symbol
    std::string_view symbol;
    if (payload_obj["symbol"].get_string().get(symbol) != simdjson::SUCCESS) {
        return ErrorCode::JSON_PARSE_ERROR;
    }

    // Extract value (can be integer or double)
    double value = 0.0;
    if (payload_obj["value"].get_double().get(value) != simdjson::SUCCESS) {
        int64_t int_val;
        if (payload_obj["value"].get_int64().get(int_val) != simdjson::SUCCESS) {
            return ErrorCode::JSON_PARSE_ERROR;
        }
        value = static_cast<double>(int_val);
    }

    // Extract payload timestamp
    int64_t exchange_ts = 0;
    if (payload_obj["timestamp"].get_int64().get(exchange_ts) != simdjson::SUCCESS) {
        // Try outer timestamp as fallback
        auto outer_err = doc["timestamp"].get_int64().get(exchange_ts);
        (void)outer_err;
    }

    out.symbol = CryptoSymbol(symbol);
    out.value = value;
    out.exchange_ts_ms = exchange_ts;
    out.recv_ts = recv_ts;

    return ErrorCode::OK;
}

}  // namespace lt
