#include "application.h"
#include "esp_log.h"
#include "freertos/task.h"
using namespace pump;
extern "C" void app_main() {
  static Storage storage;
  static Hardware hardware(storage);
  hardware.safeOff();
  if (!storage.open(board::ProfileId) || !hardware.init()) { ESP_LOGE("boot", "slave storage/profile/hardware invalid: valve OFF; NVS preserved"); return; }
  auto config = board::valveConfig(); config.k = storage.state().k; config.version = storage.state().kVersion;
  static FlowJobController valve(hardware, config);
  valve.boot(storage.state().fence, storage.state().revisionBase);
  if (storage.state().resultValid) valve.restore(storage.state().result);
  static MeshTransport mesh;
  static ControlGateway gateway;
  if (!mesh.init(storage.state().master) || !gateway.init(storage.state().gatewayPin, mesh.deviceName())) { hardware.safeOff(); ESP_LOGE("boot", "slave Mesh/gateway initialization failed: valve OFF"); return; }
  ESP_LOGI("boot", "Mesh identity: %s; phone connects to the master", mesh.deviceName());
  static Application application(storage, hardware, mesh, gateway, valve);
  constexpr BaseType_t controlCore = portNUM_PROCESSORS > 1 ? 1 : 0;
  if (xTaskCreatePinnedToCore([](void *arg) { static_cast<Application *>(arg)->run(); }, "slave_control", 20480, &application, 10, nullptr, controlCore) != pdPASS) { hardware.safeOff(); ESP_LOGE("boot", "slave control task allocation failed: valve OFF"); }
}
