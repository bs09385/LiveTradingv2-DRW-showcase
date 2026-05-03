add_library(events INTERFACE)
target_include_directories(events INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(events INTERFACE common)
