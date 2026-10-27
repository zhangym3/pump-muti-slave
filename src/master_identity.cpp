#include "master_identity.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/md.h"
#include <cstring>

namespace pump::recovery {
namespace {
template<size_t N> bool nonzero(const std::array<uint8_t,N> &v) {
  uint8_t bits=0; for (auto b:v) bits|=b; return bits!=0;
}
int rng(void *context,unsigned char *out,size_t size) {
  const auto random=*static_cast<RandomSource *>(context);
  return random && random(out,size)?0:MBEDTLS_ERR_ECP_RANDOM_FAILED;
}
struct Ec {
  mbedtls_ecp_group group;
  mbedtls_ecp_point point;
  mbedtls_mpi secret,r,s,half;
  Ec() {
    mbedtls_ecp_group_init(&group); mbedtls_ecp_point_init(&point);
    mbedtls_mpi_init(&secret); mbedtls_mpi_init(&r); mbedtls_mpi_init(&s); mbedtls_mpi_init(&half);
  }
  ~Ec() {
    mbedtls_mpi_free(&half); mbedtls_mpi_free(&s); mbedtls_mpi_free(&r); mbedtls_mpi_free(&secret);
    mbedtls_ecp_point_free(&point); mbedtls_ecp_group_free(&group);
  }
  bool load() { return mbedtls_ecp_group_load(&group,MBEDTLS_ECP_DP_SECP256R1)==0; }
  bool loadSecret(const Key &key) {
    return load() && mbedtls_mpi_read_binary(&secret,key.data(),key.size())==0 &&
      mbedtls_ecp_check_privkey(&group,&secret)==0;
  }
  bool loadPublic(const PublicKey &key) {
    return key[0]==4 && load() && mbedtls_ecp_point_read_binary(&group,&point,key.data(),key.size())==0 &&
      mbedtls_ecp_check_pubkey(&group,&point)==0;
  }
};
bool hash(const uint8_t *message,size_t size,Proof &digest) {
  const auto md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return md && message && size && mbedtls_md(md,message,size,digest.data())==0;
}
bool proofMessage(const MasterIdentity &id,const Proof &challenge,uint8_t out[169]) {
  // Domain(20), physical UUID(16), epoch(4), network(16), master(16), pub(65), nonce(32).
  constexpr char domain[]="PUMP-MASTER-PROOF-V1";
  static_assert(sizeof(domain)-1==20);
  Id fingerprint{};
  if (!id.epoch || !nonzero(id.device) || !nonzero(id.network) || !nonzero(challenge) ||
      !identityFingerprint(id.publicKey,fingerprint) || fingerprint!=id.master) return false;
  memcpy(out,domain,20); memcpy(out+20,id.device.data(),16); put32(out+36,id.epoch);
  memcpy(out+40,id.network.data(),16); memcpy(out+56,id.master.data(),16);
  memcpy(out+72,id.publicKey.data(),65); memcpy(out+137,challenge.data(),32);
  return true;
}
}
bool identityPublicKey(const Key &secret,PublicKey &publicKey,RandomSource random) {
  publicKey.fill(0); Ec ec; size_t size=0;
  return random && ec.loadSecret(secret) &&
    mbedtls_ecp_mul(&ec.group,&ec.point,&ec.secret,&ec.group.G,rng,&random)==0 &&
    mbedtls_ecp_point_write_binary(&ec.group,&ec.point,MBEDTLS_ECP_PF_UNCOMPRESSED,&size,
                                 publicKey.data(),publicKey.size())==0 && size==publicKey.size();
}
bool identityFingerprint(const PublicKey &publicKey,Id &fingerprint) {
  fingerprint.fill(0); Ec ec; Proof digest{};
  if (!ec.loadPublic(publicKey) || !hash(publicKey.data(),publicKey.size(),digest)) return false;
  memcpy(fingerprint.data(),digest.data(),fingerprint.size()); return nonzero(fingerprint);
}
bool identitySign(const Key &secret,const uint8_t *message,size_t size,Signature &signature,RandomSource random) {
  signature.fill(0); Ec ec; Proof digest{};
  if (!random || !ec.loadSecret(secret) || !hash(message,size,digest) ||
      mbedtls_ecdsa_sign_det_ext(&ec.group,&ec.r,&ec.s,&ec.secret,digest.data(),digest.size(),
                               MBEDTLS_MD_SHA256,rng,&random)!=0 ||
      mbedtls_mpi_copy(&ec.half,&ec.group.N)!=0 || mbedtls_mpi_shift_r(&ec.half,1)!=0) return false;
  if (mbedtls_mpi_cmp_mpi(&ec.s,&ec.half)>0 && mbedtls_mpi_sub_mpi(&ec.s,&ec.group.N,&ec.s)!=0) return false;
  const bool ok=mbedtls_mpi_write_binary(&ec.r,signature.data(),32)==0 &&
    mbedtls_mpi_write_binary(&ec.s,signature.data()+32,32)==0;
  if (!ok) signature.fill(0);
  return ok;
}
bool identityVerify(const PublicKey &publicKey,const uint8_t *message,size_t size,const Signature &signature) {
  Ec ec; Proof digest{};
  return ec.loadPublic(publicKey) && hash(message,size,digest) &&
    mbedtls_mpi_read_binary(&ec.r,signature.data(),32)==0 &&
    mbedtls_mpi_read_binary(&ec.s,signature.data()+32,32)==0 &&
    mbedtls_mpi_copy(&ec.half,&ec.group.N)==0 && mbedtls_mpi_shift_r(&ec.half,1)==0 &&
    mbedtls_mpi_cmp_int(&ec.r,1)>=0 && mbedtls_mpi_cmp_mpi(&ec.r,&ec.group.N)<0 &&
    mbedtls_mpi_cmp_int(&ec.s,1)>=0 && mbedtls_mpi_cmp_mpi(&ec.s,&ec.half)<=0 &&
    mbedtls_ecdsa_verify(&ec.group,digest.data(),digest.size(),&ec.point,&ec.r,&ec.s)==0;
}
bool encodePreparedReceipt(const PreparedReceipt &r,uint8_t out[PreparedReceiptSize]) {
  if (!out || !r.attempt || !r.generation || !r.credentialEpoch || !r.budgetMs || r.budgetMs>60000 ||
      r.capabilities!=PreparedReceiptCapabilities || !nonzero(r.operation) || !nonzero(r.device) ||
      !nonzero(r.master) || !nonzero(r.network) || !nonzero(r.binding) || !nonzero(r.oobCommitment) ||
      !nonzero(r.challenge)) return false;
  memset(out,0,PreparedReceiptSize); put32(out,0x3152504d); out[4]=1; out[6]=uint8_t(PreparedReceiptSize);
  memcpy(out+8,r.operation.data(),16); put32(out+24,r.attempt); memcpy(out+28,r.device.data(),16);
  memcpy(out+44,r.master.data(),16); memcpy(out+60,r.network.data(),16); memcpy(out+76,r.binding.data(),16);
  put64(out+92,r.generation); put32(out+100,r.credentialEpoch); memcpy(out+104,r.oobCommitment.data(),32);
  memcpy(out+136,r.challenge.data(),32); put32(out+168,r.capabilities); put32(out+172,r.budgetMs);
  return true;
}
bool decodePreparedReceipt(const uint8_t *body,size_t size,PreparedReceipt &receipt) {
  receipt={};
  if (!body || size!=PreparedReceiptSize) return false;
  PreparedReceipt decoded{};
  memcpy(decoded.operation.data(),body+8,16); decoded.attempt=get32(body+24);
  memcpy(decoded.device.data(),body+28,16); memcpy(decoded.master.data(),body+44,16);
  memcpy(decoded.network.data(),body+60,16); memcpy(decoded.binding.data(),body+76,16);
  decoded.generation=get64(body+92); decoded.credentialEpoch=get32(body+100);
  memcpy(decoded.oobCommitment.data(),body+104,32); memcpy(decoded.challenge.data(),body+136,32);
  decoded.capabilities=get32(body+168); decoded.budgetMs=get32(body+172);
  uint8_t canonical[PreparedReceiptSize]{};
  if (!encodePreparedReceipt(decoded,canonical) || memcmp(canonical,body,size)) return false;
  receipt=decoded; return true;
}
bool signPreparedReceipt(const Key &secret,const PreparedReceipt &receipt,Signature &signature,RandomSource random) {
  signature.fill(0);
  constexpr char domain[]="PUMP-MASTER-READY-V1";
  uint8_t message[sizeof(domain)-1+PreparedReceiptSize]{}; PublicKey pub{}; Id id{};
  if (!identityPublicKey(secret,pub,random) || !identityFingerprint(pub,id) || id!=receipt.master ||
      !encodePreparedReceipt(receipt,message+sizeof(domain)-1)) return false;
  memcpy(message,domain,sizeof(domain)-1);
  return identitySign(secret,message,sizeof(message),signature,random);
}
bool verifyPreparedReceipt(const PublicKey &trusted,const PreparedReceipt &expected,
                           const uint8_t *body,size_t size,const Signature &signature) {
  constexpr char domain[]="PUMP-MASTER-READY-V1";
  uint8_t message[sizeof(domain)-1+PreparedReceiptSize]{}; Id id{};
  if (!body || size!=PreparedReceiptSize || !identityFingerprint(trusted,id) || expected.master!=id ||
      !encodePreparedReceipt(expected,message+sizeof(domain)-1) ||
      memcmp(message+sizeof(domain)-1,body,size)) return false;
  memcpy(message,domain,sizeof(domain)-1); return identityVerify(trusted,message,sizeof(message),signature);
}
JournalState MasterIdentityStore::load(Id expectedDevice) {
  state_=JournalState::Fault; wipe(secret_.data(),secret_.size()); identity_={}; identity_.device=expectedDevice;
  if (!random_ || !nonzero(expectedDevice)) return state_;
  const auto loaded=journal_.load(); if (loaded!=JournalState::Ready) return state_=loaded;
  const auto *p=journal_.data();
  if (journal_.size()!=144 || get32(p)!=0x3144494d || p[4]!=1 || p[5] || p[6] || p[7] ||
      memcmp(p+8,expectedDevice.data(),16) || !get32(p+24) || p[141] || p[142] || p[143]) return state_;
  Key candidate{}; PublicKey derived{},saved{}; Id fingerprint{};
  memcpy(candidate.data(),p+28,32); memcpy(saved.data(),p+60,65);
  const bool valid=identityPublicKey(candidate,derived,random_) && derived==saved && identityFingerprint(saved,fingerprint);
  if (valid) secret_=candidate;
  wipe(candidate.data(),candidate.size());
  if (!valid) return state_;
  memcpy(identity_.network.data(),p+125,16);
  if (!nonzero(identity_.network)) { wipe(secret_.data(),secret_.size()); return state_; }
  identity_.epoch=get32(p+24); identity_.publicKey=saved; identity_.master=fingerprint;
  return state_=JournalState::Ready;
}
bool MasterIdentityStore::initialize() {
  if (state_!=JournalState::Empty || !random_) return false;
  Ec ec; std::array<uint8_t,144> record{};
  Key secret{}; PublicKey pub{}; Id network{};
  const bool generated=ec.load() && mbedtls_ecp_gen_keypair(&ec.group,&ec.secret,&ec.point,rng,&random_)==0 &&
    mbedtls_mpi_write_binary(&ec.secret,secret.data(),secret.size())==0 &&
    identityPublicKey(secret,pub,random_) && random_(network.data(),network.size()) && nonzero(network);
  if (!generated) { wipe(secret.data(),secret.size()); return false; }
  put32(record.data(),0x3144494d); record[4]=1; memcpy(record.data()+8,identity_.device.data(),16);
  put32(record.data()+24,1); memcpy(record.data()+28,secret.data(),32);
  memcpy(record.data()+60,pub.data(),65); memcpy(record.data()+125,network.data(),16);
  wipe(secret.data(),secret.size()); state_=JournalState::Fault;
  const bool saved=journal_.save(record.data(),record.size()); wipe(record.data(),record.size());
  return saved && load(identity_.device)==JournalState::Ready;
}
bool MasterIdentityStore::prove(const Proof &challenge,Signature &signature) const {
  signature.fill(0); uint8_t message[169]{};
  return state_==JournalState::Ready && proofMessage(identity_,challenge,message) &&
    identitySign(secret_,message,sizeof(message),signature,random_);
}
bool verifyMasterProof(const MasterIdentity &trusted,const Proof &challenge,const Signature &signature) {
  uint8_t message[169]{};
  return proofMessage(trusted,challenge,message) && identityVerify(trusted.publicKey,message,sizeof(message),signature);
}
}
