cmake_minimum_required(VERSION 3.20)
project(LiveTradingv2 LANGUAGES C CXX)

# Force static libraries (secp256k1 FetchContent can flip BUILD_SHARED_LIBS ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler warnings
include(cmake/CompilerWarnings.cmake)

# Fetch dependencies (simdjson, SPSCQueue, doctest, Boost)
include(cmake/FetchDependencies.cmake)

# Find system packages
# OpenSSL hint for MinGW (Linux finds system OpenSSL automatically)
if(WIN32)
    set(OPENSSL_ROOT_DIR "C:/ProgramData/mingw64/mingw64/opt" CACHE PATH "OpenSSL root")
endif()
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Sub-libraries
add_subdirectory(src/common)
add_subdirectory(src/events)
add_subdirectory(src/queue)
add_subdirectory(src/parser)
add_subdirectory(src/book)
add_subdirectory(src/logger)
add_subdirectory(src/state)
add_subdirectory(src/ws)
add_subdirectory(src/scheduler)
add_subdirectory(src/crypto)
add_subdirectory(src/rest)
add_subdirectory(src/exec)
add_subdirectory(src/inventory)
add_subdirectory(src/ui_bridge)
add_subdirectory(src/rotation)
add_subdirectory(src/slot)
add_subdirectory(src/rtds)
add_subdirectory(src/binance)
add_subdirectory(src/recorder)

# Main executable
add_executable(engine src/main.cpp)
target_link_libraries(engine PRIVATE
    common events queue parser book logger state ws scheduler lt_crypto rest exec inventory ui_bridge rotation slot rtds binance_md recorder
    Boost::beast Boost::asio Boost::system
    OpenSSL::SSL OpenSSL::Crypto Threads::Threads
)
if(WIN32)
    target_link_libraries(engine PRIVATE ws2_32 wsock32 mswsock)
endif()
target_include_directories(engine PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_project_warnings(engine)

# Tests
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
