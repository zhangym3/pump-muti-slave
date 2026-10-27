#include "recovery_attempt_secret.h"
#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include <cstring>

namespace pump::recovery {
namespace {
template<size_t N> bool nonzero(const std::array<uint8_t,N> &v) {
  uint8_t bits=0; for (auto b:v) bits|=b; return bits!=0;
}
constexpr size_t HeaderSize=64,PlainSize=PreparedReceiptSize+16,TagSize=16;
static_assert(HeaderSize+PlainSize+TagSize==AttemptSecretStore::RecordSize);
constexpr char StoreDomain[]="PUMP-ATTEMPT-STORE-V1";
constexpr char KeyDomain[]="PUMP-ATTEMPT-STORE-KEY-V1";
bool contextBytes(const PreparedReceipt &r,uint8_t out[96]) {
  if (!r.attempt || !r.generation || !r.credentialEpoch || !nonzero(r.operation) || !nonzero(r.device) ||
      !nonzero(r.master) || !nonzero(r.network) || !nonzero(r.binding)) return false;
  memcpy(out,r.operation.data(),16); put32(out+16,r.attempt); memcpy(out+20,r.device.data(),16);
  put32(out+36,r.credentialEpoch); memcpy(out+40,r.master.data(),16); memcpy(out+56,r.network.data(),16);
  memcpy(out+72,r.binding.data(),16); put64(out+88,r.generation); return true;
}
bool derive(const Credential &credential,Key &key) {
  key.fill(0); uint8_t salt[20]{};
  memcpy(salt,credential.device.data(),16); put32(salt+16,credential.epoch);
  const auto md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return md && mbedtls_hkdf(md,salt,sizeof(salt),credential.key.data(),credential.key.size(),
    reinterpret_cast<const uint8_t *>(KeyDomain),sizeof(KeyDomain)-1,key.data(),key.size())==0 && nonzero(key);
}
bool crypt(bool encrypt,const Key &key,uint8_t *record,uint8_t *plain) {
  uint8_t aad[sizeof(StoreDomain)-1+HeaderSize]{};
  memcpy(aad,StoreDomain,sizeof(StoreDomain)-1); memcpy(aad+sizeof(StoreDomain)-1,record,HeaderSize);
  mbedtls_gcm_context context; mbedtls_gcm_init(&context);
  const bool keyed=mbedtls_gcm_setkey(&context,MBEDTLS_CIPHER_ID_AES,key.data(),256)==0;
  const bool ok=keyed && (encrypt?
    mbedtls_gcm_crypt_and_tag(&context,MBEDTLS_GCM_ENCRYPT,PlainSize,record+48,12,aad,sizeof(aad),
      plain,record+HeaderSize,TagSize,record+HeaderSize+PlainSize):
    mbedtls_gcm_auth_decrypt(&context,PlainSize,record+48,12,aad,sizeof(aad),
      record+HeaderSize+PlainSize,TagSize,record+HeaderSize,plain))==0;
  mbedtls_gcm_free(&context); return ok;
}
}
bool makeAttemptOobCommitment(const PreparedReceipt &context,const AttemptOob &oob,Proof &out) {
  constexpr char domain[]="PUMP-ATTEMPT-OOB-V1";
  uint8_t message[sizeof(domain)-1+96+16]{}; out.fill(0);
  if (!nonzero(oob) || !contextBytes(context,message+sizeof(domain)-1)) return false;
  memcpy(message,domain,sizeof(domain)-1); memcpy(message+sizeof(domain)-1+96,oob.data(),oob.size());
  const auto md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  const bool ok=md && mbedtls_md(md,message,sizeof(message),out.data())==0 && nonzero(out);
  wipe(message,sizeof(message)); if (!ok) out.fill(0); return ok;
}
void AttemptSecretStore::clear() {
  wipe(wrapping_.data(),wrapping_.size()); wipe(oob_.data(),oob_.size());
  device_={}; reference_={}; epoch_=0; receipt_={}; state_=JournalState::Fault;
}
JournalState AttemptSecretStore::load(const Credential &credential) {
  clear();
  if (!random_ || !credential.epoch || !nonzero(credential.device) || !nonzero(credential.key) ||
      !derive(credential,wrapping_)) { clear(); return state_; }
  device_=credential.device; epoch_=credential.epoch;
  const auto loaded=journal_.load();
  if (loaded==JournalState::Empty) return state_=loaded;
  if (loaded!=JournalState::Ready || !decode()) { clear(); return state_; }
  return state_=JournalState::Ready;
}
bool AttemptSecretStore::decode() {
  if (journal_.size()!=RecordSize) return false;
  std::array<uint8_t,RecordSize> record{}; uint8_t plain[PlainSize]{};
  memcpy(record.data(),journal_.data(),record.size()); const auto *p=record.data();
  bool valid=get32(p)==0x31534150 && p[4]==1 && !p[5] && p[6]==uint8_t(RecordSize) && p[7]==uint8_t(RecordSize>>8) &&
    !memcmp(p+8,device_.data(),16) && get32(p+24)==epoch_ && !get32(p+28) && !get32(p+60);
  Id reference{}; std::array<uint8_t,12> nonce{};
  memcpy(reference.data(),p+32,reference.size()); memcpy(nonce.data(),p+48,nonce.size());
  PreparedReceipt receipt{}; AttemptOob oob{}; Proof commitment{};
  valid=valid && nonzero(reference) && nonzero(nonce) && crypt(false,wrapping_,record.data(),plain) &&
    decodePreparedReceipt(plain,PreparedReceiptSize,receipt) && receipt.device==device_ && receipt.credentialEpoch==epoch_;
  if (valid) {
    memcpy(oob.data(),plain+PreparedReceiptSize,oob.size());
    valid=makeAttemptOobCommitment(receipt,oob,commitment) &&
      equalSecret(commitment.data(),receipt.oobCommitment.data(),commitment.size());
  }
  if (valid) { reference_=reference; receipt_=receipt; oob_=oob; }
  wipe(plain,sizeof(plain)); wipe(oob.data(),oob.size()); return valid;
}
bool AttemptSecretStore::matches(const PreparedReceipt &receipt) const {
  uint8_t a[PreparedReceiptSize]{},b[PreparedReceiptSize]{};
  return encodePreparedReceipt(receipt,a) && encodePreparedReceipt(receipt_,b) && !memcmp(a,b,sizeof(a));
}
AttemptSecretResult AttemptSecretStore::install(const PreparedReceipt &receipt,const AttemptOob &oob) {
  if (state_==JournalState::Fault) return AttemptSecretResult::StorageFault;
  Proof commitment{}; uint8_t plain[PlainSize]{};
  bool valid=receipt.device==device_ && receipt.credentialEpoch==epoch_ && encodePreparedReceipt(receipt,plain) &&
    makeAttemptOobCommitment(receipt,oob,commitment) && equalSecret(commitment.data(),receipt.oobCommitment.data(),32);
  if (!valid) return AttemptSecretResult::Rejected;
  if (state_==JournalState::Ready) return matches(receipt) && equalSecret(oob.data(),oob_.data(),oob.size())?
    AttemptSecretResult::Existing:AttemptSecretResult::Rejected;
  std::array<uint8_t,RecordSize> record{};
  Id reference{}; std::array<uint8_t,12> nonce{};
  if (!random_(reference.data(),reference.size()) || !nonzero(reference) ||
      !random_(nonce.data(),nonce.size()) || !nonzero(nonce)) return AttemptSecretResult::Rejected;
  auto *p=record.data(); put32(p,0x31534150); p[4]=1; p[6]=uint8_t(RecordSize); p[7]=uint8_t(RecordSize>>8);
  memcpy(p+8,device_.data(),16); put32(p+24,epoch_); memcpy(p+32,reference.data(),16); memcpy(p+48,nonce.data(),12);
  memcpy(plain+PreparedReceiptSize,oob.data(),oob.size()); valid=crypt(true,wrapping_,p,plain); wipe(plain,sizeof(plain));
  if (!valid) { clear(); return AttemptSecretResult::StorageFault; }
  state_=JournalState::Fault;
  if (!journal_.save(p,record.size()) || !decode()) { clear(); return AttemptSecretResult::StorageFault; }
  state_=JournalState::Ready; return AttemptSecretResult::Applied;
}
bool AttemptSecretStore::resolve(const Id &reference,const PreparedReceipt &expected,AttemptOob &out) const {
  out.fill(0);
  if (state_!=JournalState::Ready || reference!=reference_ || !matches(expected)) return false;
  out=oob_; return true;
}
}
