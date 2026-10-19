#pragma once
#include "control_gateway.h"
#include "control_request.h"
#include "hardware.h"
#include "mesh_transport.h"
namespace pump {
class Application {
 public:
  Application(Storage &store, Hardware &hw, MeshTransport &mesh, ControlGateway &gateway,
              FlowJobController &valve)
    : store_(store), hw_(hw), mesh_(mesh), gateway_(gateway), valve_(valve) {}
  void run();
 private:
  void incoming(const Packet &p, uint32_t now);
  void command(CommandLine &c, uint32_t now);
  void status(bool force = false);
  void reject(JobKey key, Op command, Reply reply);
  void periodic(uint32_t now);
  void localState(uint32_t now);
  Storage &store_;
  Hardware &hw_;
  MeshTransport &mesh_;
  ControlGateway &gateway_;
  FlowJobController &valve_;
  uint32_t masterEpoch_ = 0, masterSeenAt_ = 0, helloAt_ = 0, eventAt_ = 0, summaryAt_ = 0;
  uint32_t ackedRevision_ = UINT32_MAX, sentRevision_ = UINT32_MAX, savedRevision_ = UINT32_MAX;
  uint32_t eventAttempts_ = 0;
  JobKey ackedKey_, sentKey_, savedKey_;
  JobKey leaseAckKey_;
  uint32_t leaseAckSequence_ = 0, leaseAckReceivedAt_ = 0;
  Reply leaseAckReply_ = Reply::Stale;
  bool leaseAckPending_ = false;
  bool storageHealthy_ = true, savedJob_ = true;
};
}
