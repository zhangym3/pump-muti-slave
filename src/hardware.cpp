#include "hardware.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"

namespace pump {
uint32_t nowMs() { return uint32_t(uint64_t(esp_timer_get_time()) / 1000); }
static void IRAM_ATTR edge(void *arg) {
  auto *c = static_cast<PulseCounter *>(arg);
  const uint64_t us = uint64_t(esp_timer_get_time());
  const uint32_t low = uint32_t(us);
  portENTER_CRITICAL_ISR(&c->mux);
  if (uint32_t(low - c->lastUs) >= board::HallMinEdgeUs) {
    ++c->count; c->lastUs = low; c->lastMs = uint32_t(us / 1000);
  }
  portEXIT_CRITICAL_ISR(&c->mux);
}
static FlowReading read(PulseCounter &c) {
  portENTER_CRITICAL(&c.mux); const FlowReading result{c.count, c.lastMs};
  portEXIT_CRITICAL(&c.mux); return result;
}
void Hardware::safeOff() {
  gpio_set_level(gpio_num_t(board::Output), 0);
  gpio_set_direction(gpio_num_t(board::Output), GPIO_MODE_OUTPUT);
}
static bool flowPin(int pin, PulseCounter *counter) {
  gpio_config_t cfg{}; cfg.pin_bit_mask = 1ULL << pin; cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE; cfg.intr_type = GPIO_INTR_NEGEDGE;
  return gpio_config(&cfg) == ESP_OK && gpio_isr_handler_add(gpio_num_t(pin), edge, counter) == ESP_OK;
}
bool Hardware::init() {
  safeOff();
  if (gpio_install_isr_service(ESP_INTR_FLAG_IRAM) != ESP_OK) return false;
  if constexpr (board::SlaveInterlockEnabled) {
    gpio_config_t interlock{}; interlock.mode = GPIO_MODE_INPUT;
    interlock.pin_bit_mask = 1ULL << board::SlaveInterlock;
    if (gpio_config(&interlock) != ESP_OK) return false;
  }
  return flowPin(board::BranchFlow, &primary_);
}
FlowReading Hardware::flow() { return read(primary_); }
bool Hardware::interlock() {
  if constexpr (!board::SlaveInterlockEnabled) return true;
  return gpio_get_level(gpio_num_t(board::SlaveInterlock)) == 1;
}
void Hardware::valve(bool on) { gpio_set_level(gpio_num_t(board::Output), on ? 1 : 0); }
}
