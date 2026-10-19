#ifndef PUMP_VALVE_STATUS_H
#define PUMP_VALVE_STATUS_H
#include "protocol.h"
namespace pump {
// Remote valve telemetry; field layout is also part of NVS format 1.
struct ValveStatus {
  JobKey key;
  uint32_t pulses = 0, closePulses = 0, k = 5880;
  Phase phase = Phase::Idle;
  Reason reason = Reason::None;
  bool valveCommand = false, quiet = false, truncated = false;
  uint32_t revision = 0;
};
}

#endif
