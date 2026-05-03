#include "parser/user_message_parser.h"

#include <charconv>

#include "common/price.h"

namespace lt {

namespace {

template <typename JsonObj>
ErrorCode get_required_string(JsonObj& obj, const char* key, std::string_view& out) {
    auto err = obj[key].get_string().get(out);
    if (err == simdjson::SUCCESS) return ErrorCode::OK;
    if (err == simdjson::NO_SUCH_FIELD) return ErrorCode::JSON_MISSING_FIELD;
    return ErrorCode::JSON_TYPE_ERROR;
}

template <typename JsonObj>
void parse_optional_timestamp(JsonObj& obj, const char* key, Timestamp_ns& out) {
    // Polymarket timestamps can be string or integer
    std::string_view ts_str;
    if (obj[key].get_string().get(ts_str) == simdjson::SUCCESS) {
        int64_t ts_val = 0;
        auto r = std::from_chars(ts_str.data(), ts_str.data() + ts_str.size(), ts_val);
        if (r.ec == std::errc{}) out = ts_val;
        return;
    }
    int64_t ts_int;
    if (obj[key].get_int64().get(ts_int) == simdjson::SUCCESS) {
        out = ts_int;
    }
}


ErrorCode parse_trade_status(std::string_view sv, TradeStatus& out) {
    if (sv == "MATCHED") { out = TradeStatus::MATCHED; return ErrorCode::OK; }
    if (sv == "MINED") { out = TradeStatus::MINED; return ErrorCode::OK; }
    if (sv == "CONFIRMED") { out = TradeStatus::CONFIRMED; return ErrorCode::OK; }
    if (sv == "RETRYING") { out = TradeStatus::RETRYING; return ErrorCode::OK; }
    if (sv == "FAILED") { out = TradeStatus::FAILED; return ErrorCode::OK; }
    return ErrorCode::USER_UNKNOWN_STATUS;
}

}  // namespace

UserMessageParser::UserMessageParser() = default;

ErrorCode UserMessageParser::parse(std::string_view payload, Timestamp_ns recv_ts, SeqNum_t seq,
                                   UserMessageEvent& out) {
    if (payload.empty()) return ErrorCode::EMPTY_INPUT;

    // Handle PONG (not valid JSON)
    if (payload == "PONG") return ErrorCode::PONG_MESSAGE;

    auto required_capacity = payload.size() + simdjson::SIMDJSON_PADDING;
    if (scratch_.capacity() < required_capacity) {
        scratch_.reserve(required_capacity);
    }
    scratch_.assign(payload.data(), payload.size());

    simdjson::ondemand::document doc;
    auto err = parser_.iterate(scratch_).get(doc);
    if (err) return ErrorCode::JSON_PARSE_ERROR;

    out.recv_ts = recv_ts;
    out.seq = seq;

    // Read event_type
    std::string_view event_type;
    auto et_err = get_required_string(doc, "event_type", event_type);
    if (et_err != ErrorCode::OK) {
        // Check for server error messages: {"error":"..."}
        std::string_view error_msg;
        if (get_required_string(doc, "error", error_msg) == ErrorCode::OK) {
            auto copy_len = std::min(error_msg.size(), size_t(239));
            std::memcpy(out.server_error_msg, error_msg.data(), copy_len);
            out.server_error_msg[copy_len] = '\0';
            return ErrorCode::USER_WS_SERVER_ERROR;
        }
        return et_err;
    }

    if (event_type == "order") {
        return parse_order(doc, out);
    } else if (event_type == "trade") {
        return parse_trade(doc, out);
    }

    // Capture unknown event type string for observability
    auto copy_len = std::min(event_type.size(), size_t(239));
    std::memcpy(out.server_error_msg, event_type.data(), copy_len);
    out.server_error_msg[copy_len] = '\0';
    return ErrorCode::UNKNOWN_EVENT_TYPE;
}

ErrorCode UserMessageParser::parse_order(simdjson::ondemand::document& doc,
                                         UserMessageEvent& out) {
    UserOrderUpdate upd;

    // order id
    std::string_view id_sv;
    auto id_err = get_required_string(doc, "id", id_sv);
    if (id_err != ErrorCode::OK) return id_err;
    upd.order_id = OrderId(id_sv);
    if (id_sv.size() >= kMaxOrderIdLen) ++out.truncated_fields;

    // asset_id
    std::string_view aid_sv;
    auto aid_err = get_required_string(doc, "asset_id", aid_sv);
    if (aid_err != ErrorCode::OK) return aid_err;
    upd.asset_id = AssetId(aid_sv);
    if (aid_sv.size() >= kMaxAssetIdLen) ++out.truncated_fields;

    // client_order_id - optional (for M4 REST correlation)
    std::string_view coid_sv;
    if (get_required_string(doc, "client_order_id", coid_sv) == ErrorCode::OK) {
        upd.client_order_id = OrderId(coid_sv);
    }

    // market (condition ID) - optional
    std::string_view mkt_sv;
    if (get_required_string(doc, "market", mkt_sv) == ErrorCode::OK) {
        upd.market_id = AssetId(mkt_sv);
    }

    // type -> event_type
    std::string_view type_sv;
    auto type_err = get_required_string(doc, "type", type_sv);
    if (type_err != ErrorCode::OK) return type_err;
    if (type_sv == "PLACEMENT") {
        upd.event_type = OrderEventType::PLACEMENT;
    } else if (type_sv == "UPDATE") {
        upd.event_type = OrderEventType::UPDATE;
    } else if (type_sv == "CANCELLATION") {
        upd.event_type = OrderEventType::CANCELLATION;
    } else {
        return ErrorCode::USER_UNKNOWN_EVENT_TYPE;
    }

    // side
    std::string_view side_sv;
    auto side_err = get_required_string(doc, "side", side_sv);
    if (side_err != ErrorCode::OK) return side_err;
    auto parsed_side = parse_side(side_sv);
    if (!parsed_side.ok()) return parsed_side.error;
    upd.side = parsed_side.value;

    // price (string -> 10000x fixed-point)
    std::string_view price_sv;
    auto price_err = get_required_string(doc, "price", price_sv);
    if (price_err != ErrorCode::OK) return price_err;
    auto parsed_price = parse_price(price_sv);
    if (!parsed_price.ok()) return parsed_price.error;
    upd.price = parsed_price.value;

    // original_size (string -> int64)
    std::string_view orig_sv;
    auto orig_err = get_required_string(doc, "original_size", orig_sv);
    if (orig_err != ErrorCode::OK) return orig_err;
    auto parsed_orig = parse_qty(orig_sv);
    if (!parsed_orig.ok()) return parsed_orig.error;
    upd.original_size = parsed_orig.value;

    // size_matched (string -> int64)
    std::string_view matched_sv;
    auto matched_err = get_required_string(doc, "size_matched", matched_sv);
    if (matched_err != ErrorCode::OK) return matched_err;
    auto parsed_matched = parse_qty(matched_sv);
    if (!parsed_matched.ok()) return parsed_matched.error;
    upd.size_matched = parsed_matched.value;

    // size_matched > original_size sanity check
    if (upd.size_matched > upd.original_size && upd.original_size > 0) {
        upd.size_matched_exceeds_original = true;
    }

    // timestamp (optional)
    parse_optional_timestamp(doc, "timestamp", upd.exchange_ts);

    out.payload = upd;
    return ErrorCode::OK;
}

ErrorCode UserMessageParser::parse_trade(simdjson::ondemand::document& doc,
                                         UserMessageEvent& out) {
    UserTradeUpdate upd;

    // trade id
    std::string_view id_sv;
    auto id_err = get_required_string(doc, "id", id_sv);
    if (id_err != ErrorCode::OK) return id_err;
    upd.trade_id = TradeId(id_sv);
    if (id_sv.size() >= kMaxTradeIdLen) ++out.truncated_fields;

    // asset_id
    std::string_view aid_sv;
    auto aid_err = get_required_string(doc, "asset_id", aid_sv);
    if (aid_err != ErrorCode::OK) return aid_err;
    upd.asset_id = AssetId(aid_sv);
    if (aid_sv.size() >= kMaxAssetIdLen) ++out.truncated_fields;

    // market (condition ID) - optional
    std::string_view mkt_sv;
    if (get_required_string(doc, "market", mkt_sv) == ErrorCode::OK) {
        upd.market_id = AssetId(mkt_sv);
    }

    // status
    std::string_view status_sv;
    auto status_err = get_required_string(doc, "status", status_sv);
    if (status_err != ErrorCode::OK) return status_err;
    auto ts_err = parse_trade_status(status_sv, upd.status);
    if (ts_err != ErrorCode::OK) return ts_err;

    // side
    std::string_view side_sv;
    auto side_err = get_required_string(doc, "side", side_sv);
    if (side_err != ErrorCode::OK) return side_err;
    auto parsed_side = parse_side(side_sv);
    if (!parsed_side.ok()) return parsed_side.error;
    upd.side = parsed_side.value;

    // price (string -> 10000x fixed-point)
    std::string_view price_sv;
    auto price_err = get_required_string(doc, "price", price_sv);
    if (price_err != ErrorCode::OK) return price_err;
    auto parsed_price = parse_price(price_sv);
    if (!parsed_price.ok()) return parsed_price.error;
    upd.fill_price = parsed_price.value;

    // size (string -> int64)
    std::string_view size_sv;
    auto size_err = get_required_string(doc, "size", size_sv);
    if (size_err != ErrorCode::OK) return size_err;
    auto parsed_size = parse_qty(size_sv);
    if (!parsed_size.ok()) return parsed_size.error;
    upd.fill_size = parsed_size.value;

    // taker_order_id
    std::string_view taker_sv;
    auto taker_err = get_required_string(doc, "taker_order_id", taker_sv);
    if (taker_err != ErrorCode::OK) return taker_err;
    upd.taker_order_id = OrderId(taker_sv);

    // matchtime (optional)
    parse_optional_timestamp(doc, "matchtime", upd.match_ts);

    // last_update (optional)
    parse_optional_timestamp(doc, "last_update", upd.last_update_ts);

    // timestamp (optional, fallback)
    if (upd.match_ts == 0) {
        parse_optional_timestamp(doc, "timestamp", upd.match_ts);
    }

    // maker_orders (optional array of {order_id, matched_amount, ...})
    // Parsed last: simdjson ondemand is forward-only, and maker_orders
    // may appear at any position in the JSON object.
    simdjson::ondemand::array maker_arr;
    if (doc["maker_orders"].get_array().get(maker_arr) == simdjson::SUCCESS) {
        for (auto elem : maker_arr) {
            if (upd.maker_entry_count >= kMaxMakerOrders) {
                ++out.truncated_fields;
                break;
            }
            simdjson::ondemand::object mobj;
            if (elem.get_object().get(mobj) != simdjson::SUCCESS) continue;

            std::string_view mo_id_sv;
            if (get_required_string(mobj, "order_id", mo_id_sv) != ErrorCode::OK) continue;

            std::string_view mo_amt_sv;
            if (get_required_string(mobj, "matched_amount", mo_amt_sv) != ErrorCode::OK) continue;
            auto parsed_amt = parse_qty(mo_amt_sv);
            if (!parsed_amt.ok()) continue;

            auto& entry = upd.maker_entries[upd.maker_entry_count++];
            entry.order_id = OrderId(mo_id_sv);
            entry.matched_amount = parsed_amt.value;
        }
    }

    out.payload = upd;
    return ErrorCode::OK;
}

}  // namespace lt
