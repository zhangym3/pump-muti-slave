#pragma once
#include "recovery_credentials.h"
#include <algorithm>
#include <cstring>

namespace pump::recovery {
inline uint16_t get16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(uint16_t(p[1])<<8); }
inline void put16(uint8_t *p,uint16_t n) { p[0]=uint8_t(n); p[1]=uint8_t(n>>8); }
// Fixed 20-byte ATT values work at MTU 23. Header: kind=1, transfer LE16,
// total LE16, offset LE16; at most 13 payload bytes. Pull: kind=2, id, offset.
// Pull + read makes lost reads repeatable; no notification or implicit retry.
constexpr size_t RecoveryMessageMax=1+SecureSession::MaxFrame;
class RecoveryInbox {
 public:
  ~RecoveryInbox() { reset(); }
  void reset() { wipe(bytes_.data(),bytes_.size()); used_=size_=last_=0; deadline_=0; active_=ready_=false; }
  bool push(const uint8_t *p,size_t n,uint64_t now) {
    if (!p || n<8 || n>20 || p[0]!=1 || ready_) return reject();
    const auto id=get16(p+1),total=get16(p+3),offset=get16(p+5);
    if (!id || !total || total>bytes_.size() || offset>=total || n-7>size_t(total-offset)) return reject();
    if (active_ && now>=deadline_) return reject();
    if (!active_) {
      if (offset || id<=last_) return reject();
      last_=id; size_=total; used_=0; active_=true; deadline_=now+3000;
    }
    if (id!=last_ || total!=size_ || offset!=used_) return reject();
    memcpy(bytes_.data()+used_,p+7,n-7); used_+=n-7;
    if (used_==size_) { active_=false; ready_=true; }
    return true;
  }
  bool expired(uint64_t now) const { return active_ && now>=deadline_; }
  bool ready() const { return ready_; }
  uint16_t id() const { return last_; }
  const uint8_t *data() const { return bytes_.data(); }
  size_t size() const { return ready_ ? size_ : 0; }
  void consume() { wipe(bytes_.data(),bytes_.size()); used_=size_=0; ready_=false; }
 private:
  bool reject() { consume(); active_=false; return false; }
  std::array<uint8_t,RecoveryMessageMax> bytes_{};
  uint16_t used_=0,size_=0,last_=0;
  uint64_t deadline_=0;
  bool active_=false,ready_=false;
};

class RecoveryOutbox {
 public:
  ~RecoveryOutbox() { clear(); }
  void clear() { wipe(bytes_.data(),bytes_.size()); id_=size_=offset_=0; }
  bool set(uint16_t id,const uint8_t *p,size_t n) {
    clear(); if (!id || !p || !n || n>bytes_.size()) return false;
    id_=id; size_=uint16_t(n); memcpy(bytes_.data(),p,n); return true;
  }
  bool pull(const uint8_t *p,size_t n) {
    if (!p || n!=5 || p[0]!=2 || !id_ || get16(p+1)!=id_ || get16(p+3)>=size_) return false;
    offset_=get16(p+3); return true;
  }
  size_t read(uint8_t out[20]) const {
    if (!id_) return 0;
    out[0]=1; put16(out+1,id_); put16(out+3,size_); put16(out+5,offset_);
    const auto count=std::min(size_t(13),size_t(size_-offset_));
    memcpy(out+7,bytes_.data()+offset_,count); return count+7;
  }
 private:
  std::array<uint8_t,RecoveryMessageMax> bytes_{};
  uint16_t id_=0,size_=0,offset_=0;
};

// Authentication + encrypted capability/status only. This endpoint has no
// Mesh, NVS or output dependency, and cannot prepare/commit a network reset.
class RecoveryEndpoint {
 public:
  using Random=bool (*)(uint8_t *,size_t);
  explicit RecoveryEndpoint(Random random): random_(random) {}
  ~RecoveryEndpoint() { disconnect(); wipe(&credential_,sizeof(credential_)); }
  void configure(const Credential *credential) {
    disconnect(); wipe(&credential_,sizeof(credential_)); configured_=false;
    if (credential && credential->epoch && nonzero(credential->device.data(),16) && nonzero(credential->key.data(),32)) {
      credential_=*credential; configured_=true;
    }
  }
  bool connect(uint64_t now) {
    disconnect();
    if (!configured_ || now<allowedAt_) return false;
    stage_=Stage::Fresh; started_=activity_=now; return true;
  }
  void disconnect() { stage_=Stage::Closed; session_.clear(); wipe(&challenge_,sizeof(challenge_)); }
  bool expired(uint64_t now) const {
    return stage_==Stage::Closed || now<activity_ || now-started_>=60000 ||
      now-activity_>=(stage_==Stage::Authenticated ? 15000u : 10000u);
  }
  bool handle(const uint8_t *p,size_t n,uint8_t *out,size_t &capacity,uint64_t now) {
    if (!p || !n || !out || capacity<RecoveryMessageMax || expired(now)) return fail(now);
    size_t size=0;
    if (stage_==Stage::Fresh && p[0]==1 && n==55 && get16(p+1)==RecoveryVersion &&
        !memcmp(p+3,credential_.device.data(),16) && get32(p+19)==credential_.epoch && nonzero(p+23,32)) {
      challenge_.device=credential_.device; challenge_.credentialEpoch=credential_.epoch;
      memcpy(challenge_.clientNonce.data(),p+23,32);
      if (!random_ || !random_(challenge_.deviceNonce.data(),32)) return fail(now);
      Proof proof{};
      if (!makeProof(credential_.key,challenge_,true,proof)) return fail(now);
      out[0]=0x81; memcpy(out+1,challenge_.deviceNonce.data(),32); memcpy(out+33,proof.data(),32);
      size=65; stage_=Stage::Challenged;
    } else if (stage_==Stage::Challenged && p[0]==2 && n==33) {
      Proof proof{}; memcpy(proof.data(),p+1,32);
      if (!session_.authenticate(false,credential_.key,challenge_,proof)) return fail(now);
      stage_=Stage::Authenticated; failures_=0;
      out[0]=0x82; size=capacity-1;
      if (!status(out+1,size)) return fail(now);
      ++size;
    } else if (stage_==Stage::Authenticated && p[0]==3 && n>1) {
      std::array<uint8_t,SecureSession::MaxPlain> plain{}; size_t length=plain.size();
      if (!session_.open(p+1,n-1,plain.data(),length)) return fail(now);
      const bool query=length==1 && plain[0]==1; wipe(plain.data(),plain.size());
      out[0]=0x83; size=capacity-1;
      if (query) { if (!status(out+1,size)) return fail(now); }
      else {
        const uint8_t unsupported[]={0xff,1};
        if (!session_.seal(unsupported,sizeof(unsupported),out+1,size)) return fail(now);
      }
      ++size;
    } else return fail(now);
    capacity=size; activity_=now; return true;
  }
 private:
  enum class Stage { Closed,Fresh,Challenged,Authenticated };
  static bool nonzero(const uint8_t *p,size_t n) { uint8_t v=0; while(n--) v|=*p++; return v!=0; }
  bool fail(uint64_t now) {
    failures_=std::min(failures_+1,5u); allowedAt_=now+std::min(uint64_t(1000)<<failures_,uint64_t(30000));
    disconnect(); return false;
  }
  bool status(uint8_t *out,size_t &capacity) {
    uint8_t plain[9]={0x81}; put16(plain+1,1); put16(plain+3,0); put32(plain+5,credential_.epoch);
    return session_.seal(plain,sizeof(plain),out,capacity);
  }
  Random random_;
  Credential credential_{}; Challenge challenge_{}; SecureSession session_;
  Stage stage_=Stage::Closed;
  uint64_t started_=0,activity_=0,allowedAt_=0;
  unsigned failures_=0;
  bool configured_=false;
};
}
