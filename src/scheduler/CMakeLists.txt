add_library(scheduler
    strategy_scheduler_stub.cpp
    strategy_scheduler.cpp
    quote_planner.cpp
    strategy_state_stub.cpp
    strategy_stub.cpp
    risk_gate_stub.cpp
    exec_intent_stub_sink.cpp
    quoter_v2_strategy.cpp
    test_strategy.cpp
    inventory_test_strategy.cpp
    risk_gate.cpp
    working_order_tracker.cpp
    mode_filtered_sink.cpp
    dry_run_simulator.cpp
    strategy_book_store.cpp
)
target_include_directories(scheduler PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(scheduler PUBLIC common events queue logger book rtds binance_md)
set_project_warnings(scheduler)
