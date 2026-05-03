#pragma once

#include <string>
#include <string_view>

#include "common/error.h"
#include "common/types.h"
#include "events/user_events.h"
#include "simdjson.h"

namespace lt {

class UserMessageParser {
public:
    UserMessageParser();

    // HOT PATH: Parse a raw user WS payload into a UserMessageEvent
    // Returns ErrorCode::PONG_MESSAGE for "PONG" strings (not an error)
    ErrorCode parse(std::string_view payload, Timestamp_ns recv_ts, SeqNum_t seq,
                    UserMessageEvent& out);

private:
    simdjson::ondemand::parser parser_;
    std::string scratch_;

    ErrorCode parse_order(simdjson::ondemand::document& doc, UserMessageEvent& out);
    ErrorCode parse_trade(simdjson::ondemand::document& doc, UserMessageEvent& out);
};

}  // namespace lt
