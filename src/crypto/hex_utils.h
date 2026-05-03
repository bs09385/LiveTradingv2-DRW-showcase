#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <array>

namespace lt {

// 32-byte hash/key type used throughout crypto layer
using Bytes32 = std::array<uint8_t, 32>;

// ---------------------------------------------------------------------------
// Hex encode: binary -> lowercase hex string
// ---------------------------------------------------------------------------
inline std::string hex_encode(const uint8_t* data, size_t len) {
    static constexpr char kHexChars[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHexChars[data[i] >> 4]);
        out.push_back(kHexChars[data[i] & 0x0F]);
    }
    return out;
}

inline std::string hex_encode(const Bytes32& data) {
    return hex_encode(data.data(), data.size());
}

// ---------------------------------------------------------------------------
// Hex decode: hex string -> binary. Returns bytes written, 0 on error.
// Accepts optional "0x" prefix.
// ---------------------------------------------------------------------------
inline size_t hex_decode(std::string_view hex, uint8_t* out, size_t max_len) {
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex.remove_prefix(2);
    }
    if (hex.size() % 2 != 0) return 0;
    size_t byte_count = hex.size() / 2;
    if (byte_count > max_len) return 0;

    for (size_t i = 0; i < byte_count; ++i) {
        uint8_t hi, lo;
        char ch = hex[i * 2];
        if (ch >= '0' && ch <= '9') hi = static_cast<uint8_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') hi = static_cast<uint8_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') hi = static_cast<uint8_t>(ch - 'A' + 10);
        else return 0;

        ch = hex[i * 2 + 1];
        if (ch >= '0' && ch <= '9') lo = static_cast<uint8_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') lo = static_cast<uint8_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') lo = static_cast<uint8_t>(ch - 'A' + 10);
        else return 0;

        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return byte_count;
}

inline bool hex_decode_to_bytes32(std::string_view hex, Bytes32& out) {
    return hex_decode(hex, out.data(), 32) == 32;
}

// ---------------------------------------------------------------------------
// Parse Ethereum address (20 bytes from hex, with or without 0x prefix)
// ---------------------------------------------------------------------------
inline bool parse_eth_address(std::string_view addr_hex, uint8_t out[20]) {
    return hex_decode(addr_hex, out, 20) == 20;
}

// ---------------------------------------------------------------------------
// Convert decimal string to uint256 (32-byte big-endian).
// Handles arbitrarily large decimal numbers up to 2^256-1.
// ---------------------------------------------------------------------------
inline bool decimal_to_uint256(std::string_view decimal, uint8_t out[32]) {
    std::memset(out, 0, 32);
    if (decimal.empty()) return false;

    // Process each decimal digit: result = result * 10 + digit
    for (char c : decimal) {
        if (c < '0' || c > '9') return false;
        uint8_t digit = static_cast<uint8_t>(c - '0');

        // Multiply out[0..31] by 10 and add digit (big-endian)
        uint16_t carry = digit;
        for (int i = 31; i >= 0; --i) {
            uint16_t val = static_cast<uint16_t>(out[i]) * 10 + carry;
            out[i] = static_cast<uint8_t>(val & 0xFF);
            carry = val >> 8;
        }
        if (carry != 0) return false;  // overflow
    }
    return true;
}

// ---------------------------------------------------------------------------
// Convert uint64 to uint256 big-endian (for amounts that fit in 64 bits)
// ---------------------------------------------------------------------------
inline void uint64_to_uint256_be(uint64_t val, uint8_t out[32]) {
    std::memset(out, 0, 32);
    for (int i = 0; i < 8; ++i) {
        out[31 - i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }
}

// ---------------------------------------------------------------------------
// Pad a 20-byte address to 32 bytes (left-padded with zeros, EIP-712 style)
// ---------------------------------------------------------------------------
inline Bytes32 address_to_bytes32(const uint8_t addr[20]) {
    Bytes32 out{};
    std::memcpy(out.data() + 12, addr, 20);
    return out;
}

// ---------------------------------------------------------------------------
// EIP-55 mixed-case checksum encoding for Ethereum addresses
// Returns "0x"-prefixed 42-char checksummed address string
// ---------------------------------------------------------------------------
std::string eip55_checksum(const uint8_t address[20]);

}  // namespace lt
