#pragma once
#include "recovery_crypto.h"
#include "recovery_journal.h"

namespace pump::recovery {
inline void wipe(void *data,size_t size) {
  volatile uint8_t *p=static_cast<volatile uint8_t *>(data);
  while (size--) *p++=0;
}
struct Credential {
  Id device{};
  uint32_t epoch=0;
  Key key{};
};
// Independent namespace from pumpmulti and Mesh. Initialization is trusted USB
// only. An existing credential can never be overwritten by initialization.
class CredentialStore {
 public:
  explicit CredentialStore(Journal &journal): journal_(journal) {}
  ~CredentialStore() { wipe(&credential_,sizeof(credential_)); }
  CredentialStore(const CredentialStore &)=delete;
  CredentialStore &operator=(const CredentialStore &)=delete;
  JournalState load(const Id &expected) {
    wipe(&credential_,sizeof(credential_)); device_=expected;
    state_=JournalState::Fault;
    if (!nonzero(expected.data(),expected.size())) return state_;
    const auto loaded=journal_.load();
    if (loaded!=JournalState::Ready) return state_=loaded;
    const auto *p=journal_.data();
    if (journal_.size()!=64 || get32(p)!=0x31435250 || p[4]!=1 || p[5] || p[6]!=2 || p[7] ||
        memcmp(p+8,device_.data(),16) || !get32(p+24) || !nonzero(p+28,32) || get32(p+60)) return state_;
    credential_.device=device_; credential_.epoch=get32(p+24); memcpy(credential_.key.data(),p+28,32);
    return state_=JournalState::Ready;
  }
  bool initialize(const Key &key) {
    if (state_!=JournalState::Empty || !nonzero(key.data(),key.size())) return false;
    std::array<uint8_t,64> record{};
    put32(record.data(),0x31435250); record[4]=1; record[6]=2;
    memcpy(record.data()+8,device_.data(),16); put32(record.data()+24,1);
    memcpy(record.data()+28,key.data(),key.size());
    state_=JournalState::Fault;
    const bool saved=journal_.save(record.data(),record.size());
    wipe(record.data(),record.size());
    if (!saved) return false;
    // Verify schema and device as well as the journal's byte readback.
    return load(device_)==JournalState::Ready;
  }
  JournalState state() const { return state_; }
  const Id &device() const { return device_; }
  const Credential *get() const { return state_==JournalState::Ready ? &credential_ : nullptr; }
 private:
  static bool nonzero(const uint8_t *p,size_t size) { uint8_t value=0; while(size--) value|=*p++; return value!=0; }
  Journal &journal_;
  JournalState state_=JournalState::Fault;
  Id device_{};
  Credential credential_{};
};
}
