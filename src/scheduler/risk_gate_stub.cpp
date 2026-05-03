#include "scheduler/risk_gate_stub.h"

namespace lt {

RiskDecision RiskGateStub::evaluate(const ExecutionIntent& /*intent*/) {
    ++check_count_;
    ++allow_count_;
    return RiskDecision::ALLOW;
}

}  // namespace lt
