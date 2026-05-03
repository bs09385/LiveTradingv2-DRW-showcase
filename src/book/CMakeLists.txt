add_library(book
    order_book.cpp
)
target_include_directories(book PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(book PUBLIC common events)
set_project_warnings(book)
