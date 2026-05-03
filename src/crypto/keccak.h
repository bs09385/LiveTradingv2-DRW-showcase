#pragma once

#include <cstdint>
#include <cstddef>
#include "crypto/hex_utils.h"

namespace lt {

// One-shot Keccak-256 hash (NOT SHA-3-256 — uses Keccak padding)
Bytes32 keccak256(const uint8_t* data, size_t len);

// Convenience: hash from Bytes32
inline Bytes32 keccak256(const Bytes32& data) {
    return keccak256(data.data(), data.size());
}

// Streaming Keccak-256 hasher
class Keccak256Hasher {
public:
    Keccak256Hasher();
    void update(const uint8_t* data, size_t len);
    Bytes32 finalize();

private:
    // Opaque storage for sha3_context (avoid including SHA3IUF in header)
    // sha3_context is typically ~232 bytes; we use 256 for safety
    alignas(8) uint8_t ctx_storage_[256];
};

}  // namespace lt
