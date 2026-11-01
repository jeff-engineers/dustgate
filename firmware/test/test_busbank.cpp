// test_busbank.cpp — the bus board's servo bank (motor/st3215/BusBank.h) against a fake bus: one move at a time, arrival,
// the stopped-short and not-answering faults, the peak load, the report, and numbering.
#include <ArduinoJson.h>
#include "../motor/st3215/BusBank.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>

static int passed = 0, failed = 0;
static void ok(const char* name, bool cond, const std::string& detail = "") {
  if (cond) { passed++; printf("  ok   %s\n", name); }
  else      { failed++; printf("  FAIL %s%s%s\n", name, detail.empty() ? "" : "  — ", detail.c_str()); }
}

// A fake bus: each servo goes a fixed number of counts toward its goal per read, and reports a load while it moves.
struct FakeServo { uint16_t pos = 2048, goal = 2048; int load = 0; bool torque = false; bool positionMode = false; bool jammedAt = false; uint16_t jam = 0; };
struct FakeBus : busbank::Port {
  std::map<int, FakeServo> servos;   // by number; two at one number = garbled
  std::map<int, int> atNumber;       // how many answer as each number
  std::vector<std::string> log;
  bool silent = false;
  int step = 200;
  Heard ping(uint8_t id) override {
    const int n = count(id);
    return n == 0 ? Heard::None : n == 1 ? Heard::One : Heard::Garbled;
  }
  int count(int id) { auto it = atNumber.find(id); return it == atNumber.end() ? (servos.count(id) ? 1 : 0) : it->second; }
  bool read(uint8_t id, busbank::Reading& r) override {
    if (silent || !servos.count(id)) return false;
    FakeServo& s = servos[id];
    const int d = (int)s.goal - (int)s.pos;
    int stepTo = d > step ? step : d < -step ? -step : d;
    if (s.jammedAt && ((d > 0 && s.pos + stepTo > s.jam) || (d < 0 && s.pos + stepTo < s.jam))) stepTo = (int)s.jam - (int)s.pos;
    s.pos = (uint16_t)((int)s.pos + stepTo);
    r.pos = s.pos; r.moving = stepTo != 0; r.load = stepTo ? s.load : 2; r.v = 12.1f; r.temp = 30;
    return true;
  }
  bool moveTo(uint8_t id, uint16_t pos, uint16_t) override {
    if (!servos.count(id)) return false;
    servos[id].goal = pos; log.push_back("move " + std::to_string(id) + " " + std::to_string(pos)); return true;
  }
  bool torque(uint8_t id, bool on) override { if (!servos.count(id)) return false; servos[id].torque = on; return true; }
  bool positionMode(uint8_t id) override { if (!servos.count(id)) return false; servos[id].positionMode = true; log.push_back("mode " + std::to_string(id)); return true; }
  bool renumber(uint8_t from, uint8_t to) override {
    if (!servos.count(from)) return false;
    servos[to] = servos[from]; servos.erase(from); atNumber.erase(from); return true;
  }
  void pause(uint32_t) override {}
};

static busbank::Ended runUntilEnd(busbank::Bank& b, uint32_t& now, uint32_t limitMs = 20000) {
  for (uint32_t t = 0; t < limitMs; t += 10) { now += 10; busbank::Ended e = b.tick(now); if (e.any) return e; }
  return busbank::Ended();
}

int main() {
  using namespace busbank;
  printf("BB1 angles and counts\n");
  ok("90° is a quarter turn", degToCounts(90) == 1024);
  ok("359° stays inside one turn", degToCounts(359) == 4085 && degToCounts(400) == 4095);

  printf("BB2 a move runs to arrival, in position mode, and keeps its peak\n");
  {
    FakeBus bus; bus.servos[4].load = 16; busbank::Bank b(bus);
    uint32_t now = 1000;
    ok("a move is queued", b.want(4, 270, "valve", "open") && b.busy());
    Ended e = runUntilEnd(b, now);
    ok("it arrives with no error", e.any && !e.err && std::string(e.sel) == "valve" && std::string(e.state) == "open");
    ok("...at the angle asked", bus.servos[4].pos == degToCounts(270));
    ok("the servo was put in position mode first, once", bus.log.size() >= 2 && bus.log[0] == "mode 4");
    BusServoReport r[kMaxBusServos];
    const size_t n = b.report(r);
    ok("the report has it, with its peak and one move", n == 1 && r[0].id == 4 && r[0].ok && r[0].peak == 16 && r[0].moves == 1, std::to_string(r[0].peak));
    b.want(4, 0, "valve", "closed"); runUntilEnd(b, now);
    int modes = 0; for (auto& l : bus.log) if (l == "mode 4") modes++;
    ok("...and the mode is not written again", modes == 1);
  }

  printf("BB3 one at a time\n");
  {
    FakeBus bus; bus.servos[4]; bus.servos[5]; busbank::Bank b(bus);
    uint32_t now = 0;
    b.want(4, 270, "a", "open"); b.want(5, 270, "b", "open");
    b.tick(now += 10);
    ok("the first starts", b.moving() && b.movingId() == 4);
    ok("the second has not been sent", bus.servos[5].goal == 2048);
    Ended e1 = runUntilEnd(b, now);
    ok("the first ends first", std::string(e1.sel) == "a");
    Ended e2 = runUntilEnd(b, now);
    ok("then the second runs", std::string(e2.sel) == "b" && !e2.err && bus.servos[5].pos == degToCounts(270));
    b.want(4, 10, "a", "closed"); b.want(4, 200, "a", "half");
    Ended e3 = runUntilEnd(b, now);
    ok("a second move for the same servo replaces the first", std::string(e3.state) == "half" && !b.busy());
  }

  printf("BB4 faults say why, in words\n");
  {
    FakeBus bus; bus.servos[4].jammedAt = true; bus.servos[4].jam = 2500; busbank::Bank b(bus);
    uint32_t now = 0;
    b.want(4, 270, "valve", "open");
    Ended e = runUntilEnd(b, now);
    ok("a jam ends the move stopped short", e.any && e.err && std::string(e.err).find("stopped short") != std::string::npos);
    FakeBus quiet; busbank::Bank q(quiet);
    q.want(9, 90, "v", "open");
    Ended e2 = runUntilEnd(q, now);
    ok("a servo that is not there says not answering", e2.any && e2.err && std::string(e2.err).find("not answering") != std::string::npos);
    FakeBus drop; drop.servos[4]; busbank::Bank d(drop);
    d.want(4, 270, "v", "open"); d.tick(now += 10); d.tick(now += 30);
    drop.silent = true;
    Ended e3 = runUntilEnd(d, now);
    ok("one that goes quiet mid-move says it stopped answering", e3.any && e3.err && std::string(e3.err).find("stopped answering") != std::string::npos);
    BusServoReport r[kMaxBusServos];
    d.report(r);
    ok("...and the report counts the missed reads", r[0].missed > 0 && !r[0].ok);
  }

  printf("BB5 a board knows at most six\n");
  {
    FakeBus bus; busbank::Bank b(bus);
    bool all = true; for (int id = 2; id < 8; id++) all = all && b.want((uint8_t)id, 90, "g", "s");
    ok("six numbers are fine", all);
    ok("a seventh is refused", !b.want(8, 90, "g", "s"));
  }

  printf("BB6 numbering\n");
  {
    FakeBus bus; busbank::Bank b(bus);
    ok("nothing new: none", b.findNew() == Port::Heard::None);
    bus.servos[1];
    ok("one new: one", b.findNew() == Port::Heard::One);
    const char* err = nullptr;
    ok("it takes number 12", b.assign(12, err) && bus.servos.count(12) && !bus.servos.count(1));
    bus.servos[1]; bus.atNumber[1] = 2;
    ok("two new at once: garbled", b.findNew() == Port::Heard::Garbled);
    ok("...and assign refuses, saying so", !b.assign(13, err) && std::string(err).find("more than one") != std::string::npos);
    bus.atNumber.erase(1);
    ok("a number already answering is refused", !b.assign(12, err) && std::string(err).find("already") != std::string::npos);
    uint8_t ids[8]; const size_t n = b.scan(ids, 8);
    ok("scan lists who answers", n == 2 && ids[0] == 1 && ids[1] == 12);
    ok("reset gives a servo back number 1", b.reset(12) && bus.servos.count(1));
  }

  printf("\n%d/%d passed%s\n", passed, passed + failed, failed ? (", " + std::to_string(failed) + " FAILED").c_str() : "");
  return failed ? 1 : 0;
}
