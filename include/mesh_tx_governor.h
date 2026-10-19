#pragma once
#include "protocol.h"

namespace pump {
struct MeshSubmission { Packet packet; uint32_t issuedAt = 0; int error = 0; bool committed = false; };
// Pure bounded state machine. Caller serializes access; SDK calls occur outside it.
class MeshTxGovernor {
 public:
  static constexpr size_t Pending = 64, Reserved = 16, Tickets = 8;
  struct Dispatch { Packet packet; uint32_t ticket = 0, issuedAt = 0; };
  struct Metrics {
    uint32_t merged = 0, expired = 0, submitted = 0, ended = 0, faults = 0;
    uint32_t maxSarMs = 0, traceLost = 0; uint8_t peakSar = 0, peakPending = 0;
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
    // A fault never reopens the business path. Only freshly constructed,
    // non-authorizing diagnostics may resume after all old tickets settle.
    if (frozen_ && (!diagnosticReady(now) || !diagnostic(p))) return false;
    if (authorizes(p) && key(p) == fenced_ && fenced_.valid()) return false;
    size_t free = Pending, used = 0;
    for (size_t i = 0; i < Pending; ++i) {
      if (!pending_[i].used) { if (free == Pending) free = i; continue; }
      ++used;
      auto &old = pending_[i];
      if (p.op == Op::Status && old.p.op == Op::Status && p.peer == old.p.peer &&
          p.size == 27 && old.p.size == 27 && key(p) == key(old.p)) {
        Reader a{old.p}, b{p}; a.at = b.at = 20;
        const auto previous=a.u32(), incoming=b.u32();
        if (serialNewer(previous,incoming)) { ++metrics_.merged; return true; }
        if (serialNewer(incoming,previous)) { old.p=p; ++metrics_.merged; return true; }
      }
      if (sameIntent(old.p, p)) {
        // Repeated enqueue never slides a critical deadline.
        if (p.op != Op::LeaseAck || p.bytes[12] == uint8_t(Reply::Ok)) old.p = p;
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
        if (!frozen_ && (authorizes(v.p) || v.p.op == Op::LeaseAck)) {
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
      for (unsigned i=0; i<4; ++i) s.p.bytes[8+i] = uint8_t(seq >> (8*i));
    }
    v.used = false; ++metrics_.submitted;
    if (s.segmented && sar + 1 > metrics_.peakSar) metrics_.peakSar = uint8_t(sar + 1);
    trace(s,0,0,now);
    out = {s.p, s.id, now}; return true;
  }
  // Event 1=core accepted, 2=core/API rejected, 3=true SAR end.
  // Fixed per-ticket bits tolerate end-before-accepted and duplicate/late events.
  void event(uint32_t ticket, uint8_t type, int error, uint32_t now = 0) {
    for (auto &s : slots_) if (s.used && s.id == ticket) {
      if (type == 1 || type == 2) {
        if (!s.stack) { s.stack = true; s.error = type == 2 ? (error ? error : -1) : error; }
      } else if (type == 3 && !s.end) { s.end = true; s.endError = error; s.endAt = now; }
      trace(s,type,error,now);
      if (!s.faultReported && ((s.stack && s.error) || (s.end && s.endError))) {
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
  static JobKey key(const Packet &p) { Reader r{p}; return r.key(); }
 private:
  bool diagnosticReady(uint32_t now) const {
    return frozen_ && !restartRequired_ && !inFlight() && elapsed(now,lastFault_.at,backoffMs_);
  }
  // Explicit wire allowlist: never send an open-state snapshot or an OK
  // authorization reply after a transport fault. No queued business replay.
  static bool diagnostic(const Packet &p) {
    switch (p.op) {
      case Op::Hello: return p.size == 9;
      case Op::HelloReply: return p.size == 6;
      case Op::CapabilitiesGet: return p.size == 4;
      case Op::Capabilities: return p.size == 12;
      case Op::StatusGet: return p.size == 8;
      case Op::Close: return p.size == 9;
      case Op::EventAck: return p.size == 12;
      case Op::Status: return p.size == 27 && !(p.bytes[26] & 1) &&
        (p.bytes[24] == uint8_t(Phase::Idle) ||
         (p.bytes[24] >= uint8_t(Phase::Settling) && p.bytes[24] <= uint8_t(Phase::Fault)));
      case Op::CommandReply: return p.size == 10 && p.bytes[9] > uint8_t(Reply::Ok) && p.bytes[9] <= uint8_t(Reply::Unknown);
      case Op::LeaseAck: return p.size == 13 && p.bytes[12] > uint8_t(Reply::Ok) && p.bytes[12] <= uint8_t(Reply::Unknown);
      case Op::CommissionReply: return p.size == 14 && p.bytes[4] > uint8_t(Reply::Ok) && p.bytes[4] <= uint8_t(Reply::Unknown);
      default: return false;
    }
  }
  unsigned dispatchPriority(const Packet &p) const {
    if (frozen_ && (p.op == Op::Hello || p.op == Op::HelloReply)) return 1;
    return priority(p);
  }
  static bool authorizes(const Packet &p) { return p.op == Op::Prepare || p.op == Op::Start || p.op == Op::Lease; }
  static bool critical(const Packet &p) {
    return authorizes(p) || p.op == Op::Close || p.op == Op::LeaseAck ||
      p.op == Op::Status || p.op == Op::EventAck || p.op == Op::CommandReply;
  }
  static unsigned priority(const Packet &p) {
    if (p.op == Op::Close) return 0;
    if (p.op == Op::Lease || p.op == Op::LeaseAck) return 1;
    if (p.op == Op::Status || p.op == Op::EventAck || p.op == Op::CommandReply) return 2;
    if (p.op == Op::Prepare || p.op == Op::Start) return 3;
    return 4;
  }
  static uint32_t ttl(const Packet &p) { return p.op == Op::Lease || p.op == Op::LeaseAck ? 800 : 2000; }
  static bool sameIntent(const Packet &a, const Packet &b) {
    if (a.peer != b.peer || a.op != b.op) return false;
    if (a.op == Op::Hello || a.op == Op::HelloReply) return true;
    if (a.op == Op::Summary) return key(a) == key(b);
    if (a.op == Op::Status && a.size == 27 && b.size == 27 && key(a) == key(b)) {
      // Revision is the physical event, pulses are a replaceable snapshot.
      Reader ar{a}, br{b}; ar.at = br.at = 20;
      return ar.u32() == br.u32();
    }
    if (a.op == Op::Lease) return key(a) == key(b);
    if (a.op == Op::LeaseAck) {
      if (a.size == 13 && b.size == 13 && key(a) == key(b) &&
          a.bytes[12] == uint8_t(Reply::Ok) && b.bytes[12] == uint8_t(Reply::Ok)) return true;
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
    bool used = false, segmented = false, stack = false, end = false, reported = false, commitReported = false;
    bool faultReported = false, timeoutReported = false;
  };
  void trace(const Slot &s, uint8_t event, int error, uint32_t now) {
    if (traceCount_ == 64) { ++metrics_.traceLost; return; }
    Reader r{s.p}; const auto job=r.key();
    const uint32_t seq = s.p.op == Op::Lease || s.p.op == Op::LeaseAck ? r.u32() : 0;
    uint32_t revision=0;
    if (s.p.op==Op::Status || s.p.op==Op::Summary || s.p.op==Op::EventAck) {
      r.at=s.p.op==Op::Status ? 20 : s.p.op==Op::Summary ? 12 : 8; revision=r.u32();
    }
    traces_[traceWrite_++ % 64] = {now,s.id,s.issuedAt,s.queuedAt,job,seq,revision,error,s.p.peer,uint8_t(s.p.op),event,s.segmented};
    ++traceCount_;
  }
  Intent pending_[Pending]; Slot slots_[Tickets]; Metrics metrics_;
  Trace traces_[64]; uint32_t traceRead_ = 0, traceWrite_ = 0; uint8_t traceCount_ = 0;
  uint8_t sarLimit_; uint32_t ticket_ = 0, leaseSeq_ = 0, order_ = 0;
  JobKey fenced_; bool frozen_ = false, restartRequired_ = false;
  Fault firstFault_, lastFault_; uint32_t backoffMs_ = 0;
};
}
