#include "crypto/hex_utils.h"
#include "crypto/keccak.h"

namespace lt {

std::string eip55_checksum(const uint8_t address[20]) {
    std::string hex_lower = hex_encode(address, 20);  // 40-char lowercase
    Bytes32 addr_hash = keccak256(
        reinterpret_cast<const uint8_t*>(hex_lower.data()), hex_lower.size());

    std::string result = "0x";
    result.reserve(42);
    for (size_t i = 0; i < 40; ++i) {
        uint8_t hash_nibble = (addr_hash[i / 2] >> (i % 2 == 0 ? 4 : 0)) & 0x0F;
        if (hex_lower[i] >= 'a' && hex_lower[i] <= 'f' && hash_nibble >= 8) {
            result += static_cast<char>(hex_lower[i] - 32);  // uppercase
        } else {
            result += hex_lower[i];
        }
    }
    return result;
}

}  // namespace lt
