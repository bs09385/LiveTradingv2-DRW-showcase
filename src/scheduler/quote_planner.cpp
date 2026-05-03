#include "scheduler/quote_planner.h"

namespace lt {

IntentBatch QuotePlanner::plan(const ExecutionIntent& intent) {
    IntentBatch out;
    // Engine policy: preserve strategy instructions exactly.
    // No side/asset/price conversion is performed here.
    out.add(intent);
    return out;
}

}  // namespace lt
