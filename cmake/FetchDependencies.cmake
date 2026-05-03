include(FetchContent)

# simdjson - fastest JSON parser
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v3.12.2
    GIT_SHALLOW    TRUE
)

# rigtorp/SPSCQueue - cache-line optimized single-producer single-consumer queue
FetchContent_Declare(
    SPSCQueue
    GIT_REPOSITORY https://github.com/rigtorp/SPSCQueue.git
    GIT_TAG        v1.1
    GIT_SHALLOW    TRUE
)

# doctest - single header testing framework
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE
)

# Boost - header-only (Beast, Asio, System are all header-only in modern Boost)
set(BOOST_INCLUDE_LIBRARIES beast asio system)
set(BOOST_ENABLE_CMAKE ON)
FetchContent_Declare(
    Boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.87.0/boost-1.87.0-cmake.tar.gz
    EXCLUDE_FROM_ALL
)

# SHA3IUF - Keccak-256 implementation (no CMakeLists.txt, manual target)
FetchContent_Declare(
    sha3iuf
    GIT_REPOSITORY https://github.com/brainhub/SHA3IUF.git
    GIT_TAG        fc8504750a5c2174a1874094dd05e6a0d8797753
    GIT_SHALLOW    FALSE
)

# libsecp256k1 - ECDSA recoverable signing for EIP-712 order signatures
set(SECP256K1_ENABLE_MODULE_RECOVERY ON CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    secp256k1
    GIT_REPOSITORY https://github.com/bitcoin-core/secp256k1.git
    GIT_TAG        v0.6.0
    GIT_SHALLOW    TRUE
)

# nghttp2 - HTTP/2 C library for stream-multiplexed REST transport
set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)
set(ENABLE_APP OFF CACHE BOOL "" FORCE)
set(ENABLE_DOC OFF CACHE BOOL "" FORCE)
set(ENABLE_FAILMALLOC OFF CACHE BOOL "" FORCE)
set(WITH_LIBXML2 OFF CACHE BOOL "" FORCE)
set(WITH_JEMALLOC OFF CACHE BOOL "" FORCE)
set(WITH_SPDYLAY OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TESTING_NGHTTP2 OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nghttp2
    GIT_REPOSITORY https://github.com/nghttp2/nghttp2.git
    GIT_TAG        v1.64.0
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(simdjson SPSCQueue doctest Boost sha3iuf secp256k1 nghttp2)

# SHA3IUF has no CMakeLists.txt — create a manual INTERFACE/STATIC target
if(NOT TARGET sha3iuf)
    add_library(sha3iuf STATIC ${sha3iuf_SOURCE_DIR}/sha3.c)
    target_include_directories(sha3iuf PUBLIC ${sha3iuf_SOURCE_DIR})
endif()
