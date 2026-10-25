#include "flow_job_controller.h"
namespace pump {
bool FlowJobController::busy() const {
  return localActive_ || s_.phase == Phase::Prepared || s_.phase == Phase::Opening ||
    s_.phase == Phase::Running || s_.phase == Phase::Settling;
}
void FlowJobController::change(Phase p) { s_.phase = p; ++s_.revision; }
void FlowJobController::boot(JobKey persisted, uint32_t revisionBase) {
  io_.valve(false); fence_ = persisted; s_ = {}; s_.k = cfg_.k; storageFault_ = false;
  revisionBase_ = s_.revision = revisionBase;
  transportFault_ = false;
  localActive_ = localOn_ = false; localReason_ = Reason::None;
  localBaseline_ = localPulses_ = localOpenedAt_ = localClosedAt_ = localLastPulseAt_ = 0;
  if (persisted.valid()) { s_.key = persisted; s_.reason = Reason::Reboot; s_.truncated = true; change(Phase::Fault); }
}
void FlowJobController::restore(const ValveStatus &result) {
  if (result.key == fence_ && !result.valveCommand &&
      (result.phase == Phase::Done || result.phase == Phase::Fault)) {
    s_ = result; s_.revision = revisionBase_ + 1;
  }
}
Reply FlowJobController::configure(uint32_t k, uint32_t version) {
  const Reply validation = canConfigure(k, version);
  if (validation != Reply::Ok) return validation;
  cfg_.k = k; cfg_.version = version;
  if (!s_.key.valid()) s_.k = k; // Completed doses retain their original calibration.
  return Reply::Ok;
}
Reply FlowJobController::canConfigure(uint32_t k, uint32_t version) const {
  if (busy()) return Reply::Busy;
  if (!validK(k) || !version || version <= cfg_.version) return Reply::Invalid;
  return Reply::Ok;
}
Reply FlowJobController::canCommission() {
  if (transportFault_) return Reply::NotReady;
  if (busy() || output()) return Reply::Busy;
  if (!validK(cfg_.k) || !cfg_.version) return Reply::Invalid;
  if (!io_.interlock()) return Reply::NotReady;
  return Reply::Ok;
}
Reply FlowJobController::prepare(const Dose &d, uint32_t now) {
  if (transportFault_) return Reply::NotReady;
  if (localActive_) return Reply::Busy;
  if (!d.key.valid()) return Reply::Invalid;
  if (d.key == s_.key) {
    if (d.ml != dose_.ml || d.pulses != dose_.pulses || d.k != dose_.k ||
        d.version != dose_.version || d.maxMs != dose_.maxMs ||
        d.leaseFloor != dose_.leaseFloor || d.leaseMs != dose_.leaseMs || d.speedFactorPermille != dose_.speedFactorPermille ||
        d.calibrated != dose_.calibrated || d.liveTelemetry != dose_.liveTelemetry ||
        d.manualCalibration != dose_.manualCalibration) return Reply::Conflict;
    return s_.phase >= Phase::Prepared ? Reply::Ok : Reply::Stale;
  }
  if (!d.key.newer(fence_)) return Reply::Stale;
  if (busy()) return Reply::Busy;
  if (storageFault_) return Reply::Storage;
  if (!cfg_.commissioned || !io_.interlock()) return Reply::NotReady;
  uint32_t expected = 0;
  const bool doseValid = d.manualCalibration ?
      d.ml >= 50 && d.ml <= 2000 && d.pulses == 0 && d.maxMs <= 60000 :
      targetPulses(d.ml, d.k, expected) && d.pulses == expected && cfg_.preclose < d.pulses;
  if (!d.leaseFloor || !validSpeedFactor(d.speedFactorPermille) || !d.leaseMs || d.leaseMs > 60000 ||
      !doseValid || !validK(d.k) || (!d.calibrated && d.k != cfg_.k) ||
      d.version != cfg_.version || !d.maxMs || d.maxMs > cfg_.maxRunMs ||
      (d.manualCalibration && !d.liveTelemetry)) return Reply::Invalid;
  if (!io_.saveFence(d.key)) { storageFault_ = true; return Reply::Storage; }
  fence_ = d.key; dose_ = d; s_ = {}; s_.key = d.key; s_.k = d.k;
  measurementStart_ = 0; measurementMarked_ = false;
  s_.revision = revisionBase_;
  preparedAt_ = now; change(Phase::Prepared); return Reply::Ok;
}
Reply FlowJobController::start(JobKey key, uint32_t now) {
  if (transportFault_) return Reply::NotReady;
  if (localActive_) return Reply::Busy;
  if (key != s_.key) return Reply::Stale;
  if (s_.phase == Phase::Opening || s_.phase == Phase::Running) return Reply::Ok;
  if (s_.phase != Phase::Prepared) return Reply::Stale;
  if (elapsed(now, preparedAt_, cfg_.prepareMs) || !io_.interlock()) {
    shut(Reason::PrepareTimeout, now); return Reply::NotReady;
  }
  baseline_ = io_.flow().pulses; openedAt_ = lastPulseAt_ = leaseAt_ = now;
  leaseSeq_ = lastCount_ = 0; s_.valveCommand = true;
  // Counting starts before energising; residual-pressure flow belongs to this dose.
  io_.valve(true); change(Phase::Opening); return Reply::Ok;
}
Reply FlowJobController::markMeasurement(JobKey key, uint32_t &pulses) {
  if (!dose_.manualCalibration || key != s_.key || !s_.valveCommand ||
      (s_.phase != Phase::Opening && s_.phase != Phase::Running)) return Reply::NotReady;
  if (!measurementMarked_) {
    measurementStart_ = io_.flow().pulses - baseline_;
    measurementMarked_ = true;
  }
  pulses = measurementStart_;
  return Reply::Ok;
}
Reply FlowJobController::compactLease(uint32_t epoch, uint32_t seq, uint32_t now) {
  if (epoch != s_.key.epoch) return Reply::Stale;
  return lease(s_.key,seq,now);
}
Reply FlowJobController::lease(JobKey key, uint32_t seq, uint32_t now) {
  if (transportFault_) return Reply::NotReady;
  if (key != s_.key || !s_.valveCommand) return Reply::Stale;
  // A delayed renewal cannot resurrect an already expired lease.
  if (elapsed(now, leaseAt_, dose_.leaseMs)) { shut(Reason::Lease, now); return Reply::Stale; }
  // Retransmitted multicast renewals are acknowledged without extending the
  // lease a second time; this makes reply loss safe and deterministic.
  if (!seq || seq < dose_.leaseFloor) return Reply::Stale;
  if (seq == leaseSeq_) return Reply::Ok;
  if (seq < leaseSeq_) return Reply::Stale;
  leaseSeq_ = seq; leaseAt_ = now; return Reply::Ok;
}
void FlowJobController::shut(Reason reason, uint32_t now) {
  io_.valve(false); s_.valveCommand = false;
  if (s_.phase == Phase::Prepared) {
    s_.reason = reason; s_.quiet = true;
    change(reason == Reason::Manual ? Phase::Done : Phase::Fault); return;
  }
  if (s_.phase != Phase::Opening && s_.phase != Phase::Running) return;
  s_.pulses = io_.flow().pulses - baseline_; s_.closePulses = s_.pulses;
  s_.reason = reason; closedAt_ = lastPulseAt_ = now; lastCount_ = s_.pulses;
  change(Phase::Settling);
}
void FlowJobController::close(JobKey key, Reason reason, uint32_t now) {
  if (localActive_) { stopLocalHold(reason, now); return; }
  if (key == s_.key) { shut(reason, now); return; }
  // CLOSE can overtake PREPARE in a mesh. Persist a cancellation fence before
  // acknowledging it so delayed PREPARE/START cannot open this job after reboot.
  if (key.valid() && key.newer(fence_) && !busy()) {
    io_.valve(false);
    storageFault_ = !io_.saveFence(key) || storageFault_;
    fence_ = key; dose_ = {}; s_ = {}; s_.key = key; s_.k = cfg_.k;
    s_.revision = revisionBase_;
    s_.reason = reason; s_.quiet = true;
    change(reason == Reason::Manual && !storageFault_ ? Phase::Done : Phase::Fault);
  }
}
void FlowJobController::emergency(Reason r, uint32_t now) {
  if (r == Reason::Transport) transportFault_ = true;
  stopLocalHold(r, now);
  shut(r, now);
}
Reply FlowJobController::startLocalHold(uint32_t now) {
  if (transportFault_) return Reply::NotReady;
  if (busy()) return Reply::Busy;
  if (storageFault_) return Reply::Storage;
  if (!cfg_.commissioned || !io_.interlock()) return Reply::NotReady;
  if (!cfg_.localHoldMaxMs || !cfg_.maxRunMs) return Reply::Invalid;
  localBaseline_ = io_.flow().pulses; localPulses_ = 0;
  localOpenedAt_ = localLastPulseAt_ = now;
  localReason_ = Reason::None; localActive_ = localOn_ = true;
  io_.valve(true);
  return Reply::Ok;
}
void FlowJobController::stopLocalHold(Reason reason, uint32_t now) {
  if (!localOn_) return;
  io_.valve(false); localOn_ = false; localReason_ = reason;
  localPulses_ = io_.flow().pulses - localBaseline_;
  localClosedAt_ = localLastPulseAt_ = now;
}
void FlowJobController::tickLocalHold(uint32_t now) {
  const uint32_t pulses = io_.flow().pulses - localBaseline_;
  if (pulses != localPulses_) localLastPulseAt_ = now;
  localPulses_ = pulses;
  if (localOn_) {
    if (!cfg_.commissioned || !io_.interlock()) stopLocalHold(Reason::LocalInterlock, now);
    else if (elapsed(now, localOpenedAt_, cfg_.localHoldMaxMs) ||
             elapsed(now, localOpenedAt_, cfg_.maxRunMs)) stopLocalHold(Reason::Runtime, now);
    else if ((!pulses && elapsed(now, localOpenedAt_, cfg_.startWaitMs)) ||
             (pulses && elapsed(now, localLastPulseAt_, cfg_.noFlowMs))) stopLocalHold(Reason::NoFlow, now);
  } else if (elapsed(now, localClosedAt_, cfg_.quietMs) && elapsed(now, localLastPulseAt_, cfg_.quietMs)) {
    localActive_ = false;
  } else if (elapsed(now, localClosedAt_, cfg_.settleLimitMs)) {
    localReason_ = Reason::TailFlow; localActive_ = false;
  }
}
void FlowJobController::tick(uint32_t now) {
  if (localActive_) { tickLocalHold(now); return; }
  if (s_.phase == Phase::Prepared) {
    if (!io_.interlock()) shut(Reason::LocalInterlock, now);
    else if (elapsed(now, preparedAt_, cfg_.prepareMs)) shut(Reason::PrepareTimeout, now);
    return;
  }
  if (!s_.valveCommand && s_.phase != Phase::Settling) return;
  s_.pulses = io_.flow().pulses - baseline_;
  if (s_.pulses != lastCount_) { lastCount_ = s_.pulses; lastPulseAt_ = now; }
  if (s_.valveCommand) {
    // Safety checks precede target closure, and never call the radio or Flash.
    if (!io_.interlock()) shut(Reason::LocalInterlock, now);
    else if (elapsed(now, leaseAt_, dose_.leaseMs)) shut(Reason::Lease, now);
    else if (elapsed(now, openedAt_, dose_.maxMs)) shut(Reason::Runtime, now);
    else if (!dose_.manualCalibration && s_.pulses >= dose_.pulses - cfg_.preclose) shut(Reason::Target, now);
    else if (!s_.pulses && elapsed(now, openedAt_, cfg_.startWaitMs)) shut(Reason::NoFlow, now);
    else if (s_.pulses && elapsed(now, lastPulseAt_, cfg_.noFlowMs)) shut(Reason::NoFlow, now);
    else if (s_.pulses && s_.phase == Phase::Opening) change(Phase::Running);
  } else {
    if (elapsed(now, closedAt_, cfg_.quietMs) && elapsed(now, lastPulseAt_, cfg_.quietMs)) {
      s_.quiet = true; change(s_.reason == Reason::Target || s_.reason == Reason::Manual ? Phase::Done : Phase::Fault);
    } else if (elapsed(now, closedAt_, cfg_.settleLimitMs)) {
      s_.truncated = true; s_.reason = Reason::TailFlow; change(Phase::Fault);
    }
  }
}
}
