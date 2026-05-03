add_library(slot
    market_slot_manager.cpp
)
target_include_directories(slot PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(slot PUBLIC common logger events queue)
set_project_warnings(slot)
