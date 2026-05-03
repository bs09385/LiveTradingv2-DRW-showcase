#include <doctest/doctest.h>
#include "exec/heartbeat_manager.h"

TEST_SUITE("HeartbeatManager") {

TEST_CASE("due on first call") {
    lt::HeartbeatConfig cfg;
    cfg.interval_ms = 5000;
    lt::HeartbeatManager mgr(cfg);

    CHECK(mgr.is_due(1000000000LL));  // any time
}

TEST_CASE("not due immediately after success") {
    lt::HeartbeatConfig cfg;
    cfg.interval_ms = 5000;
    lt::HeartbeatManager mgr(cfg);

    lt::Timestamp_ns now = 1000000000LL;
    mgr.on_success("hb-1", now);

    // 1 second later - not due yet
    CHECK_FALSE(mgr.is_due(now + 1000LL * 1000000LL));
}

TEST_CASE("due after interval elapsed") {
    lt::HeartbeatConfig cfg;
    cfg.interval_ms = 5000;
    lt::HeartbeatManager mgr(cfg);

    lt::Timestamp_ns start = 1000000000LL;
    // Simulate first send
    mgr.on_success("hb-1", start);

    // After 5 seconds
    lt::Timestamp_ns later = start + 5001LL * 1000000LL;
    CHECK(mgr.is_due(later));
}

TEST_CASE("heartbeat ID chaining") {
    lt::HeartbeatConfig cfg;
    lt::HeartbeatManager mgr(cfg);

    CHECK(mgr.current_heartbeat_id().empty());

    mgr.on_success("hb-100", 0);
    CHECK(mgr.current_heartbeat_id() == "hb-100");

    mgr.on_success("hb-101", 0);
    CHECK(mgr.current_heartbeat_id() == "hb-101");
}

TEST_CASE("success/failure counters") {
    lt::HeartbeatConfig cfg;
    lt::HeartbeatManager mgr(cfg);

    CHECK(mgr.success_count() == 0);
    CHECK(mgr.failure_count() == 0);

    mgr.on_success("hb-1", 0);
    mgr.on_success("hb-2", 0);
    CHECK(mgr.success_count() == 2);

    mgr.on_failure(0);
    CHECK(mgr.failure_count() == 1);
    CHECK(mgr.consecutive_failures() == 1);
}

TEST_CASE("consecutive failures reset on success") {
    lt::HeartbeatConfig cfg;
    lt::HeartbeatManager mgr(cfg);

    mgr.on_failure(0);
    mgr.on_failure(0);
    CHECK(mgr.consecutive_failures() == 2);

    mgr.on_success("hb-1", 0);
    CHECK(mgr.consecutive_failures() == 0);
}

TEST_CASE("cancel_all policy triggered after threshold") {
    lt::HeartbeatConfig cfg;
    cfg.max_consecutive_failures = 3;
    cfg.cancel_all_on_failure = true;
    lt::HeartbeatManager mgr(cfg);

    mgr.on_failure(0);
    mgr.on_failure(0);
    CHECK_FALSE(mgr.should_cancel_all());

    mgr.on_failure(0);
    CHECK(mgr.should_cancel_all());
}

TEST_CASE("cancel_all not triggered when disabled") {
    lt::HeartbeatConfig cfg;
    cfg.max_consecutive_failures = 3;
    cfg.cancel_all_on_failure = false;
    lt::HeartbeatManager mgr(cfg);

    mgr.on_failure(0);
    mgr.on_failure(0);
    mgr.on_failure(0);
    CHECK_FALSE(mgr.should_cancel_all());
}

TEST_CASE("disabled interval never due") {
    lt::HeartbeatConfig cfg;
    cfg.interval_ms = 0;  // disabled
    lt::HeartbeatManager mgr(cfg);

    CHECK_FALSE(mgr.is_due(999999999999LL));
}

}  // TEST_SUITE
