#include "recovery_gatt.h"
#include "recovery_channel.h"
#include "esp_ble_mesh_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gap_ble_api.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace pump {
namespace {
// a64d1001-2f9c-4b6e-8db7-a570530e1000 (on-air little endian).
uint8_t serviceUuid[16] = {0x00,0x10,0x0e,0x53,0x70,0xa5,0xb7,0x8d,0x6e,0x4b,0x9c,0x2f,0x01,0x10,0x4d,0xa6};
uint8_t infoUuid[16] = {0x00,0x10,0x0e,0x53,0x70,0xa5,0xb7,0x8d,0x6e,0x4b,0x9c,0x2f,0x02,0x10,0x4d,0xa6};
uint8_t rxUuid[16] = {0x00,0x10,0x0e,0x53,0x70,0xa5,0xb7,0x8d,0x6e,0x4b,0x9c,0x2f,0x03,0x10,0x4d,0xa6};
uint8_t txUuid[16] = {0x00,0x10,0x0e,0x53,0x70,0xa5,0xb7,0x8d,0x6e,0x4b,0x9c,0x2f,0x04,0x10,0x4d,0xa6};
uint16_t primaryUuid = ESP_GATT_UUID_PRI_SERVICE, declarationUuid = ESP_GATT_UUID_CHAR_DECLARE;
uint8_t readProperty = ESP_GATT_CHAR_PROP_BIT_READ;
uint8_t writeProperty = ESP_GATT_CHAR_PROP_BIT_WRITE;
// v1 discovery schema, capability=0 (authenticated recovery not yet enabled), UUID.
uint8_t publicInfo[20] = {1, 0, 0, 0};
esp_gatts_attr_db_t attributes[7]{};
uint16_t handles[7]{};
std::atomic<bool> serviceReady{false}, pending{false}, advertising{false}, connected{false};
std::atomic<uint16_t> connection{0};
std::atomic<int64_t> connectedAt{0};
std::atomic<uint32_t> generation{0};
esp_gatt_if_t gattInterface = ESP_GATT_IF_NONE;
uint8_t advertisingIndex = 0;
char name[24]{};
struct Request {
  uint32_t generation=0,transaction=0;
  uint16_t connection=0,handle=0,offset=0,length=0;
  bool write=false;
  uint8_t bytes[20]{};
};
QueueHandle_t requests=nullptr;
bool randomBytes(uint8_t *p,size_t size) { esp_fill_random(p,size); return true; }
recovery::RecoveryEndpoint endpoint(randomBytes);
bool authAvailable=false;

void queueRequest(const Request &request) {
  if (xQueueSend(requests,&request,0)!=pdTRUE) esp_ble_gatts_close(gattInterface,request.connection);
}

void advertise() {
  if (!serviceReady || connected || advertising || pending.exchange(true)) return;
  esp_ble_mesh_ble_adv_param_t params{};
  params.interval = 0xa0; params.adv_type = ADV_TYPE_IND;
  params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
  // SDK requires at least 100 ms; 120 ms also leaves scheduler margin.
  params.duration = 120; params.period = 500;
  params.count = ESP_BLE_MESH_BLE_ADV_INFINITE;
  params.priority = ESP_BLE_MESH_BLE_ADV_PRIO_LOW;
  esp_ble_mesh_ble_adv_data_t data{};
  data.adv_data[0] = 2; data.adv_data[1] = 1; data.adv_data[2] = 6;
  data.adv_data[3] = 17; data.adv_data[4] = 7;
  memcpy(data.adv_data + 5, serviceUuid, 16); data.adv_data_len = 21;
  const size_t length = strlen(name);
  data.scan_rsp_data[0] = uint8_t(length + 1); data.scan_rsp_data[1] = 9;
  memcpy(data.scan_rsp_data + 2, name, length); data.scan_rsp_data_len = uint8_t(length + 2);
  if (esp_ble_mesh_start_ble_advertising(&params, &data) != ESP_OK) pending = false;
}
void bleEvent(esp_ble_mesh_ble_cb_event_t event, esp_ble_mesh_ble_cb_param_t *p) {
  if (!p) return;
  if (event == ESP_BLE_MESH_START_BLE_ADVERTISING_COMP_EVT) {
    pending = false;
    advertising = p->start_ble_advertising_comp.err_code == 0;
    advertisingIndex = p->start_ble_advertising_comp.index;
    ESP_LOGI("recovery", "discovery advertising active=%u name=%s", unsigned(advertising.load()), name);
    if (advertising && connected) esp_ble_mesh_stop_ble_advertising(advertisingIndex);
  } else if (event == ESP_BLE_MESH_STOP_BLE_ADVERTISING_COMP_EVT) {
    advertising = false;
  }
}
void gattEvent(esp_gatts_cb_event_t event, esp_gatt_if_t iface, esp_ble_gatts_cb_param_t *p) {
  if (!p) return;
  if (event == ESP_GATTS_REG_EVT) {
    if (p->reg.status != ESP_GATT_OK || p->reg.app_id != 0x5052) return;
    gattInterface = iface;
    esp_ble_gatts_create_attr_tab(attributes, iface, 7, 0);
  } else if (iface == gattInterface && event == ESP_GATTS_CREAT_ATTR_TAB_EVT) {
    if (p->add_attr_tab.status == ESP_GATT_OK && p->add_attr_tab.num_handle == 7) {
      memcpy(handles,p->add_attr_tab.handles,sizeof(handles));
      esp_ble_gatts_start_service(p->add_attr_tab.handles[0]);
    }
  } else if (iface == gattInterface && event == ESP_GATTS_START_EVT) {
    serviceReady = p->start.status == ESP_GATT_OK;
    advertise();
  } else if (iface == gattInterface && event == ESP_GATTS_CONNECT_EVT) {
    // Never wrap a generation onto a queued request from an old connection.
    if (connected || generation.load()==UINT32_MAX) { esp_ble_gatts_close(iface,p->connect.conn_id); return; }
    connection = p->connect.conn_id; connectedAt = esp_timer_get_time(); connected = true;
    ++generation;
    if (advertising) esp_ble_mesh_stop_ble_advertising(advertisingIndex);
    ESP_LOGI("recovery", "connected; authenticated_status=%u recovery_enabled=0",authAvailable);
  } else if (iface == gattInterface && event == ESP_GATTS_DISCONNECT_EVT) {
    if (p->disconnect.conn_id!=connection.load()) return;
    connected = false;
    if (generation.load()!=UINT32_MAX) ++generation;
    ESP_LOGI("recovery", "discovery disconnected reason=0x%x", unsigned(p->disconnect.reason));
  } else if (iface==gattInterface && event==ESP_GATTS_WRITE_EVT) {
    if (p->write.handle!=handles[4]) return;
    if (!connected || p->write.conn_id!=connection.load()) { esp_ble_gatts_close(iface,p->write.conn_id); return; }
    if (!p->write.need_rsp || p->write.is_prep || p->write.offset || p->write.len>20 || !p->write.len) {
      if (p->write.need_rsp) esp_ble_gatts_send_response(iface,p->write.conn_id,p->write.trans_id,ESP_GATT_REQ_NOT_SUPPORTED,nullptr);
      esp_ble_gatts_close(iface,p->write.conn_id); return;
    }
    Request request; request.generation=generation.load(); request.transaction=p->write.trans_id;
    request.connection=p->write.conn_id; request.handle=p->write.handle; request.write=true;
    request.length=p->write.len; memcpy(request.bytes,p->write.value,request.length); queueRequest(request);
  } else if (iface==gattInterface && event==ESP_GATTS_READ_EVT && p->read.handle==handles[6]) {
    if (!connected || p->read.conn_id!=connection.load()) { esp_ble_gatts_close(iface,p->read.conn_id); return; }
    Request request; request.generation=generation.load(); request.transaction=p->read.trans_id;
    request.connection=p->read.conn_id; request.handle=p->read.handle; request.offset=p->read.offset; queueRequest(request);
  } else if (iface==gattInterface && event==ESP_GATTS_EXEC_WRITE_EVT) {
    esp_ble_gatts_send_response(iface,p->exec_write.conn_id,p->exec_write.trans_id,ESP_GATT_REQ_NOT_SUPPORTED,nullptr);
  }
}
void discoveryWorker(void *) {
  recovery::RecoveryInbox inbox; recovery::RecoveryOutbox outbox;
  uint32_t activeGeneration=0;
  bool closing=false,authentication=false;
  for (;;) {
    Request request;
    const bool received=xQueueReceive(requests,&request,pdMS_TO_TICKS(100))==pdTRUE;
    const uint64_t now=uint64_t(esp_timer_get_time()/1000);
    const auto current=generation.load();
    if (connected && activeGeneration!=current) {
      activeGeneration=current; closing=false; inbox.reset(); outbox.clear(); endpoint.disconnect();
      authentication=connected && authAvailable && endpoint.connect(now);
      if (connected && authAvailable && !authentication) closing=true;
    }
    if (!connected) { activeGeneration=0; endpoint.disconnect(); inbox.reset(); outbox.clear(); }
    if (connected && ((authentication && endpoint.expired(now)) || inbox.expired(now) ||
        (!authentication && esp_timer_get_time()-connectedAt.load()>10000000))) closing=true;
    if (received && connected && request.generation==current && request.connection==connection.load()) {
      esp_gatt_status_t status=ESP_GATT_OK;
      esp_gatt_rsp_t response{};
      if (closing || !authentication) status=ESP_GATT_INSUF_AUTHORIZATION;
      else if (request.write) {
        if (request.bytes[0]==2) {
          if (!outbox.pull(request.bytes,request.length)) status=ESP_GATT_INVALID_PDU;
        } else if (!inbox.push(request.bytes,request.length,now)) status=ESP_GATT_INVALID_PDU;
        else if (inbox.ready()) {
          uint8_t output[recovery::RecoveryMessageMax]{}; size_t size=sizeof(output);
          if (!endpoint.handle(inbox.data(),inbox.size(),output,size,now) || !outbox.set(inbox.id(),output,size))
            status=ESP_GATT_INSUF_AUTHORIZATION;
          recovery::wipe(output,sizeof(output)); inbox.consume();
        }
      } else if (request.offset) status=ESP_GATT_INVALID_OFFSET;
      else {
        response.attr_value.handle=request.handle;
        response.attr_value.len=uint16_t(outbox.read(response.attr_value.value));
        if (!response.attr_value.len) status=ESP_GATT_INVALID_OFFSET;
      }
      // A callback may have disconnected/reconnected while crypto ran. Never
      // send an old response into a reused SDK connection id.
      if (generation.load()==current && connected) {
        esp_ble_gatts_send_response(gattInterface,request.connection,request.transaction,status,
          !request.write && status==ESP_GATT_OK ? &response : nullptr);
        if (status!=ESP_GATT_OK) closing=true;
      }
    }
    if (closing && connected && generation.load()==current) esp_ble_gatts_close(gattInterface,connection);
    advertise();
  }
}
}
bool initRecoveryDiscovery(const uint8_t deviceUuid[16],const recovery::Credential *credential) {
  if (!deviceUuid) return false;
  if (credential && memcmp(credential->device.data(),deviceUuid,16)) return false;
  authAvailable=credential!=nullptr; endpoint.configure(credential);
  requests=xQueueCreate(8,sizeof(Request)); if (!requests) return false;
  memcpy(publicInfo + 4, deviceUuid, 16);
  snprintf(name, sizeof(name), "PUMP_REC_%02X%02X", deviceUuid[8], deviceUuid[9]);
  auto attr = [](size_t i, uint16_t uuidLength, uint8_t *uuid, uint16_t length, uint8_t *value) {
    attributes[i].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
    auto &a = attributes[i].att_desc;
    a.uuid_length = uuidLength; a.uuid_p = uuid; a.perm = ESP_GATT_PERM_READ;
    a.max_length = length; a.length = length; a.value = value;
  };
  attr(0, 2, reinterpret_cast<uint8_t *>(&primaryUuid), 16, serviceUuid);
  attr(1, 2, reinterpret_cast<uint8_t *>(&declarationUuid), 1, &readProperty);
  attr(2, 16, infoUuid, sizeof(publicInfo), publicInfo);
  attr(3,2,reinterpret_cast<uint8_t *>(&declarationUuid),1,&writeProperty);
  attr(4,16,rxUuid,20,nullptr);
  attributes[4].attr_control.auto_rsp=ESP_GATT_RSP_BY_APP;
  attributes[4].att_desc.perm=ESP_GATT_PERM_WRITE;
  attributes[4].att_desc.length=0;
  attr(5,2,reinterpret_cast<uint8_t *>(&declarationUuid),1,&readProperty);
  attr(6,16,txUuid,20,nullptr);
  attributes[6].attr_control.auto_rsp=ESP_GATT_RSP_BY_APP;
  attributes[6].att_desc.length=0;
  return esp_ble_mesh_register_ble_callback(bleEvent) == ESP_OK &&
    esp_ble_gatts_register_callback(gattEvent) == ESP_OK &&
    esp_ble_gatts_app_register(0x5052) == ESP_OK &&
    xTaskCreate(discoveryWorker, "recovery_ble", 8192, nullptr, 2, nullptr) == pdPASS;
}
}
