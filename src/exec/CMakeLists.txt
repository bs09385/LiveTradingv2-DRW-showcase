add_library(exec
    rate_limiter.cpp
    heartbeat_manager.cpp
    order_builder.cpp
    execution_gateway.cpp
)
target_include_directories(exec PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(exec PUBLIC
    common events queue lt_crypto rest logger scheduler
    Boost::beast Boost::asio Boost::system
    $<$<PLATFORM_ID:Windows>:bcrypt>
)
set_project_warnings(exec)
