#pragma once

#include "inventory/inventory_sink.h"
#include "queue/spsc_queue.h"

namespace lt {

class Metrics;

// T2 producer -> inventory service consumer queue adapter.
class InventoryQueueSink final : public InventoryOpSink {
public:
    explicit InventoryQueueSink(SpscQueue<InventoryOpRequest>& queue, Metrics* metrics = nullptr)
        : queue_(queue), metrics_(metrics) {}

    bool try_request(const InventoryOpRequest& request) override;

private:
    SpscQueue<InventoryOpRequest>& queue_;
    Metrics* metrics_ = nullptr;
};

}  // namespace lt
