#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lt {

// RLP encode a byte string.
// Single byte [0x00..0x7f]: encoded as itself.
// 0-55 bytes: 0x80+len, then data.
// >55 bytes: 0xb7+len_of_len, then len (big-endian), then data.
void rlp_encode_string(std::vector<uint8_t>& out, const uint8_t* data, size_t len);

// RLP encode an unsigned integer (big-endian, no leading zeros).
// 0 is encoded as empty string (0x80).
void rlp_encode_uint64(std::vector<uint8_t>& out, uint64_t value);

// RLP encode a uint256 (32-byte big-endian), stripping leading zeros.
void rlp_encode_uint256(std::vector<uint8_t>& out, const uint8_t value[32]);

// Wrap already-RLP-encoded items into an RLP list.
// 0-55 bytes payload: 0xc0+len, then items.
// >55 bytes: 0xf7+len_of_len, then len (big-endian), then items.
void rlp_wrap_list(std::vector<uint8_t>& out, const std::vector<uint8_t>& items);

}  // namespace lt
