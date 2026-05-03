#include "crypto/keccak.h"

#include <cstring>
extern "C" {
#include <sha3.h>
}

static_assert(sizeof(sha3_context) <= 256, "sha3_context exceeds Keccak256Hasher storage");

namespace lt {

Bytes32 keccak256(const uint8_t* data, size_t len) {
    sha3_context ctx;
    sha3_Init256(&ctx);
    sha3_SetFlags(&ctx, SHA3_FLAGS_KECCAK);  // Keccak padding, NOT SHA-3
    sha3_Update(&ctx, data, len);

    const void* hash = sha3_Finalize(&ctx);
    Bytes32 result;
    std::memcpy(result.data(), hash, 32);
    return result;
}

Keccak256Hasher::Keccak256Hasher() {
    auto* ctx = reinterpret_cast<sha3_context*>(ctx_storage_);
    sha3_Init256(ctx);
    sha3_SetFlags(ctx, SHA3_FLAGS_KECCAK);
}

void Keccak256Hasher::update(const uint8_t* data, size_t len) {
    auto* ctx = reinterpret_cast<sha3_context*>(ctx_storage_);
    sha3_Update(ctx, data, len);
}

Bytes32 Keccak256Hasher::finalize() {
    auto* ctx = reinterpret_cast<sha3_context*>(ctx_storage_);
    const void* hash = sha3_Finalize(ctx);
    Bytes32 result;
    std::memcpy(result.data(), hash, 32);
    return result;
}

}  // namespace lt
