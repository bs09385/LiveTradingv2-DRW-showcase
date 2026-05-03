add_library(inventory
    inventory_queue_sink.cpp
    inventory_service.cpp
    abi_encoder.cpp
    relayer_auth.cpp
    safe_tx_builder.cpp
    proxy_tx_builder.cpp
    relayer_client.cpp
    rlp_encoder.cpp
    tx_builder.cpp
    polygon_rpc_client.cpp
)
target_include_directories(inventory PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(inventory PUBLIC
    common queue logger lt_crypto
    Boost::beast OpenSSL::SSL OpenSSL::Crypto
    simdjson
    $<$<PLATFORM_ID:Windows>:ws2_32 wsock32 mswsock>
)
set_project_warnings(inventory)
