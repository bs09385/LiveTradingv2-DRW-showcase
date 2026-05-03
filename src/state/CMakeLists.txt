add_library(state
    market_state_store.cpp
    exec_state_store.cpp
)
target_include_directories(state PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(state PUBLIC common events book queue logger)
set_project_warnings(state)
