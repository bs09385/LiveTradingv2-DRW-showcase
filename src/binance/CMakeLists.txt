add_library(binance_md
    binance_message_parser.cpp
    binance_ws_client.cpp
)

target_include_directories(binance_md PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(binance_md PUBLIC
    common rtds simdjson
    Boost::beast Boost::asio Boost::system
    OpenSSL::SSL OpenSSL::Crypto
    $<$<PLATFORM_ID:Windows>:ws2_32 wsock32 mswsock>
)
