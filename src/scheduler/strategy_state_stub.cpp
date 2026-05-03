#include "scheduler/strategy_state_stub.h"

namespace lt {

void StrategyStateStub::on_event(const SchedulerEvent& ev) {
    ++total_events_;

    auto src_idx = static_cast<int>(ev.source);
    if (src_idx >= 0 && src_idx < kEventSourceCount) {
        ++source_counts_[src_idx];
    }

    auto kind_idx = static_cast<int>(ev.kind);
    if (kind_idx >= 0 && kind_idx < kSchedulerEventKindCount) {
        ++kind_counts_[kind_idx];
    }

    // Update per-asset state if asset_id is present
    if (ev.asset_id.len > 0) {
        auto it = asset_states_.find(ev.asset_id);
        if (it == asset_states_.end()) {
            if (!strict_assets_) {
                auto [inserted_it, _] = asset_states_.try_emplace(ev.asset_id);
                it = inserted_it;
            }
        }
        if (it != asset_states_.end()) {
            auto& ast = it->second;
            ast.last_seen_ts = ev.recv_ts;
            ++ast.event_count;

            // Cache BBO from market events
            if (ev.source == EventSource::MARKET_WS) {
                ast.last_bbo = ev.bbo;
            }
        }
    }

    // Record trace entry if tracing is enabled
    if (trace_enabled_ && trace_len_ < kMaxTraceEntries) {
        trace_buf_[trace_len_].source = ev.source;
        trace_buf_[trace_len_].kind = ev.kind;
        trace_buf_[trace_len_].seq = ev.seq;
        ++trace_len_;
    }
}

const AssetSchedulerState* StrategyStateStub::get_asset_state(const AssetId& id) const {
    auto it = asset_states_.find(id);
    if (it == asset_states_.end()) return nullptr;
    return &it->second;
}

}  // namespace lt
