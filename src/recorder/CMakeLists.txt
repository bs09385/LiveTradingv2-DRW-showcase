add_library(recorder
    data_recorder.cpp
)

target_include_directories(recorder PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(recorder PUBLIC common queue)
