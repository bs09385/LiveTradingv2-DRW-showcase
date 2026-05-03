#include "inventory/rlp_encoder.h"

#include <cstring>

namespace lt {

// Encode the length prefix for a big-endian integer length value.
static void encode_length_bytes(std::vector<uint8_t>& out, size_t len) {
    uint8_t buf[8];
    int n = 0;
    size_t tmp = len;
    while (tmp > 0) {
        buf[7 - n] = static_cast<uint8_t>(tmp & 0xff);
        tmp >>= 8;
        ++n;
    }
    out.insert(out.end(), buf + 8 - n, buf + 8);
}

static int count_length_bytes(size_t len) {
    int n = 0;
    while (len > 0) {
        len >>= 8;
        ++n;
    }
    return n;
}

void rlp_encode_string(std::vector<uint8_t>& out, const uint8_t* data, size_t len) {
    if (len == 1 && data[0] <= 0x7f) {
        // Single byte in [0x00, 0x7f] range: encoded as itself
        out.push_back(data[0]);
    } else if (len <= 55) {
        out.push_back(static_cast<uint8_t>(0x80 + len));
        out.insert(out.end(), data, data + len);
    } else {
        int len_bytes = count_length_bytes(len);
        out.push_back(static_cast<uint8_t>(0xb7 + len_bytes));
        encode_length_bytes(out, len);
        out.insert(out.end(), data, data + len);
    }
}

void rlp_encode_uint64(std::vector<uint8_t>& out, uint64_t value) {
    if (value == 0) {
        // 0 is encoded as empty string
        out.push_back(0x80);
        return;
    }

    // Convert to big-endian, strip leading zeros
    uint8_t buf[8];
    for (int i = 7; i >= 0; --i) {
        buf[i] = static_cast<uint8_t>(value & 0xff);
        value >>= 8;
    }
    int start = 0;
    while (start < 8 && buf[start] == 0) ++start;

    rlp_encode_string(out, buf + start, static_cast<size_t>(8 - start));
}

void rlp_encode_uint256(std::vector<uint8_t>& out, const uint8_t value[32]) {
    // Strip leading zeros
    int start = 0;
    while (start < 32 && value[start] == 0) ++start;

    if (start == 32) {
        // All zeros = 0
        out.push_back(0x80);
        return;
    }

    rlp_encode_string(out, value + start, static_cast<size_t>(32 - start));
}

void rlp_wrap_list(std::vector<uint8_t>& out, const std::vector<uint8_t>& items) {
    size_t len = items.size();
    if (len <= 55) {
        out.push_back(static_cast<uint8_t>(0xc0 + len));
    } else {
        int len_bytes = count_length_bytes(len);
        out.push_back(static_cast<uint8_t>(0xf7 + len_bytes));
        encode_length_bytes(out, len);
    }
    out.insert(out.end(), items.begin(), items.end());
}

}  // namespace lt
