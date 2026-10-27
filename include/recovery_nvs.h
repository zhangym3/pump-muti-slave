#pragma once
#include "recovery_journal.h"
#include "nvs.h"
namespace pump::recovery {
class NvsBlobStore final : public BlobStore {
 public:
  ~NvsBlobStore() override { if (handle_) nvs_close(handle_); }
  // NVS must already be initialized by the legacy storage path.
  bool open(const char *name) { return !handle_ && nvs_open(name,NVS_READWRITE,&handle_)==ESP_OK; }
  ReadResult read(const char *key,uint8_t *out,size_t &size) override {
    if (!handle_) return ReadResult::Fault;
    const auto err=nvs_get_blob(handle_,key,out,&size);
    return err==ESP_OK ? ReadResult::Found : err==ESP_ERR_NVS_NOT_FOUND ? ReadResult::Missing : ReadResult::Fault;
  }
  bool write(const char *key,const uint8_t *data,size_t size) override {
    return handle_ && nvs_set_blob(handle_,key,data,size)==ESP_OK && nvs_commit(handle_)==ESP_OK;
  }
 private:
  nvs_handle_t handle_=0;
};
}
