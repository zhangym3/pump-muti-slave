#pragma once
#include "valve_status.h"
namespace pump {
inline bool terminalResult(const ValveStatus &s) {
  return s.key.valid() && s.revision && !s.valveCommand && validK(s.k) && s.reason <= Reason::Protocol &&
    s.closePulses <= s.pulses && (s.phase == Phase::Done || s.phase == Phase::Fault);
}
// Compare wire fields explicitly: NVS structs have compiler padding.
inline bool sameResult(const ValveStatus &a, const ValveStatus &b) {
  return a.key == b.key && a.revision == b.revision && a.pulses == b.pulses &&
    a.closePulses == b.closePulses && a.k == b.k && a.phase == b.phase &&
    a.reason == b.reason && a.valveCommand == b.valveCommand &&
    a.quiet == b.quiet && a.truncated == b.truncated;
}
inline bool sameMeasurement(ValveStatus a, ValveStatus b) {
  a.revision=b.revision; return sameResult(a,b);
}
// A new cancellation fence must not erase the only copy of an unsaved result.
template<class Controller>
void closePreservingResult(Controller &valve, JobKey key, Reason reason, uint32_t now, bool storageHealthy) {
  valve.stopLocalHold(reason,now);
  if (storageHealthy || key==valve.status().key) valve.close(key,reason,now);
  else valve.emergency(reason,now);
}
class ResultCommit {
 public:
  template<class Save> bool commit(const ValveStatus &s, Save save) {
    if (!healthy_) return false;
    if (s.phase!=Phase::Done && s.phase!=Phase::Fault) return healthy_;
    if (!terminalResult(s)) { healthy_=false; return false; }
    if (valid_ && sameResult(saved_, s)) return true;
    if (!save(s)) { healthy_ = false; return false; }
    saved_ = s; valid_ = true; return true;
  }
  bool contains(const ValveStatus &s) const { return valid_ && sameResult(saved_, s); }
  bool healthy() const { return healthy_; }
 private:
  ValveStatus saved_;
  bool valid_ = false, healthy_ = true;
};
}
