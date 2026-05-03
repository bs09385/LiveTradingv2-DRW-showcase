add_library(rtds
    rtds_message_parser.cpp
    rtds_ws_client.cpp
)

target_include_directories(rtds PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rtds PUBLIC
    common simdjson
    Boost::beast Boost::asio Boost::system
    OpenSSL::SSL OpenSSL::Crypto
    $<$<PLATFORM_ID:Windows>:ws2_32 wsock32 mswsock>
)
