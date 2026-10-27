#pragma once
#include <cstdint>
#include "recovery_credentials.h"
namespace pump {
// Discovery and authenticated status only; no Mesh reset or output endpoint.
// Reuses the Bluetooth controller already initialized by MeshTransport.
bool initRecoveryDiscovery(const uint8_t deviceUuid[16],const recovery::Credential *credential);
}
