#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace pump::recovery {
constexpr uint16_t RecoveryVersion=1;
using Key=std::array<uint8_t,32>;
using Id=std::array<uint8_t,16>;
using Proof=std::array<uint8_t,32>;
struct Challenge {
  Id device{};
  uint32_t credentialEpoch=0;
  Key clientNonce{},deviceNonce{};
};
bool equalSecret(const uint8_t *a,const uint8_t *b,size_t size);
bool makeProof(const Key &key,const Challenge &challenge,bool device,Proof &proof);
// Instantiate only per connection. Caller supplies fresh CSPRNG nonces and
// destroys it on disconnect/timeout. No transport handler is authorized yet.
class SecureSession {
 public:
  static constexpr size_t MaxPlain=512, Header=20, Tag=16, MaxFrame=Header+MaxPlain+Tag;
  ~SecureSession() { clear(); }
  SecureSession()=default;
  SecureSession(const SecureSession &)=delete;
  SecureSession &operator=(const SecureSession &)=delete;
  bool authenticate(bool client,const Key &key,const Challenge &challenge,const Proof &peerProof);
  bool seal(const uint8_t *plain,size_t size,uint8_t *frame,size_t &capacity);
  bool open(const uint8_t *frame,size_t size,uint8_t *plain,size_t &capacity);
  void clear();
  bool ready() const { return ready_; }
 private:
  Key txKey_{},rxKey_{};
  std::array<uint8_t,8> id_{};
  uint64_t tx_=1,rx_=1;
  bool client_=false,ready_=false;
};
}
