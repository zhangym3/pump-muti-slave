#pragma once
#include "protocol.h"
#include "flow_job_controller.h"
#include "control_request.h"
#include "nvs.h"
#include "recovery_credentials.h"
#include "recovery_nvs.h"
namespace pump {
// These types remain solely to decode and preserve the deployed format=1 blob.
// They are not used as a scheduler on the C3 slave.
enum class SchedulePhase : uint8_t { Idle, Preparing, Opening, Supplying, Draining, Between, Complete, Failed };
struct JobReport {
  JobKey key;
  uint32_t selected = 0, complete = 0, failed = 0, unknown = 0;
  SchedulePhase phase = SchedulePhase::Idle;
};
struct NodeConfig { uint16_t address = 0; uint32_t k = 5880, version = 1; };
struct PersistentState {
  uint32_t magic = 0x504D5431, format = 1, profile = 0, epoch = 0, jobCounter = 0;
  uint16_t master = 1;
  uint32_t gatewayPin = 0;
  uint32_t k = 5880, kVersion = 1;
  uint32_t revisionBase = 0;
  JobKey fence;
  bool resultValid = false;
  ValveStatus result;
  NodeConfig nodes[MaxNodes] = {};
  ClientRecord clients[16] = {};
  JobReport lastJob;
  ValveStatus lastResults[MaxNodes] = {};
};
class Storage {
 public:
  bool open(uint32_t profile);
  const PersistentState &state() const { return state_; }
  bool write(const PersistentState &next);
  bool commissioned() const { return commissioned_; }
  bool setCommissioned(bool value);
  bool fence(JobKey key);
  bool result(const ValveStatus &s);
  recovery::CredentialStore &recoveryCredentials() { return recoveryCredentials_; }
 private:
  recovery::NvsBlobStore recoveryBackend_;
  recovery::Journal recoveryJournal_{recoveryBackend_};
  recovery::CredentialStore recoveryCredentials_{recoveryJournal_};
  nvs_handle_t handle_ = 0;
  PersistentState state_;
  bool commissioned_ = false;
};
}
