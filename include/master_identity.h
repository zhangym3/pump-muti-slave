#pragma once
#include "recovery_credentials.h"

namespace pump::recovery {
using PublicKey = std::array<uint8_t,65>;
using Signature = std::array<uint8_t,64>;
using RandomSource = bool (*)(uint8_t *,size_t);

// P-256 SEC1 uncompressed public key; signatures are fixed-width r||s, low-S.
bool identityPublicKey(const Key &secret,PublicKey &publicKey,RandomSource random);
bool identityFingerprint(const PublicKey &publicKey,Id &fingerprint);
bool identitySign(const Key &secret,const uint8_t *message,size_t size,Signature &signature,RandomSource random);
bool identityVerify(const PublicKey &publicKey,const uint8_t *message,size_t size,const Signature &signature);

struct MasterIdentity {
  Id device{},master{},network{};
  uint32_t epoch=0;
  PublicKey publicKey{};
};

// No SDK readiness is inferred by this codec. The coordinator may sign a
// PreparedReceipt only AFTER durable reservation and successful OOB callback.
struct PreparedReceipt {
  Id operation{},device{},master{},network{},binding{};
  uint32_t attempt=0,credentialEpoch=0,capabilities=0,budgetMs=0;
  uint64_t generation=0;
  Proof oobCommitment{},challenge{};
};
constexpr size_t PreparedReceiptSize=176;
constexpr uint32_t PreparedReceiptCapabilities=0x0f; // V7, per-binding AppKey, forced OOB, armed
bool encodePreparedReceipt(const PreparedReceipt &receipt,uint8_t out[PreparedReceiptSize]);
// Strict canonical decoding; this does not verify a signature or confer authority.
bool decodePreparedReceipt(const uint8_t *body,size_t size,PreparedReceipt &receipt);
bool signPreparedReceipt(const Key &secret,const PreparedReceipt &receipt,Signature &signature,RandomSource random);
// Compare the entire expected transaction, including fresh APP challenge,
// before accepting a signature. The trusted public key comes from installation.
bool verifyPreparedReceipt(const PublicKey &trusted,const PreparedReceipt &expected,
                           const uint8_t *body,size_t size,const Signature &signature);

// Dedicated NVS namespace. USB-only explicit initialization; no first-client
// ownership, no regeneration on missing/corrupt committed records.
class MasterIdentityStore {
 public:
  MasterIdentityStore(Journal &journal,RandomSource random):journal_(journal),random_(random) {}
  ~MasterIdentityStore() { wipe(secret_.data(),secret_.size()); }
  MasterIdentityStore(const MasterIdentityStore &)=delete;
  MasterIdentityStore &operator=(const MasterIdentityStore &)=delete;
  JournalState load(Id expectedDevice);
  bool initialize();
  JournalState state() const { return state_; }
  const MasterIdentity *get() const { return state_==JournalState::Ready?&identity_:nullptr; }
  bool prove(const Proof &challenge,Signature &signature) const;
 private:
  Journal &journal_;
  RandomSource random_;
  JournalState state_=JournalState::Fault;
  MasterIdentity identity_{};
  Key secret_{};
};
bool verifyMasterProof(const MasterIdentity &trusted,const Proof &challenge,const Signature &signature);
}
