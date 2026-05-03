#pragma once

#include <cstdint>
#include <cstring>

#include "common/types.h"
#include "slot/market_slot_types.h"

namespace lt {

// T2-owned flat array mapping token_id -> slot association.
// Max 12 entries: 6 slots x 2 tokens each.
// Updated only by T2 when processing slot lifecycle events from slot_queue.
// No synchronization needed (single-writer T2).
class SlotTokenMap {
public:
    static constexpr int kMaxEntries = 12;

    struct Entry {
        AssetId token_id;
        AssetId condition_id;
        SlotName slot = SlotName::CURRENT_5M;
        bool active = false;     // true for CURRENT slots (eligible for quoting)
        bool occupied = false;   // true if this entry is in use
    };

    // Find entry by token_id. Returns nullptr if not found.
    const Entry* find_by_token(const AssetId& token_id) const {
        for (int i = 0; i < count_; ++i) {
            if (entries_[i].occupied && entries_[i].token_id == token_id) {
                return &entries_[i];
            }
        }
        return nullptr;
    }

    // Find entry by condition_id. Returns pointer to first match (up token).
    const Entry* find_by_condition(const AssetId& condition_id) const {
        for (int i = 0; i < count_; ++i) {
            if (entries_[i].occupied && entries_[i].condition_id == condition_id) {
                return &entries_[i];
            }
        }
        return nullptr;
    }

    // Check if a token belongs to an active (quotable) slot
    bool is_active_token(const AssetId& token_id) const {
        const Entry* e = find_by_token(token_id);
        return e && e->active;
    }

    // Check if a condition belongs to an active slot
    bool is_active_condition(const AssetId& condition_id) const {
        const Entry* e = find_by_condition(condition_id);
        return e && e->active;
    }

    // Get the slot name for a token, or return false if not found
    bool slot_for_token(const AssetId& token_id, SlotName& out) const {
        const Entry* e = find_by_token(token_id);
        if (!e) return false;
        out = e->slot;
        return true;
    }

    // Add two tokens (up + down) for a slot
    void add(SlotName slot, const AssetId& condition_id,
             const AssetId& token_up, const AssetId& token_down, bool active) {
        if (count_ + 2 > kMaxEntries) return;

        auto& e1 = entries_[count_++];
        e1.token_id = token_up;
        e1.condition_id = condition_id;
        e1.slot = slot;
        e1.active = active;
        e1.occupied = true;

        auto& e2 = entries_[count_++];
        e2.token_id = token_down;
        e2.condition_id = condition_id;
        e2.slot = slot;
        e2.active = active;
        e2.occupied = true;
    }

    // Remove all entries for a given slot
    void remove(SlotName slot) {
        int write = 0;
        for (int i = 0; i < count_; ++i) {
            if (entries_[i].slot != slot) {
                if (write != i) entries_[write] = entries_[i];
                ++write;
            }
        }
        count_ = write;
    }

    // Set active flag for all entries belonging to a slot
    void set_active(SlotName slot, bool active) {
        for (int i = 0; i < count_; ++i) {
            if (entries_[i].slot == slot) {
                entries_[i].active = active;
            }
        }
    }

    // Promote: relabel entries from one slot to another (e.g., NEXT->CURRENT)
    void promote(SlotName from_slot, SlotName to_slot) {
        for (int i = 0; i < count_; ++i) {
            if (entries_[i].slot == from_slot) {
                entries_[i].slot = to_slot;
            }
        }
    }

    // Demote: relabel + deactivate (e.g., CURRENT->PREVIOUS)
    void demote(SlotName from_slot, SlotName to_slot) {
        for (int i = 0; i < count_; ++i) {
            if (entries_[i].slot == from_slot) {
                entries_[i].slot = to_slot;
                entries_[i].active = false;
            }
        }
    }

    int count() const { return count_; }
    const Entry& entry(int i) const { return entries_[i]; }

    void clear() { count_ = 0; }

private:
    Entry entries_[kMaxEntries]{};
    int count_ = 0;
};

}  // namespace lt
