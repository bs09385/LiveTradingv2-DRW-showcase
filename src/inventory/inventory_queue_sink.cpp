#include "inventory/inventory_queue_sink.h"

#include "logger/metrics.h"

namespace lt {

bool InventoryQueueSink::try_request(const InventoryOpRequest& request) {
    if (!queue_.try_push(request)) {
        if (metrics_) metrics_->inc(MetricId::UI_COMMANDS_DROPPED);
        return false;
    }
    return true;
}

}  // namespace lt
