#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
namespace pump {
struct CommandLine { bool bluetooth = false; char text[160] = {}; };
class ControlGateway {
 public:
  bool init(uint32_t passkey, const char *name);
  bool receive(CommandLine &cmd);
  bool submit(const char *text, size_t length, bool bluetooth);
  void publish(const char *text);
  void ioWorker();
  bool overflow() { return overflow_.exchange(false); }
 private:
  QueueHandle_t commands_ = nullptr, output_ = nullptr;
  std::atomic<bool> overflow_{false};
};
}
