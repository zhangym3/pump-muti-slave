#pragma once
#include "master_identity.h"

namespace pump::recovery {
using AttemptOob = std::array<uint8_t,16>;

// Domain-separated SHA-256 of immutable attempt context and a 128-bit OOB.
// Does not authenticate the receipt, establish trust or authorize a Mesh reset.
bool makeAttemptOobCommitment(const PreparedReceipt &context,const AttemptOob &oob,Proof &out);

enum class AttemptSecretResult { Rejected, Applied, Existing, StorageFault };

// Internal single-attempt persistence primitive. No transport/SDK/output calls.
// The control-task coordinator must authenticate the user and verify a fresh
// signed receipt before installation. install() never overwrites an old attempt;
// retirement/retry requires the future cross-journal lifecycle coordinator.
// The wrapping key is derived from this device's recovery credential. This is
// NOT protection against physical extraction of that credential from Flash.
class AttemptSecretStore {
 public:
  static constexpr size_t RecordSize=272;
  AttemptSecretStore(Journal &journal,RandomSource random):journal_(journal),random_(random) {}
  ~AttemptSecretStore() { clear(); }
  AttemptSecretStore(const AttemptSecretStore &)=delete;
  AttemptSecretStore &operator=(const AttemptSecretStore &)=delete;

  JournalState load(const Credential &credential);
  AttemptSecretResult install(const PreparedReceipt &receipt,const AttemptOob &oob);
  // Caller supplies both the durable intent reference AND its entire receipt.
  // On every failure the output is cleared. No unscoped plaintext getter.
  bool resolve(const Id &reference,const PreparedReceipt &expected,AttemptOob &out) const;
  JournalState state() const { return state_; }
  Id reference() const { return state_==JournalState::Ready?reference_:Id{}; }

 private:
  void clear();
  bool decode();
  bool matches(const PreparedReceipt &receipt) const;
  Journal &journal_;
  RandomSource random_;
  JournalState state_=JournalState::Fault;
  Id device_{},reference_{};
  uint32_t epoch_=0;
  Key wrapping_{};
  PreparedReceipt receipt_{};
  AttemptOob oob_{};
};
static_assert(AttemptSecretStore::RecordSize<=Journal::MaxPayload);
}
