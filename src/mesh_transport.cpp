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
#include "esp_ble_mesh_tracked.h"
#include "esp_timer.h"
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
  {ESP_BLE_MESH_MODEL_OP_3(0xD0, CompanyId), 4, 0},
  {ESP_BLE_MESH_MODEL_OP_3(0xD1, CompanyId), 12, 0},
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
  if (code < uint8_t(Op::Prepare) || code > uint8_t(Op::Capabilities)) return;
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
  if (!rx_ || !urgentRx_) return false;
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
  if (!ready()) return false;
  portENTER_CRITICAL(&txLock_);
  const bool ok = tx_.enqueue(p, uint32_t(esp_timer_get_time()/1000));
  portEXIT_CRITICAL(&txLock_);
  return ok;
}
void MeshTransport::fence(JobKey job) {
  portENTER_CRITICAL(&txLock_); tx_.fence(job); portEXIT_CRITICAL(&txLock_);
}
bool MeshTransport::healthy() const {
  portENTER_CRITICAL(&txLock_); const bool ok = tx_.healthy(); portEXIT_CRITICAL(&txLock_); return ok;
}
MeshTxGovernor::Metrics MeshTransport::metrics() const {
  portENTER_CRITICAL(&txLock_); const auto m = tx_.metrics(); portEXIT_CRITICAL(&txLock_); return m;
}
MeshTxGovernor::Diagnostics MeshTransport::diagnostics() const {
  portENTER_CRITICAL(&txLock_);
  const auto d = tx_.diagnostics(uint32_t(esp_timer_get_time()/1000));
  portEXIT_CRITICAL(&txLock_); return d;
}
bool MeshTransport::takeSubmission(MeshSubmission &out) {
  portENTER_CRITICAL(&txLock_);
  const bool ok = tx_.collect(uint32_t(esp_timer_get_time()/1000), out);
  portEXIT_CRITICAL(&txLock_); return ok;
}
void MeshTransport::trackedEvent(uint32_t ticket, uint8_t event, int error) {
  portENTER_CRITICAL(&txLock_); tx_.event(ticket,event,error,uint32_t(esp_timer_get_time()/1000)); portEXIT_CRITICAL(&txLock_);
}
extern "C" void esp_ble_mesh_tracked_send_event(uint32_t ticket, uint8_t event, int error) {
  if (instance) instance->trackedEvent(ticket,event,error);
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
    MeshTxGovernor::Dispatch d;
    bool dispatch = false;
    if (ready()) {
      // This lock is the commit point shared with fence(). SDK/BTC may block;
      // an already committed dispatch remains tracked across a later stop.
      portENTER_CRITICAL(&txLock_);
      dispatch = tx_.next(uint32_t(esp_timer_get_time()/1000), d);
      portEXIT_CRITICAL(&txLock_);
    }
    if (dispatch) {
      esp_ble_mesh_msg_ctx_t ctx{};
      ctx.net_idx=0; ctx.app_idx=0; ctx.addr=d.packet.peer; ctx.send_ttl=3;
      const auto err = esp_ble_mesh_server_model_send_msg_tracked(&vendorModels[0], &ctx,
        ESP_BLE_MESH_MODEL_OP_3(uint8_t(d.packet.op), CompanyId), d.packet.size, d.packet.bytes, d.ticket);
      if (err != ESP_OK) trackedEvent(d.ticket, ESP_BLE_MESH_TRACKED_REJECTED, err);
    }
    for (unsigned i=0;i<4;++i) {
      MeshTxGovernor::Trace t;
      portENTER_CRITICAL(&txLock_); const bool found=tx_.takeTrace(t); portEXIT_CRITICAL(&txLock_);
      if (!found) break;
      ESP_LOGI("mesh_tx", "t=%lu ticket=%lu peer=%04x op=%02x event=%u job=%lu:%lu seq=%lu rev=%lu issued=%lu age=%lu queued=%lu wait=%lu seg=%u err=%d",
        (unsigned long)t.at,(unsigned long)t.ticket,t.peer,t.op,t.event,(unsigned long)t.job.epoch,
        (unsigned long)t.job.id,(unsigned long)t.sequence,(unsigned long)t.revision,(unsigned long)t.issuedAt,
        (unsigned long)(t.at-t.issuedAt),(unsigned long)t.queuedAt,(unsigned long)(t.issuedAt-t.queuedAt),t.segmented,t.error);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
}
