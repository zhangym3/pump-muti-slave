#pragma once
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <cstdio>

namespace pump::recovery {
// Read-only USB diagnostic. No iteration over NVS keys or secret material.
// These are instantaneous measurements, not a worst-case capacity acceptance.
template<class Publish> void publishRecoveryResources(Publish publish) {
  char line[192]; nvs_stats_t stats{};
  const auto err=nvs_get_stats(nullptr,&stats);
  snprintf(line,sizeof(line),
    "RECOVERY_RESOURCES schema=1 nvs_err=%ld total=%lu used=%lu free=%lu available=%lu namespaces=%lu",
    (long)err,(unsigned long)stats.total_entries,(unsigned long)stats.used_entries,
    (unsigned long)stats.free_entries,(unsigned long)stats.available_entries,(unsigned long)stats.namespace_count);
  publish(line);
  constexpr uint32_t caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
  snprintf(line,sizeof(line),
    "RECOVERY_HEAP schema=1 internal_free=%lu internal_min=%lu internal_largest=%lu",
    (unsigned long)heap_caps_get_free_size(caps),(unsigned long)heap_caps_get_minimum_free_size(caps),
    (unsigned long)heap_caps_get_largest_free_block(caps));
  publish(line);
#ifdef CONFIG_BLE_MESH_PROVISIONER
  constexpr unsigned provisionerKeys=CONFIG_BLE_MESH_PROVISIONER_APP_KEY_COUNT;
#else
  constexpr unsigned provisionerKeys=0;
#endif
  snprintf(line,sizeof(line),
    "RECOVERY_LIMITS schema=1 provisioner_keys=%u model_keys=%u node_keys=%u rx_sdu=%u tx_seg=%u",
    provisionerKeys,unsigned(CONFIG_BLE_MESH_MODEL_KEY_COUNT),unsigned(CONFIG_BLE_MESH_APP_KEY_COUNT),
    unsigned(CONFIG_BLE_MESH_RX_SDU_MAX),unsigned(CONFIG_BLE_MESH_TX_SEG_MAX));
  publish(line);
}
}
