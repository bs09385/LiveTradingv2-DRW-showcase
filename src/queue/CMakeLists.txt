add_library(queue INTERFACE)
target_include_directories(queue INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(queue INTERFACE SPSCQueue)
