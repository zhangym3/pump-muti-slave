#pragma once
#include "mesh_tx_governor.h"
#include <cstdio>

namespace pump {
// Each line fits the 192-byte local gateway. First-fault evidence survives
// trace draining and can be queried again without resetting the device.
template<class Publish>
void publishMeshDiagnostics(const MeshTxGovernor::Diagnostics &d, bool healthy, Publish publish) {
  char line[192];
  snprintf(line,sizeof(line),"MESH_STATE healthy=%u diagnostic_ready=%u restart_required=%u in_flight=%u pending=%u backoff_ms=%lu",
    healthy,d.diagnosticReady,d.restartRequired,d.inFlight,d.pending,(unsigned long)d.backoffMs);
  publish(line);
  auto fault = [&](const MeshTxGovernor::Fault &f, bool first) {
    if (f.reason == MeshTxGovernor::FaultReason::None) return;
    const auto op=f.packet.op;
    const bool keyed = op==Op::Prepare || op==Op::Start || op==Op::Lease || op==Op::Close ||
      op==Op::StatusGet || op==Op::Status || op==Op::Summary || op==Op::EventAck ||
      op==Op::LeaseAck || op==Op::CommandReply;
    Reader r{f.packet}; const auto job=keyed ? r.key() : JobKey{};
    uint32_t sequence=0, revision=0;
    if (op==Op::Lease || op==Op::LeaseAck) sequence=r.u32();
    if (op==Op::Status || op==Op::Summary || op==Op::EventAck) {
      r.at=op==Op::Status ? 20 : op==Op::Summary ? 12 : 8; revision=r.u32();
    }
    snprintf(line,sizeof(line),"MESH_FAULT first=%u reason=%s t=%lu ticket=%lu peer=%04x op=%02x err=%d job=%lu:%lu",
      first,MeshTxGovernor::faultName(f.reason),(unsigned long)f.at,(unsigned long)f.ticket,
      f.packet.peer,unsigned(f.packet.op),f.error,(unsigned long)job.epoch,(unsigned long)job.id);
    publish(line);
    snprintf(line,sizeof(line),"MESH_FAULT_TIMING first=%u issued=%lu queued=%lu in_flight=%u pending=%u seq=%lu rev=%lu",
      first,(unsigned long)f.issuedAt,(unsigned long)f.queuedAt,f.inFlight,f.pending,
      (unsigned long)sequence,(unsigned long)revision);
    publish(line);
  };
  fault(d.first,true);
  if (d.first.at != d.last.at || d.first.ticket != d.last.ticket || d.first.reason != d.last.reason)
    fault(d.last,false);
}
}
