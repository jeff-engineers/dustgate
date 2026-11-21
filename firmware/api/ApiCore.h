// =============================================================================
// api/ApiCore.h — the API's request handling with no web server in it.
//
// api/HttpApiServer.cpp is 2,500 lines of ESPAsyncWebServer handlers that each do the same four things —
// check the key, parse a body, validate it, answer — with the logic they decide with written inline.
// A second brain (the native one) cannot reuse any of that: the logic is tangled in the transport. This is
// the other shape. A handler takes a Request and a Backend (what the brain can DO: move a gate, pair a node,
// stage an image) and returns a Response; the shell owns the socket, the key check and the threading.
//
// STATUS (2026-10-05): the native brain serves everything here. The ESP32's HttpApiServer still has its own
// copies of these routes and moves onto this one route at a time, each needing a bench run — none has yet.
// Routes are added HERE first, so a new one is never written twice.
//
// PURE — STL + ArduinoJson, no Arduino.h; test_apicore.cpp drives it with a fake backend.
// =============================================================================
#pragma once
#include <cstdint>
#include <string>
#include <cstdlib>
#include <ArduinoJson.h>
#include "../control/NodeLink.h"   // the bus servo bounds and parseServoFrame, so a refusal here is the board's own

namespace api {

struct Request {
    std::string method;     // "GET" | "POST" | "PUT" | "DELETE"
    std::string path;       // no query string
    std::string query;      // after '?', undecoded
    std::string body;
    // A query parameter's value, or "" — no decoding, enough for the numbers and words these routes carry.
    std::string param(const char* key) const {
        const std::string k = std::string(key) + "=";
        size_t at = 0;
        while (at <= query.size()) {
            const size_t amp = query.find('&', at);
            const std::string pair = query.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
            if (pair.compare(0, k.size(), k) == 0) return pair.substr(k.size());
            if (amp == std::string::npos) break;
            at = amp + 1;
        }
        return "";
    }
};

struct Response {
    int         status = 200;
    std::string body   = "{\"ok\":true}";
    std::string type   = "application/json";
};

inline Response error(int status, const char* msg) {
    Response r; r.status = status;
    StaticJsonDocument<192> d; d["error"] = msg;
    r.body.clear(); serializeJson(d, r.body);
    return r;
}
inline Response ok() { return Response(); }
inline Response json(const std::string& body, int status = 200) { Response r; r.status = status; r.body = body; return r; }

// What the brain can do. Every default REFUSES, so a shell implements only what its platform has and the
// rest answers honestly instead of pretending: a Pi has no servo bank of its own, an ESP32 without a
// slider has no homing, and "not here" is a real answer.
class Backend {
public:
    virtual ~Backend() {}
    virtual bool setMachineManual(const std::string& /*machineId*/, bool /*on*/) { return false; }       // false = no such tool
    virtual bool setCollectorManual(const std::string& /*systemId*/, bool /*on*/) { return false; }   // "" = the first system
    // A setup-time jog of one channel on a paired board. `why` is set on a refusal.
    virtual bool jog(const std::string& /*controllerId*/, int /*channel*/, int /*angle*/, bool /*detach*/, std::string& why) {
        why = "no servo support on this brain"; return false;
    }
    // A bus servo, by number, on a paired board (2026-11-01): a setup move to `angle` (0..359), a SERVO order (find,
    // assign, wiggle, torque, reset, scan — nodelink.js servo()) and its reply. `why` is set on a refusal.
    virtual bool jogBus(const std::string& /*controllerId*/, int /*bus*/, int /*angle*/, std::string& why) {
        why = "no bus servos on this brain"; return false;
    }
    virtual uint32_t servoOp(const std::string& /*controllerId*/, const char* /*op*/, int /*bus*/, int /*to*/, int /*on*/, std::string& why) {
        why = "no bus servos on this brain"; return 0;
    }
    virtual std::string servoReply(const std::string& /*controllerId*/, uint32_t /*seq*/) { return std::string(); }
    virtual void resetAll() {}
    virtual void pairNode(const std::string& /*host*/, const std::string& /*name*/, bool /*remove*/, bool /*takeover*/) {}
    virtual void pauseLinks(bool /*paused*/) {}
    virtual std::string discoverNodes() { return "[]"; }
    virtual bool updateNode(const std::string& /*id*/, std::string& why) { why = "node updates are not available on this brain"; return false; }
    // Setting up a slider on a node: drive it to `mm` from its home end (TopologyRuntime::driveLinearTo), and whether
    // anything is still moving. `why` is set on a refusal.
    virtual bool linearGoto(const std::string& /*selectorId*/, float /*mm*/, bool /*homeFirst*/, std::string& why) {
        why = "this brain cannot drive a slider on a node"; return false;
    }
    virtual bool gateMoving() { return false; }
    // Why the last move of this selector's board did not get there — the board's move fault, in the node's own words
    // when it gave some (STATE.err) — or "" (2026-10-16). The slider page shows it instead of waiting on in silence.
    virtual std::string gateFault(const std::string& /*selectorId*/) { return std::string(); }
    // POST /api/node-image: the shell hands over the raw body and the headers it parsed. Not part of handle().
};

// Answers a request this module owns, or returns false and leaves `out` alone (the shell tries its own
// routes, then 404s). The caller has ALREADY checked the API key.
inline bool handle(const Request& req, Backend& be, Response& out) {
    const std::string& p = req.path;
    const bool post = req.method == "POST";

    // ── manual switches ────────────────────────────────────────────────────
    if (post && p == "/api/tool") {
        StaticJsonDocument<160> d;
        if (deserializeJson(d, req.body)) { out = error(400, "invalid JSON"); return true; }
        const char* id = d["toolId"].as<const char*>();
        if (!id || !*id) { out = error(400, "missing 'toolId'"); return true; }
        if (!d.containsKey("on")) { out = error(400, "missing 'on'"); return true; }
        out = be.setMachineManual(id, d["on"].as<bool>()) ? ok() : error(404, "unknown tool");
        return true;
    }
    if (post && p == "/api/collector") {
        StaticJsonDocument<160> d;
        if (deserializeJson(d, req.body)) { out = error(400, "invalid JSON"); return true; }
        if (!d.containsKey("on")) { out = error(400, "missing 'on'"); return true; }
        const char* sys = d["systemId"].as<const char*>();
        out = be.setCollectorManual(sys ? sys : "", d["on"].as<bool>()) ? ok() : error(404, "unknown system");
        return true;
    }
    // The pre-multi-system spelling of the same thing, kept because the app still calls it.
    if (post && p == "/api/dustcollector/switch") {
        StaticJsonDocument<64> d;
        if (deserializeJson(d, req.body)) { out = error(400, "invalid JSON"); return true; }
        out = be.setCollectorManual("", d["on"] | false) ? ok() : error(404, "unknown system");
        return true;
    }

    // ── a servo, set up by hand ────────────────────────────────────────────
    if (post && p == "/api/servo/jog") {
        StaticJsonDocument<160> d;
        if (deserializeJson(d, req.body)) { out = error(400, "invalid JSON"); return true; }
        const char* cid = d["controllerId"].as<const char*>();
        const std::string controller = cid ? cid : "";
        // A bus servo, by number: angles are its encoder's, a whole turn.
        if (d.containsKey("bus")) {
            const int bus = d["bus"] | 0, angle = d["angle"] | -1;
            if (!topo::nodelink::busIdOk(bus)) { out = error(400, "bus servo number out of range"); return true; }
            if (angle < 0 || angle > topo::nodelink::kMaxBusAngle) { out = error(400, "angle out of range (0-359)"); return true; }
            std::string why;
            out = be.jogBus(controller, bus, angle, why) ? ok() : error(why.rfind("no bus", 0) == 0 ? 501 : 502, why.c_str());
            return true;
        }
        if (!d.containsKey("channel")) { out = error(400, "missing 'channel'"); return true; }
        const int ch = d["channel"].as<int>();
        // A paired board may have up to 16 channels; the brain's own bank is its build's, which a shell knows.
        if (ch < 0 || ch >= 16) { out = error(400, "channel out of range"); return true; }
        const bool detach = d["detach"] | false;
        int angle = 0;
        if (!detach) {
            if (!d.containsKey("angle")) { out = error(400, "missing 'angle'"); return true; }
            angle = d["angle"].as<int>();
            if (angle < 0 || angle > 180) { out = error(400, "angle out of range (0-180)"); return true; }
        }
        std::string why;
        if (be.jog(controller, ch, angle, detach, why)) out = ok();
        else out = error(why.rfind("no servo", 0) == 0 ? 501 : 502, why.c_str());
        return true;
    }

    // ── a bus servo's SERVO orders (2026-11-01) ────────────────────────────
    // POST {controllerId, op, bus?, to?, on?} → {seq}; GET ?controllerId=&seq= → {done:false} or {done:true, reply}.
    // Two calls rather than one that waits: a scan takes seconds and the brain's one thread must not.
    if (post && p == "/api/servo/op") {
        StaticJsonDocument<256> d;
        if (deserializeJson(d, req.body)) { out = error(400, "invalid JSON"); return true; }
        const char* cid = d["controllerId"].as<const char*>();
        if (!cid || !*cid) { out = error(400, "missing 'controllerId'"); return true; }
        // Bounded here exactly as the board bounds the frame (parseServoFrame), so a refusal is said now, not lost.
        StaticJsonDocument<192> f;
        f["t"] = "SERVO"; f["seq"] = 1; f["op"] = d["op"];
        if (d.containsKey("bus")) f["bus"] = d["bus"];
        if (d.containsKey("to"))  f["to"]  = d["to"];
        if (d.containsKey("on"))  f["on"]  = d["on"];
        topo::nodelink::ServoOrder o; const char* err = nullptr;
        if (!topo::nodelink::parseServoFrame(f.as<JsonObjectConst>(), o, err)) { out = error(400, err); return true; }
        std::string why;
        const uint32_t seq = be.servoOp(cid, topo::nodelink::kServoOps[o.op], o.bus, o.to, o.op == 3 ? (o.on ? 1 : 0) : -1, why);
        if (!seq) { out = error(why.rfind("no bus", 0) == 0 ? 501 : 409, why.c_str()); return true; }
        StaticJsonDocument<48> r; r["seq"] = seq;
        std::string s; serializeJson(r, s); out = json(s);
        return true;
    }
    if (req.method == "GET" && p == "/api/servo/reply") {
        const std::string rep = be.servoReply(req.param("controllerId"), (uint32_t)std::strtoul(req.param("seq").c_str(), nullptr, 10));
        out = json(rep.empty() ? std::string("{\"done\":false}") : "{\"done\":true,\"reply\":" + rep + "}");
        return true;
    }

    // ── the linear slider: nothing here has one unless a shell says so ─────
    // A brain without a rack answers these as the ESP32 does when built without one.
    // Settings' "reset gate calibration" presses this on every build. With no rack there is nothing to clear, which is a
    // successful no-op, not an error toast.
    if (post && p == "/api/clearcal") { out = ok(); return true; }
    if (post && p == "/api/wifi/reset") { out = error(501, "this brain is not on Wi-Fi of its own"); return true; }
    if (p == "/api/jog" || p == "/api/home" || p == "/api/calibrate" || p == "/api/clearcal" ||
        p == "/api/move" || p == "/api/setstop" || p == "/api/motion" || p == "/api/stops" ||
        p.rfind("/api/config/", 0) == 0) {
        out = error(501, "this brain has no slider of its own"); return true;
    }

    // ── a slider on a NODE, set up by hand ─────────────────────────────────
    // The single-board routes below address the brain's own rack; these address a sliding gate by its selector id,
    // wherever it lives. POST {selectorId, mm, home?}; GET answers {"moving": bool} (a home takes a while).
    if (post && p == "/api/linear/goto") {
        StaticJsonDocument<160> d;
        if (deserializeJson(d, req.body)) { out = error(400, "invalid JSON"); return true; }
        const char* id = d["selectorId"].as<const char*>();
        if (!id || !*id) { out = error(400, "missing 'selectorId'"); return true; }
        if (!d["mm"].is<float>() && !d["mm"].is<int>()) { out = error(400, "missing 'mm'"); return true; }
        std::string why;
        // `home`: find the datum again first (the first move of a setup) — SET.home on the wire.
        out = be.linearGoto(id, d["mm"].as<float>(), d["home"] | false, why) ? ok() : error(409, why.c_str());
        return true;
    }
    if (req.method == "GET" && p == "/api/linear/state") {
        // ?selectorId= adds `fault`: why that gate's last move did not get there, when it did not.
        StaticJsonDocument<384> d;
        d["moving"] = be.gateMoving();
        const std::string sel = req.param("selectorId");
        if (!sel.empty()) {
            const std::string f = be.gateFault(sel);
            if (!f.empty()) d["fault"] = f;
        }
        std::string s; serializeJson(d, s);
        out = json(s);
        return true;
    }

    // ── the shop ───────────────────────────────────────────────────────────
    if (post && p == "/api/reset-all") { be.resetAll(); out = ok(); return true; }

    // ── boards ─────────────────────────────────────────────────────────────
    if (post && p == "/api/nodes/pair") {
        StaticJsonDocument<256> d;
        if (deserializeJson(d, req.body)) { out = error(400, "invalid JSON"); return true; }
        const char* host = d["host"].as<const char*>();
        if (!host || !*host) { out = error(400, "missing 'host'"); return true; }
        be.pairNode(host, d["name"] | "", d["remove"] | false, d["takeover"] | false);
        out = ok(); return true;
    }
    if (post && p == "/api/nodes/pause") {
        StaticJsonDocument<64> d;
        if (deserializeJson(d, req.body) || !d["paused"].is<bool>()) { out = error(400, "need {\"paused\": true|false}"); return true; }
        be.pauseLinks(d["paused"].as<bool>());
        out = ok(); return true;
    }
    if (req.method == "GET" && p == "/api/nodes/discover") { out = json(be.discoverNodes()); return true; }
    if (post && p == "/api/nodes/update") {
        StaticJsonDocument<128> d;
        if (deserializeJson(d, req.body)) { out = error(400, "invalid JSON"); return true; }
        const char* id = d["id"].as<const char*>();
        if (!id || !*id) { out = error(400, "missing 'id'"); return true; }
        std::string why;
        out = be.updateNode(id, why) ? ok() : error(409, why.c_str());
        return true;
    }
    return false;
}

}  // namespace api
