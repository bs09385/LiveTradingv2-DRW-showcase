#pragma once

#include <cstdint>
#include <cstring>

#include "common/types.h"

namespace lt {

// ---- Binary record file header (16 bytes) ----
// Written once at the start of each .bin file.
enum class RecordSource : uint8_t {
    RTDS = 0,          // Fixed-size RtdsRecord (64 bytes each)
    MARKET_RAW = 1,    // Variable-length raw JSON (24B header + payload)
    USER_RAW = 2,      // Variable-length raw JSON (24B header + payload)
    JOURNAL = 3,       // Fixed-size JournalRecord (256 bytes each)
};

struct RecordFileHeader {
    char magic[4] = {'L', 'T', 'R', 'B'};
    uint16_t version = 1;
    uint8_t source = 0;       // RecordSource enum value
    uint8_t reserved1 = 0;
    uint32_t record_size = 0; // Fixed record size (0 = variable-length raw JSON)
    uint32_t reserved2 = 0;
};
static_assert(sizeof(RecordFileHeader) == 16);

// ---- RTDS crypto price record (64 bytes, fixed-size) ----
struct alignas(8) RtdsRecord {
    int64_t recv_ts = 0;          // monotonic receive timestamp (ns)
    int64_t wall_clock_ms = 0;    // system time (ms since epoch)
    int64_t exchange_ts_ms = 0;   // exchange timestamp (ms since epoch)
    double value = 0.0;           // price value
    char symbol[24]{};            // crypto symbol (null-terminated)
    uint8_t symbol_len = 0;
    uint8_t pad_[7]{};
};
static_assert(sizeof(RtdsRecord) == 64);
static_assert(std::is_trivially_copyable_v<RtdsRecord>);

// ---- Raw WebSocket message for market/user recording ----
// Each entry holds one raw JSON payload from the WS connection.
// Variable-length on disk: [RawRecordHeader 24B] [payload_len bytes of JSON]

inline constexpr std::size_t kMaxRawWsPayload = 65536;  // 64KB max per message

// On-disk header for each variable-length record (24 bytes)
struct RawRecordHeader {
    int64_t recv_ts;          // monotonic ns
    int64_t wall_clock_ms;    // system time ms
    uint32_t payload_len;     // actual JSON length in bytes
    uint32_t flags;           // bit 0: truncated (payload exceeded kMaxRawWsPayload)
};
static_assert(sizeof(RawRecordHeader) == 24);

// Queue entry for SPSC transport (T0/T1 -> T_rec)
// Fixed-size so it works in rigtorp::SPSCQueue. Only payload_len bytes are valid.
struct RawWsMessage {
    int64_t recv_ts = 0;
    int64_t wall_clock_ms = 0;
    uint32_t payload_len = 0;
    uint32_t flags = 0;
    char payload[kMaxRawWsPayload]{};
};
static_assert(std::is_trivially_copyable_v<RawWsMessage>);

// Flag bits
inline constexpr uint32_t kRawFlagTruncated = 1;

}  // namespace lt
