add_library(rotation
    rotation_coordinator.cpp
)
target_include_directories(rotation PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rotation PUBLIC common logger)
set_project_warnings(rotation)
