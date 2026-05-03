add_library(ui_bridge
    ui_serializer.cpp
    ui_command_parser.cpp
    ipc_bridge.cpp
    account_poll_service.cpp
    series_rolling_fsm.cpp
    btc_series_registry.cpp
    watcher_book_store.cpp
    watch_manager.cpp
    watcher_service.cpp
)
target_include_directories(ui_bridge PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ui_bridge PUBLIC
    common events queue logger scheduler parser ws rest
    Boost::beast Boost::asio Boost::system
    simdjson
    OpenSSL::SSL OpenSSL::Crypto
    $<$<PLATFORM_ID:Windows>:ws2_32 wsock32 mswsock>
)
set_project_warnings(ui_bridge)
