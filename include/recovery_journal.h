#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pump::recovery {
enum class ReadResult { Missing, Found, Fault };
enum class JournalState { Empty, Ready, Fault };
// A write must include the backend's durable commit. No erase-on-error path.
class BlobStore {
 public:
  virtual ~BlobStore() = default;
  virtual ReadResult read(const char *key, uint8_t *out, size_t &size) = 0;
  virtual bool write(const char *key, const uint8_t *data, size_t size) = 0;
};
inline uint32_t crc32(const uint8_t *data, size_t size) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return ~crc;
}
inline void put32(uint8_t *out, uint32_t value) { for (unsigned i=0;i<4;++i) out[i]=uint8_t(value>>(8*i)); }
inline uint32_t get32(const uint8_t *in) { uint32_t v=0; for (unsigned i=0;i<4;++i) v|=uint32_t(in[i])<<(8*i); return v; }
inline void put64(uint8_t *out, uint64_t value) { put32(out,uint32_t(value)); put32(out+4,uint32_t(value>>32)); }
inline uint64_t get64(const uint8_t *in) { return get32(in) | (uint64_t(get32(in+4))<<32); }

// One namespace per journal. CRC detects corruption, not malicious Flash edits.
// Only the selector commits a slot. A newer unselected slot is never promoted.
class Journal {
 public:
  static constexpr size_t MaxPayload = 768, Header = 24, SelectorSize = 24;
  explicit Journal(BlobStore &store) : store_(store) {}
  JournalState load() {
    payload_.fill(0); size_=0; revision_=0; active_=1; state_=JournalState::Fault;
    std::array<uint8_t, SelectorSize> selector{}; size_t size=selector.size();
    const auto result=store_.read("active",selector.data(),size);
    if (result==ReadResult::Missing) return state_=JournalState::Empty;
    if (result!=ReadResult::Found || size!=SelectorSize || get32(selector.data())!=0x31435250 ||
        selector[4]!=1 || selector[5]>1 || selector[6] || selector[7] ||
        !get64(selector.data()+8) || get32(selector.data()+20)!=crc32(selector.data(),20)) return state_;
    std::array<uint8_t, Header+MaxPayload> record{}; size=record.size();
    active_=selector[5]; revision_=get64(selector.data()+8);
    if (store_.read(active_ ? "slot_b" : "slot_a",record.data(),size)!=ReadResult::Found ||
        size<Header || get32(record.data())!=0x314A5250 || record[4]!=1 || record[5] || record[6] || record[7] ||
        get64(record.data()+8)!=revision_ || get32(record.data()+16)>MaxPayload ||
        size!=Header+get32(record.data()+16) ||
        get32(record.data()+20)!=crc32(record.data()+Header,size-Header) ||
        get32(selector.data()+16)!=crc32(record.data(),size)) return state_;
    size_=size-Header; memcpy(payload_.data(),record.data()+Header,size_);
    return state_=JournalState::Ready;
  }
  bool save(const uint8_t *data, size_t size) {
    if (state_==JournalState::Fault || !data || !size || size>MaxPayload || revision_==UINT64_MAX) return false;
    std::array<uint8_t,Header+MaxPayload> record{},verify{};
    const uint64_t revision=revision_+1; const uint8_t slot=active_^1;
    put32(record.data(),0x314A5250); record[4]=1; put64(record.data()+8,revision);
    put32(record.data()+16,uint32_t(size)); put32(record.data()+20,crc32(data,size));
    memcpy(record.data()+Header,data,size);
    // If any durability/readback result is uncertain, this instance is unusable.
    state_=JournalState::Fault;
    if (!store_.write(slot ? "slot_b" : "slot_a",record.data(),Header+size)) return false;
    size_t length=verify.size();
    if (store_.read(slot ? "slot_b" : "slot_a",verify.data(),length)!=ReadResult::Found ||
        length!=Header+size || memcmp(record.data(),verify.data(),length)) return false;
    std::array<uint8_t,SelectorSize> selector{},selectorVerify{};
    put32(selector.data(),0x31435250); selector[4]=1; selector[5]=slot;
    put64(selector.data()+8,revision); put32(selector.data()+16,crc32(record.data(),Header+size));
    put32(selector.data()+20,crc32(selector.data(),20));
    if (!store_.write("active",selector.data(),selector.size())) return false;
    length=selectorVerify.size();
    if (store_.read("active",selectorVerify.data(),length)!=ReadResult::Found ||
        length!=selector.size() || selector!=selectorVerify) return false;
    memcpy(payload_.data(),data,size); size_=size; revision_=revision; active_=slot; state_=JournalState::Ready;
    return true;
  }
  JournalState state() const { return state_; }
  const uint8_t *data() const { return payload_.data(); }
  size_t size() const { return size_; }
  uint64_t revision() const { return revision_; }
 private:
  BlobStore &store_;
  std::array<uint8_t,MaxPayload> payload_{};
  size_t size_=0;
  uint64_t revision_=0;
  uint8_t active_=1;
  JournalState state_=JournalState::Fault;
};
}
