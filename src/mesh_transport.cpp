#include "mesh_transport.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

namespace pump {
namespace {
MeshTransport *instance = nullptr;
uint8_t uuid[16] = {0xDD, 0xDD, 0x50, 0x4D};
esp_ble_mesh_cfg_srv_t config;
esp_ble_mesh_model_op_t operations[] = {
  {ESP_BLE_MESH_MODEL_OP_3(0xC0, CompanyId), 28, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC1, CompanyId), 8, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC2, CompanyId), 12, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC3, CompanyId), 9, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC4, CompanyId), 8, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC5, CompanyId), 27, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC6, CompanyId), 18, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC7, CompanyId), 12, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC8, CompanyId), 37, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xC9, CompanyId), 46, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCA, CompanyId), 9, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCB, CompanyId), 6, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCC, CompanyId), 4, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCD, CompanyId), 14, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCE, CompanyId), 13, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xCF, CompanyId), 10, 0},
  ESP_BLE_MESH_MODEL_OP_END
};
esp_ble_mesh_model_t sigModels[] = { ESP_BLE_MESH_MODEL_CFG_SRV(&config) };
esp_ble_mesh_model_t vendorModels[] = { ESP_BLE_MESH_VENDOR_MODEL(CompanyId, ModelId, operations, nullptr, nullptr) };
esp_ble_mesh_elem_t elements[] = { ESP_BLE_MESH_ELEMENT(0, sigModels, vendorModels) };
esp_ble_mesh_comp_t composition;
esp_ble_mesh_prov_t provisioning{};

void provisionCb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *p) {
  if (!p) return;
  if (event == ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT) {
    if (p->node_prov_enable_comp.err_code) {
      ESP_LOGE("mesh", "node provisioning enable failed: %d", p->node_prov_enable_comp.err_code);
    } else {
      ESP_LOGI("mesh", "node provisioning enabled (PB-ADV)");
    }
  } else if (event == ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT) {
    ESP_LOGI("mesh", "PB-ADV provisioning link opened: bearer=0x%x",
      unsigned(p->node_prov_link_open.bearer));
  } else if (event == ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT) {
    ESP_LOGW("mesh", "PB-ADV provisioning link closed: bearer=0x%x reason=0x%02x",
      unsigned(p->node_prov_link_close.bearer), unsigned(p->node_prov_link_close.reason));
  } else if (event == ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT) {
    ESP_LOGI("mesh", "slave provisioned addr=0x%04x net_idx=%u flags=0x%02x iv=%lu",
      p->node_prov_complete.addr, p->node_prov_complete.net_idx,
      unsigned(p->node_prov_complete.flags), (unsigned long)p->node_prov_complete.iv_index);
  }
}
void modelCb(esp_ble_mesh_model_cb_event_t event, esp_ble_mesh_model_cb_param_t *p) {
  if (!instance || event != ESP_BLE_MESH_MODEL_OPERATION_EVT || !p) return;
  const auto &m = p->model_operation;
  if (!m.ctx || m.ctx->net_idx != 0 || m.ctx->app_idx != 0 ||
      m.length > sizeof(Packet::bytes) || (m.opcode & 0xFFFF) != CompanyId) return;
  const uint8_t code = uint8_t(m.opcode >> 16);
  if (code < uint8_t(Op::Prepare) || code > uint8_t(Op::CommandReply)) return;
  auto in = packet(Op(code), m.ctx->addr);
  in.size = uint8_t(m.length);
  memcpy(in.bytes, m.msg, in.size);
  instance->incoming(in);
}
bool urgent(const Packet &p) {
  return p.op == Op::Close || p.op == Op::Lease || p.op == Op::LeaseAck ||
    p.op == Op::CommandReply;
}
}

bool MeshTransport::init(uint16_t masterAddress) {
  instance = this;
  master_ = masterAddress;
  rx_ = xQueueCreate(48, sizeof(Packet));
  urgentRx_ = xQueueCreate(12, sizeof(Packet));
  tx_ = xQueueCreate(48, sizeof(Packet));
  urgentTx_ = xQueueCreate(12, sizeof(Packet));
  if (!rx_ || !urgentRx_ || !tx_ || !urgentTx_) return false;
  if (esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) return false;
  esp_bt_controller_config_t bt = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  if (esp_bt_controller_init(&bt) != ESP_OK || esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK ||
      esp_bluedroid_init() != ESP_OK || esp_bluedroid_enable() != ESP_OK) return false;
  if (esp_read_mac(uuid + 4, ESP_MAC_BT) != ESP_OK) return false;
  uuid[10] = 2; // fixed slave role marker used by the master's discovery filter
  snprintf(deviceName_, sizeof(deviceName_), "PUMP_SLAVE_%02X%02X", uuid[8], uuid[9]);
  config.net_transmit = ESP_BLE_MESH_TRANSMIT(1, 20);
  config.relay = ESP_BLE_MESH_RELAY_DISABLED;
  config.relay_retransmit = ESP_BLE_MESH_TRANSMIT(1, 20);
  config.beacon = ESP_BLE_MESH_BEACON_ENABLED;
  // The slave is managed by the Master over PB-ADV.  It does not expose a
  // phone-facing Mesh GATT Proxy or a PB-GATT provisioning endpoint.
  config.gatt_proxy = ESP_BLE_MESH_GATT_PROXY_DISABLED;
  config.friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED;
  config.default_ttl = 3;
  composition.cid = CompanyId;
  composition.element_count = 1;
  composition.elements = elements;
  provisioning.uuid = uuid;
  if (esp_ble_mesh_register_prov_callback(provisionCb) != ESP_OK ||
      esp_ble_mesh_register_custom_model_callback(modelCb) != ESP_OK ||
      esp_ble_mesh_init(&provisioning, &composition) != ESP_OK) return false;
  if (esp_ble_gatt_set_local_mtu(185) != ESP_OK) return false;
  // PB-ADV carries the UUID, not a device name.  ESP-IDF's
  // esp_ble_mesh_set_unprovisioned_device_name() only puts the name in the
  // PB-GATT scan response, which is deliberately disabled for this role.
  char uuidText[33] = {};
  for (size_t i = 0; i < sizeof(uuid); ++i) snprintf(uuidText + i * 2, 3, "%02X", uuid[i]);
  ESP_LOGI("mesh", "PB-ADV only; UUID=%s identifies %s (no phone GAP name)", uuidText, deviceName_);
  if (!esp_ble_mesh_node_is_provisioned() &&
      esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV) != ESP_OK) return false;
  return xTaskCreatePinnedToCore([](void *arg) { static_cast<MeshTransport *>(arg)->worker(); },
                                 "mesh_tx", 4096, this, 5, nullptr, 0) == pdPASS;
}

bool MeshTransport::ready() const {
  if (!address()) return false;
  for (const auto key : vendorModels[0].keys) if (key == 0) return true;
  return false;
}
uint16_t MeshTransport::address() const { return esp_ble_mesh_get_primary_element_address(); }
bool MeshTransport::send(const Packet &p) {
  if (!ready() || !p.peer || p.size > sizeof(p.bytes)) return false;
  const bool ok = xQueueSend(urgent(p) ? urgentTx_ : tx_, &p, 0) == pdTRUE;
  if (!ok && urgent(p)) overflow_ = true;
  return ok;
}
void MeshTransport::incoming(const Packet &p) {
  // A slave accepts application traffic only from its persisted master address.
  if (!p.peer || p.peer != master_.load()) return;
  if (xQueueSend(urgent(p) ? urgentRx_ : rx_, &p, 0) != pdTRUE && urgent(p)) overflow_ = true;
}
bool MeshTransport::receive(Packet &p) {
  return xQueueReceive(urgentRx_, &p, 0) == pdTRUE || xQueueReceive(rx_, &p, 0) == pdTRUE;
}
void MeshTransport::worker() {
  for (;;) {
    Packet p;
    if (xQueueReceive(urgentTx_, &p, 0) != pdTRUE && xQueueReceive(tx_, &p, pdMS_TO_TICKS(10)) != pdTRUE) continue;
    if (ready()) {
      esp_ble_mesh_msg_ctx_t ctx{};
      ctx.net_idx = 0; ctx.app_idx = 0; ctx.addr = p.peer; ctx.send_ttl = 3;
      const auto err = esp_ble_mesh_server_model_send_msg(&vendorModels[0], &ctx,
        ESP_BLE_MESH_MODEL_OP_3(uint8_t(p.op), CompanyId), p.size, p.bytes);
      if (err != ESP_OK) ESP_LOGW("mesh", "send op=%02x err=%s", uint8_t(p.op), esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
}

