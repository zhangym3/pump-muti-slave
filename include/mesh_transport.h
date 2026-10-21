#pragma once
#include "protocol.h"
#include "mesh_tx_governor.h"
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
  bool takeReset() { return reset_.exchange(false); }
  void nodeReset() { reset_ = true; }
  void incoming(const Packet &p);
  void worker();
  bool takeSubmission(MeshSubmission &out);
  void fence(JobKey job);
  void retire(uint16_t peer, JobKey job, Op op);
  bool healthy() const;
  MeshTxGovernor::Metrics metrics() const;
  MeshTxGovernor::Diagnostics diagnostics() const;
  void trackedEvent(uint32_t ticket, uint8_t event, int error);
 private:
  char deviceName_[24] = {};
  QueueHandle_t rx_ = nullptr, urgentRx_ = nullptr;
  mutable portMUX_TYPE txLock_ = portMUX_INITIALIZER_UNLOCKED;
  MeshTxGovernor tx_{1};
  std::atomic<bool> overflow_{false};
  std::atomic<bool> reset_{false};
  std::atomic<uint16_t> master_{1};
};
}
