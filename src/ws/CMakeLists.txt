add_library(ws
    market_ws_client.cpp
    user_ws_client.cpp
)
target_include_directories(ws PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ws PUBLIC
    common logger
    Boost::beast Boost::asio Boost::system
    OpenSSL::SSL OpenSSL::Crypto
)
if(WIN32)
    target_link_libraries(ws PUBLIC ws2_32 wsock32 mswsock)
endif()
set_project_warnings(ws)
