#include "storage.h"
#include "nvs_flash.h"
#include "esp_mac.h"
namespace pump {
bool Storage::open(uint32_t profile) {
  commissioned_ = false;
  if (nvs_flash_init() != ESP_OK || nvs_open("pumpmulti", NVS_READWRITE, &handle_) != ESP_OK) return false;
  recovery::Id uuid{0xdd,0xdd,0x50,0x4d}; uuid[10]=2;
  if (esp_read_mac(uuid.data()+4,ESP_MAC_BT)!=ESP_OK || !recoveryBackend_.open("pump_rec_cred") ||
      recoveryCredentials_.load(uuid)==recovery::JournalState::Fault) return false;
  size_t size = sizeof(state_);
  const auto err = nvs_get_blob(handle_, "state", &state_, &size);
  if (err == ESP_ERR_NVS_NOT_FOUND) { state_ = PersistentState{}; state_.profile = profile; }
  else if (err != ESP_OK || size != sizeof(state_) || state_.magic != 0x504D5431 ||
           state_.format != 1 || state_.profile != profile) return false;
  if (!unicast(state_.master) || !validK(state_.k) || !state_.kVersion) return false;
  uint8_t commissioned = 0;
  const auto commissionErr = nvs_get_u8(handle_, "commissioned", &commissioned);
  if (commissionErr == ESP_OK) {
    if (commissioned > 1) return false;
    commissioned_ = commissioned == 1;
  } else if (commissionErr == ESP_ERR_NVS_NOT_FOUND) commissioned_ = false;
  else return false;
  for (size_t i = 0; i < MaxNodes; ++i) {
    const auto &node = state_.nodes[i];
    if (!node.address) continue;
    if (!unicast(node.address) || node.address == state_.master || !validK(node.k) || !node.version) return false;
    for (size_t j = 0; j < i; ++j) if (state_.nodes[j].address == node.address) return false;
  }
  if (state_.revisionBase > UINT32_MAX - 16) return false;
  state_.revisionBase += 16;
  return write(state_);
}
bool Storage::write(const PersistentState &next) {
  if (!handle_ || nvs_set_blob(handle_, "state", &next, sizeof(next)) != ESP_OK || nvs_commit(handle_) != ESP_OK) return false;
  PersistentState verify{}; size_t size = sizeof(verify);
  if (nvs_get_blob(handle_, "state", &verify, &size) != ESP_OK || size != sizeof(verify) || memcmp(&verify, &next, size) != 0) return false;
  state_ = next; return true;
}
bool Storage::setCommissioned(bool value) {
  if (!handle_) return false;
  if (commissioned_ == value) return true;
  if (nvs_set_u8(handle_, "commissioned", value ? 1 : 0) != ESP_OK || nvs_commit(handle_) != ESP_OK) return false;
  uint8_t verify = 0;
  if (nvs_get_u8(handle_, "commissioned", &verify) != ESP_OK || verify != (value ? 1 : 0)) return false;
  commissioned_ = value;
  return true;
}
bool Storage::fence(JobKey key) { auto next = state_; next.fence = key; next.resultValid = false; return write(next); }
bool Storage::result(const ValveStatus &status) { auto next = state_; next.result = status; next.resultValid = true; return write(next); }
}
