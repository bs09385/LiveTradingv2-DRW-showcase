#include "doctest/doctest.h"

#include "ui_bridge/ui_command_parser.h"
#include "scheduler/execution_mode.h"

TEST_SUITE("UiCommandParser") {

TEST_CASE("parse enable_strategy") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"enable_strategy"})");
    CHECK(cmd.valid);
    CHECK(cmd.event.source == lt::EventSource::CONTROL);
    CHECK(cmd.event.kind == lt::SchedulerEventKind::CONTROL_ENABLE_STRATEGY);
    CHECK(cmd.event.control_bool_param == true);
}

TEST_CASE("parse disable_strategy") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"disable_strategy"})");
    CHECK(cmd.valid);
    CHECK(cmd.event.source == lt::EventSource::CONTROL);
    CHECK(cmd.event.kind == lt::SchedulerEventKind::CONTROL_ENABLE_STRATEGY);
    CHECK(cmd.event.control_bool_param == false);
}

TEST_CASE("parse cancel_all") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"cancel_all"})");
    CHECK(cmd.valid);
    CHECK(cmd.event.source == lt::EventSource::CONTROL);
    CHECK(cmd.event.kind == lt::SchedulerEventKind::CONTROL_CANCEL_ALL);
}

TEST_CASE("parse set_mode DRY_RUN") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"set_mode","mode":"DRY_RUN"})");
    CHECK(cmd.valid);
    CHECK(cmd.event.kind == lt::SchedulerEventKind::CONTROL_SET_MODE);
    CHECK(cmd.event.control_mode == static_cast<uint8_t>(lt::ExecutionMode::DRY_RUN));
}

TEST_CASE("parse set_mode LIVE") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"set_mode","mode":"LIVE"})");
    CHECK(cmd.valid);
    CHECK(cmd.event.control_mode == static_cast<uint8_t>(lt::ExecutionMode::LIVE));
}

TEST_CASE("parse inventory_split") {
    auto cmd = lt::parse_ui_command(
        R"({"cmd":"inventory_split","condition_id":"cond-1","shares":25})");
    CHECK(cmd.valid);
    CHECK(cmd.event.kind == lt::SchedulerEventKind::CONTROL_INVENTORY_SPLIT);
    CHECK(cmd.event.control_condition_id == lt::AssetId("cond-1"));
    CHECK(cmd.event.control_qty_param == lt::qty_from_int(25));
}

TEST_CASE("parse inventory_merge") {
    auto cmd = lt::parse_ui_command(
        R"({"cmd":"inventory_merge","condition_id":"cond-1","shares":10})");
    CHECK(cmd.valid);
    CHECK(cmd.event.kind == lt::SchedulerEventKind::CONTROL_INVENTORY_MERGE);
    CHECK(cmd.event.control_condition_id == lt::AssetId("cond-1"));
    CHECK(cmd.event.control_qty_param == lt::qty_from_int(10));
}

TEST_CASE("parse inventory_redeem with token and shares") {
    auto cmd = lt::parse_ui_command(
        R"({"cmd":"inventory_redeem","condition_id":"cond-1","token_id":"tok-up","shares":5})");
    CHECK(cmd.valid);
    CHECK(cmd.event.kind == lt::SchedulerEventKind::CONTROL_INVENTORY_REDEEM);
    CHECK(cmd.event.control_condition_id == lt::AssetId("cond-1"));
    CHECK(cmd.event.control_token_id == lt::AssetId("tok-up"));
    CHECK(cmd.event.control_qty_param == lt::qty_from_int(5));
}

TEST_CASE("parse inventory_redeem defaults shares to zero") {
    auto cmd = lt::parse_ui_command(
        R"({"cmd":"inventory_redeem","condition_id":"cond-1"})");
    CHECK(cmd.valid);
    CHECK(cmd.event.kind == lt::SchedulerEventKind::CONTROL_INVENTORY_REDEEM);
    CHECK(cmd.event.control_qty_param == 0);
}

TEST_CASE("parse set_mode invalid mode string") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"set_mode","mode":"INVALID"})");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("parse set_mode missing mode field") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"set_mode"})");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("set_spread is no longer a valid command") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"set_spread","ticks":3})");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("set_size is no longer a valid command") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"set_size","size":10})");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("inventory_split requires positive shares") {
    auto cmd = lt::parse_ui_command(
        R"({"cmd":"inventory_split","condition_id":"cond-1","shares":0})");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("inventory_merge rejects negative shares") {
    auto cmd = lt::parse_ui_command(
        R"({"cmd":"inventory_merge","condition_id":"cond-1","shares":-1})");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("inventory command requires condition_id") {
    auto cmd = lt::parse_ui_command(
        R"({"cmd":"inventory_split","shares":10})");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("parse unknown command") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"unknown_cmd"})");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("parse empty JSON") {
    auto cmd = lt::parse_ui_command("");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("parse invalid JSON") {
    auto cmd = lt::parse_ui_command("not json at all");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("parse JSON missing cmd field") {
    auto cmd = lt::parse_ui_command(R"({"foo":"bar"})");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("parse JSON with extra fields is OK") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"cancel_all","extra":"ignored"})");
    CHECK(cmd.valid);
    CHECK(cmd.event.kind == lt::SchedulerEventKind::CONTROL_CANCEL_ALL);
}

TEST_CASE("parse start_session with end_time") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"start_session","end_time":1711036800})");
    CHECK(cmd.valid);
    CHECK(cmd.event.kind == lt::SchedulerEventKind::CONTROL_START_SESSION);
    CHECK(cmd.event.control_qty_param == 1711036800);
}

TEST_CASE("parse start_session indefinite (no end_time)") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"start_session"})");
    CHECK(cmd.valid);
    CHECK(cmd.event.kind == lt::SchedulerEventKind::CONTROL_START_SESSION);
    CHECK(cmd.event.control_qty_param == 0);
}

TEST_CASE("parse stop_session") {
    auto cmd = lt::parse_ui_command(R"({"cmd":"stop_session"})");
    CHECK(cmd.valid);
    CHECK(cmd.event.kind == lt::SchedulerEventKind::CONTROL_STOP_SESSION);
}

}  // TEST_SUITE
