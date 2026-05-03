add_executable(tests
    test_main.cpp
    test_price.cpp
    test_order_book.cpp
    test_market_message_parser.cpp
    test_market_state_store.cpp
    test_spsc_queue.cpp
    test_strategy_scheduler_stub.cpp
    test_strategy_scheduler.cpp
    test_config.cpp
    test_integration_t0_pipeline.cpp
    test_user_message_parser.cpp
    test_exec_state_store.cpp
    test_user_scheduler_integration.cpp
    test_keccak.cpp
    test_hmac_sha256.cpp
    test_eip712.cpp
    test_order_signer.cpp
    test_rest_auth.cpp
    test_rate_limiter.cpp
    test_heartbeat_manager.cpp
    test_order_builder.cpp
    test_rest_response_parser.cpp
    test_exec_gateway_integration.cpp
    test_m4_audit.cpp
    test_m4_audit_r2.cpp
    token_pair_support/test_complement_math.cpp
    token_pair_support/test_quote_planner.cpp
    token_pair_support/test_inventory_safety.cpp
    token_pair_support/test_token_pair_integration.cpp
    token_pair_support/test_audit_fixes.cpp
    test_working_order_tracker.cpp
    test_mode_filtered_sink.cpp
    test_risk_gate.cpp
    test_test_strategy.cpp
    test_inventory_test_strategy.cpp
    test_strategy_book_store.cpp
    test_ui_types.cpp
    test_ui_serializer.cpp
    test_ui_command_parser.cpp
    test_ui_bridge_integration.cpp
    test_series_rolling_fsm.cpp
    test_btc_series_registry.cpp
    test_watcher_book_merge.cpp
    test_watch_manager.cpp
    test_watcher_commands.cpp
    test_watcher_rollover.cpp
    test_account.cpp
    test_rotation.cpp
    test_abi_encoder.cpp
    test_safe_tx_builder.cpp
    test_proxy_tx_builder.cpp
    test_relayer_auth.cpp
    test_rtds_message_parser.cpp
    test_binance_message_parser.cpp
    test_binance_status_emission.cpp
    test_data_recorder.cpp
    test_rtds_scheduler_integration.cpp
    test_journal_writer.cpp
    test_pnl_tracker.cpp
    test_http2_client.cpp
    test_rlp_encode.cpp
    test_tx_builder.cpp
    test_position_bootstrap.cpp
)

target_include_directories(tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tests PRIVATE
    common events queue parser book logger state scheduler
    lt_crypto rest exec ui_bridge rotation inventory rtds binance_md recorder
    doctest::doctest
    simdjson
    OpenSSL::SSL OpenSSL::Crypto
    Threads::Threads
    $<$<PLATFORM_ID:Windows>:bcrypt>
)

# Copy fixture files to build directory
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/fixtures DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

# Define fixture path for tests
target_compile_definitions(tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixtures/"
)

# Use simple add_test instead of doctest_discover_tests to avoid DLL issues at build time.
# Exclude Benchmarks from default CI/dev runs to avoid long/sticky test sessions.
add_test(NAME all_tests COMMAND tests --test-suite-exclude=Benchmarks)
set_tests_properties(all_tests PROPERTIES TIMEOUT 180)

# Benchmarks stay available, but are opt-in.
add_test(NAME benchmark_tests COMMAND tests --test-suite=Benchmarks)
set_tests_properties(benchmark_tests PROPERTIES DISABLED TRUE TIMEOUT 180 LABELS benchmark)
