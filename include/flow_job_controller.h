#pragma once
#include "valve_status.h"
namespace pump {
struct FlowReading { uint32_t pulses = 0, lastMs = 0; };
struct ValveConfig {
  bool commissioned = false;
  uint32_t k = 5880, version = 1, preclose = 0;
  uint32_t startWaitMs = 5000, noFlowMs = 1500, prepareMs = 20000;
  uint32_t quietMs = 500, settleLimitMs = 3000, maxRunMs = 240000;
  uint32_t leaseMs = LeaseMs;
};
struct Dose {
  JobKey key;
  uint32_t ml = 0, pulses = 0, k = 0, version = 0, maxMs = 0, leaseMs = LeaseMs;
  uint32_t leaseFloor = 1;
  uint16_t speedFactorPermille = SpeedHighPermille;
};
class ValveIO {
 public:
  virtual ~ValveIO() = default;
  virtual FlowReading flow() = 0;
  virtual void valve(bool on) = 0;
  virtual bool interlock() = 0;
  // Called only with the valve OFF, before accepting PREPARE.
  virtual bool saveFence(JobKey key) = 0;
};
class FlowJobController {
 public:
  FlowJobController(ValveIO &io, ValveConfig cfg) : io_(io), cfg_(cfg) {}
  void boot(JobKey persisted, uint32_t revisionBase = 0);
  void restore(const ValveStatus &result);
  Reply prepare(const Dose &dose, uint32_t now);
  Reply start(JobKey key, uint32_t now);
  Reply compactLease(uint32_t epoch, uint32_t sequence, uint32_t now);
  Reply lease(JobKey key, uint32_t sequence, uint32_t now);
  void close(JobKey key, Reason reason, uint32_t now);
  void emergency(Reason reason, uint32_t now);
  void tick(uint32_t now);
  Reply configure(uint32_t k, uint32_t version);
  Reply canConfigure(uint32_t k, uint32_t version) const;
  Reply canCommission();
  void setCommissioned(bool value) { cfg_.commissioned = value; }
  bool busy() const;
  bool output() const { return s_.valveCommand; }
  bool transportFault() const { return transportFault_; }
  ValveStatus status() const { return s_; }
  const ValveConfig &config() const { return cfg_; }
 private:
  void change(Phase p);
  void shut(Reason reason, uint32_t now);
  ValveIO &io_;
  ValveConfig cfg_;
  ValveStatus s_;
  Dose dose_;
  JobKey fence_;
  bool storageFault_ = false, transportFault_ = false;
  uint32_t revisionBase_ = 0;
  uint32_t baseline_ = 0, preparedAt_ = 0, openedAt_ = 0, leaseAt_ = 0;
  uint32_t leaseSeq_ = 0, closedAt_ = 0, lastPulseAt_ = 0, lastCount_ = 0;
};
}
