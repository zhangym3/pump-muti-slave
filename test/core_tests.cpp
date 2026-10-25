#include "flow_job_controller.h"
#include "valve_mesh_session.h"
#include "control_request.h"
#include "hold_button.h"
#include "result_commit.h"
#include "maintenance_session.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <initializer_list>
using namespace pump;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (false)
struct FakeValve : ValveIO {
  FlowReading reading; bool on = false, healthy = true, storage = true; unsigned opens = 0, writes = 0; JobKey fence;
  FlowReading flow() override { return reading; }
  void valve(bool value) override { on = value; if (value) ++opens; }
  bool interlock() override { return healthy; }
  bool saveFence(JobKey key) override { CHECK(!on); ++writes; if (storage) fence = key; return storage; }
};
ValveConfig config() { ValveConfig c; c.commissioned = true; c.k = 1000; return c; }
Dose dose(JobKey key = {1, 1}) { return {key, 100, 100, 1000, 1, 10000}; }
void unsaved_result_blocks_next_fence() {
  FakeValve io; FlowJobController valve(io,config()); valve.boot({});
  CHECK(valve.prepare(dose(),0)==Reply::Ok);
  CHECK(valve.start(dose().key,1)==Reply::Ok);
  io.reading={100,20}; valve.tick(20); valve.tick(520);
  const auto result=valve.status(); CHECK(terminalResult(result));
  ResultCommit commit;
  CHECK(!commit.commit(result,[](const ValveStatus &) { return false; }));
  closePreservingResult(valve,{1,2},Reason::Manual,521,commit.healthy());
  CHECK(sameResult(valve.status(),result) && io.writes==1 && !io.on);
  ValveMeshSession session(valve); std::vector<Packet> replies;
  auto prepare=packet(Op::Prepare,1); Writer w{prepare}; auto d=dose({1,3});
  w.key(d.key); w.u32(d.ml); w.u32(d.pulses); w.u32(d.k); w.u32(d.version); w.u32(d.maxMs);
  w.u16(d.speedFactorPermille); w.u32(d.leaseMs); w.u8(0); w.u32(d.leaseFloor);
  CHECK(session.handle(prepare,522,true,commit.healthy(),[&](const Packet &p) { replies.push_back(p); return true; },[](uint16_t,JobKey,Op) {}));
  CHECK(replies.size()==1 && replies[0].op==Op::CommandAck);
  Reader r{replies[0]}; r.key(); r.u8(); CHECK(Reply(r.u8())==Reply::Storage);
  CHECK(sameResult(valve.status(),result) && io.writes==1 && !io.on);
  // A successful commit allows a subsequent cancellation fence; late START remains stale.
  ResultCommit saved; CHECK(saved.commit(result,[](const ValveStatus &) { return true; }));
  closePreservingResult(valve,{1,2},Reason::Manual,523,saved.healthy());
  CHECK(io.writes==2 && saved.contains(result));
  CHECK(valve.start(result.key,524)==Reply::Stale && !io.on);
  std::puts("PASS unsaved terminal measurement survives newer CLOSE and PREPARE; saved cancellation fence rejects late START");
}
void maintenance_expiry_with_held_button() {
  FakeValve io; FlowJobController valve(io,config()); valve.boot({});
  MaintenanceSession permit(16); HoldButton button;
  button.update(false,0); button.update(false,30);
  CHECK(permit.retire({1,1}) && permit.grant({1,2},16,1000,100));
  CHECK(button.update(true,101)==HoldButton::Event::None);
  CHECK(button.update(true,131)==HoldButton::Event::Press);
  CHECK(permit.allowed(131) && valve.startLocalHold(131)==Reply::Ok && io.on);
  CHECK(!permit.allowed(1100)); valve.stopLocalHold(Reason::Manual,1100);
  CHECK(!io.on && !permit.grant({1,2},16,60000,1101));
  valve.tick(1600); CHECK(!valve.busy());
  CHECK(button.update(true,1601)==HoldButton::Event::None && io.opens==1);
  permit.boot(32); CHECK(!permit.grant({1,3},16,1000,1602));
  CHECK(!io.on && io.opens==1);
  std::puts("PASS maintenance expiry closes held valve; held button and old boot grant cannot reopen it");
}
void target_and_tail() {
  FakeValve io; FlowJobController controller(io, config()); controller.boot({});
  io.reading.pulses = 900;
  CHECK(controller.prepare(dose(), 10) == Reply::Ok); CHECK(!io.on && io.writes == 1);
  CHECK(controller.start({1, 1}, 20) == Reply::Ok);
  io.reading.pulses += 99; controller.tick(30); CHECK(io.on);
  io.reading.pulses++; controller.tick(40); CHECK(!io.on && controller.status().phase == Phase::Settling);
  io.reading.pulses += 3; controller.tick(100); controller.tick(599); CHECK(controller.busy()); controller.tick(600);
  CHECK(controller.status().phase == Phase::Done && controller.status().quiet);
  CHECK(controller.status().closePulses == 100 && controller.status().pulses == 103);
}
void validation_and_calibration() {
  FakeValve io; auto disabled = config(); disabled.commissioned = false;
  FlowJobController locked(io, disabled); locked.boot({}); CHECK(locked.prepare(dose(), 0) == Reply::NotReady);
  FlowJobController controller(io, config()); controller.boot({});
  auto bad = dose(); bad.pulses++; CHECK(controller.prepare(bad, 0) == Reply::Invalid);
  CHECK(controller.configure(0, 2) == Reply::Invalid); CHECK(controller.configure(2000, 2) == Reply::Ok);
  CHECK(controller.prepare(dose(), 0) == Reply::Invalid);
  auto calibrated = dose(); calibrated.pulses = 200; calibrated.k = 2000; calibrated.version = 2;
  CHECK(controller.prepare(calibrated, 0) == Reply::Ok); CHECK(controller.configure(3000, 3) == Reply::Busy);
}
void job_calibration() {
  for(auto speed:{SpeedLowPermille,SpeedMediumPermille,SpeedHighPermille}) {
    FakeValve io; FlowJobController valve(io,config()); valve.boot({});
    auto d=dose(); d.speedFactorPermille=speed; d.k=2500; d.pulses=250;
    CHECK(valve.prepare(d,0)==Reply::Invalid); // Legacy K guard remains.
    d.calibrated=true; CHECK(valve.prepare(d,0)==Reply::Ok);
    auto changed=d; changed.calibrated=false; CHECK(valve.prepare(changed,1)==Reply::Conflict);
    CHECK(valve.prepare(d,1)==Reply::Ok && io.writes==1);
    CHECK(valve.start(d.key,2)==Reply::Ok);
    io.reading={249,20}; valve.tick(20); CHECK(io.on);
    io.reading={250,30}; valve.tick(30); CHECK(!io.on);
    valve.tick(530); CHECK(valve.status().phase==Phase::Done && valve.status().k==2500);
    CHECK(valve.configure(3000,2)==Reply::Ok && valve.status().k==2500);
    CHECK(volumeMl(valve.status().pulses,valve.status().k)==100);
    auto next=d; next.key.id++; next.version=2; next.k=0;
    CHECK(valve.prepare(next,600)==Reply::Invalid);
    next.k=2500; next.pulses++; CHECK(valve.prepare(next,600)==Reply::Invalid);
    next.pulses--; next.version=1; CHECK(valve.prepare(next,600)==Reply::Invalid);
    next.version=2; CHECK(valve.prepare(next,600)==Reply::Ok);
    valve.tick(5600); CHECK(!io.on); // No START, no opening.
  }
  std::puts("PASS job K selection, duplicate identity, target cutoff, historical K, invalid K/target/version");
}
void manual_flow_calibration_marks_and_settles() {
  FakeValve io; FlowJobController valve(io,config()); valve.boot({});
  const JobKey key{1,2};
  auto d=dose(key); d.ml=500; d.pulses=0; d.maxMs=60000;
  d.manualCalibration=true; d.liveTelemetry=true;
  io.reading.pulses=100;
  CHECK(valve.prepare(d,0)==Reply::Ok);
  CHECK(valve.start(key,2)==Reply::Ok && io.on);

  io.reading.pulses=1100; valve.tick(20);
  CHECK(io.on && valve.status().pulses==1000); // The saved K target cannot stop a manual sample.

  ValveMeshSession mesh(valve); std::vector<Packet> replies;
  auto send=[&](const Packet &p) { replies.push_back(p); return true; };
  auto retire=[](uint16_t,JobKey,Op) {};
  auto mark=packet(Op::MeasureMark,0x1234); Writer markWriter{mark}; markWriter.key(key);
  CHECK(mesh.handle(mark,20,true,true,send,retire));
  CHECK(replies.size()==1 && replies.back().op==Op::MeasureMarkAck && replies.back().peer==0x1234);
  CHECK(controlFormat(replies.back()) && replies.back().size==13);
  Reader ack{replies.back()}; CHECK(ack.key()==key && ack.u32()==1000 && ack.u8()==uint8_t(Reply::Ok) && ack.done());

  io.reading.pulses=1200; valve.tick(30);
  CHECK(mesh.handle(mark,30,true,true,send,retire));
  Reader duplicate{replies.back()}; CHECK(duplicate.key()==key && duplicate.u32()==1000);
  CHECK(duplicate.u8()==uint8_t(Reply::Ok) && duplicate.done());

  valve.close(key,Reason::Manual,40);
  CHECK(!io.on && valve.status().phase==Phase::Settling && valve.status().closePulses==1100);
  io.reading.pulses=1215; valve.tick(100); valve.tick(600);
  const auto result=valve.status();
  CHECK(result.phase==Phase::Done && result.reason==Reason::Manual && result.quiet && !result.truncated);
  CHECK(result.closePulses==1100 && result.pulses==1115);
  CHECK(result.closePulses-1000==100 && result.pulses-result.closePulses==15);

  FakeValve disabledIo; FlowJobController disabled(disabledIo,config()); disabled.boot({});
  d.liveTelemetry=false;
  CHECK(disabled.prepare(d,0)==Reply::Invalid && !disabledIo.on);
}
void commission_gate() {
  FakeValve io; auto uncommissioned = config(); uncommissioned.commissioned = false;
  FlowJobController controller(io, uncommissioned); controller.boot({});
  CHECK(controller.canCommission() == Reply::Ok);
  io.healthy = false; CHECK(controller.canCommission() == Reply::NotReady);
  io.healthy = true;
  auto invalid = uncommissioned; invalid.k = 0;
  FlowJobController invalidController(io, invalid); invalidController.boot({});
  CHECK(invalidController.canCommission() == Reply::Invalid);

  FlowJobController busyController(io, config()); busyController.boot({});
  CHECK(busyController.prepare(dose(), 0) == Reply::Ok);
  CHECK(busyController.canCommission() == Reply::Busy);
  CHECK(busyController.start({1, 1}, 1) == Reply::Ok);
  CHECK(busyController.output()); CHECK(busyController.canCommission() == Reply::Busy);
  busyController.emergency(Reason::Manual, 2);
  CHECK(!busyController.output()); CHECK(busyController.canCommission() == Reply::Busy);
  busyController.tick(502);
  CHECK(busyController.status().phase == Phase::Done);
  CHECK(busyController.canCommission() == Reply::Ok);
}
void leases_and_interlock() {
  FakeValve io; FlowJobController controller(io, config()); controller.boot({});
  CHECK(controller.prepare(dose(), 0) == Reply::Ok); CHECK(controller.start({1, 1}, 1) == Reply::Ok);
  CHECK(controller.lease({1, 1}, 1, 1000) == Reply::Ok);
  CHECK(controller.lease({1, 1}, 1, 12999) == Reply::Ok); // Duplicate ACK does not renew the lease.
  CHECK(controller.status().revision == 2 && controller.output());
  controller.tick(13000); CHECK(!io.on && controller.status().reason == Reason::Lease);
  FakeValve broken; FlowJobController unsafe(broken, config()); unsafe.boot({});
  CHECK(unsafe.prepare(dose(), 0) == Reply::Ok); CHECK(unsafe.start({1, 1}, 1) == Reply::Ok);
  broken.healthy = false;
  unsafe.tick(1); CHECK(unsafe.status().reason == Reason::LocalInterlock && !broken.on);
}
void wire_contract_vectors() {
  CHECK(uint16_t(CompanyId) == 0x02E5 && uint16_t(ModelId) == 0x0001 && uint16_t(AllSlaves) == 0xC001);
  CHECK(uint8_t(Op::Prepare) == 0xC0 && uint8_t(Op::HelloReply) == 0xCB && uint8_t(Op::Progress) == 0xD6);
  CHECK(uint8_t(Op::Commission) == 0xCC && uint8_t(Op::CommissionReply) == 0xCD);
  ControlRequest q; q.action = Action::Start; q.epoch = 0x11223344; q.client = 0x0102030405060708ULL;
  q.sequence = 0x99AABBCC; q.mask = 0x80000001; q.ml = 500; q.maxMs = 60000; q.stopKey = {0x55667788, 0xAABBCCDD};
  const uint8_t expected[] = {1, 0x44, 0x33, 0x22, 0x11, 8, 7, 6, 5, 4, 3, 2, 1,
    0xCC, 0xBB, 0xAA, 0x99, 1, 0, 0, 0x80, 0xF4, 1, 0, 0, 0x60, 0xEA, 0, 0,
    0x88, 0x77, 0x66, 0x55, 0xDD, 0xCC, 0xBB, 0xAA};
  Packet p = encodeRequest(0x1234, q); CHECK(p.op == Op::Request && p.peer == 0x1234 && p.size == sizeof(expected));
  for (size_t i = 0; i < sizeof(expected); ++i) CHECK(p.bytes[i] == expected[i]);
  ControlRequest decoded; CHECK(decodeRequest(p, decoded)); CHECK(decoded.action == q.action && decoded.epoch == q.epoch &&
    decoded.client == q.client && decoded.sequence == q.sequence && decoded.mask == q.mask && decoded.ml == q.ml &&
    decoded.maxMs == q.maxMs && decoded.stopKey == q.stopKey);
  --p.size; CHECK(!decodeRequest(p, decoded));
  auto progress=packet(Op::Progress,2); Writer progressWriter{progress};
  progressWriter.key({1,2}); progressWriter.u32(120); progressWriter.u32(2000);
  const uint8_t expectedProgress[] = {1,0,0,0,2,0,0,0,0x78,0,0,0,0xD0,0x07,0,0};
  CHECK(progress.peer==2 && unicast(progress.peer) && controlFormat(progress) && progress.size==sizeof(expectedProgress));
  for(size_t i=0;i<sizeof(expectedProgress);++i) CHECK(progress.bytes[i]==expectedProgress[i]);
}
void transport_lock() {
  for(unsigned phase=0;phase<4;++phase) {
    FakeValve io; FlowJobController c(io,config()); c.boot({});
    if(phase>0) CHECK(c.prepare(dose(),0)==Reply::Ok);
    if(phase>1) CHECK(c.start({1,1},1)==Reply::Ok);
    if(phase==3) { io.reading.pulses=100; c.tick(2); c.tick(502); CHECK(c.status().phase==Phase::Done); }
    c.emergency(Reason::Transport,600); CHECK(!io.on && c.transportFault()); c.tick(1100);
    const auto stopped=c.status(); const auto writes=io.writes;
    CHECK(c.prepare(dose(),1200)==Reply::NotReady);
    CHECK(c.prepare(dose({1,2}),1200)==Reply::NotReady);
    CHECK(c.start({1,1},1200)==Reply::NotReady && c.lease({1,1},2,1200)==Reply::NotReady);
    CHECK(c.canCommission()==Reply::NotReady && !io.on && io.writes==writes);
    CHECK(c.configure(2000,2)==Reply::Ok); c.setCommissioned(true);
    c.close({1,1},Reason::Manual,1300); c.emergency(Reason::Manual,1300);
    CHECK(c.transportFault() && c.prepare(dose({1,2}),1400)==Reply::NotReady && !io.on);
    if(stopped.key.valid()) CHECK(c.status().key==stopped.key);
  }
}
void prepared_deadline_and_floor() {
  for(uint32_t age:{19999U,20000U}) {
    FakeValve io;FlowJobController c(io,config());c.boot({});
    auto d=dose();d.leaseFloor=0;CHECK(c.prepare(d,100)==Reply::Invalid && !io.writes);
    d.leaseFloor=0x80000000U;CHECK(c.prepare(d,100)==Reply::Ok);
    auto conflict=d;conflict.leaseFloor++;CHECK(c.prepare(conflict,110)==Reply::Conflict);
    CHECK(c.prepare(d,150)==Reply::Ok); // Does not slide the prepared deadline.
    CHECK(c.start(d.key,100+age)==(age<20000?Reply::Ok:Reply::NotReady));
    CHECK(io.on==(age<20000));
    if(io.on) {
      CHECK(c.compactLease(d.key.epoch,d.leaseFloor-1,20100)==Reply::Stale);
      CHECK(c.compactLease(d.key.epoch,d.leaseFloor,20100)==Reply::Ok);
    }
  }
  std::puts("PASS prepared 19999/20000ms boundary, non-sliding duplicate and lease floor validation");
}
void hold_button_edges() {
  using E = HoldButton::Event;
  HoldButton button;
  CHECK(button.update(true, 0) == E::None);
  CHECK(button.update(true, 1000) == E::None); // Held at boot cannot open.
  CHECK(button.update(false, 1001) == E::None);
  CHECK(button.update(true, 1020) == E::None); // Release bounce cannot arm.
  CHECK(button.update(true, 1100) == E::None);
  CHECK(button.update(false, 1101) == E::None);
  CHECK(button.update(false, 1131) == E::None);
  CHECK(button.update(true, 1140) == E::None);
  CHECK(button.update(false, 1150) == E::None);
  CHECK(button.update(false, 1180) == E::None);
  CHECK(button.update(true, 1190) == E::None);
  CHECK(button.update(true, 1219) == E::None);
  CHECK(button.update(true, 1220) == E::Press);
  CHECK(button.update(true, 100000) == E::None); // A held key never retriggers.
  CHECK(button.update(false, 100001) == E::Release); // No release debounce delay.
  CHECK(button.update(true, 100002) == E::None);
  CHECK(button.update(true, 100100) == E::None); // Cannot reopen on release bounce.
  CHECK(button.update(false, 100101) == E::None);
  CHECK(button.update(false, 100131) == E::None);
  CHECK(button.update(true, 100140) == E::None);
  CHECK(button.update(true, 100170) == E::Press);

  HoldButton wrap;
  CHECK(wrap.update(false, UINT32_MAX - 20) == E::None);
  CHECK(wrap.update(false, 10) == E::None);
  CHECK(wrap.update(true, 11) == E::None);
  CHECK(wrap.update(true, 41) == E::Press);
  CHECK(wrap.update(false, 42) == E::Release);
}
void local_hold_arbitration_and_history() {
  FakeValve io; FlowJobController c(io, config()); c.boot({});
  CHECK(c.prepare(dose(), 0) == Reply::Ok);
  CHECK(c.startLocalHold(1) == Reply::Busy && !io.on);
  CHECK(c.start({1,1}, 2) == Reply::Ok);
  c.stopLocalHold(Reason::Manual, 3); // Releasing a refused press cannot stop a remote job.
  CHECK(io.on && c.status().valveCommand);
  CHECK(c.startLocalHold(4) == Reply::Busy);
  io.reading.pulses = 100; c.tick(10);
  CHECK(c.startLocalHold(11) == Reply::Busy);
  c.tick(510);
  const auto result = c.status(); const auto writes = io.writes;
  CHECK(result.phase == Phase::Done);
  CHECK(c.startLocalHold(511) == Reply::Ok && io.on && c.output() && c.busy());
  CHECK(c.prepare(dose(), 512) == Reply::Busy); // Even duplicate old requests cannot interfere.
  CHECK(c.prepare(dose({1,2}), 512) == Reply::Busy);
  CHECK(c.start({1,1}, 512) == Reply::Busy);
  CHECK(c.canCommission() == Reply::Busy && c.configure(2000,2) == Reply::Busy);
  io.reading.pulses += 123; c.tick(520);
  CHECK(io.on && c.localHoldPulses() == 123); // No remote dose target applies to local hold.
  c.stopLocalHold(Reason::Manual, 530);
  CHECK(!io.on && !c.output() && c.localHoldBusy());
  CHECK(c.prepare(dose({1,2}), 531) == Reply::Busy);
  io.reading.pulses += 2; c.tick(540); c.tick(1040);
  CHECK(!c.busy() && c.localHoldPulses() == 125 && c.localHoldReason() == Reason::Manual);
  const auto after = c.status();
  CHECK(after.key == result.key && after.phase == result.phase && after.revision == result.revision);
  CHECK(after.pulses == result.pulses && after.reason == result.reason && after.quiet == result.quiet);
  CHECK(io.writes == writes && c.prepare(dose({1,2}), 1041) == Reply::Ok);
}
void local_hold_gates_and_faults() {
  FakeValve io; auto cfg = config(); cfg.commissioned = false;
  FlowJobController disabled(io,cfg); disabled.boot({});
  CHECK(disabled.startLocalHold(0) == Reply::NotReady && !io.on);
  FlowJobController c(io,config()); c.boot({});
  io.healthy = false; CHECK(c.startLocalHold(0) == Reply::NotReady);
  io.healthy = true; CHECK(c.startLocalHold(1) == Reply::Ok);
  c.tick(5000); CHECK(io.on); c.tick(5001);
  CHECK(!io.on && c.localHoldReason() == Reason::NoFlow);
  c.tick(5501); CHECK(!c.busy());
  CHECK(c.startLocalHold(6000) == Reply::Ok);
  io.reading.pulses = 1; c.tick(6010); c.tick(7509); CHECK(io.on);
  c.tick(7510); CHECK(!io.on && c.localHoldReason() == Reason::NoFlow);
  c.tick(8010); CHECK(c.startLocalHold(8011) == Reply::Ok);
  io.healthy = false; c.tick(8012);
  CHECK(!io.on && c.localHoldReason() == Reason::LocalInterlock);
  io.healthy = true; c.tick(8512);
  CHECK(c.startLocalHold(8513) == Reply::Ok);
  c.emergency(Reason::Transport,8514);
  CHECK(!io.on && c.transportFault() && c.localHoldReason() == Reason::Transport);
  c.tick(9014); CHECK(c.startLocalHold(9015) == Reply::NotReady);

  FakeValve brokenStorage; brokenStorage.storage = false;
  FlowJobController broken(brokenStorage,config()); broken.boot({});
  CHECK(broken.prepare(dose(),0) == Reply::Storage);
  CHECK(broken.startLocalHold(1) == Reply::Storage && !brokenStorage.on);
}
void local_hold_runtime_stop_and_rearm() {
  using E = HoldButton::Event;
  for (bool globalLimit : {false,true}) {
    FakeValve io; auto cfg = config();
    cfg.localHoldMaxMs = globalLimit ? 1000 : 100;
    cfg.maxRunMs = globalLimit ? 100 : 1000;
    FlowJobController c(io,cfg); c.boot({});
    HoldButton button;
    CHECK(button.update(false,0) == E::None);
    CHECK(button.update(false,30) == E::None);
    CHECK(button.update(true,31) == E::None);
    CHECK(button.update(true,61) == E::Press);
    CHECK(c.startLocalHold(61) == Reply::Ok);
    c.tick(160); CHECK(io.on); c.tick(161);
    CHECK(!io.on && c.localHoldReason() == Reason::Runtime);
    c.tick(661); CHECK(!c.busy());
    CHECK(button.update(true,662) == E::None && io.opens == 1);
    CHECK(button.update(false,663) == E::Release);
    c.stopLocalHold(Reason::Manual,663);
    CHECK(c.localHoldReason() == Reason::Runtime);
    CHECK(button.update(false,693) == E::None);
    CHECK(button.update(true,694) == E::None);
    CHECK(button.update(true,724) == E::Press);
    CHECK(c.startLocalHold(724) == Reply::Ok);
    c.close({1,100},Reason::PumpFault,725);
    CHECK(!io.on && c.localHoldReason() == Reason::PumpFault);
  }
  FakeValve io; FlowJobController c(io,config()); c.boot({});
  CHECK(c.startLocalHold(UINT32_MAX-10) == Reply::Ok);
  c.stopLocalHold(Reason::Manual,5); CHECK(!io.on);
  io.reading.pulses = 1; c.tick(100);
  io.reading.pulses = 2; c.tick(500);
  io.reading.pulses = 3; c.tick(900);
  io.reading.pulses = 4; c.tick(1300);
  io.reading.pulses = 5; c.tick(1700);
  io.reading.pulses = 6; c.tick(2100);
  io.reading.pulses = 7; c.tick(2500);
  io.reading.pulses = 8; c.tick(2900); c.tick(3005);
  CHECK(!c.busy() && c.localHoldReason() == Reason::TailFlow);
  CHECK(c.startLocalHold(3006) == Reply::Ok);
  c.boot({}); CHECK(!io.on && !c.busy() && !c.output());
}
int main() { unsaved_result_blocks_next_fence(); maintenance_expiry_with_held_button(); hold_button_edges(); local_hold_arbitration_and_history(); local_hold_gates_and_faults(); local_hold_runtime_stop_and_rearm(); std::puts("PASS hold button debounce/boot lock/rearm, local arbitration/history and shutdown protections"); job_calibration(); manual_flow_calibration_marks_and_settles(); prepared_deadline_and_floor(); target_and_tail(); validation_and_calibration(); commission_gate(); leases_and_interlock(); wire_contract_vectors(); transport_lock(); std::puts("PASS transport fault locks idle/prepared/open/terminal control until controlled restart"); std::puts("slave flow tests passed"); }
