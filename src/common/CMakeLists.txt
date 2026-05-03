add_library(common
    price.cpp
    clock.cpp
    config.cpp
    account.cpp
    discovery.cpp
    derive_api_key.cpp
    balance_fetcher.cpp
    position_bootstrap.cpp
)
target_include_directories(common PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(common PUBLIC simdjson
    Boost::beast Boost::asio Boost::system
    OpenSSL::SSL OpenSSL::Crypto
    lt_crypto
    $<$<PLATFORM_ID:Windows>:ws2_32 wsock32 mswsock>
)
set_project_warnings(common)
