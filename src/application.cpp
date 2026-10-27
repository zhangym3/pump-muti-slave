#include "application.h"
#include "mesh_tx_diagnostics.h"
#include "recovery_diagnostics.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

namespace pump {
namespace {
void encodeStatus(Packet &p, const ValveStatus &s) {
  Writer w{p};
  w.key(s.key); w.u32(s.pulses); w.u32(s.closePulses); w.u32(s.k); w.u32(s.revision);
  w.u8(uint8_t(s.phase)); w.u8(uint8_t(s.reason));
  w.u8((s.valveCommand ? 1 : 0) | (s.quiet ? 2 : 0) | (s.truncated ? 4 : 0));
}
}

bool Application::commitResult() {
  if (!resultCommit_.commit(valve_.status(), [&](const ValveStatus &s) { return store_.result(s); })) {
    if (storageHealthy_) gateway_.publish("STORAGE result_pending outputs_locked");
    storageHealthy_ = false;
    return false;
  }
  return storageHealthy_;
}

bool Application::status(bool force) {
  const auto current = valve_.status();
  if (current.phase != Phase::Done && current.phase != Phase::Fault) return false;
  if (!commitResult() || !resultCommit_.contains(current)) return false;
  if (!force && ackedKey_ == current.key && ackedRevision_ == current.revision) return false;
  auto packetOut = packet(Op::Result, store_.state().master);
  encodeStatus(packetOut, current);
  return mesh_.send(packetOut);
}

void Application::incoming(const Packet &packetIn, uint32_t now) {
  if (packetIn.peer != store_.state().master) return;
  if (!controlFormat(packetIn)) return;
  commitResult(); // Also covers a terminal state produced by a previous packet in this batch.
  if (packetIn.op==Op::ModeGet || packetIn.op==Op::MaintenanceRetire || packetIn.op==Op::MaintenanceGrant) {
    Reader r{packetIn}; const auto token=r.key();
    if (!token.valid()) return;
    if (packetIn.op==Op::MaintenanceGrant) {
      const auto boot=r.u32(),ttl=r.u32();
      const auto reason=valve_.localHoldReason();
      if (storageHealthy_ && mesh_.healthy() && !valve_.transportFault() && !valve_.busy() &&
          !hw_.holdButtonPressed() && elapsed(now,releaseAt_,board::HoldButtonDebounceMs) &&
          elapsed(now,flowQuietAt_,valve_.config().quietMs) &&
          (reason==Reason::None || reason==Reason::Manual)) {
        maintenance_.grant(token,boot,ttl,now);
      }
    } else {
      if (packetIn.op==Op::MaintenanceRetire) r.u32(); // Any new boot also rejects the old boot-bound grant.
      maintenance_.retire(token);
      valve_.stopLocalHold(Reason::Manual,now);
    }
    modeToken_=token; modeAt_=now; modeSentAt_=now-500;
    modeState(token,now); return;
  }
  const auto retire=[&](uint16_t peer,JobKey key,Op op) { mesh_.retire(peer,key,op); };
  if (session_.handle(packetIn,now,mesh_.healthy() && !maintenance_.allowed(now),storageHealthy_,[&](const Packet &p) { return mesh_.send(p); },retire)) {
    hallDiagnostics(nowMs()); return;
  }
  Reader reader{packetIn};
  switch (packetIn.op) {
    case Op::CapabilitiesGet: {
      const uint32_t nonce=reader.u32(); if (!reader.done() || !nonce) return;
      auto reply=packet(Op::Capabilities,store_.state().master); Writer w{reply};
      w.u32(nonce); w.u16(ProtocolRevision);
      w.u16(mesh_.healthy() && !valve_.transportFault() ? RequiredFeatures | JobCalibrationFeature | ManualCalibrationFeature : 0); w.u32(PUMP_BUILD_ID);
      mesh_.send(reply); break;
    }
    case Op::HelloReply: {
      const uint32_t epoch = reader.u32();
      reader.u8(); // master pump fault, retained for wire compatibility
      reader.u8(); // master schedule phase, retained for wire compatibility
      if (reader.done() && epoch) { masterEpoch_ = epoch; masterSeenAt_ = now; }
      break;
    }
    case Op::Commission: {
      const uint32_t request = reader.u32();
      if (!reader.done() || !request) return;
      Reply result = Reply::Ok;
      if (!mesh_.healthy() || valve_.transportFault()) result = Reply::NotReady;
      else if (!storageHealthy_) result = Reply::Storage;
      else if (store_.commissioned()) result = Reply::Ok;
      else if (!mesh_.ready() || !unicast(mesh_.address())) result = Reply::NotReady;
      else {
        result = valve_.canCommission();
        if (result == Reply::Ok && !store_.setCommissioned(true)) {
          storageHealthy_ = false;
          result = Reply::Storage;
        }
        if (result == Reply::Ok) valve_.setCommissioned(true);
      }
      auto reply = packet(Op::CommissionReply, store_.state().master);
      Writer writer{reply}; writer.u32(request); writer.u8(uint8_t(result));
      writer.u8(store_.commissioned() && storageHealthy_ ? 1 : 0);
      writer.u32(valve_.config().k); writer.u32(valve_.config().version);
      mesh_.send(reply);
      if (result == Reply::Ok) {
        auto hello = packet(Op::Hello, store_.state().master);
        Writer helloWriter{hello}; helloWriter.u32(valve_.config().k); helloWriter.u32(valve_.config().version);
        helloWriter.u8(store_.commissioned() && storageHealthy_ ? 1 : 0);
        helloWriter.u32(PUMP_BUILD_ID);
        mesh_.send(hello);
      }
      char line[96];
      snprintf(line, sizeof(line), "COMMISSION req=%lu reply=%s commissioned=%u",
        static_cast<unsigned long>(request), replyName(result), store_.commissioned() && storageHealthy_);
      gateway_.publish(line);
      break;
    }
    case Op::Close: {
      const JobKey key = reader.key();
      const Reason reason = Reason(reader.u8());
      if (reader.done() && reason <= Reason::Protocol) {
        closePreservingResult(valve_,key,reason,now,storageHealthy_);
        status(true);
      }
      break;
    }
    case Op::ResultQuery: {
      const JobKey requested = reader.key();
      const auto &saved = store_.state();
      // Queries are read-only recovery; they never expose a live or unsaved run.
      if (reader.done() && saved.resultValid && saved.result.key.valid() &&
          (!requested.valid() || requested == saved.result.key)) {
        auto result = packet(Op::Result, store_.state().master);
        encodeStatus(result, saved.result);
        mesh_.send(result);
      }
      break;
    }
    case Op::ResultAck: {
      const JobKey key = reader.key();
      const uint32_t revision = reader.u32();
      if (reader.done() && key == valve_.status().key && revision == valve_.status().revision) {
        ackedKey_ = key; ackedRevision_ = revision;
        session_.resultAck(packetIn.peer,key,retire);
      }
      break;
    }
    default:
      // Requests, responses and slave-originated telemetry are never executed here.
      break;
  }
}

void Application::localState(uint32_t now) {
  const auto current = valve_.status();
  char line[192];
  snprintf(line, sizeof(line),
    "LOCAL job=%lu:%lu phase=%s reason=%s valve_command=%u pulses=%lu ml=%lu k=%lu version=%lu quiet=%u truncated=%u",
    static_cast<unsigned long>(current.key.epoch), static_cast<unsigned long>(current.key.id),
    phaseName(current.phase), reasonName(current.reason), current.valveCommand,
    static_cast<unsigned long>(current.pulses),
    static_cast<unsigned long>(volumeMl(current.pulses, current.k)),
    static_cast<unsigned long>(valve_.config().k), static_cast<unsigned long>(valve_.config().version),
    current.quiet, current.truncated);
  gateway_.publish(line);
  snprintf(line, sizeof(line), "INTERLOCK enabled=%u ready=%u pin=%d",
    board::SlaveInterlockEnabled, hw_.interlock(), board::SlaveInterlock);
  gateway_.publish(line);
  buttonState();
  hallState(now, "query");
  meshState();
}

void Application::hallState(uint32_t now, const char *event) {
  const FlowReading reading = hw_.flow();
  const auto current = valve_.status();
  char line[192];
  // Queue bounded snapshots; neither the ISR nor the control task waits on USB.
  // raw is the lifetime accepted-edge count, after HallMinEdgeUs filtering.
  snprintf(line, sizeof(line),
    "HALL t=%lu event=%s pin=%d level=%d raw=%lu delta=%lu dt_ms=%lu last_ms=%lu",
    static_cast<unsigned long>(now), event, board::BranchFlow, hw_.flowLevel(),
    static_cast<unsigned long>(reading.pulses),
    static_cast<unsigned long>(reading.pulses - hallCount_),
    static_cast<unsigned long>(now - hallAt_), static_cast<unsigned long>(reading.lastMs));
  gateway_.publish(line);
  snprintf(line, sizeof(line),
    "HALL_JOB t=%lu job=%lu:%lu phase=%s reason=%s valve=%u job_pulses=%lu job_ml=%lu",
    static_cast<unsigned long>(now), static_cast<unsigned long>(current.key.epoch),
    static_cast<unsigned long>(current.key.id), phaseName(current.phase),
    reasonName(current.reason), current.valveCommand,
    static_cast<unsigned long>(current.pulses),
    static_cast<unsigned long>(volumeMl(current.pulses, current.k)));
  gateway_.publish(line);
  hallAt_ = now;
  hallCount_ = reading.pulses;
  if (valve_.localHoldBusy()) buttonState();
}

void Application::hallDiagnostics(uint32_t now) {
  const auto current = valve_.status();
  const bool active = current.phase == Phase::Opening || current.phase == Phase::Running ||
    current.phase == Phase::Settling || valve_.localHoldBusy();
  if (active != hallActive_) {
    hallState(now, active ? "start" : "final");
  } else if (elapsed(now, hallAt_, 1000) && (active || hw_.flow().pulses != hallCount_)) {
    hallState(now, active ? "sample" : "idle_pulse");
  }
  hallActive_ = active;
}

void Application::meshState() {
  publishMeshDiagnostics(mesh_.diagnostics(),mesh_.healthy(),[&](const char *line) { gateway_.publish(line); });
}

void Application::buttonState() {
  char line[160];
  snprintf(line, sizeof(line), "BUTTON pin=%d pressed=%u local_busy=%u local_valve=%u local_pulses=%lu reason=%s output=%u",
    board::HoldButton, hw_.holdButtonPressed(), valve_.localHoldBusy(), valve_.localHoldOutput(),
    static_cast<unsigned long>(valve_.localHoldPulses()), reasonName(valve_.localHoldReason()), valve_.output());
  gateway_.publish(line);
}

void Application::holdButton(uint32_t now) {
  const auto event = holdButton_.update(hw_.holdButtonPressed(), now);
  if (event == HoldButton::Event::Press) {
    commitResult();
    const Reply result = !storageHealthy_ ? Reply::Storage :
      (!mesh_.healthy() || !maintenance_.allowed(now)) ? Reply::NotReady : valve_.startLocalHold(now);
    char line[64];
    snprintf(line, sizeof(line), "BUTTON_PRESS reply=%s", replyName(result));
    gateway_.publish(line);
  } else if (event == HoldButton::Event::Release) {
    valve_.stopLocalHold(Reason::Manual, now);
    gateway_.publish("BUTTON_RELEASE");
  }
  if (event != HoldButton::Event::None) buttonState();
}

void Application::modeState(JobKey token,uint32_t now) {
  uint8_t flags=0;
  if (maintenance_.allowed(now)) flags|=ModePermitted;
  if (valve_.output()) flags|=ModeOutput;
  if (valve_.busy()) flags|=ModeBusy;
  if (hw_.holdButtonPressed() || !elapsed(now,releaseAt_,board::HoldButtonDebounceMs)) flags|=ModePressed;
  if (elapsed(now,flowQuietAt_,valve_.config().quietMs)) flags|=ModeQuiet;
  const auto reason=valve_.localHoldReason();
  if (storageHealthy_ && mesh_.healthy() && !valve_.transportFault() && hw_.interlock() &&
      (reason==Reason::None || reason==Reason::Manual)) flags|=ModeHealthy;
  if (maintenance_.retired(token)) flags|=ModeRetired;
  auto p=packet(Op::NodeState,store_.state().master); Writer w{p};
  w.key(token); w.u32(maintenance_.bootId()); w.u8(flags); w.u8(uint8_t(reason));
  if (mesh_.send(p)) modeSentAt_=now;
}
void Application::modeTick(uint32_t now) {
  const auto pulses=hw_.flow().pulses;
  if (pulses!=modeFlow_) { modeFlow_=pulses; flowQuietAt_=now; }
  const bool pressed=hw_.holdButtonPressed();
  if (pressed || modePressed_) releaseAt_=now;
  modePressed_=pressed;
  if (!maintenance_.allowed(now)) valve_.stopLocalHold(Reason::Manual,now);
  if (modeToken_.valid() && !elapsed(now,modeAt_,15000) && elapsed(now,modeSentAt_,500)) modeState(modeToken_,now);
}

void Application::command(CommandLine &line, uint32_t now) {
  char *tokens[4] = {};
  size_t count = 0;
  char *save = nullptr;
  for (char *p = strtok_r(line.text, " \t", &save); p; p = strtok_r(nullptr, " \t", &save)) {
    if (count == 4) { gateway_.publish("INVALID too_many_fields"); return; }
    tokens[count++] = p;
  }
  if (!count) return;
  auto number = [&](size_t index, uint64_t &value, unsigned base = 10) {
    return index < count && unsignedNumber(tokens[index], value, base);
  };
  const char *op = tokens[0];
  if (!strcmp(op, "hello") && count == 1) {
    char out[160];
    snprintf(out, sizeof(out), "HELLO role=VALVE_SLAVE addr=%04x master=%04x epoch=%lu mesh=%u commissioned=%u build=%08lx tx_healthy=%u",
      mesh_.address(), store_.state().master, static_cast<unsigned long>(masterEpoch_), mesh_.ready(),
      valve_.config().commissioned && storageHealthy_,(unsigned long)PUMP_BUILD_ID,mesh_.healthy());
    gateway_.publish(out);
    return;
  }
  if (!strcmp(op, "local") && count == 1) { localState(nowMs()); return; }
  if (!strcmp(op, "hall") && count == 1) { hallState(nowMs(), "query"); return; }
  if (!strcmp(op, "mesh-diag") && count == 1) { meshState(); return; }
  if (!strcmp(op, "recovery-resources") && count == 1) {
    if (line.bluetooth) { gateway_.publish("UNAUTHORIZED diagnostics_requires_USB"); return; }
    recovery::publishRecoveryResources([&](const char *text) { gateway_.publish(text); });
    return;
  }
  if (!strcmp(op, "recovery-status") && count == 1) {
    const auto *credential=store_.recoveryCredentials().get();
    char uuid[33]{};
    const auto &device=store_.recoveryCredentials().device();
    for (size_t i=0;i<device.size();++i) snprintf(uuid+2*i,3,"%02X",device[i]);
    char out[176];
    snprintf(out,sizeof(out),"RECOVERY_CREDENTIAL schema=1 uuid=%s initialized=%u epoch=%lu recovery_enabled=0",
      uuid,credential!=nullptr,static_cast<unsigned long>(credential ? credential->epoch : 0));
    gateway_.publish(out); return;
  }
  if (!strcmp(op, "recovery-check")) {
    // A fresh challenge verifies a completed/uncertain USB initialization without
    // exporting a key. Shares the BLE transcript; never enables Mesh recovery.
    const auto *credential=store_.recoveryCredentials().get();
    if (line.bluetooth || !credential || count!=2 || strlen(tokens[1])!=64) {
      gateway_.publish("INVALID recovery_check"); return;
    }
    recovery::Challenge challenge; challenge.device=credential->device; challenge.credentialEpoch=credential->epoch;
    for (size_t i=0;i<32;++i) {
      const auto digit=[](char c)->int {
        if (c>='0' && c<='9') return c-'0';
        if (c>='a' && c<='f') return c-'a'+10;
        if (c>='A' && c<='F') return c-'A'+10;
        return -1;
      };
      const int hi=digit(tokens[1][2*i]),lo=digit(tokens[1][2*i+1]);
      if (hi<0 || lo<0) { gateway_.publish("INVALID recovery_check"); return; }
      challenge.clientNonce[i]=uint8_t(hi*16+lo);
    }
    esp_fill_random(challenge.deviceNonce.data(),challenge.deviceNonce.size());
    recovery::Proof proof{};
    if (!recovery::makeProof(credential->key,challenge,true,proof)) { gateway_.publish("FAILED recovery_check"); return; }
    char nonce[65]{},auth[65]{},out[192];
    for(size_t i=0;i<32;++i) {
      snprintf(nonce+2*i,3,"%02X",challenge.deviceNonce[i]);
      snprintf(auth+2*i,3,"%02X",proof[i]);
    }
    snprintf(out,sizeof(out),"RECOVERY_CHECK epoch=%lu nonce=%s proof=%s",(unsigned long)credential->epoch,nonce,auth);
    gateway_.publish(out); return;
  }
  if (!strcmp(op, "recovery-init")) {
    // Trusted local installation only. No key generation/export through BLE.
    if (line.bluetooth) { gateway_.publish("UNAUTHORIZED recovery_requires_USB"); return; }
    if (count!=3 || strcmp(tokens[1],"CONFIRM") || strlen(tokens[2])!=64) {
      gateway_.publish("INVALID recovery_init"); recovery::wipe(line.text,sizeof(line.text)); return;
    }
    recovery::Key key{}; bool valid=true;
    auto hex=[](char c)->int {
      if(c>='0'&&c<='9') return c-'0';
      if(c>='a'&&c<='f') return c-'a'+10;
      if(c>='A'&&c<='F') return c-'A'+10;
      return -1;
    };
    for(size_t i=0;i<key.size();++i) {
      const int high=hex(tokens[2][2*i]),low=hex(tokens[2][2*i+1]);
      if(high<0 || low<0) valid=false; else key[i]=uint8_t(high*16+low);
    }
    recovery::wipe(line.text,sizeof(line.text));
    if (!valid) { recovery::wipe(key.data(),key.size()); gateway_.publish("INVALID recovery_init"); return; }
    const bool safe=storageHealthy_ && !valve_.busy() && !valve_.output() && !hw_.holdButtonPressed() &&
      !maintenance_.allowed(now) && elapsed(now,flowQuietAt_,valve_.config().quietMs);
    if (!safe) { recovery::wipe(key.data(),key.size()); gateway_.publish("BUSY recovery_init"); return; }
    auto &credentials=store_.recoveryCredentials();
    if(credentials.state()!=recovery::JournalState::Empty) {
      recovery::wipe(key.data(),key.size()); gateway_.publish("CONFLICT recovery_already_initialized"); return;
    }
    const bool saved=credentials.initialize(key); recovery::wipe(key.data(),key.size());
    if (!saved && credentials.state()==recovery::JournalState::Fault) {
      storageHealthy_=false; valve_.emergency(Reason::Manual,now); hw_.safeOff();
    }
    gateway_.publish(saved ? "OK recovery_initialized epoch=1 recovery_enabled=0" : "FAILED recovery_init"); return;
  }
  if (!strcmp(op, "mesh-local-reset")) {
    if (line.bluetooth) { gateway_.publish("UNSUPPORTED transport"); return; }
    if (count != 2 || strcmp(tokens[1], "CONFIRM")) {
      gateway_.publish("USAGE mesh-local-reset CONFIRM"); return;
    }
    if (!storageHealthy_) { gateway_.publish("STORAGE mesh-local-reset"); return; }
    if (valve_.busy() || valve_.output() || maintenance_.allowed(now)) { gateway_.publish("BUSY mesh-local-reset"); return; }
    if (!mesh_.provisioned()) { gateway_.publish("NOT_PROVISIONED mesh-local-reset"); return; }
    if (!mesh_.localReset()) { gateway_.publish("MESH_RESET_FAILED"); return; }
    valve_.emergency(Reason::Manual, now);
    hw_.safeOff();
    valve_.setCommissioned(false);
    gateway_.publish("MESH_RESET_REQUESTED valve=OFF credentials=clearing");
    return;
  }
  if (!strcmp(op, "estop") && count == 1) {
    valve_.emergency(Reason::LocalInterlock, nowMs());
    status(true); gateway_.publish("LOCAL_STOP"); return;
  }
  if (!strcmp(op, "k") && count == 2) {
    if (maintenance_.allowed(now) || !storageHealthy_) { gateway_.publish("BUSY k"); return; }
    uint64_t k;
    if (!number(1, k) || k > 1000000 || !validK(uint32_t(k)) || store_.state().kVersion == UINT32_MAX) {
      gateway_.publish("INVALID k"); return;
    }
    auto next = store_.state(); next.k = uint32_t(k); ++next.kVersion;
    const Reply validation = valve_.canConfigure(next.k, next.kVersion);
    if (validation != Reply::Ok) { gateway_.publish(replyName(validation)); return; }
    if (!store_.write(next)) { storageHealthy_ = false; gateway_.publish("STORAGE"); return; }
    const Reply configured = valve_.configure(next.k, next.kVersion);
    if (configured != Reply::Ok) storageHealthy_ = false;
    gateway_.publish(replyName(configured));
    return;
  }
  if (!strcmp(op, "master") && count == 2) {
    uint64_t address;
    if (!number(1, address, 16) || address > 0x7FFF || !unicast(uint16_t(address))) {
      gateway_.publish("INVALID master"); return;
    }
    if (valve_.busy() || maintenance_.allowed(now) || !storageHealthy_) { gateway_.publish("BUSY master"); return; }
    auto next = store_.state(); next.master = uint16_t(address);
    if (!store_.write(next)) { storageHealthy_ = false; gateway_.publish("STORAGE"); return; }
    mesh_.master(next.master); masterEpoch_ = 0; gateway_.publish("OK master"); return;
  }
  gateway_.publish("HELP hello | local | hall | mesh-diag | recovery-status | recovery-init CONFIRM KEY64 | recovery-check NONCE64");
  gateway_.publish("HELP mesh-local-reset CONFIRM | estop | k PULSES_PER_1000_ML | master ADDR");
}

void Application::periodic(uint32_t now) {
  session_.poll(store_.state().master,(mesh_.address()-2)%5,now,[&](const Packet &p) { return mesh_.send(p); });
  const auto current = valve_.status();
  commitResult();
  if (mesh_.ready() && elapsed(now, helloAt_, mesh_.healthy() ? 10000 : 3000)) {
    auto hello = packet(Op::Hello, store_.state().master);
    Writer writer{hello}; writer.u32(valve_.config().k); writer.u32(valve_.config().version);
    writer.u8(valve_.config().commissioned && storageHealthy_);
    writer.u32(PUMP_BUILD_ID);
    if (mesh_.send(hello)) {
      helloAt_ = now;
      // Recovery can take longer than the event retry budget. A new heartbeat
      // reopens reporting of the same terminal revision, never the old task.
      if (!mesh_.healthy()) { eventAttempts_ = 0; eventAt_ = now - 1000; }
    }
  }
  if (current.key != sentKey_ || current.revision != sentRevision_) {
    sentKey_ = current.key; sentRevision_ = current.revision; eventAttempts_ = 0; eventAt_ = now - 1000;
  }
  const uint32_t offset = (mesh_.address() % 32) * 13;
  if ((current.key != ackedKey_ || current.revision != ackedRevision_) && eventAttempts_ < 6 &&
      elapsed(now, eventAt_, 1000 + offset)) {
    if (status()) { eventAt_ = now; ++eventAttempts_; }
  }
}

void Application::run() {
  esp_task_wdt_add(nullptr);
  ESP_LOGI("build", "id=%08lx protocol=%u features=%04x tracked=1",(unsigned long)PUMP_BUILD_ID,ProtocolRevision,RequiredFeatures | JobCalibrationFeature);
  uint32_t previousTick=nowMs(), maxTick=0, diagnosticAt=previousTick;
  uint32_t reportedMeshFaults=0;
  bool previousOutput=false;
  const uint32_t boot = nowMs();
  maintenance_.boot(store_.state().revisionBase);
  flowQuietAt_=releaseAt_=boot; modeFlow_=hw_.flow().pulses;
  bool resetPending = false;
  uint32_t resetAttemptAt = boot - 1000;
  helloAt_ = boot - 9000 + (mesh_.address() % 32) * 19;
  char ready[160];
  snprintf(ready, sizeof(ready), "PUMP_MULTI_V1 ready role=VALVE_SLAVE commissioned=%u; type hello or local",
    valve_.config().commissioned && storageHealthy_);
  gateway_.publish(ready);
  char hallConfig[128];
  snprintf(hallConfig, sizeof(hallConfig),
    "HALL_CONFIG pin=%d edge=falling pullup=1 min_us=%lu period_ms=1000",
    board::BranchFlow, static_cast<unsigned long>(board::HallMinEdgeUs));
  gateway_.publish(hallConfig);
  buttonState();
  hallState(boot, "boot");
  TickType_t tick = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = nowMs();
    if (mesh_.takeReset()) {
      valve_.emergency(Reason::Manual, now);
      hw_.safeOff();
      valve_.setCommissioned(false);
      resetPending = true;
    }
    if (resetPending) {
      // Do not execute queued commands after the network identity is removed.
      // Restart clears old transport/session queues and enables PB-ADV at boot.
      hw_.safeOff();
      if (elapsed(now, resetAttemptAt, 1000)) {
        resetAttemptAt = now;
        if (store_.setCommissioned(false)) {
          ESP_LOGI("mesh", "Node Reset: valve OFF, commissioning cleared; restarting for PB-ADV");
          vTaskDelay(pdMS_TO_TICKS(1000));
          esp_restart();
        }
        ESP_LOGE("mesh", "Node Reset commissioning save failed: valve OFF; retrying");
      }
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    const uint32_t interval=now-previousTick; previousTick=now;
    if (interval>maxTick) maxTick=interval;
    if (elapsed(now,diagnosticAt,10000)) {
      const auto m=mesh_.metrics(); diagnosticAt=now;
      ESP_LOGD("mesh_diag", "healthy=%u submitted=%lu ended=%lu merged=%lu expired=%lu faults=%lu hello_timeout=%lu peak_sar=%u peak_pending=%u sar_max_ms=%lu tick_max_ms=%lu trace_lost=%lu",
        mesh_.healthy(),(unsigned long)m.submitted,(unsigned long)m.ended,(unsigned long)m.merged,(unsigned long)m.expired,
        (unsigned long)m.faults,(unsigned long)m.helloTimeouts,m.peakSar,m.peakPending,(unsigned long)m.maxSarMs,(unsigned long)maxTick,(unsigned long)m.traceLost);
      if (m.faults != reportedMeshFaults) {
        reportedMeshFaults=m.faults;
        publishMeshDiagnostics(mesh_.diagnostics(),mesh_.healthy(),[](const char *line) { ESP_LOGW("mesh_state","%s",line); });
      }
      ESP_LOGD("runtime_mem", "heap_free=%lu heap_min=%lu largest_free=%lu app_stack_min_bytes=%lu",
        (unsigned long)esp_get_free_heap_size(),(unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
    }
    // Latch transport/queue faults before a new physical press can energise.
    if (!mesh_.healthy() && !valve_.transportFault()) {
      ackedKey_ = {}; ackedRevision_ = UINT32_MAX;
      eventAttempts_ = 0; eventAt_ = now - 1000; helloAt_ = now - 10000;
    }
    if (!mesh_.healthy() || mesh_.takeOverflow() || gateway_.overflow()) valve_.emergency(Reason::Transport, now);
    modeTick(now);
    holdButton(now);
    valve_.tick(now);
    if (!valve_.output()) mesh_.retire(store_.state().master,valve_.status().key,Op::Progress);
    session_.poll(store_.state().master,(mesh_.address()-2)%5,now,[&](const Packet &p) { return mesh_.send(p); });
    // Preserve a terminal snapshot before PREPARE can replace the completed job.
    commitResult();
    hallDiagnostics(nowMs());
    MeshSubmission sent; while (mesh_.takeSubmission(sent)) {}
    Packet packetIn;
    for (unsigned count = 0; count < 4 && mesh_.receive(packetIn); ++count) incoming(packetIn, now);
    CommandLine commandLine;
    if (gateway_.receive(commandLine)) {
      command(commandLine, now);
      recovery::wipe(commandLine.text,sizeof(commandLine.text));
    }
    if (previousOutput != valve_.output()) {
      previousOutput=valve_.output(); ESP_LOGI("output","t=%lu command=%u",(unsigned long)now,previousOutput);
      buttonState();
    }
    hallDiagnostics(nowMs());
    periodic(now);
    esp_task_wdt_reset();
    vTaskDelayUntil(&tick, pdMS_TO_TICKS(5));
  }
}
}
