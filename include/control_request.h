#pragma once
#include "protocol.h"
namespace pump {
struct ControlRequest {
  Action action = Action::Query;
  uint32_t epoch = 0;
  uint64_t client = 0;
  uint32_t sequence = 0, mask = 0, ml = 0, maxMs = 0;
  JobKey stopKey;
  // 0 for unselected slots; otherwise volume / 50 (1..40). ml == 0 selects this mode.
  uint8_t amounts[MaxNodes] = {};
  bool individual() const { return action == Action::Start && mask != 0 && ml == 0; }
};
struct ClientRecord {
  uint64_t client = 0;
  uint32_t sequence = 0, mask = 0, ml = 0, maxMs = 0;
  JobKey key;
};
inline bool validAmounts(uint32_t mask, const uint8_t *amounts) {
  if (!mask || !amounts) return false;
  for (size_t i = 0; i < MaxNodes; ++i) {
    if (mask & (uint32_t(1) << i)) { if (!amounts[i] || amounts[i] > 40) return false; }
    else if (amounts[i]) return false;
  }
  return true;
}
inline Reply replayDecision(const ClientRecord &old, const ControlRequest &q, const uint8_t *amounts = nullptr) {
  if (q.sequence < old.sequence) return Reply::Stale;
  if (q.sequence > old.sequence) return Reply::Pending; // A new request still needs validation and durable acceptance.
  const bool samePlan = !q.individual() || (amounts && !memcmp(amounts, q.amounts, MaxNodes));
  return old.mask == q.mask && old.ml == q.ml && old.maxMs == q.maxMs &&
    old.key.epoch == q.epoch && samePlan ? Reply::Ok : Reply::Conflict;
}
inline Packet encodeRequest(uint16_t master, const ControlRequest &q) {
  auto p = packet(Op::Request, master); Writer w{p}; w.u8(uint8_t(q.action)); w.u32(q.epoch);
  w.u64(q.client); w.u32(q.sequence); w.u32(q.mask); w.u32(q.ml); w.u32(q.maxMs);
  if (q.individual()) { for (auto v : q.amounts) w.u8(v); }
  else w.key(q.stopKey);
  return p;
}
inline bool decodeRequest(const Packet &p, ControlRequest &q) {
  Reader r{p}; q = ControlRequest{};
  q.action = Action(r.u8()); q.epoch = r.u32(); q.client = r.u64(); q.sequence = r.u32();
  q.mask = r.u32(); q.ml = r.u32(); q.maxMs = r.u32();
  if (q.individual()) { for (auto &v : q.amounts) v = r.u8(); if (!validAmounts(q.mask, q.amounts)) return false; }
  else q.stopKey = r.key();
  return r.done() && q.client && q.sequence && q.action >= Action::Start && q.action <= Action::Query;
}
inline bool unsignedNumber(const char *s, uint64_t &out, unsigned base = 10) {
  if (!s || !*s || (base != 10 && base != 16)) return false;
  if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  if (!*s) return false;
  uint64_t v = 0;
  for (; *s; ++s) {
    unsigned digit = *s >= '0' && *s <= '9' ? unsigned(*s - '0') :
      *s >= 'a' && *s <= 'f' ? unsigned(*s - 'a' + 10) :
      *s >= 'A' && *s <= 'F' ? unsigned(*s - 'A' + 10) : 255;
    if (digit >= base || v > (UINT64_MAX - digit) / base) return false;
    v = v * base + digit;
  }
  out = v; return true;
}
inline bool parseControl(const char *const *tokens, size_t count, uint32_t currentEpoch, ControlRequest &q) {
  if (!count) return false;
  q = ControlRequest{};
  const bool each = !strcmp(tokens[0], "start-each");
  if (!strcmp(tokens[0], "start") || each) q.action = Action::Start;
  else if (!strcmp(tokens[0], "stop")) q.action = Action::Stop;
  else if (!strcmp(tokens[0], "query")) q.action = Action::Query;
  else return false;
  const size_t wanted = q.action == Action::Start ? 7 : q.action == Action::Stop ? 6 : 3;
  if (count != wanted) return false;
  uint64_t values[6] = {};
  for (size_t i = 1; i < wanted; ++i) {
    if (each && i == 6) continue;
    const unsigned base = i == 1 || (q.action == Action::Start && i == 4) ? 16 : 10;
    if (!unsignedNumber(tokens[i], values[i - 1], base) || (i != 1 && values[i - 1] > UINT32_MAX)) return false;
  }
  if (!values[0] || !values[1]) return false;
  q.client = values[0]; q.sequence = uint32_t(values[1]);
  q.epoch = q.action == Action::Query ? currentEpoch : uint32_t(values[2]);
  if (q.action == Action::Start) {
    q.mask = values[3]; q.ml = each ? 0 : values[4]; q.maxMs = each ? values[4] : values[5];
    if (each) {
      if (strlen(tokens[6]) != MaxNodes * 2) return false;
      for (size_t i = 0; i < MaxNodes; ++i) {
        char byte[3] = {tokens[6][2*i], tokens[6][2*i+1], 0}; uint64_t v = 0;
        if (!unsignedNumber(byte, v, 16) || v > 40) return false;
        q.amounts[i] = uint8_t(v);
      }
      if (!validAmounts(q.mask, q.amounts)) return false;
    } else if (!q.ml) return false;
  }
  if (q.action == Action::Stop) q.stopKey = {uint32_t(values[3]), uint32_t(values[4])};
  return true;
}
}
