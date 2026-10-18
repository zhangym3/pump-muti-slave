#include "protocol.h"
namespace pump {
const char *phaseName(Phase p) {
  static const char *n[] = {"IDLE", "PREPARED", "OPEN_COMMAND_SENT", "RUNNING", "SETTLING", "DONE", "FAULT"};
  const auto i = unsigned(p); return i < sizeof(n)/sizeof(*n) ? n[i] : "INVALID";
}
const char *reasonName(Reason r) {
  static const char *n[] = {"NONE", "TARGET", "MANUAL", "LEASE", "RUNTIME", "NO_FLOW", "PREPARE_TIMEOUT",
    "TAIL_FLOW", "LOCAL_INTERLOCK", "REBOOT", "PUMP_FAULT", "TRANSPORT", "PROTOCOL"};
  const auto i = unsigned(r); return i < sizeof(n)/sizeof(*n) ? n[i] : "INVALID";
}
const char *replyName(Reply r) {
  static const char *n[] = {"OK", "BUSY", "INVALID", "STALE", "CONFLICT", "NOT_READY", "STORAGE",
    "UNAUTHORIZED", "OFFLINE", "CAPACITY", "PENDING", "UNKNOWN"};
  const auto i = unsigned(r); return i < sizeof(n)/sizeof(*n) ? n[i] : "INVALID";
}
}
