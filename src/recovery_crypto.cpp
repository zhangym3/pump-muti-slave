#include "recovery_crypto.h"
#include "recovery_journal.h"
#include "mbedtls/md.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/gcm.h"
#include "mbedtls/platform_util.h"
#include <cstring>
namespace pump::recovery {
namespace {
constexpr char Domain[]="PUMP-RECOVERY-V1";
constexpr size_t TranscriptSize=102;
template<size_t N> bool nonzero(const std::array<uint8_t,N> &a) {
  uint8_t v=0; for (auto b:a) v|=b; return v!=0;
}
bool transcript(const Challenge &c,uint8_t out[TranscriptSize]) {
  if (!c.credentialEpoch || !nonzero(c.device) || !nonzero(c.clientNonce) || !nonzero(c.deviceNonce) ||
      c.clientNonce==c.deviceNonce) return false;
  memcpy(out,Domain,16); out[16]=uint8_t(RecoveryVersion); out[17]=uint8_t(RecoveryVersion>>8);
  memcpy(out+18,c.device.data(),16); put32(out+34,c.credentialEpoch);
  memcpy(out+38,c.clientNonce.data(),32); memcpy(out+70,c.deviceNonce.data(),32);
  return true;
}
void nonce(uint8_t out[12],uint8_t direction,uint64_t sequence) {
  out[0]='R';out[1]='E';out[2]='C';out[3]=direction;put64(out+4,sequence);
}
}
bool equalSecret(const uint8_t *a,const uint8_t *b,size_t size) {
  if (!a || !b) return false;
  volatile uint8_t diff=0; for (size_t i=0;i<size;++i) diff=uint8_t(diff | (a[i]^b[i]));
  return diff==0;
}
bool makeProof(const Key &key,const Challenge &challenge,bool device,Proof &proof) {
  proof.fill(0); uint8_t tr[TranscriptSize]{};
  if (!nonzero(key) || !transcript(challenge,tr)) return false;
  const char *purpose=device?"device-proof":"client-proof";
  const auto md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t context; mbedtls_md_init(&context);
  const bool ok=md && mbedtls_md_setup(&context,md,1)==0 &&
    mbedtls_md_hmac_starts(&context,key.data(),key.size())==0 &&
    mbedtls_md_hmac_update(&context,reinterpret_cast<const uint8_t *>(purpose),strlen(purpose))==0 &&
    mbedtls_md_hmac_update(&context,tr,sizeof(tr))==0 && mbedtls_md_hmac_finish(&context,proof.data())==0;
  mbedtls_md_free(&context); if (!ok) proof.fill(0); return ok;
}
void SecureSession::clear() {
  mbedtls_platform_zeroize(txKey_.data(),txKey_.size());
  mbedtls_platform_zeroize(rxKey_.data(),rxKey_.size());
  id_.fill(0); tx_=rx_=1; ready_=false;
}
bool SecureSession::authenticate(bool client,const Key &key,const Challenge &challenge,const Proof &peerProof) {
  clear(); Proof expected{};
  if (!makeProof(key,challenge,client,expected)) return false;
  const bool valid=equalSecret(expected.data(),peerProof.data(),expected.size());
  mbedtls_platform_zeroize(expected.data(),expected.size()); if (!valid) return false;
  uint8_t tr[TranscriptSize]{},salt[32]{},keys[72]{};
  if (!transcript(challenge,tr)) return false;
  const auto md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  constexpr char info[]="PUMP-RECOVERY-V1/session-keys";
  const bool ok=md && mbedtls_md(md,tr,sizeof(tr),salt)==0 &&
    mbedtls_hkdf(md,salt,sizeof(salt),key.data(),key.size(),reinterpret_cast<const uint8_t *>(info),sizeof(info)-1,keys,sizeof(keys))==0;
  if (ok) {
    client_=client; memcpy(txKey_.data(),keys+(client?0:32),32); memcpy(rxKey_.data(),keys+(client?32:0),32);
    memcpy(id_.data(),keys+64,8); ready_=true;
  }
  mbedtls_platform_zeroize(keys,sizeof(keys)); return ok;
}
bool SecureSession::seal(const uint8_t *plain,size_t size,uint8_t *frame,size_t &capacity) {
  if (!ready_ || !plain || !size || size>MaxPlain || !frame || capacity<Header+size+Tag) return false;
  if (tx_==UINT64_MAX) { clear(); return false; }
  const uint8_t direction=client_?0:1;
  frame[0]=1;frame[1]=direction;frame[2]=uint8_t(size);frame[3]=uint8_t(size>>8);
  memcpy(frame+4,id_.data(),8);put64(frame+12,tx_++);
  uint8_t iv[12];nonce(iv,direction,get64(frame+12));
  mbedtls_gcm_context context;mbedtls_gcm_init(&context);
  const bool ok=mbedtls_gcm_setkey(&context,MBEDTLS_CIPHER_ID_AES,txKey_.data(),256)==0 &&
    mbedtls_gcm_crypt_and_tag(&context,MBEDTLS_GCM_ENCRYPT,size,iv,sizeof(iv),frame,Header,plain,frame+Header,Tag,frame+Header+size)==0;
  mbedtls_gcm_free(&context);
  if (!ok) { mbedtls_platform_zeroize(frame,Header+size+Tag);clear();capacity=0;return false; }
  capacity=Header+size+Tag;return true;
}
bool SecureSession::open(const uint8_t *frame,size_t size,uint8_t *plain,size_t &capacity) {
  if (!ready_ || !frame || !plain || size<=Header+Tag || size>MaxFrame) return false;
  const size_t length=size-Header-Tag; const uint8_t direction=client_?1:0;
  if (capacity<length || frame[0]!=1 || frame[1]!=direction || (size_t(frame[2])|(size_t(frame[3])<<8))!=length ||
      !equalSecret(frame+4,id_.data(),8) || get64(frame+12)!=rx_ || rx_==UINT64_MAX) return false;
  uint8_t iv[12];nonce(iv,direction,rx_);
  mbedtls_gcm_context context;mbedtls_gcm_init(&context);
  const bool ok=mbedtls_gcm_setkey(&context,MBEDTLS_CIPHER_ID_AES,rxKey_.data(),256)==0 &&
    mbedtls_gcm_auth_decrypt(&context,length,iv,sizeof(iv),frame,Header,frame+Header+length,Tag,frame+Header,plain)==0;
  mbedtls_gcm_free(&context);
  if (!ok) { mbedtls_platform_zeroize(plain,length);capacity=0;return false; }
  ++rx_;capacity=length;return true;
}
}
