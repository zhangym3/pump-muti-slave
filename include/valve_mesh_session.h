#pragma once
#include "flow_job_controller.h"
// Pure wire/session adapter, used by the firmware and the link simulator.
// It owns only bounded retry/phase state. It never writes persistent structs.
namespace pump {
class ValveMeshSession {
 public:
  explicit ValveMeshSession(FlowJobController &valve):valve_(valve) {}
  template<class Send,class Retire>
  bool handle(const Packet &p,uint32_t now,bool healthy,bool storage,Send send,Retire retire) {
    if (!controlFormat(p)) return false;
    Reader r{p};
    if (p.op==Op::Prepare) {
      Dose d; d.key=r.key(); d.ml=r.u32(); d.pulses=r.u32(); d.k=r.u32();
      d.version=r.u32(); d.maxMs=r.u32(); d.speedFactorPermille=r.u16(); d.leaseMs=r.u32();
      const auto flags=r.u8(); d.leaseFloor=r.u32();
      if (!r.done() || (flags & ~(PrepareCalibrated | PrepareLiveTelemetry | PrepareManualCalibration))) return false;
      d.calibrated=(flags & PrepareCalibrated)!=0;
      d.liveTelemetry=(flags & PrepareLiveTelemetry)!=0;
      d.manualCalibration=(flags & PrepareManualCalibration)!=0;
      const auto result=!healthy ? Reply::NotReady : !storage ? Reply::Storage : valve_.prepare(d,now);
      if (result==Reply::Ok) reset(d.key);
      if (result!=Reply::Ok || !preparePassed_) command(p.peer,d.key,Op::Prepare,result,send);
    } else if (p.op==Op::Start) {
      const auto key=r.key(); const auto result=healthy ? valve_.start(key,now) : Reply::NotReady;
      if (result==Reply::Ok) {
        reset(key); preparePassed_=true; retire(p.peer,key,Op::PrepareOK);
      }
      if (result!=Reply::Ok || !startPassed_) command(p.peer,key,Op::Start,result,send);
    } else if (p.op==Op::Lease) {
      const auto epoch=r.u32(), seq=r.u32(); if (!epoch || !seq) return false;
      const auto current=valve_.status();
      if (current.key.epoch==epoch && !current.valveCommand && current.reason==Reason::Target &&
          (current.phase==Phase::Settling || current.phase==Phase::Done)) {
        closed(p.peer,current.key,send); return true;
      }
      const auto result=healthy ? valve_.compactLease(epoch,seq,now) : Reply::NotReady;
      const auto key=valve_.status().key;
      if (result==Reply::Ok) { reset(key); startPassed_=true; retire(p.peer,key,Op::StartOK); }
      auto ack=packet(Op::LeaseAck,p.peer); ack.context=key;
      Writer w{ack}; w.u32(epoch); w.u32(seq); if (result!=Reply::Ok) w.u8(uint8_t(result)); send(ack);
    } else if (p.op==Op::TargetClosedAck) {
      const auto key=r.key();
      if (key==valve_.status().key) { reset(key); closedAck_=true; retire(p.peer,key,Op::TargetClosed); }
    } else if (p.op==Op::MeasureMark) {
      const auto key=r.key(); if (!r.done()) return false;
      uint32_t pulses=0;
      const auto result=valve_.markMeasurement(key,pulses);
      auto ack=packet(Op::MeasureMarkAck,p.peer); Writer w{ack}; w.key(key); w.u32(pulses); w.u8(uint8_t(result)); send(ack);
    } else return false;
    return true;
  }
  template<class Retire> void resultAck(uint16_t peer,JobKey key,Retire retire) {
    if (key==valve_.status().key) { reset(key); closedAck_=true; retire(peer,key,Op::TargetClosed); }
  }
  template<class Send> void poll(uint16_t peer,unsigned slot,uint32_t now,Send send) {
    const auto s=valve_.status(); if (!s.key.valid()) return;
    reset(s.key);
    if (unicast(peer) && valve_.liveTelemetry() && s.valveCommand &&
        (s.phase==Phase::Opening || s.phase==Phase::Running)) {
      const uint32_t runtime=valve_.runtimeMs(now);
      // Stagger the first sample; retries wait for the next sample, even on congestion.
      if ((!liveAttempted_ && runtime>=LiveTelemetryMs+(slot%5)*300) ||
          (liveAttempted_ && elapsed(now,liveAt_,LiveTelemetryMs))) {
        auto p=packet(Op::Progress,peer); Writer w{p}; w.key(s.key); w.u32(s.pulses); w.u32(runtime);
        liveAttempted_=true; liveAt_=now; send(p);
      }
    }
    if (s.valveCommand || s.reason!=Reason::Target || closedAck_ || closedAttempts_>=6) return;
    if (!closedAttempts_ || elapsed(now,closedAt_,250+(slot%5)*10)) {
      if (closed(peer,s.key,send)) { closedAt_=now; ++closedAttempts_; }
    }
  }
 private:
  void reset(JobKey key) {
    if (key_==key) return;
    key_=key; preparePassed_=startPassed_=closedAck_=false; closedAt_=closedAttempts_=0;
    liveAttempted_=false; liveAt_=0;
  }
  template<class Send> static bool closed(uint16_t peer,JobKey key,Send send) {
    auto p=packet(Op::TargetClosed,peer); Writer w{p}; w.key(key); return send(p);
  }
  template<class Send> static void command(uint16_t peer,JobKey key,Op op,Reply reply,Send send) {
    auto p=packet(reply==Reply::Ok ? (op==Op::Prepare ? Op::PrepareOK : Op::StartOK) : Op::CommandAck,peer);
    Writer w{p}; w.key(key); if (reply!=Reply::Ok) { w.u8(uint8_t(op)); w.u8(uint8_t(reply)); } send(p);
  }
  FlowJobController &valve_; JobKey key_;
  bool preparePassed_=false,startPassed_=false,closedAck_=false;
  uint32_t closedAt_=0; unsigned closedAttempts_=0;
  bool liveAttempted_=false; uint32_t liveAt_=0;
};
}
