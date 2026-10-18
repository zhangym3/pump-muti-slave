#pragma once
#include "board_profile.h"
#include "storage.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
namespace pump {
struct PulseCounter {
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  volatile uint32_t count = 0, lastUs = 0, lastMs = 0;
};
class Hardware final : public ValveIO {
 public:
  explicit Hardware(Storage &store) : store_(store) {}
  bool init();
  FlowReading flow() override;
  bool interlock() override;
  bool saveFence(JobKey k) override { return store_.fence(k); }
  void valve(bool on) override;
  void safeOff();
 private:
  Storage &store_;
  PulseCounter primary_;
};
uint32_t nowMs();
}
