#pragma once

#include <cstdint>
#include "events/scheduler_events.h"

namespace lt {

// Result of ExecSink::accept()
enum class SinkResult : uint8_t {
    ACCEPTED = 0,
    OVERFLOW,
    NO_QUEUE,
    FILTERED,   // blocked by mode policy (not forwarded, should not be tracked)
};

// Abstract interface for execution intent sinks.
// Allows T2 scheduler to send intents to either the stub sink (M2)
// or the real execution queue (M4) polymorphically.
class ExecSink {
public:
    virtual ~ExecSink() = default;
    virtual SinkResult accept(const ExecutionIntent& intent) = 0;
};

}  // namespace lt
