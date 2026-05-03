add_library(logger
    async_logger.cpp
    metrics.cpp
)
target_include_directories(logger PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(logger PUBLIC common queue Threads::Threads)
set_project_warnings(logger)
