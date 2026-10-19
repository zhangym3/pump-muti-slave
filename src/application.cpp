#include "application.h"
#include "mesh_tx_diagnostics.h"
#include "esp_log.h"
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

bool Application::status(bool force) {
  const auto current = valve_.status();
  if (!force && ackedKey_ == current.key && ackedRevision_ == current.revision) return false;
  auto packetOut = packet(Op::Status, store_.state().master);
  encodeStatus(packetOut, current);
  return mesh_.send(packetOut);
}

void Application::reject(JobKey key, Op command, Reply reply) {
  auto packetOut = packet(Op::CommandReply, store_.state().master);
  Writer writer{packetOut}; writer.key(key); writer.u8(uint8_t(command)); writer.u8(uint8_t(reply));
  mesh_.send(packetOut);
  char line[64];
  snprintf(line, sizeof(line), "JOB_REJECT %s", replyName(reply));
  gateway_.publish(line);
}

void Application::incoming(const Packet &packetIn, uint32_t now) {
  if (packetIn.peer != store_.state().master) return;
  Reader reader{packetIn};
  switch (packetIn.op) {
    case Op::CapabilitiesGet: {
      const uint32_t nonce=reader.u32(); if (!reader.done() || !nonce) return;
      auto reply=packet(Op::Capabilities,store_.state().master); Writer w{reply};
      w.u32(nonce); w.u16(ProtocolRevision);
      w.u16(mesh_.healthy() && !valve_.transportFault() ? RequiredFeatures : 0); w.u32(PUMP_BUILD_ID);
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
        mesh_.send(hello);
      }
      char line[96];
      snprintf(line, sizeof(line), "COMMISSION req=%lu reply=%s commissioned=%u",
        static_cast<unsigned long>(request), replyName(result), store_.commissioned() && storageHealthy_);
      gateway_.publish(line);
      break;
    }
    case Op::Prepare: {
      Dose dose;
      dose.key = reader.key(); dose.ml = reader.u32(); dose.pulses = reader.u32();
      dose.k = reader.u32(); dose.version = reader.u32(); dose.maxMs = reader.u32();
      if (!reader.done()) return;
      const Reply result = !mesh_.healthy() ? Reply::NotReady :
        storageHealthy_ ? valve_.prepare(dose, now) : Reply::Storage;
      if (result == Reply::Ok) status(); else reject(dose.key, Op::Prepare, result);
      break;
    }
    case Op::Start: {
      const JobKey key = reader.key();
      if (!reader.done()) return;
      const Reply result = mesh_.healthy() ? valve_.start(key, now) : Reply::NotReady;
      if (result == Reply::Ok) status(); else reject(key, Op::Start, result);
      break;
    }
    case Op::Lease: {
      const JobKey key = reader.key();
      const uint32_t sequence = reader.u32();
      if (!reader.done() || !key.valid() || !sequence) break;
      // Target closure can race an already committed renewal. Report the
      // physical closed state; it is not a new lease proof or a rejection of
      // an open valve's authorization. Fault closures still return rejection.
      const auto current = valve_.status();
      if (current.key == key && !current.valveCommand && current.reason == Reason::Target &&
          (current.phase == Phase::Settling || current.phase == Phase::Done)) {
        status(true); break;
      }
      const Reply result = mesh_.healthy() ? valve_.lease(key, sequence, now) : Reply::NotReady;
      auto ack = packet(Op::LeaseAck, store_.state().master);
      Writer w{ack}; w.key(key); w.u32(sequence); w.u8(uint8_t(result)); mesh_.send(ack);
      break;
    }
    case Op::Close: {
      const JobKey key = reader.key();
      const Reason reason = Reason(reader.u8());
      if (reader.done() && reason <= Reason::Protocol) {
        valve_.close(key, reason, now);
        status(true);
      }
      break;
    }
    case Op::StatusGet:
      reader.key();
      if (reader.done()) status(true);
      break;
    case Op::EventAck: {
      const JobKey key = reader.key();
      const uint32_t revision = reader.u32();
      if (reader.done() && key == valve_.status().key && revision == valve_.status().revision) {
        ackedKey_ = key; ackedRevision_ = revision;
      }
      break;
    }
    default:
      // Requests, responses and slave-originated telemetry are never executed here.
      break;
  }
}

void Application::localState(uint32_t) {
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
  meshState();
}

void Application::meshState() {
  publishMeshDiagnostics(mesh_.diagnostics(),mesh_.healthy(),[&](const char *line) { gateway_.publish(line); });
}

void Application::command(CommandLine &line, uint32_t) {
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
  if (!strcmp(op, "mesh-diag") && count == 1) { meshState(); return; }
  if (!strcmp(op, "estop") && count == 1) {
    valve_.emergency(Reason::LocalInterlock, nowMs());
    status(true); gateway_.publish("LOCAL_STOP"); return;
  }
  if (!strcmp(op, "k") && count == 2) {
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
    if (valve_.busy()) { gateway_.publish("BUSY master"); return; }
    auto next = store_.state(); next.master = uint16_t(address);
    if (!store_.write(next)) { storageHealthy_ = false; gateway_.publish("STORAGE"); return; }
    mesh_.master(next.master); masterEpoch_ = 0; gateway_.publish("OK master"); return;
  }
  gateway_.publish("HELP hello | local | mesh-diag | estop | k PULSES_PER_ML | master ADDR");
}

void Application::periodic(uint32_t now) {
  const auto current = valve_.status();
  const bool final = current.phase == Phase::Done || current.phase == Phase::Fault;
  if (final && (current.key != savedKey_ || current.revision != savedRevision_) && current.key.valid()) {
    storageHealthy_ = store_.result(current);
    savedKey_ = current.key;
    savedRevision_ = current.revision;
  }
  if (mesh_.ready() && elapsed(now, helloAt_, mesh_.healthy() ? 10000 : 3000)) {
    auto hello = packet(Op::Hello, store_.state().master);
    Writer writer{hello}; writer.u32(valve_.config().k); writer.u32(valve_.config().version);
    writer.u8(valve_.config().commissioned && storageHealthy_);
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
  if (current.key.valid() && valve_.busy() && elapsed(now, summaryAt_, TelemetryMs)) {
    auto summary = packet(Op::Summary, store_.state().master);
    Writer writer{summary}; writer.key(current.key); writer.u32(current.pulses); writer.u32(current.revision);
    writer.u8(uint8_t(current.phase)); writer.u8(current.valveCommand ? 1 : 0);
    if (mesh_.send(summary)) summaryAt_ = now;
  }
}

void Application::run() {
  esp_task_wdt_add(nullptr);
  ESP_LOGI("build", "id=%08lx protocol=%u features=%04x tracked=1",(unsigned long)PUMP_BUILD_ID,ProtocolRevision,RequiredFeatures);
  uint32_t previousTick=nowMs(), maxTick=0, diagnosticAt=previousTick;
  bool previousOutput=false;
  const uint32_t boot = nowMs();
  helloAt_ = boot - 9000 + (mesh_.address() % 32) * 19;
  summaryAt_ = boot - TelemetryMs + (mesh_.address() % 32) * 53;
  char ready[160];
  snprintf(ready, sizeof(ready), "PUMP_MULTI_V1 ready role=VALVE_SLAVE commissioned=%u; type hello or local",
    valve_.config().commissioned && storageHealthy_);
  gateway_.publish(ready);
  TickType_t tick = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = nowMs();
    const uint32_t interval=now-previousTick; previousTick=now;
    if (interval>maxTick) maxTick=interval;
    if (elapsed(now,diagnosticAt,10000)) {
      const auto m=mesh_.metrics(); diagnosticAt=now;
      ESP_LOGI("mesh_diag", "healthy=%u submitted=%lu ended=%lu merged=%lu expired=%lu faults=%lu peak_sar=%u peak_pending=%u sar_max_ms=%lu tick_max_ms=%lu trace_lost=%lu",
        mesh_.healthy(),(unsigned long)m.submitted,(unsigned long)m.ended,(unsigned long)m.merged,(unsigned long)m.expired,
        (unsigned long)m.faults,m.peakSar,m.peakPending,(unsigned long)m.maxSarMs,(unsigned long)maxTick,(unsigned long)m.traceLost);
      publishMeshDiagnostics(mesh_.diagnostics(),mesh_.healthy(),[](const char *line) { ESP_LOGI("mesh_state","%s",line); });
    }
    valve_.tick(now);
    MeshSubmission sent; while (mesh_.takeSubmission(sent)) {}
    if (!mesh_.healthy() && !valve_.transportFault()) {
      ackedKey_ = {}; ackedRevision_ = UINT32_MAX;
      eventAttempts_ = 0; eventAt_ = now - 1000; helloAt_ = now - 10000;
    }
    if (!mesh_.healthy() || mesh_.takeOverflow() || gateway_.overflow()) valve_.emergency(Reason::Transport, now);
    Packet packetIn;
    for (unsigned count = 0; count < 4 && mesh_.receive(packetIn); ++count) incoming(packetIn, now);
    CommandLine commandLine;
    if (gateway_.receive(commandLine)) command(commandLine, now);
    if (previousOutput != valve_.output()) {
      previousOutput=valve_.output(); ESP_LOGI("output","t=%lu command=%u",(unsigned long)now,previousOutput);
    }
    periodic(now);
    esp_task_wdt_reset();
    vTaskDelayUntil(&tick, pdMS_TO_TICKS(5));
  }
}
}
