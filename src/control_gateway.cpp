#include "control_gateway.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "recovery_credentials.h"
#include <cstdio>
#include <cstring>

namespace pump {
bool ControlGateway::init(uint32_t, const char *name) {
  if (!name || !name[0]) return false;
  commands_ = xQueueCreate(12, sizeof(CommandLine));
  output_ = xQueueCreate(32, 192);
  if (!commands_ || !output_) return false;
  usb_serial_jtag_driver_config_t usb{};
  usb.tx_buffer_size = 1024;
  usb.rx_buffer_size = 1024;
  if (usb_serial_jtag_driver_install(&usb) != ESP_OK) return false;
  // Send ESP_LOG output through the same buffered USB driver as command replies.
  usb_serial_jtag_vfs_use_driver();
  ESP_LOGI("gateway", "Slave USB Serial/JTAG maintenance ready: %s; phone connects to Master", name);
  return xTaskCreatePinnedToCore([](void *p) { static_cast<ControlGateway *>(p)->ioWorker(); },
    "console_usb", 4096, this, 3, nullptr, 0) == pdPASS;
}
bool ControlGateway::submit(const char *text, size_t length, bool bluetooth) {
  // No phone business GATT endpoint on a slave; Mesh provisioning remains available.
  if (bluetooth || !text || !length || length >= sizeof(CommandLine::text) || memchr(text, 0, length)) return false;
  CommandLine cmd;
  memcpy(cmd.text, text, length);
  for (size_t i = 0; i < length; ++i) if (cmd.text[i] == '\r' || cmd.text[i] == '\n') {
    if (i != length - 1) return false;
    cmd.text[i] = 0;
  }
  // estop runs on the local application task, before queued maintenance requests.
  const bool stop = !strcmp(cmd.text, "estop") || !strncmp(cmd.text, "stop ", 5);
  if ((stop ? xQueueSendToFront(commands_, &cmd, 0) : xQueueSend(commands_, &cmd, 0)) != pdTRUE) {
    if (stop) overflow_ = true;
    recovery::wipe(cmd.text,sizeof(cmd.text));
    return false;
  }
  recovery::wipe(cmd.text,sizeof(cmd.text));
  return true;
}
bool ControlGateway::receive(CommandLine &cmd) { return xQueueReceive(commands_, &cmd, 0) == pdTRUE; }
void ControlGateway::publish(const char *text) {
  char line[192] = {};
  snprintf(line, sizeof(line), "%s\n", text);
  xQueueSend(output_, line, 0);
}
void ControlGateway::ioWorker() {
  char input[160] = {};
  size_t used = 0;
  bool discarding = false;
  for (;;) {
    uint8_t bytes[32];
    const int n = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), pdMS_TO_TICKS(5));
    for (int i = 0; i < n; ++i) {
      const char c = char(bytes[i]);
      if (c == '\r' || c == '\n') {
        if (used && !discarding) submit(input, used, false);
        recovery::wipe(input,sizeof(input));
        used = 0;
        discarding = false;
      } else if (used < sizeof(input) - 1 && !discarding) input[used++] = c;
      else { discarding = true; recovery::wipe(input,sizeof(input)); used = 0; }
    }
    char out[192];
    if (xQueueReceive(output_, out, 0) == pdTRUE) {
      // USB may be unplugged in service. Bound console waits independently of control.
      usb_serial_jtag_write_bytes(out, strlen(out), pdMS_TO_TICKS(5));
    }
  }
}
}
