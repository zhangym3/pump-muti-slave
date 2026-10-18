#include "application.h"
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

void Application::status(bool force) {
  const auto current = valve_.status();
  if (!force && ackedKey_ == current.key && ackedRevision_ == current.revision) return;
  auto packetOut = packet(Op::Status, store_.state().master);
  encodeStatus(packetOut, current);
  mesh_.send(packetOut);
}

void Application::reject(JobKey key, Reply reply) {
  ValveStatus fault;
  fault.key = key;
  fault.k = valve_.config().k;
  fault.phase = Phase::Fault;
  fault.reason = Reason::Protocol;
  fault.revision = valve_.status().revision;
  auto packetOut = packet(Op::Status, store_.state().master);
  encodeStatus(packetOut, fault);
  mesh_.send(packetOut);
  char line[64];
  snprintf(line, sizeof(line), "JOB_REJECT %s", replyName(reply));
  gateway_.publish(line);
}

void Application::incoming(const Packet &packetIn, uint32_t now) {
  if (packetIn.peer != store_.state().master) return;
  Reader reader{packetIn};
  switch (packetIn.op) {
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
      if (!storageHealthy_) result = Reply::Storage;
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
      const Reply result = storageHealthy_ ? valve_.prepare(dose, now) : Reply::Storage;
      if (result == Reply::Ok) status(true); else reject(dose.key, result);
      break;
    }
    case Op::Start: {
      const JobKey key = reader.key();
      if (!reader.done()) return;
      valve_.start(key, now);
      status(true);
      break;
    }
    case Op::Lease: {
      const JobKey key = reader.key();
      const uint32_t sequence = reader.u32();
      if (reader.done()) valve_.lease(key, sequence, now);
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
    snprintf(out, sizeof(out), "HELLO role=VALVE_SLAVE addr=%04x master=%04x epoch=%lu mesh=%u commissioned=%u",
      mesh_.address(), store_.state().master, static_cast<unsigned long>(masterEpoch_), mesh_.ready(),
      valve_.config().commissioned && storageHealthy_);
    gateway_.publish(out);
    return;
  }
  if (!strcmp(op, "local") && count == 1) { localState(nowMs()); return; }
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
    if (!store_.write(next)) { storageHealthy_ = false; gateway_.publish("STORAGE"); return; }
    gateway_.publish(replyName(valve_.configure(next.k, next.kVersion)));
    return;
  }
  if (!strcmp(op, "master") && count == 2) {
    uint64_t address;
    if (!number(1, address, 16) || address > 0x7FFF || !unicast(uint16_t(address))) {
      gateway_.publish("INVALID master"); return;
    }
    auto next = store_.state(); next.master = uint16_t(address);
    if (!store_.write(next)) { storageHealthy_ = false; gateway_.publish("STORAGE"); return; }
    mesh_.master(next.master); masterEpoch_ = 0; gateway_.publish("OK master"); return;
  }
  gateway_.publish("HELP hello | local | estop | k PULSES_PER_ML | master ADDR");
}

void Application::periodic(uint32_t now) {
  const auto current = valve_.status();
  const bool final = current.phase == Phase::Done || current.phase == Phase::Fault;
  if (final && (current.key != savedKey_ || current.revision != savedRevision_) && current.key.valid()) {
    storageHealthy_ = store_.result(current);
    savedKey_ = current.key;
    savedRevision_ = current.revision;
  }
  if (mesh_.ready() && elapsed(now, helloAt_, 10000)) {
    auto hello = packet(Op::Hello, store_.state().master);
    Writer writer{hello}; writer.u32(valve_.config().k); writer.u32(valve_.config().version);
    writer.u8(valve_.config().commissioned && storageHealthy_);
    mesh_.send(hello); helloAt_ = now;
  }
  if (current.key != sentKey_ || current.revision != sentRevision_) {
    sentKey_ = current.key; sentRevision_ = current.revision; eventAttempts_ = 0; eventAt_ = now - 1000;
  }
  const uint32_t offset = (mesh_.address() % 32) * 13;
  if ((current.key != ackedKey_ || current.revision != ackedRevision_) && eventAttempts_ < 6 &&
      elapsed(now, eventAt_, 1000 + offset)) {
    status(); eventAt_ = now; ++eventAttempts_;
  }
  if (current.key.valid() && valve_.busy() && elapsed(now, summaryAt_, TelemetryMs)) {
    auto summary = packet(Op::Summary, store_.state().master);
    Writer writer{summary}; writer.key(current.key); writer.u32(current.pulses); writer.u32(current.revision);
    writer.u8(uint8_t(current.phase)); writer.u8(current.valveCommand ? 1 : 0);
    mesh_.send(summary); summaryAt_ = now;
  }
}

void Application::run() {
  esp_task_wdt_add(nullptr);
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
    valve_.tick(now);
    if (mesh_.takeOverflow() || gateway_.overflow()) valve_.emergency(Reason::Transport, now);
    Packet packetIn;
    for (unsigned count = 0; count < 4 && mesh_.receive(packetIn); ++count) incoming(packetIn, now);
    CommandLine commandLine;
    if (gateway_.receive(commandLine)) command(commandLine, now);
    periodic(now);
    esp_task_wdt_reset();
    vTaskDelayUntil(&tick, pdMS_TO_TICKS(5));
  }
}
}
