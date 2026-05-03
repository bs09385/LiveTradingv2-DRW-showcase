add_library(parser
    market_message_parser.cpp
    user_message_parser.cpp
)
target_include_directories(parser PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(parser PUBLIC common events simdjson)
set_project_warnings(parser)
