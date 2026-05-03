#include "binance/binance_message_parser.h"

#include <charconv>
#include <cstring>

#include "simdjson.h"

namespace lt {

namespace {

// Ignore simdjson's [[nodiscard]] error_code when the caller has already
// initialized the output to a sane default and treats missing fields as
// optional.
inline void ignore_err(simdjson::error_code e) { (void)e; }

// Parse a decimal number from a string_view. Returns true on success.
// Binance sends all prices/quantities as stringified decimals
// (e.g. "25.35190000"). Strings from simdjson are not guaranteed to be
// null-terminated, so std::from_chars (pointer-based) is used.
inline bool parse_decimal(std::string_view sv, double& out) {
    if (sv.empty()) return false;
    const char* first = sv.data();
    const char* last = first + sv.size();
    auto res = std::from_chars(first, last, out);
    return res.ec == std::errc() && res.ptr == last;
}

// In-place lowercase of a small char buffer.
inline void lowercase_inplace(char* data, uint8_t len) {
    for (uint8_t i = 0; i < len; ++i) {
        char c = data[i];
        if (c >= 'A' && c <= 'Z') data[i] = static_cast<char>(c + 32);
    }
}

// Infer stream type from the stream-name suffix (e.g. "btcusdt@bookTicker").
BinanceStreamType stream_type_from_name(std::string_view name) {
    auto at = name.rfind('@');
    if (at == std::string_view::npos) return BinanceStreamType::UNKNOWN;
    std::string_view suffix = name.substr(at + 1);
    if (suffix == "bookTicker") return BinanceStreamType::BOOK_TICKER;
    if (suffix == "trade")      return BinanceStreamType::TRADE;
    if (suffix == "aggTrade")   return BinanceStreamType::AGG_TRADE;
    return BinanceStreamType::UNKNOWN;
}

// Infer stream type from the event-type field ("e") inside a data object.
// Note: bookTicker payloads do NOT contain "e".
BinanceStreamType stream_type_from_event(std::string_view ev) {
    if (ev == "trade")    return BinanceStreamType::TRADE;
    if (ev == "aggTrade") return BinanceStreamType::AGG_TRADE;
    return BinanceStreamType::UNKNOWN;
}

}  // namespace

struct BinanceMessageParser::Impl {
    simdjson::dom::parser parser;
};

BinanceMessageParser::BinanceMessageParser() : impl_(std::make_unique<Impl>()) {}
BinanceMessageParser::~BinanceMessageParser() = default;

ErrorCode BinanceMessageParser::parse(std::string_view payload,
                                      Timestamp_ns recv_ts,
                                      BinanceMarketUpdate& out) {
    simdjson::dom::element doc;
    auto err = impl_->parser.parse(payload.data(), payload.size()).get(doc);
    if (err) return ErrorCode::JSON_PARSE_ERROR;

    // Ignore subscribe-acks and similar control frames:
    //   {"result": null, "id": 1}
    simdjson::dom::element maybe_id;
    if (doc["id"].get(maybe_id) == simdjson::SUCCESS &&
        doc["stream"].get(maybe_id) != simdjson::SUCCESS &&
        doc["e"].get(maybe_id) != simdjson::SUCCESS) {
        return ErrorCode::UNKNOWN_EVENT_TYPE;
    }

    // Unwrap combined-stream envelope if present:
    //   {"stream":"btcusdt@bookTicker","data":{...}}
    BinanceStreamType type = BinanceStreamType::UNKNOWN;
    simdjson::dom::element data_obj = doc;
    bool envelope_present = false;

    std::string_view stream_name;
    if (doc["stream"].get_string().get(stream_name) == simdjson::SUCCESS) {
        envelope_present = true;
        type = stream_type_from_name(stream_name);
        if (doc["data"].get(data_obj) != simdjson::SUCCESS) {
            return ErrorCode::JSON_PARSE_ERROR;
        }
        // Envelope with an unrecognized stream suffix (e.g. kline_1m) is a
        // valid but unsupported frame -- don't try to guess the shape.
        if (type == BinanceStreamType::UNKNOWN) {
            return ErrorCode::UNKNOWN_EVENT_TYPE;
        }
    }

    // No envelope: sniff stream type from the event-type field.
    if (!envelope_present) {
        std::string_view ev;
        if (data_obj["e"].get_string().get(ev) == simdjson::SUCCESS) {
            type = stream_type_from_event(ev);
            if (type == BinanceStreamType::UNKNOWN) {
                return ErrorCode::UNKNOWN_EVENT_TYPE;
            }
        } else {
            // No "e" and no envelope -> assume bookTicker (which omits "e").
            type = BinanceStreamType::BOOK_TICKER;
        }
    }

    // Common: symbol (always present as "s", uppercase)
    std::string_view sym;
    if (data_obj["s"].get_string().get(sym) != simdjson::SUCCESS) {
        return ErrorCode::JSON_PARSE_ERROR;
    }

    BinanceMarketUpdate upd{};
    upd.type = static_cast<uint8_t>(type);
    upd.symbol = CryptoSymbol(sym);
    lowercase_inplace(upd.symbol.data, upd.symbol.len);
    upd.recv_ts = recv_ts;

    switch (type) {
    case BinanceStreamType::BOOK_TICKER: {
        // {"u": 400900217, "s": "BTCUSDT",
        //  "b": "25.3519", "B": "31.21", "a": "25.3652", "A": "40.66"}
        std::string_view b, B, a, A;
        if (data_obj["b"].get_string().get(b) != simdjson::SUCCESS ||
            data_obj["B"].get_string().get(B) != simdjson::SUCCESS ||
            data_obj["a"].get_string().get(a) != simdjson::SUCCESS ||
            data_obj["A"].get_string().get(A) != simdjson::SUCCESS) {
            return ErrorCode::JSON_PARSE_ERROR;
        }
        if (!parse_decimal(b, upd.bid_price) ||
            !parse_decimal(B, upd.bid_qty) ||
            !parse_decimal(a, upd.ask_price) ||
            !parse_decimal(A, upd.ask_qty)) {
            return ErrorCode::JSON_PARSE_ERROR;
        }
        int64_t u_id = 0;
        // "u" is optional on some bookTicker variants; tolerate missing.
        ignore_err(data_obj["u"].get_int64().get(u_id));
        upd.update_id = u_id;
        // bookTicker has no exchange timestamp — leave as 0.
        break;
    }
    case BinanceStreamType::TRADE: {
        // {"e":"trade","E":...,"s":"BTCUSDT","t":12345,
        //  "p":"0.001","q":"100","T":...,"m":true}
        std::string_view p, q;
        if (data_obj["p"].get_string().get(p) != simdjson::SUCCESS ||
            data_obj["q"].get_string().get(q) != simdjson::SUCCESS) {
            return ErrorCode::JSON_PARSE_ERROR;
        }
        if (!parse_decimal(p, upd.last_price) ||
            !parse_decimal(q, upd.last_qty)) {
            return ErrorCode::JSON_PARSE_ERROR;
        }
        int64_t t_id = 0;
        ignore_err(data_obj["t"].get_int64().get(t_id));
        upd.update_id = t_id;
        int64_t ts = 0;
        if (data_obj["T"].get_int64().get(ts) != simdjson::SUCCESS) {
            ignore_err(data_obj["E"].get_int64().get(ts));
        }
        upd.exchange_ts_ms = ts;
        bool maker = false;
        ignore_err(data_obj["m"].get_bool().get(maker));
        upd.buyer_is_maker = maker ? 1 : 0;
        break;
    }
    case BinanceStreamType::AGG_TRADE: {
        // {"e":"aggTrade","E":...,"s":"BTCUSDT","a":12345,
        //  "p":"0.001","q":"100","f":...,"l":...,"T":...,"m":true}
        std::string_view p, q;
        if (data_obj["p"].get_string().get(p) != simdjson::SUCCESS ||
            data_obj["q"].get_string().get(q) != simdjson::SUCCESS) {
            return ErrorCode::JSON_PARSE_ERROR;
        }
        if (!parse_decimal(p, upd.last_price) ||
            !parse_decimal(q, upd.last_qty)) {
            return ErrorCode::JSON_PARSE_ERROR;
        }
        int64_t a_id = 0;
        ignore_err(data_obj["a"].get_int64().get(a_id));
        upd.update_id = a_id;
        int64_t ts = 0;
        if (data_obj["T"].get_int64().get(ts) != simdjson::SUCCESS) {
            ignore_err(data_obj["E"].get_int64().get(ts));
        }
        upd.exchange_ts_ms = ts;
        bool maker = false;
        ignore_err(data_obj["m"].get_bool().get(maker));
        upd.buyer_is_maker = maker ? 1 : 0;
        break;
    }
    default:
        return ErrorCode::UNKNOWN_EVENT_TYPE;
    }

    out = upd;
    return ErrorCode::OK;
}

}  // namespace lt
