#pragma once
#include "protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
namespace pump {
class MeshTransport final {
 public:
  bool init(uint16_t master);
  bool send(const Packet &p);
  bool receive(Packet &p);
  bool ready() const;
  uint16_t address() const;
  const char *deviceName() const { return deviceName_; }
  void master(uint16_t m) { master_.store(m); }
  uint16_t master() const { return master_.load(); }
  bool takeOverflow() { return overflow_.exchange(false); }
  void incoming(const Packet &p);
  void worker();
 private:
  char deviceName_[24] = {};
  QueueHandle_t rx_ = nullptr, urgentRx_ = nullptr, tx_ = nullptr, urgentTx_ = nullptr;
  std::atomic<bool> overflow_{false};
  std::atomic<uint16_t> master_{1};
};
}
