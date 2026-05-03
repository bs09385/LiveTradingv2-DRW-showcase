add_library(rest
    rest_auth.cpp
    rest_client.cpp
    http2_client.cpp
    rest_transport.cpp
    rest_response_parser.cpp
)
target_include_directories(rest PUBLIC ${CMAKE_SOURCE_DIR}/src)
find_package(ZLIB REQUIRED)
target_link_libraries(rest PUBLIC
    common lt_crypto
    Boost::beast Boost::asio Boost::system
    OpenSSL::SSL OpenSSL::Crypto
    simdjson
    nghttp2_static
    ZLIB::ZLIB
    $<$<PLATFORM_ID:Windows>:ws2_32 wsock32 mswsock>
)
set_project_warnings(rest)
