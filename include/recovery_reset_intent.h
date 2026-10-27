#pragma once
#include "master_identity.h"

namespace pump::recovery {
// This ledger is a persistence primitive, not an authenticated command handler.
// Only the future single control-task coordinator may call it, after checking
// the recovery session, fresh expected receipt, durable OOB reference and trust.
// It has no SDK, output, automatic retry, binding-activation or enable operation.
enum class ResetPhase : uint8_t { Prepared=1, ResetCommitted=2, MeshCleared=3, Cancelled=4 };
enum class IntentResult { Rejected, Applied, Existing, StorageFault };

struct ResetSafetySnapshot {
  bool admissionLocked=false,taskIdle=false,outputsClosed=false,localHoldReleased=false;
  bool maintenanceAbsent=false,flowQuiet=false,transportQuiescent=false;
  bool safe() const {
    return admissionLocked && taskIdle && outputsClosed && localHoldReleased &&
      maintenanceAbsent && flowQuiet && transportQuiescent;
  }
};
struct ResetIntent {
  ResetPhase phase=ResetPhase::Prepared;
  PreparedReceipt receipt{};
  PublicKey masterPublic{};
  Signature masterSignature{};
  Proof oldMeshDigest{};
  Id attemptSecretRef{};
};

class ResetIntentStore {
 public:
  static constexpr size_t RecordSize=408;
  explicit ResetIntentStore(Journal &journal):journal_(journal) {}
  ResetIntentStore(const ResetIntentStore &)=delete;
  ResetIntentStore &operator=(const ResetIntentStore &)=delete;

  // minGeneration comes from the committed binding/migration record. A missing
  // journal never implies that the device's existing binding generation is zero.
  JournalState load(const Id &device,uint32_t credentialEpoch,uint64_t minGeneration) {
    state_=JournalState::Fault; current_={}; device_=device; epoch_=credentialEpoch; floor_=minGeneration;
    if (!nonzero(device.data(),device.size()) || !epoch_) return state_;
    const auto loaded=journal_.load();
    if (loaded!=JournalState::Ready) return state_=loaded;
    const auto *p=journal_.data();
    if (journal_.size()!=RecordSize || get32(p)!=0x31545250 || p[4]!=1 || p[5] ||
        (uint16_t(p[6]) | uint16_t(uint16_t(p[7])<<8))!=RecordSize ||
        p[8]<1 || p[8]>4 || nonzero(p+9,7) || memcmp(p+16,device.data(),16) ||
        get32(p+32)!=epoch_ || get32(p+36) || nonzero(p+401,7) ||
        !decodePreparedReceipt(p+96,PreparedReceiptSize,current_.receipt)) return state_;
    current_.phase=ResetPhase(p[8]);
    memcpy(current_.oldMeshDigest.data(),p+48,32); memcpy(current_.attemptSecretRef.data(),p+80,16);
    memcpy(current_.masterPublic.data(),p+272,65); memcpy(current_.masterSignature.data(),p+337,64);
    if (get64(p+40)!=current_.receipt.generation || get64(p+40)<=minGeneration ||
        !valid(current_)) { current_={}; return state_; }
    floor_=get64(p+40); return state_=JournalState::Ready;
  }
  JournalState state() const { return state_; }
  const ResetIntent *get() const { return state_==JournalState::Ready?&current_:nullptr; }
  uint64_t generationFloor() const { return floor_; }

  IntentResult prepare(const PreparedReceipt &expected,const uint8_t *body,size_t size,
                       const PublicKey &trusted,const Signature &signature,const Proof &oldMeshDigest,
                       const Id &durableSecretRef,const ResetSafetySnapshot &safety) {
    if (state_==JournalState::Fault) return IntentResult::StorageFault;
    if (!verifyPreparedReceipt(trusted,expected,body,size,signature)) return IntentResult::Rejected;
    ResetIntent next{}; next.receipt=expected; next.masterPublic=trusted; next.masterSignature=signature;
    next.oldMeshDigest=oldMeshDigest; next.attemptSecretRef=durableSecretRef;
    if (!valid(next)) return IntentResult::Rejected;
    if (state_==JournalState::Ready && current_.receipt.operation==expected.operation) {
      // Return the actual durable phase; never make a second clear decision.
      return current_.phase!=ResetPhase::Cancelled && sameRequest(next,current_)?
        IntentResult::Existing:IntentResult::Rejected;
    }
    if (!safety.safe() || floor_==UINT64_MAX || expected.generation!=floor_+1 ||
        (state_==JournalState::Ready && current_.phase!=ResetPhase::Cancelled)) return IntentResult::Rejected;
    return persist(next);
  }

  IntentResult commit(const Id &operation,uint32_t attempt,const Proof &observedOldMesh,
                      const Id &verifiedSecretRef,const ResetSafetySnapshot &safety) {
    if (state_==JournalState::Fault) return IntentResult::StorageFault;
    if (!matches(operation,attempt) || current_.oldMeshDigest!=observedOldMesh ||
        current_.attemptSecretRef!=verifiedSecretRef) return IntentResult::Rejected;
    if (current_.phase==ResetPhase::ResetCommitted || current_.phase==ResetPhase::MeshCleared)
      return IntentResult::Existing;
    if (current_.phase!=ResetPhase::Prepared || !safety.safe()) return IntentResult::Rejected;
    auto next=current_; next.phase=ResetPhase::ResetCommitted; return persist(next);
  }

  // Caller must re-read actual SDK state after obtaining/retaining its admission
  // lock. A missing ACK or GATT write success is never evidence of Mesh clear.
  IntentResult observeMeshCleared(const Id &operation,uint32_t attempt,bool sdkUnprovisioned,
                                 const ResetSafetySnapshot &safety) {
    if (state_==JournalState::Fault) return IntentResult::StorageFault;
    if (!matches(operation,attempt) || !sdkUnprovisioned || !safety.safe()) return IntentResult::Rejected;
    if (current_.phase==ResetPhase::MeshCleared) return IntentResult::Existing;
    if (current_.phase!=ResetPhase::ResetCommitted) return IntentResult::Rejected;
    auto next=current_; next.phase=ResetPhase::MeshCleared; return persist(next);
  }

  IntentResult cancel(const Id &operation,uint32_t attempt,const Proof &observedOldMesh,
                      const ResetSafetySnapshot &safety) {
    if (state_==JournalState::Fault) return IntentResult::StorageFault;
    if (!matches(operation,attempt) || current_.oldMeshDigest!=observedOldMesh || !safety.safe())
      return IntentResult::Rejected;
    if (current_.phase==ResetPhase::Cancelled) return IntentResult::Existing;
    if (current_.phase!=ResetPhase::Prepared) return IntentResult::Rejected;
    // Keep the tombstone and reserved generation; never reuse a cancelled ID.
    auto next=current_; next.phase=ResetPhase::Cancelled; return persist(next);
  }

 private:
  static bool nonzero(const uint8_t *p,size_t n) { uint8_t v=0; while(n--) v|=*p++; return v!=0; }
  bool valid(const ResetIntent &intent) const {
    uint8_t body[PreparedReceiptSize]{};
    return intent.receipt.device==device_ && intent.receipt.credentialEpoch==epoch_ &&
      nonzero(intent.oldMeshDigest.data(),32) && nonzero(intent.attemptSecretRef.data(),16) &&
      encodePreparedReceipt(intent.receipt,body) && verifyPreparedReceipt(intent.masterPublic,intent.receipt,
        body,sizeof(body),intent.masterSignature);
  }
  static bool sameRequest(const ResetIntent &a,const ResetIntent &b) {
    uint8_t first[PreparedReceiptSize]{},second[PreparedReceiptSize]{};
    return encodePreparedReceipt(a.receipt,first) && encodePreparedReceipt(b.receipt,second) &&
      !memcmp(first,second,sizeof(first)) && a.masterPublic==b.masterPublic &&
      a.masterSignature==b.masterSignature && a.oldMeshDigest==b.oldMeshDigest && a.attemptSecretRef==b.attemptSecretRef;
  }
  bool matches(const Id &operation,uint32_t attempt) const {
    return state_==JournalState::Ready && current_.receipt.operation==operation && current_.receipt.attempt==attempt;
  }
  IntentResult persist(const ResetIntent &next) {
    uint8_t record[RecordSize]{};
    put32(record,0x31545250); record[4]=1; record[6]=uint8_t(RecordSize); record[7]=uint8_t(RecordSize>>8);
    record[8]=uint8_t(next.phase); memcpy(record+16,device_.data(),16); put32(record+32,epoch_);
    put64(record+40,next.receipt.generation); memcpy(record+48,next.oldMeshDigest.data(),32);
    memcpy(record+80,next.attemptSecretRef.data(),16);
    if (!valid(next) || !encodePreparedReceipt(next.receipt,record+96)) return IntentResult::Rejected;
    memcpy(record+272,next.masterPublic.data(),65); memcpy(record+337,next.masterSignature.data(),64);
    state_=JournalState::Fault;
    if (!journal_.save(record,sizeof(record))) return IntentResult::StorageFault;
    current_=next; floor_=next.receipt.generation; state_=JournalState::Ready;
    return IntentResult::Applied;
  }
  Journal &journal_;
  Id device_{};
  uint32_t epoch_=0;
  uint64_t floor_=0;
  ResetIntent current_{};
  JournalState state_=JournalState::Fault;
};
static_assert(ResetIntentStore::RecordSize<=Journal::MaxPayload);
}
