#pragma once

#include <cstdint>

#include "common/types.h"

namespace lt {

struct BookDeltaChange {
    Price_t price = 0;
    Side side = Side::BID;
    Qty_t new_size = 0;
};

enum class BookDeltaKind : uint8_t {
    INCREMENTAL,
    SNAPSHOT_BEGIN,
    SNAPSHOT_CHUNK,
    SNAPSHOT_END,
};

inline constexpr int kMaxDeltaChanges = 64;

struct BookDelta {
    AssetId asset_id;
    BookDeltaKind kind = BookDeltaKind::INCREMENTAL;
    BookDeltaChange changes[kMaxDeltaChanges]{};
    uint16_t change_count = 0;
    TickSize_t tick_size = 0;  // non-zero for tick size changes
};

}  // namespace lt
