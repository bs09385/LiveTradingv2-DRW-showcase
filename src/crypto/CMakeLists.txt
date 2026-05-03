add_library(lt_crypto STATIC
    hex_utils.cpp
    keccak.cpp
    hmac_sha256.cpp
    eip712.cpp
    order_signer.cpp
)
target_include_directories(lt_crypto PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lt_crypto PUBLIC
    sha3iuf
    secp256k1
    OpenSSL::Crypto
    $<$<PLATFORM_ID:Windows>:bcrypt>
)
set_project_warnings(lt_crypto)
