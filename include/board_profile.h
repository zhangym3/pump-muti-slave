#pragma once
#include "flow_job_controller.h"

#include "sdkconfig.h"
#if !defined(CONFIG_IDF_TARGET_ESP32C3)
#error This project builds only the ESP32-C3 valve_slave_c3. Use pump-muti for the ESP32-C3 pump master.
#endif
namespace pump::board {
constexpr uint32_t ProfileId = 0x56530002; // Preserve the ESP32-C3 slave hardware identity.
// ESP32-C3 Super Mini: use printed GPIO numbers, not header position numbers.
constexpr int Output = 4; // Active-HIGH valve driver input; external pull-down.
constexpr int BranchFlow = 5, SlaveInterlock = 6;
constexpr int HoldButton = 3; // Normally-open momentary switch to GND; internal pull-up.
constexpr uint32_t HoldButtonDebounceMs = 30;
// No external safety switch is fitted on the current three-wire flow-sensor installation.
constexpr bool SlaveInterlockEnabled = false;
// When enabled: healthy HIGH at 3.3 V; fit an external pull-down for broken-wire detection.
constexpr uint32_t HallMinEdgeUs = 100;
constexpr uint8_t MaxConcurrent = 1; // Raise only after hydraulic validation.
constexpr ValveConfig valveConfig() {
  ValveConfig c;
  c.commissioned = false; // Set true only after verifying valve wiring and all limits.
  return c;
}
static_assert(MaxConcurrent >= 1 && MaxConcurrent <= MaxNodes);
static_assert(Output != BranchFlow && Output != SlaveInterlock);
static_assert(HoldButton != Output && HoldButton != BranchFlow && HoldButton != SlaveInterlock);
}
