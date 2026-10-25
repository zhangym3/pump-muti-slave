#pragma once
#include "protocol.h"
namespace pump {
constexpr uint16_t MaintenanceFeature=0x1000;
constexpr uint8_t ModePermitted=1, ModeOutput=2, ModeBusy=4, ModePressed=8,
  ModeQuiet=16, ModeHealthy=32, ModeRetired=64;
constexpr uint8_t AutoSafeFlags=ModeQuiet|ModeHealthy|ModeRetired;
struct MaintenanceRecord {
  uint32_t format=1, epoch=0, generation=0, boot=0;
  uint16_t address=0, locked=0;
};
// A boot-bound, non-renewable permit. Retiring a token is irreversible in this boot.
class MaintenanceSession {
 public:
  explicit MaintenanceSession(uint32_t boot=0):boot_(boot) {}
  void boot(uint32_t value) { *this=MaintenanceSession(value); }
  bool retire(JobKey token) {
    if (!token.valid() || boundary_.newer(token)) return false;
    boundary_=token;
    if (!permit_.newer(token)) active_=false;
    return true;
  }
  bool grant(JobKey token,uint32_t boot,uint32_t ttl,uint32_t now) {
    if (!boot_ || boot!=boot_ || !token.valid() || ttl<1000 || ttl>60000 ||
        !token.newer(boundary_)) return false;
    if (token==permit_) return active_ && !elapsed(now,at_,ttl_);
    if (permit_.newer(token) || active_) return false;
    permit_=token; at_=now; ttl_=ttl; active_=true; return true;
  }
  bool allowed(uint32_t now) {
    if (active_ && elapsed(now,at_,ttl_)) { active_=false; retire(permit_); }
    return active_;
  }
  bool retired(JobKey token) const { return token.valid() && !token.newer(boundary_) && !active_; }
  uint32_t bootId() const { return boot_; }
 private:
  uint32_t boot_=0, at_=0, ttl_=0;
  JobKey boundary_,permit_;
  bool active_=false;
};
}
