#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pump {
constexpr size_t MaxNodes = 32;
constexpr uint16_t CompanyId = 0x02E5; // Espressif: development only; obtain own CID for production.
constexpr uint16_t ModelId = 0x0001;
constexpr uint16_t AllSlaves = 0xC001;
constexpr uint32_t LeaseMs = 3000;
constexpr uint32_t TelemetryMs = 2000;
struct JobKey {
  uint32_t epoch = 0, id = 0;
  bool operator==(const JobKey &b) const { return epoch == b.epoch && id == b.id; }
  bool operator!=(const JobKey &b) const { return !(*this == b); }
  bool valid() const { return epoch && id; }
  bool newer(const JobKey &b) const { return epoch > b.epoch || (epoch == b.epoch && id > b.id); }
};
enum class Phase : uint8_t { Idle, Prepared, Opening, Running, Settling, Done, Fault };
enum class Reason : uint8_t { None, Target, Manual, Lease, Runtime, NoFlow, PrepareTimeout,
  TailFlow, LocalInterlock, Reboot, PumpFault, Transport, Protocol };
enum class Reply : uint8_t { Ok, Busy, Invalid, Stale, Conflict, NotReady, Storage, Unauthorized,
  Offline, Capacity, Pending, Unknown };
enum class Op : uint8_t { Prepare = 0xC0, Start, Lease, Close, StatusGet, Status,
  Summary, EventAck, Request, Response, Hello, HelloReply, Commission, CommissionReply,
  LeaseAck, CommandReply };
enum class Action : uint8_t { Start = 1, Stop, Query };

// All multibyte wire fields are explicitly little endian. Never memcpy C++ structs.
struct Packet {
  Op op = Op::StatusGet;
  uint16_t peer = 0;
  uint8_t size = 0;
  uint8_t bytes[64] = {};
};
struct Writer {
  Packet &p;
  void u8(uint8_t v) { if (p.size < sizeof(p.bytes)) p.bytes[p.size++] = v; }
  void u16(uint16_t v) { u8(v); u8(v >> 8); }
  void u32(uint32_t v) { u16(v); u16(v >> 16); }
  void u64(uint64_t v) { u32(v); u32(v >> 32); }
  void key(JobKey v) { u32(v.epoch); u32(v.id); }
};
struct Reader {
  const Packet &p;
  size_t at = 0;
  bool ok = true;
  uint8_t u8() { if (at >= p.size) { ok = false; return 0; } return p.bytes[at++]; }
  uint16_t u16() { uint16_t a = u8(), b = u8(); return a | (b << 8); }
  uint32_t u32() { uint32_t a = u16(), b = u16(); return a | (b << 16); }
  uint64_t u64() { uint64_t a = u32(), b = u32(); return a | (b << 32); }
  JobKey key() { JobKey k; k.epoch = u32(); k.id = u32(); return k; }
  bool done() const { return ok && at == p.size; }
};
inline Packet packet(Op op, uint16_t peer) { Packet p; p.op = op; p.peer = peer; return p; }
inline bool unicast(uint16_t a) { return a > 0 && a < 0x8000; }
inline bool validK(uint32_t k) { return k > 0 && k <= 1000000; }
inline bool targetPulses(uint32_t ml, uint32_t k, uint32_t &out) {
  if (!ml || ml > 1000000 || !validK(k)) return false;
  const uint64_t p = (uint64_t(ml) * k + 500) / 1000;
  if (!p || p > UINT32_MAX) return false;
  out = uint32_t(p); return true;
}
inline uint32_t volumeMl(uint32_t pulses, uint32_t k) {
  const uint64_t v = k ? uint64_t(pulses) * 1000 / k : 0;
  return v > UINT32_MAX ? UINT32_MAX : uint32_t(v);
}
inline bool elapsed(uint32_t now, uint32_t since, uint32_t ms) { return uint32_t(now - since) >= ms; }
inline bool serialNewer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }
const char *phaseName(Phase p);
const char *reasonName(Reason r);
const char *replyName(Reply r);
}
