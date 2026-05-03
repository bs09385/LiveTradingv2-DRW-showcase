#include "parser/market_message_parser.h"

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
void parse_optional_timestamp(JsonObj& obj, Timestamp_ns& out) {
    std::string_view ts_str;
    if (obj["timestamp"].get_string().get(ts_str) != simdjson::SUCCESS) return;
    int64_t ts_val = 0;
    auto r = std::from_chars(ts_str.data(), ts_str.data() + ts_str.size(), ts_val);
    if (r.ec == std::errc{}) out = ts_val;
}

template <typename JsonObj>
ErrorCode parse_required_price_field(JsonObj& obj, const char* key, Price_t& out) {
    std::string_view sv;
    auto field_err = get_required_string(obj, key, sv);
    if (field_err != ErrorCode::OK) return field_err;
    auto parsed = parse_price(sv);
    if (!parsed.ok()) return parsed.error;
    out = parsed.value;
    return ErrorCode::OK;
}

template <typename JsonObj>
ErrorCode parse_required_qty_field(JsonObj& obj, const char* key, Qty_t& out) {
    std::string_view sv;
    auto field_err = get_required_string(obj, key, sv);
    if (field_err != ErrorCode::OK) return field_err;
    auto parsed = parse_qty(sv);
    if (!parsed.ok()) return parsed.error;
    out = parsed.value;
    return ErrorCode::OK;
}

template <typename JsonObj>
ErrorCode parse_required_side_field(JsonObj& obj, const char* key, Side& out) {
    std::string_view sv;
    auto field_err = get_required_string(obj, key, sv);
    if (field_err != ErrorCode::OK) return field_err;
    auto parsed = parse_side(sv);
    if (!parsed.ok()) return parsed.error;
    out = parsed.value;
    return ErrorCode::OK;
}

}  // namespace

MarketMessageParser::MarketMessageParser() = default;

ErrorCode MarketMessageParser::parse(std::string_view payload, Timestamp_ns recv_ts, SeqNum_t seq,
                                     MarketEvent& out) {
    if (payload.empty()) return ErrorCode::EMPTY_INPUT;

    // Handle PONG (not valid JSON)
    if (payload == "PONG") return ErrorCode::PONG_MESSAGE;

    // Reuse backing storage and keep some extra capacity for simdjson padding.
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
        // Watcher WS messages can omit event_type. Infer by shape.
        simdjson::ondemand::array probe_arr;
        if (doc["price_changes"].get_array().get(probe_arr) == simdjson::SUCCESS) {
            return parse_price_change(doc, out);
        }
        if (doc["bids"].get_array().get(probe_arr) == simdjson::SUCCESS &&
            doc["asks"].get_array().get(probe_arr) == simdjson::SUCCESS) {
            return parse_book(doc, out);
        }

        std::string_view probe_sv;
        if (doc["best_bid"].get_string().get(probe_sv) == simdjson::SUCCESS &&
            doc["best_ask"].get_string().get(probe_sv) == simdjson::SUCCESS) {
            return parse_best_bid_ask(doc, out);
        }
        if (doc["price"].get_string().get(probe_sv) == simdjson::SUCCESS &&
            doc["size"].get_string().get(probe_sv) == simdjson::SUCCESS &&
            doc["side"].get_string().get(probe_sv) == simdjson::SUCCESS) {
            return parse_last_trade_price(doc, out);
        }
        return et_err;
    }

    if (event_type == "book") {
        return parse_book(doc, out);
    } else if (event_type == "price_change") {
        return parse_price_change(doc, out);
    } else if (event_type == "best_bid_ask") {
        return parse_best_bid_ask(doc, out);
    } else if (event_type == "tick_size_change") {
        return parse_tick_size_change(doc, out);
    } else if (event_type == "last_trade_price") {
        return parse_last_trade_price(doc, out);
    } else if (event_type == "new_market") {
        return parse_new_market(doc, out);
    } else if (event_type == "market_resolved") {
        return parse_market_resolved(doc, out);
    }

    return ErrorCode::UNKNOWN_EVENT_TYPE;
}

ErrorCode MarketMessageParser::parse_book(simdjson::ondemand::document& doc, MarketEvent& out) {
    BookSnapshot snap;

    // asset_id
    std::string_view aid;
    auto asset_err = get_required_string(doc, "asset_id", aid);
    if (asset_err != ErrorCode::OK) return asset_err;
    snap.asset_id = AssetId(aid);

    // market (optional condition_id)
    std::string_view market_sv;
    if (doc["market"].get_string().get(market_sv) == simdjson::SUCCESS) {
        snap.market_id = AssetId(market_sv);
    }

    // hash (optional)
    std::string_view hash_sv;
    if (doc["hash"].get_string().get(hash_sv) == simdjson::SUCCESS) {
        snap.hash_len = static_cast<uint8_t>(
            std::min(hash_sv.size(), sizeof(snap.hash) - 1));
        std::memcpy(snap.hash, hash_sv.data(), snap.hash_len);
        snap.hash[snap.hash_len] = '\0';
    }

    // timestamp (string)
    parse_optional_timestamp(doc, snap.exchange_ts);

    // bids array
    simdjson::ondemand::array bids_arr;
    auto bids_err = doc["bids"].get_array().get(bids_arr);
    if (bids_err != simdjson::SUCCESS) {
        return bids_err == simdjson::NO_SUCH_FIELD ? ErrorCode::JSON_MISSING_FIELD
                                                   : ErrorCode::JSON_TYPE_ERROR;
    }

    for (auto bid : bids_arr) {
        if (snap.bid_count >= snap.bids.size()) return ErrorCode::OUT_OF_RANGE;

        simdjson::ondemand::object obj;
        if (bid.get_object().get(obj) != simdjson::SUCCESS) return ErrorCode::JSON_TYPE_ERROR;

        Price_t price = 0;
        Qty_t size = 0;
        auto p_err = parse_required_price_field(obj, "price", price);
        if (p_err != ErrorCode::OK) return p_err;
        auto s_err = parse_required_qty_field(obj, "size", size);
        if (s_err != ErrorCode::OK) return s_err;

        snap.bids[snap.bid_count].price = price;
        snap.bids[snap.bid_count].size = size;
        ++snap.bid_count;
    }

    // asks array
    simdjson::ondemand::array asks_arr;
    auto asks_err = doc["asks"].get_array().get(asks_arr);
    if (asks_err != simdjson::SUCCESS) {
        return asks_err == simdjson::NO_SUCH_FIELD ? ErrorCode::JSON_MISSING_FIELD
                                                   : ErrorCode::JSON_TYPE_ERROR;
    }

    for (auto ask : asks_arr) {
        if (snap.ask_count >= snap.asks.size()) return ErrorCode::OUT_OF_RANGE;

        simdjson::ondemand::object obj;
        if (ask.get_object().get(obj) != simdjson::SUCCESS) return ErrorCode::JSON_TYPE_ERROR;

        Price_t price = 0;
        Qty_t size = 0;
        auto p_err = parse_required_price_field(obj, "price", price);
        if (p_err != ErrorCode::OK) return p_err;
        auto s_err = parse_required_qty_field(obj, "size", size);
        if (s_err != ErrorCode::OK) return s_err;

        snap.asks[snap.ask_count].price = price;
        snap.asks[snap.ask_count].size = size;
        ++snap.ask_count;
    }

    out.payload = std::move(snap);
    return ErrorCode::OK;
}

ErrorCode MarketMessageParser::parse_price_change(simdjson::ondemand::document& doc,
                                                  MarketEvent& out) {
    PriceChangeEvent evt;

    // timestamp
    parse_optional_timestamp(doc, evt.exchange_ts);

    // price_changes array
    simdjson::ondemand::array pc_arr;
    auto pc_err = doc["price_changes"].get_array().get(pc_arr);
    if (pc_err != simdjson::SUCCESS) {
        return pc_err == simdjson::NO_SUCH_FIELD ? ErrorCode::JSON_MISSING_FIELD
                                                 : ErrorCode::JSON_TYPE_ERROR;
    }

    for (auto pc_elem : pc_arr) {
        if (evt.asset_count >= evt.asset_changes.size()) return ErrorCode::OUT_OF_RANGE;
        simdjson::ondemand::object pc_obj;
        if (pc_elem.get_object().get(pc_obj) != simdjson::SUCCESS) return ErrorCode::JSON_TYPE_ERROR;

        auto& ac = evt.asset_changes[evt.asset_count];

        std::string_view aid;
        auto asset_err = get_required_string(pc_obj, "asset_id", aid);
        if (asset_err != ErrorCode::OK) return asset_err;
        ac.asset_id = AssetId(aid);

        // best_bid/best_ask (optional)
        std::string_view bb_sv, ba_sv;
        if (pc_obj["best_bid"].get_string().get(bb_sv) == simdjson::SUCCESS) {
            auto p = parse_price(bb_sv);
            if (!p.ok()) return p.error;
            ac.best_bid = p.value;
        }
        if (pc_obj["best_ask"].get_string().get(ba_sv) == simdjson::SUCCESS) {
            auto p = parse_price(ba_sv);
            if (!p.ok()) return p.error;
            ac.best_ask = p.value;
        }

        // Try flat format first (Polymarket default), fall back to nested changes[]
        std::string_view probe_sv;
        if (pc_obj["price"].get_string().get(probe_sv) == simdjson::SUCCESS) {
            // Flat form: {"asset_id":"...","price":"0.52","side":"BUY","size":"10"}
            if (ac.change_count >= ac.changes.size()) return ErrorCode::OUT_OF_RANGE;
            Price_t price = 0;
            Side side = Side::BID;
            Qty_t size = 0;

            auto p = parse_price(probe_sv);
            if (!p.ok()) return p.error;
            price = p.value;
            auto sd_err = parse_required_side_field(pc_obj, "side", side);
            if (sd_err != ErrorCode::OK) return sd_err;
            auto sz_err = parse_required_qty_field(pc_obj, "size", size);
            if (sz_err != ErrorCode::OK) return sz_err;

            ac.changes[ac.change_count].price = price;
            ac.changes[ac.change_count].side = side;
            ac.changes[ac.change_count].size = size;
            ++ac.change_count;
        } else {
            // Nested changes[] array (legacy/internal/watcher)
            simdjson::ondemand::array changes_arr;
            auto changes_err = pc_obj["changes"].get_array().get(changes_arr);
            if (changes_err == simdjson::SUCCESS) {
                for (auto ch : changes_arr) {
                    if (ac.change_count >= ac.changes.size()) return ErrorCode::OUT_OF_RANGE;
                    simdjson::ondemand::object ch_obj;
                    if (ch.get_object().get(ch_obj) != simdjson::SUCCESS) return ErrorCode::JSON_TYPE_ERROR;

                    Price_t price = 0;
                    Side side = Side::BID;
                    Qty_t size = 0;

                    auto p_err = parse_required_price_field(ch_obj, "price", price);
                    if (p_err != ErrorCode::OK) return p_err;
                    auto sd_err = parse_required_side_field(ch_obj, "side", side);
                    if (sd_err != ErrorCode::OK) return sd_err;
                    auto sz_err = parse_required_qty_field(ch_obj, "size", size);
                    if (sz_err != ErrorCode::OK) return sz_err;

                    ac.changes[ac.change_count].price = price;
                    ac.changes[ac.change_count].side = side;
                    ac.changes[ac.change_count].size = size;
                    ++ac.change_count;
                }
            } else if (changes_err != simdjson::NO_SUCH_FIELD) {
                return ErrorCode::JSON_TYPE_ERROR;
            }
        }

        ++evt.asset_count;
    }

    if (evt.asset_count == 0) return ErrorCode::JSON_MISSING_FIELD;

    out.payload = std::move(evt);
    return ErrorCode::OK;
}

ErrorCode MarketMessageParser::parse_best_bid_ask(simdjson::ondemand::document& doc,
                                                  MarketEvent& out) {
    BestBidAskEvent evt;

    std::string_view aid;
    auto asset_err = get_required_string(doc, "asset_id", aid);
    if (asset_err != ErrorCode::OK) return asset_err;
    evt.asset_id = AssetId(aid);

    auto bb_err = parse_required_price_field(doc, "best_bid", evt.best_bid);
    if (bb_err != ErrorCode::OK) return bb_err;

    auto ba_err = parse_required_price_field(doc, "best_ask", evt.best_ask);
    if (ba_err != ErrorCode::OK) return ba_err;

    std::string_view sp_sv;
    if (doc["spread"].get_string().get(sp_sv) == simdjson::SUCCESS) {
        auto p = parse_price(sp_sv);
        if (!p.ok()) return p.error;
        evt.spread = p.value;
    }

    parse_optional_timestamp(doc, evt.exchange_ts);

    out.payload = std::move(evt);
    return ErrorCode::OK;
}

ErrorCode MarketMessageParser::parse_tick_size_change(simdjson::ondemand::document& doc,
                                                     MarketEvent& out) {
    TickSizeChangeEvent evt;

    std::string_view aid;
    auto asset_err = get_required_string(doc, "asset_id", aid);
    if (asset_err != ErrorCode::OK) return asset_err;
    evt.asset_id = AssetId(aid);

    std::string_view old_sv, new_sv;
    if (doc["old_tick_size"].get_string().get(old_sv) == simdjson::SUCCESS) {
        auto p = parse_price(old_sv);
        if (!p.ok()) return p.error;
        evt.old_tick_size = p.value;
    }
    auto new_tick_err = get_required_string(doc, "new_tick_size", new_sv);
    if (new_tick_err != ErrorCode::OK) return new_tick_err;
    auto p = parse_price(new_sv);
    if (!p.ok()) return p.error;
    evt.new_tick_size = p.value;
    if (evt.new_tick_size <= 0) return ErrorCode::OUT_OF_RANGE;

    parse_optional_timestamp(doc, evt.exchange_ts);

    out.payload = std::move(evt);
    return ErrorCode::OK;
}

ErrorCode MarketMessageParser::parse_last_trade_price(simdjson::ondemand::document& doc,
                                                     MarketEvent& out) {
    LastTradePriceEvent evt;

    std::string_view aid;
    auto asset_err = get_required_string(doc, "asset_id", aid);
    if (asset_err != ErrorCode::OK) return asset_err;
    evt.asset_id = AssetId(aid);

    // market (optional condition_id)
    std::string_view market_sv;
    if (doc["market"].get_string().get(market_sv) == simdjson::SUCCESS) {
        evt.market_id = AssetId(market_sv);
    }

    auto price_err = parse_required_price_field(doc, "price", evt.price);
    if (price_err != ErrorCode::OK) return price_err;
    auto size_err = parse_required_qty_field(doc, "size", evt.size);
    if (size_err != ErrorCode::OK) return size_err;
    auto side_err = parse_required_side_field(doc, "side", evt.side);
    if (side_err != ErrorCode::OK) return side_err;

    // fee_rate_bps (optional)
    std::string_view fee_sv;
    if (doc["fee_rate_bps"].get_string().get(fee_sv) == simdjson::SUCCESS) {
        uint16_t fee_val = 0;
        auto r = std::from_chars(fee_sv.data(), fee_sv.data() + fee_sv.size(), fee_val);
        if (r.ec == std::errc{}) evt.fee_rate_bps = fee_val;
    }

    parse_optional_timestamp(doc, evt.exchange_ts);

    out.payload = std::move(evt);
    return ErrorCode::OK;
}

ErrorCode MarketMessageParser::parse_new_market(simdjson::ondemand::document& doc,
                                                MarketEvent& out) {
    NewMarketEvent evt;

    // market (condition_id)
    std::string_view market_sv;
    auto market_err = get_required_string(doc, "market", market_sv);
    if (market_err != ErrorCode::OK) return market_err;
    evt.market_id = AssetId(market_sv);

    // assets_ids array
    simdjson::ondemand::array assets_arr;
    auto assets_err = doc["assets_ids"].get_array().get(assets_arr);
    if (assets_err == simdjson::SUCCESS) {
        for (auto asset : assets_arr) {
            if (evt.asset_count >= 2) break;
            std::string_view asset_sv;
            if (asset.get_string().get(asset_sv) == simdjson::SUCCESS) {
                evt.assets[evt.asset_count] = AssetId(asset_sv);
                ++evt.asset_count;
            }
        }
    }

    // outcomes array
    simdjson::ondemand::array outcomes_arr;
    auto outcomes_err = doc["outcomes"].get_array().get(outcomes_arr);
    if (outcomes_err == simdjson::SUCCESS) {
        for (auto outcome : outcomes_arr) {
            if (evt.outcome_count >= 2) break;
            std::string_view outcome_sv;
            if (outcome.get_string().get(outcome_sv) == simdjson::SUCCESS) {
                auto len = std::min(outcome_sv.size(), sizeof(evt.outcomes[0]) - 1);
                std::memcpy(evt.outcomes[evt.outcome_count], outcome_sv.data(), len);
                evt.outcomes[evt.outcome_count][len] = '\0';
                ++evt.outcome_count;
            }
        }
    }

    parse_optional_timestamp(doc, evt.exchange_ts);

    out.payload = std::move(evt);
    return ErrorCode::OK;
}

ErrorCode MarketMessageParser::parse_market_resolved(simdjson::ondemand::document& doc,
                                                     MarketEvent& out) {
    MarketResolvedEvent evt;

    // market (condition_id)
    std::string_view market_sv;
    auto market_err = get_required_string(doc, "market", market_sv);
    if (market_err != ErrorCode::OK) return market_err;
    evt.market_id = AssetId(market_sv);

    // winning_asset_id
    std::string_view winner_sv;
    if (doc["winning_asset_id"].get_string().get(winner_sv) == simdjson::SUCCESS) {
        evt.winning_asset_id = AssetId(winner_sv);
    }

    // winning_outcome
    std::string_view wo_sv;
    if (doc["winning_outcome"].get_string().get(wo_sv) == simdjson::SUCCESS) {
        evt.winning_outcome_len = static_cast<uint8_t>(
            std::min(wo_sv.size(), sizeof(evt.winning_outcome) - 1));
        std::memcpy(evt.winning_outcome, wo_sv.data(), evt.winning_outcome_len);
        evt.winning_outcome[evt.winning_outcome_len] = '\0';
    }

    // assets_ids array
    simdjson::ondemand::array assets_arr;
    auto assets_err = doc["assets_ids"].get_array().get(assets_arr);
    if (assets_err == simdjson::SUCCESS) {
        for (auto asset : assets_arr) {
            if (evt.asset_count >= 2) break;
            std::string_view asset_sv;
            if (asset.get_string().get(asset_sv) == simdjson::SUCCESS) {
                evt.assets[evt.asset_count] = AssetId(asset_sv);
                ++evt.asset_count;
            }
        }
    }

    parse_optional_timestamp(doc, evt.exchange_ts);

    out.payload = std::move(evt);
    return ErrorCode::OK;
}

}  // namespace lt
