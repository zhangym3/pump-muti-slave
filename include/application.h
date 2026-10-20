#pragma once
#include "control_gateway.h"
#include "control_request.h"
#include "hardware.h"
#include "valve_mesh_session.h"
#include "mesh_transport.h"
namespace pump {
class Application {
 public:
  Application(Storage &store, Hardware &hw, MeshTransport &mesh, ControlGateway &gateway,
              FlowJobController &valve)
    : store_(store), hw_(hw), mesh_(mesh), gateway_(gateway), valve_(valve), session_(valve) {}
  void run();
 private:
  void incoming(const Packet &p, uint32_t now);
  void command(CommandLine &c, uint32_t now);
  bool status(bool force = false);

  void periodic(uint32_t now);
  void localState(uint32_t now);
  void hallState(uint32_t now, const char *event);
  void hallDiagnostics(uint32_t now);
  void meshState();
  Storage &store_;
  Hardware &hw_;
  MeshTransport &mesh_;
  ControlGateway &gateway_;
  FlowJobController &valve_;
  ValveMeshSession session_;
  uint32_t masterEpoch_ = 0, masterSeenAt_ = 0, helloAt_ = 0, eventAt_ = 0;
  uint32_t ackedRevision_ = UINT32_MAX, sentRevision_ = UINT32_MAX, savedRevision_ = UINT32_MAX;
  uint32_t eventAttempts_ = 0;
  uint32_t hallAt_ = 0, hallCount_ = 0;
  bool hallActive_ = false;
  JobKey ackedKey_, sentKey_, savedKey_;
  bool storageHealthy_ = true;
};
}
