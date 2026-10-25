#pragma once
#include "protocol.h"
#include <errno.h>

namespace pump {
struct MeshSubmission { Packet packet; uint32_t issuedAt = 0; int error = 0; bool committed = false; };
// Pure bounded state machine. Caller serializes access; SDK calls occur outside it.
class MeshTxGovernor {
 public:
  static constexpr size_t Pending = 64, Reserved = 16, Tickets = 8;
  struct Dispatch { Packet packet; uint32_t ticket = 0, issuedAt = 0; };
  struct Metrics {
    uint32_t merged = 0, expired = 0, submitted = 0, ended = 0, faults = 0;
    uint32_t maxSarMs = 0, traceLost = 0, helloTimeouts = 0;
    uint8_t peakSar = 0, peakPending = 0;
  };
  enum class FaultReason : uint8_t { None, Rejected, SarEnd, QueueFull, QueueExpired,
    MissingCompletion, TicketExhausted, SequenceExhausted };
  struct Fault {
    FaultReason reason = FaultReason::None;
    uint32_t at = 0, ticket = 0, issuedAt = 0, queuedAt = 0;
    Packet packet;
    int error = 0;
    uint8_t inFlight = 0, pending = 0;
  };
  struct Diagnostics {
    Fault first, last;
    uint32_t backoffMs = 0;
    uint8_t inFlight = 0, pending = 0;
    bool diagnosticReady = false, restartRequired = false;
  };
  struct Trace { uint32_t at, ticket, issuedAt, queuedAt; JobKey job; uint32_t sequence, revision; int error; uint16_t peer; uint8_t op, event; bool segmented; };
  explicit MeshTxGovernor(uint8_t sarLimit = 2) : sarLimit_(sarLimit) {}
  bool enqueue(const Packet &p, uint32_t now) {
    if (!p.peer || p.size > sizeof(p.bytes)) return false;
    if (p.op == Op::Progress && (!unicast(p.peer) || p.size != 16 || !key(p).valid())) return false;
    if (p.op == Op::Lease && (!p.context.valid() || p.size != 8)) return false;
    if (scope_.valid() && authorizes(p) && key(p) != scope_) return false;
    // Only identical successful command replies already owned by the SDK merge.
    if (p.op == Op::PrepareOK || p.op == Op::StartOK)
      for (const auto &v:slots_) if (v.used && sameIntent(v.p,p)) { ++metrics_.merged; return true; }
    if (p.op == Op::CommandAck && p.size == 10) {
      const Op ok = p.bytes[8] == uint8_t(Op::Prepare) ? Op::PrepareOK : Op::StartOK;
      retire(p.peer,key(p),ok);
    }
    // A fault never reopens the business path. Only freshly constructed,
    // non-authorizing diagnostics may resume after all old tickets settle.
    if (frozen_ && (!diagnosticReady(now) || !diagnostic(p))) return false;
    if (authorizes(p) && key(p) == fenced_ && fenced_.valid()) return false;
    size_t free = Pending, used = 0;
    for (size_t i = 0; i < Pending; ++i) {
      if (!pending_[i].used) { if (free == Pending) free = i; continue; }
      ++used;
      auto &old = pending_[i];
      if (p.op == Op::Progress && old.p.op == Op::Progress && p.peer == old.p.peer && key(p) == key(old.p)) {
        Reader previous{old.p}, incoming{p}; previous.at = incoming.at = 12;
        const uint32_t oldRuntime = previous.u32(), newRuntime = incoming.u32();
        if (serialNewer(newRuntime, oldRuntime)) old.p = p;
        ++metrics_.merged;
        return true;
      }
      if (p.op == Op::Result && old.p.op == Op::Result && p.peer == old.p.peer &&
          p.size == 27 && old.p.size == 27 && key(p) == key(old.p)) {
        Reader a{old.p}, b{p}; a.at = b.at = 20;
        const auto previous=a.u32(), incoming=b.u32();
        if (serialNewer(previous,incoming)) { ++metrics_.merged; return true; }
        if (serialNewer(incoming,previous)) { old.p=p; ++metrics_.merged; return true; }
      }
      if (sameIntent(old.p, p)) {
        // Repeated enqueue never slides a critical deadline.
        if (p.op != Op::LeaseAck || p.size != 8 || leaseSequence(p) > leaseSequence(old.p)) old.p = p;
        ++metrics_.merged; return true;
      }
    }
    if (free == Pending || (!critical(p) && used >= Pending - Reserved)) {
      if (critical(p)) fault(FaultReason::QueueFull,p,now,0,0,now);
      return false;
    }
    pending_[free] = {p, now, ++order_, true};
    if (used + 1 > metrics_.peakPending) metrics_.peakPending = uint8_t(used + 1);
    return true;
  }
  // Called under the same lock as next(): no old authorization can commit
  // after the floor is captured; SDK-owned tickets remain real and tracked.
  bool beginLeaseScope(JobKey job, uint32_t &floor) {
    if (!job.valid() || frozen_ || leaseSeq_ == UINT32_MAX) return false;
    for (auto &v:pending_) if (v.used && authorizes(v.p)) v.used=false;
    scope_=job; floor=leaseSeq_+1; return true;
  }
  enum class Admission { Accepted, Busy, Failed };
  Admission prepare(const Packet &p, uint32_t now) {
    if (!healthy()) return Admission::Failed;
    for (const auto &v:pending_) if (v.used && v.p.op == Op::Prepare) return Admission::Busy;
    return enqueue(p,now) ? Admission::Accepted : Admission::Failed;
  }
  void retire(uint16_t peer, JobKey job, Op op) {
    for (auto &v:pending_) if (v.used && v.p.peer==peer && key(v.p)==job && v.p.op==op) v.used=false;
  }
  void fence(JobKey job) {
    fenced_ = job;
    for (auto &v : pending_) if (v.used && authorizes(v.p) && key(v.p) == job) v.used = false;
  }
  bool next(uint32_t now, Dispatch &out) {
    unsigned sar = 0, ordinary = 0, btc = 0; size_t free = Tickets;
    for (size_t i = 0; i < Tickets; ++i) {
      auto &s = slots_[i];
      if (!s.used) { if (free == Tickets) free = i; continue; }
      const bool missing = !s.stack || (!s.error && s.segmented && !s.end);
      if (missing && elapsed(now, s.issuedAt, 6000) && !s.timeoutReported) {
        s.timeoutReported = true;
        fault(FaultReason::MissingCompletion,s.p,now,s.id,s.issuedAt,s.queuedAt);
      } // Never fake-release, even if a different fault already froze TX.
      if (s.segmented) { ++sar; if (priority(s.p) > 1) ++ordinary; }
      if (!s.stack) ++btc;
    }
    if (frozen_ && !diagnosticReady(now)) return false;
    if (free == Tickets || btc >= 4) return false;
    size_t best = Pending;
    for (size_t i = 0; i < Pending; ++i) {
      auto &v = pending_[i]; if (!v.used) continue;
      if (elapsed(now, v.queuedAt, ttl(v.p))) {
        v.used = false; ++metrics_.expired;
        if (!frozen_ && (authorizes(v.p) || v.p.op==Op::MaintenanceGrant || v.p.op == Op::LeaseAck)) {
          fault(FaultReason::QueueExpired,v.p,now,0,0,v.queuedAt); return false;
        }
        continue;
      }
      if (frozen_ && !diagnostic(v.p)) { v.used = false; continue; }
      bool blocked = false;
      if (segmented(v.p)) {
        blocked = sar >= sarLimit_ || (priority(v.p) > 1 && ordinary >= 1);
        for (const auto &s : slots_) if (s.used && s.segmented && s.p.peer == v.p.peer) blocked = true;
      }
      if (blocked) continue;
      if (best == Pending || dispatchPriority(v.p) < dispatchPriority(pending_[best].p) ||
          (dispatchPriority(v.p) == dispatchPriority(pending_[best].p) &&
           (uint32_t(now-v.queuedAt) > uint32_t(now-pending_[best].queuedAt) ||
            (v.queuedAt == pending_[best].queuedAt && serialNewer(pending_[best].order, v.order))))) best = i;
    }
    if (best == Pending) return false;
    auto &v = pending_[best];
    if (ticket_ == UINT32_MAX) { fault(FaultReason::TicketExhausted,v.p,now,0,0,v.queuedAt); return false; }
    if (v.p.op == Op::Lease && leaseSeq_ == UINT32_MAX) {
      fault(FaultReason::SequenceExhausted,v.p,now,0,0,v.queuedAt); return false;
    }
    auto &s = slots_[free]; s = {};
    s.p = v.p; s.issuedAt = now; s.queuedAt = v.queuedAt; s.id = ++ticket_; s.used = true; s.segmented = segmented(v.p);
    if (s.p.op == Op::Lease) {
      const uint32_t seq = ++leaseSeq_;
      for (unsigned i=0; i<4; ++i) s.p.bytes[4+i] = uint8_t(seq >> (8*i));
    }
    v.used = false; ++metrics_.submitted;
    if (s.segmented && sar + 1 > metrics_.peakSar) metrics_.peakSar = uint8_t(sar + 1);
    trace(s,0,0,now);
    out = {s.p, s.id, now}; return true;
  }
  // Event 1=core accepted, 2=core/API rejected, 3=SAR end, 4=SAR timeout.
  // Fixed per-ticket bits tolerate end-before-accepted and duplicate/late events.
  void event(uint32_t ticket, uint8_t type, int error, uint32_t now = 0) {
    for (auto &s : slots_) if (s.used && s.id == ticket) {
      bool newEnd = false;
      if (type == 1 || type == 2) {
        if (!s.stack) { s.stack = true; s.error = type == 2 ? (error ? error : -1) : error; }
      } else if ((type == 3 || type == 4) && !s.end) {
        s.end = true; s.endType = type; s.endError = error; s.endAt = now; newEnd = true;
      }
      trace(s,type,error,now);
      const bool helloTimeout = s.p.op == Op::Hello && s.endType == 4 && s.endError == -ETIMEDOUT;
      if (newEnd && helloTimeout) ++metrics_.helloTimeouts;
      const bool endFailure = s.end && (s.endError || s.endType == 4) && !helloTimeout;
      // A lost progress snapshot is replaceable. Missing SDK callbacks still fault in next().
      if (s.p.op != Op::Progress && !s.faultReported && ((s.stack && s.error) || endFailure)) {
        s.faultReported = true;
        fault(s.stack && s.error ? FaultReason::Rejected : FaultReason::SarEnd,
          s.p,now,s.id,s.issuedAt,s.queuedAt,s.stack && s.error ? s.error : s.endError);
      }
      return;
    }
  }
  bool collect(uint32_t now, MeshSubmission &out) {
    bool found = false;
    for (auto &s : slots_) if (s.used) {
      if (!s.commitReported && !found) {
        out = {s.p,s.issuedAt,0,true}; s.commitReported = true; found = true;
      } else if (s.stack && !s.reported && !found) {
        out = {s.p, s.issuedAt, s.error}; s.reported = true; found = true;
      }
      if (s.reported && (s.error || !s.segmented || s.end)) {
        if (s.segmented && s.end) {
          ++metrics_.ended; const auto age = uint32_t((s.endAt ? s.endAt : now) - s.issuedAt);
          if (age > metrics_.maxSarMs) metrics_.maxSarMs = age;
        }
        s.used = false;
      }
    }
    return found;
  }
  bool healthy() const { return !frozen_; }
  Metrics metrics() const { return metrics_; }
  Diagnostics diagnostics(uint32_t now) const {
    return {firstFault_,lastFault_,backoffMs_,uint8_t(inFlight()),uint8_t(queued()),
      diagnosticReady(now),restartRequired_};
  }
  static const char *faultName(FaultReason reason) {
    switch (reason) {
      case FaultReason::None: return "NONE";
      case FaultReason::Rejected: return "REJECTED";
      case FaultReason::SarEnd: return "SAR_END";
      case FaultReason::QueueFull: return "QUEUE_FULL";
      case FaultReason::QueueExpired: return "QUEUE_EXPIRED";
      case FaultReason::MissingCompletion: return "MISSING_COMPLETION";
      case FaultReason::TicketExhausted: return "TICKET_EXHAUSTED";
      case FaultReason::SequenceExhausted: return "SEQUENCE_EXHAUSTED";
    }
    return "UNKNOWN";
  }
  bool takeTrace(Trace &out) {
    if (!traceCount_) return false;
    out=traces_[traceRead_++ % 64]; --traceCount_; return true;
  }
  size_t inFlight() const { size_t n=0; for (const auto &s : slots_) n += s.used; return n; }
  size_t queued() const { size_t n=0; for (const auto &s : pending_) n += s.used; return n; }
  static bool segmented(const Packet &p) { return p.size > 8; }
  static JobKey key(const Packet &p) {
    if (p.op == Op::Lease || p.op == Op::LeaseAck) return p.context;
    Reader r{p}; return r.key();
  }
 private:
#ifdef PUMP_HOST_TEST
  friend struct GovernorTestAccess;
#endif
  bool diagnosticReady(uint32_t now) const {
    return frozen_ && !restartRequired_ && !inFlight() && elapsed(now,lastFault_.at,backoffMs_);
  }
  // Explicit wire allowlist: never send an open-state snapshot or an OK
  // authorization reply after a transport fault. No queued business replay.
  static bool diagnostic(const Packet &p) {
    switch (p.op) {
      case Op::ModeGet: case Op::MaintenanceRetire: return controlFormat(p);
      case Op::NodeState: return controlFormat(p) && !(p.bytes[12] & 3);
      case Op::Hello: return p.size == 13;
      case Op::HelloReply: return p.size == 6;
      case Op::CapabilitiesGet: return p.size == 4;
      case Op::Capabilities: return p.size == 12;
      case Op::ResultQuery: return p.size == 8;
      case Op::Close: return p.size == 9;
      case Op::ResultAck: return p.size == 12;
      case Op::Result: return p.size == 27 && !(p.bytes[26] & 1) &&
        (p.bytes[24] == uint8_t(Phase::Done) || p.bytes[24] == uint8_t(Phase::Fault));
      case Op::CommandAck: return p.size == 10 && p.bytes[9] > uint8_t(Reply::Ok) && p.bytes[9] <= uint8_t(Reply::Unknown);
      case Op::LeaseAck: return p.size == 9 && p.bytes[8] > uint8_t(Reply::Ok) && p.bytes[8] <= uint8_t(Reply::Unknown);
      case Op::TargetClosed: case Op::TargetClosedAck: return p.size == 8;
      case Op::CommissionReply: return p.size == 14 && p.bytes[4] > uint8_t(Reply::Ok) && p.bytes[4] <= uint8_t(Reply::Unknown);
      default: return false;
    }
  }
  unsigned dispatchPriority(const Packet &p) const {
    if (frozen_ && (p.op == Op::Hello || p.op == Op::HelloReply)) return 0;
    return priority(p);
  }
  static bool authorizes(const Packet &p) { return p.op == Op::Prepare || p.op == Op::Start || p.op == Op::Lease; }
  static bool critical(const Packet &p) {
      return authorizes(p) || p.op==Op::ModeGet || p.op==Op::MaintenanceGrant || p.op==Op::MaintenanceRetire || p.op==Op::NodeState || p.op == Op::Close || p.op == Op::LeaseAck ||
      p.op == Op::Result || p.op == Op::ResultAck || p.op == Op::CommandAck ||
      p.op == Op::TargetClosed || p.op == Op::TargetClosedAck || p.op == Op::PrepareOK || p.op == Op::StartOK;
  }
  static unsigned priority(const Packet &p) {
    if (p.op==Op::MaintenanceRetire || p.op == Op::Close || p.op == Op::TargetClosed || p.op == Op::TargetClosedAck) return 0;
    if (p.op == Op::Lease || p.op == Op::LeaseAck) return 1;
    if (p.op == Op::Result || p.op == Op::ResultAck || p.op==Op::ModeGet || p.op==Op::NodeState) return 1;
    if (p.op == Op::CommandAck || p.op == Op::PrepareOK || p.op == Op::StartOK) return 2;
    if (p.op == Op::Prepare || p.op == Op::Start) return 3;
    return 4;
  }
  static uint32_t ttl(const Packet &p) { return p.op==Op::MaintenanceGrant ? 1000 : critical(p) ? CriticalQueueTtlMs : p.op == Op::Progress ? 5000 : 2000; }
  static bool sameIntent(const Packet &a, const Packet &b) {
    if (a.peer != b.peer || a.op != b.op) return false;
    if (a.op == Op::Hello || a.op == Op::HelloReply) return true;
    if (a.op == Op::Result && a.size == 27 && b.size == 27 && key(a) == key(b)) {
      // Revision is the physical event, pulses are a replaceable snapshot.
      Reader ar{a}, br{b}; ar.at = br.at = 20;
      return ar.u32() == br.u32();
    }
    if (a.op == Op::Lease) return key(a) == key(b);
    if (a.op == Op::LeaseAck) {
      if (a.size == 8 && b.size == 8 && a.context.valid() && key(a) == key(b)) return true;
    }
    return a.size == b.size && !memcmp(a.bytes, b.bytes, a.size);
  }
  void fault(FaultReason reason, const Packet &p, uint32_t now, uint32_t ticket,
             uint32_t issuedAt, uint32_t queuedAt, int error = 0) {
    lastFault_ = {reason,now,ticket,issuedAt,queuedAt,p,error,uint8_t(inFlight()),uint8_t(queued())};
    if (!frozen_) firstFault_ = lastFault_;
    backoffMs_ = !frozen_ ? 1000 : backoffMs_ < 8000 ? backoffMs_ * 2 : 8000;
    ++metrics_.faults; frozen_ = true;
    if (reason == FaultReason::MissingCompletion || reason == FaultReason::TicketExhausted ||
        reason == FaultReason::SequenceExhausted) restartRequired_ = true;
    for (auto &v : pending_) v.used = false;
  }
  struct Intent { Packet p; uint32_t queuedAt = 0, order = 0; bool used = false; };
  struct Slot {
    Packet p; uint32_t issuedAt = 0, queuedAt = 0, id = 0, endAt = 0; int error = 0, endError = 0;
    uint8_t endType = 0;
    bool used = false, segmented = false, stack = false, end = false, reported = false, commitReported = false;
    bool faultReported = false, timeoutReported = false;
  };
  void trace(const Slot &s, uint8_t event, int error, uint32_t now) {
    if (traceCount_ == 64) { ++metrics_.traceLost; return; }
    Reader r{s.p}; const auto job=key(s.p);
    const uint32_t seq = s.p.op == Op::Lease || s.p.op == Op::LeaseAck ? leaseSequence(s.p) : 0;
    uint32_t revision=0;
    if (s.p.op==Op::Result || s.p.op==Op::ResultAck) {
      r.at=s.p.op==Op::Result ? 20 : 8; revision=r.u32();
    }
    traces_[traceWrite_++ % 64] = {now,s.id,s.issuedAt,s.queuedAt,job,seq,revision,error,s.p.peer,uint8_t(s.p.op),event,s.segmented};
    ++traceCount_;
  }
  Intent pending_[Pending]; Slot slots_[Tickets]; Metrics metrics_;
  Trace traces_[64]; uint32_t traceRead_ = 0, traceWrite_ = 0; uint8_t traceCount_ = 0;
  uint8_t sarLimit_; uint32_t ticket_ = 0, leaseSeq_ = 0, order_ = 0;
  JobKey fenced_, scope_; bool frozen_ = false, restartRequired_ = false;
  Fault firstFault_, lastFault_; uint32_t backoffMs_ = 0;
};
}
