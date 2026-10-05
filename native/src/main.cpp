// native/src/main.cpp — dustgate-brain, the Linux/macOS shell around the shared brain core.
//
// STATUS (2026-10-04): step 1 of the native build. This runs the node half of the core — the SAME
// NodeSession the ESP32 runs — against real nodes: it accepts a node's JOIN on /nodelink, does the
// HELLO/WELCOME handshake, pings, and answers GET /api/nodes. There is no layout, routing or UI yet;
// those come as the API handlers move behind the platform interface (docs/brain-options.md).
//
// ONE THREAD. Everything runs on the io_context's thread, which is what makes NodeSession's "the shell
// serialises access" a non-issue here.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <csignal>
#include <deque>
#include <functional>
#include <random>
#include <iostream>
#include <fstream>
#include <sstream>
#include "ApiCore.h"
#include "Log.h"
#include "Md5.h"
#include "NodeHub.h"
#include "CollectorDriver.h"
#include "DeviceProblems.h"
#include "NodeStatus.h"
#include "PlugPoller.h"
#include "Sweep.h"
#include "RemoteRfPresser.h"
#include "TopologyRuntime.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using udp = net::ip::udp;
using namespace dgbrain;
namespace nl = topo::nodelink;

static NodeHub* g_hub = nullptr;
static topo::TopologyRuntime g_rt;
static std::string g_topoJson, g_topoPath, g_topoErr, g_stateDir;

// ── the plugs the brain polls itself ──────────────────────────────────────────────────────────────
// Tools whose plug no node polls, and the collector's own plugs. The drivers are the ESP32's; this only decides
// WHICH plugs and hands each reading to the runtime the way the sketch's loop does.
namespace plughttp { void setPort(const std::string&); }
static dgbrain::PlugPoller g_poller;
static dgbrain::Sweep g_sweep;
static std::string g_localIp, g_plugPort = "80";
static outletops::Self selfIdentity();
static uint32_t g_plugSyncAtMs = 0;
static std::map<std::string, int> g_swAsserted;       // "s:<system>" -> 0/1 last asked of the plug
static std::map<std::string, uint32_t> g_onSince;     // system -> when it was commanded on

static void syncPlugs() {
    std::vector<dgbrain::PlugPoller::Target> t;
    for (const topo::PlannedSensor& p : g_rt.sensorPlan()) {
        if (p.kind != topo::PlannedSensor::Kind::Plug) continue;
        topo::TopologyRuntime::NodePlugReading np;
        if (g_rt.nodePlug(p.id, np)) continue;             // a node polls this one
        t.push_back({"m:" + p.id, p.ip, p.tasmota});
    }
    for (const std::string& sys : g_rt.systemIds()) {
        JsonObjectConst so = g_rt.collectorSensorOutlet(sys), co = g_rt.collectorOutlet(sys);
        if (const char* ip = so["ip"].as<const char*>()) if (*ip) t.push_back({"c:" + sys, ip, std::string(so["kind"] | "") == "tasmota"});
        if (const char* ip = co["ip"].as<const char*>()) if (*ip) t.push_back({"s:" + sys, ip, false});
    }
    g_poller.sync(t);
    g_plugSyncAtMs = dgbrain::nowMs();
}

// The problems a person must see about boards and plugs: the rules are control/DeviceProblems.h, shared with the ESP32.
static void raiseDeviceProblems(uint32_t now) {
    static topo::DeviceProblems problems;
    static uint32_t lastMs = 0;
    if (now - lastMs < 2000) return;
    lastMs = now;
    std::vector<topo::BoardView> boards;
    for (auto& kv : g_hub->nodes()) {
        const auto h = kv.second->session.health();
        topo::BoardView b; b.host = kv.first; b.linked = h.linked; b.refused = h.refused; b.downForMs = h.downForMs; b.moveFault = h.moveFault;
        boards.push_back(b);
    }
    std::vector<topo::PlugView> plugs;
    for (const topo::PlannedSensor& p : g_rt.sensorPlan()) {
        if (p.kind != topo::PlannedSensor::Kind::Plug) continue;
        topo::PlugView v; v.key = "plug:" + p.id; v.ip = p.ip;
        const char* nm = topo::machineDoc(g_rt.topology(), p.id)["sensor"]["outlet"]["name"].as<const char*>();
        v.name = (nm && *nm) ? nm : p.id;
        topo::TopologyRuntime::NodePlugReading np;
        v.reachable = g_rt.nodePlug(p.id, np) ? np.reachable : g_poller.read("m:" + p.id).reachable;
        plugs.push_back(v);
    }
    problems.update(g_rt, boards, plugs, now);
}

static void feedPlugs(uint32_t now) {
    if (now - g_plugSyncAtMs > 5000) syncPlugs();          // a node joining or leaving changes who polls what
    for (const topo::PlannedSensor& p : g_rt.sensorPlan()) {
        if (p.kind != topo::PlannedSensor::Kind::Plug) continue;
        const auto r = g_poller.read("m:" + p.id);
        if (r.have) g_rt.setMachinePower(p.id, r.reachable ? r.watts : 0.0f);
    }
    for (const std::string& sys : g_rt.systemIds()) {
        const bool want = g_rt.collectorOn(sys);
        if (!want) g_onSince[sys] = 0; else if (!g_onSince[sys]) g_onSince[sys] = now;
        if (g_rt.collectorHasOutlet(sys)) {                 // switched by a plug of ours: a plain on/off
            const std::string key = "s:" + sys;
            auto it = g_swAsserted.find(key);
            if (it == g_swAsserted.end() || it->second != (want ? 1 : 0)) { g_poller.setSwitch(key, want); g_swAsserted[key] = want ? 1 : 0; }
        }
        if (g_rt.collectorHasClamp(sys)) continue;          // the clamp's reading stands (pollSensors writes it)
        const bool haveSensor = !g_rt.collectorSensorOutlet(sys)["ip"].isNull();
        const auto r = g_poller.read((haveSensor ? "c:" : "s:") + sys);
        if (r.have) g_rt.setCollectorPlug(sys, r.watts, r.reachable, g_onSince[sys] ? now - g_onSince[sys] : 0);
    }
}

// ── the collector: one presser and one press-state per system that has a remote ──────────────────
struct CollectorSlot { std::string sys; std::unique_ptr<topo::RemoteRfPresser> presser; topo::PressState ps; };
static std::vector<CollectorSlot> g_collectors;
static std::vector<topo::PressState> g_oldPress;

static void rebuildPressers() {
    g_collectors.clear();
    for (const std::string& sys : g_rt.systemIds()) {
        JsonObjectConst rf = g_rt.collectorRf(sys);
        if (rf.isNull()) continue;
        // There is no pad on this machine: the transmitter is always a paired node's, named by controllerId.
        const std::string board = rf["controllerId"] | "";
        if (board.empty() || topo::isOwnBoard(board, "")) { dglog::linef("[RF] collector %s: the layout names no board for its transmitter\n", sys.c_str()); continue; }
        CollectorSlot c; c.sys = sys;
        c.presser.reset(new topo::RemoteRfPresser(&g_hub->bus(), board,
            (uint8_t)(rf["address"] | (int)topo::rf::kRocklerAddress), (uint8_t)(rf["data"] | (int)topo::rf::kRocklerData),
            (uint32_t)(rf["tickUs"] | (int)topo::rf::kDefaultTickUs), (uint32_t)(rf["repeats"] | (int)topo::rf::kDefaultRepeats)));
        dglog::linef("[RF] collector %s pressed by RF through board %s\n", sys.c_str(), board.c_str());
        g_collectors.push_back(std::move(c));
    }
}

struct StdoutHooks : topo::DriverHooks { void say(const std::string& l) override { dglog::line(l); } };
static void driveCollectors(uint32_t now) {
    static StdoutHooks hooks;
    for (auto& c : g_collectors) topo::driveCollectorPress(g_rt, c.sys, *c.presser, c.ps, g_rt.collectorOn(c.sys), now, hooks);
}

static std::string g_pairPath;
static void savePairs() {
    if (g_pairPath.empty()) return;
    DynamicJsonDocument d(4096); JsonArray a = d.to<JsonArray>();
    for (auto& kv : g_hub->nodes()) { JsonObject o = a.createNestedObject(); o["host"] = kv.first; o["name"] = kv.second->name; }
    std::ofstream f(g_pairPath, std::ios::binary | std::ios::trunc); serializeJson(d, f);
}

static bool adoptLayout(const std::string& json) {
    std::string err;
    if (!g_rt.adopt(json.data(), json.size(), err)) { g_topoErr = err; return false; }
    g_topoErr.clear(); g_topoJson = json;
    rebuildPressers();
    syncPlugs();
    if (!g_topoPath.empty()) { std::ofstream f(g_topoPath, std::ios::binary | std::ios::trunc); f << json; }
    dglog::linef("[TOPO] layout adopted (%zu bytes)\n", json.size());
    return true;
}
static unsigned g_nextLinkId = 1;
struct Knock { std::string ip; uint32_t atMs; };
static std::map<std::string, Knock> g_knocks;   // unpaired nodes that dialled in lately: GET /api/nodes/discover
static bool g_trace = false;   // --trace: print every frame a node sends

static std::string refuseFrame(const char* reason) {
    StaticJsonDocument<96> d; nl::buildRefuse(d.to<JsonObject>(), reason);
    std::string o; serializeJson(d, o); return o;
}

// ── one node's WebSocket ────────────────────────────────────────────────────
class NodeWs : public std::enable_shared_from_this<NodeWs> {
public:
    NodeWs(beast::tcp_stream&& s, std::string remote) : _ws(std::move(s)), _timer(_ws.get_executor()), _remote(std::move(remote)) {}

    void run(http::request<http::string_body> req) {
        _ws.set_option(ws::stream_base::timeout{std::chrono::seconds(5), ws::stream_base::none(), false});
        _ws.control_callback([self = shared_from_this()](ws::frame_type k, beast::string_view) {
            if (k == ws::frame_type::pong && self->_node) self->_node->session.onPong();
        });
        _ws.async_accept(req, [self = shared_from_this()](beast::error_code ec) {
            if (ec) return;
            beast::get_lowest_layer(self->_ws).expires_never();
            self->read();
            self->tick();
        });
    }

private:
    struct Item { bool ping; std::string text; bool closeAfter; };

    void read() {
        _ws.async_read(_buf, [self = shared_from_this()](beast::error_code ec, size_t) {
            if (ec) { self->gone(); return; }
            std::string m = beast::buffers_to_string(self->_buf.data());
            self->_buf.consume(self->_buf.size());
            self->onText(m);
            if (!self->_closing) self->read();
        });
    }

    void onText(const std::string& m) {
        if (_node) { if (g_trace) dglog::linef("[TRACE] %s <- %s\n", _node->id.c_str(), m.c_str()); _node->session.onFrame(m.data(), m.size()); return; }
        StaticJsonDocument<192> d;
        if (deserializeJson(d, m) || std::strcmp(d["t"] | "", "JOIN") != 0) return;
        const std::string id = d["nodeId"] | "";
        if ((d["v"] | 0) != nl::kVersion) { enqueue({false, refuseFrame("busy"), true}); return; }
        std::shared_ptr<Node> n = g_hub->find(id);
        if (g_hub->paused) { enqueue({false, refuseFrame("busy"), true}); return; }
        if (!n) {
            dglog::linef("[NODE] JOIN from %s (%s) - not paired, refused", id.c_str(), _remote.c_str());
            g_knocks[id] = Knock{_remote, nowMs()};
            enqueue({false, refuseFrame("not-paired"), true}); return;
        }
        if (!n->session.onAttach()) {
            dglog::linef("[NODE] JOIN from %s - already linked, refused as a duplicate\n", id.c_str());
            enqueue({false, refuseFrame("duplicate"), true}); return;
        }
        _node = n; _linkId = g_nextLinkId++; n->linkId = _linkId;
        _attachedMs = _lastOnlineMs = nowMs(); _lastPingMs = nowMs();
        dglog::linef("[NODE] %s dialled in from %s\n", id.c_str(), _remote.c_str());
        enqueue({false, n->session.helloFrame(), false});
    }

    void tick() {
        _timer.expires_after(std::chrono::milliseconds(200));
        _timer.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (ec || self->_closing) return;
            if (self->_node && (self->_node->removed || g_hub->paused)) { self->enqueue({false, "", true}); self->_node->session.onDown(); }
            else if (self->_node) {
                for (int i = 0; i < 4; i++) { std::string f; if (!self->_node->session.nextFrame(f)) break; if (g_trace) dglog::linef("[TRACE] %s -> %s\n", self->_node->id.c_str(), f.c_str()); self->enqueue({false, f, false}); }
                const uint32_t now = nowMs();
                if (now - self->_lastPingMs >= nl::kPingIntervalMs) { self->_lastPingMs = now; self->enqueue({true, "", false}); }
                if (self->_node->session.online()) self->_lastOnlineMs = now;
                else if (now - self->_lastOnlineMs > 2 * nl::kPongTimeoutMs) {
                    dglog::linef("[NODE] %s silent - closing its socket\n", self->_node->id.c_str());
                    self->enqueue({false, "", true});
                }
            }
            self->tick();
        });
    }

    void enqueue(Item it) { _q.push_back(std::move(it)); if (!_writing) write(); }

    void write() {
        if (_q.empty()) { _writing = false; return; }
        _writing = true;
        Item it = std::move(_q.front()); _q.pop_front();
        auto self = shared_from_this();
        auto done = [self, closeAfter = it.closeAfter](beast::error_code ec, size_t) {
            if (ec) { self->gone(); return; }
            if (closeAfter) { self->_closing = true; self->_ws.async_close(ws::close_code::normal, [self](beast::error_code) { self->gone(); }); return; }
            self->write();
        };
        if (it.ping) _ws.async_ping({}, [done](beast::error_code ec) { done(ec, 0); });
        else if (it.text.empty() && it.closeAfter) { _closing = true; _ws.async_close(ws::close_code::normal, [self](beast::error_code) { self->gone(); }); }
        else { _ws.text(true); _ws.async_write(net::buffer(it.text), done); }
        _hold = std::move(it.text);   // keep the buffer alive for the write
    }

    void gone() {
        _closing = true;
        _timer.cancel();
        if (_node && _node->linkId == _linkId) {
            _node->linkId = 0; _node->session.onDown();
            dglog::linef("[NODE] %s link closed\n", _node->id.c_str());
        }
        _node.reset();
    }

    ws::stream<beast::tcp_stream> _ws;
    net::steady_timer _timer;
    beast::flat_buffer _buf;
    std::deque<Item> _q;
    std::string _hold, _remote;
    bool _writing = false, _closing = false;
    std::shared_ptr<Node> _node;
    unsigned _linkId = 0;
    uint32_t _attachedMs = 0, _lastOnlineMs = 0, _lastPingMs = 0;
};

// ── plain HTTP (the API's first two routes) ─────────────────────────────────
// ── staged node firmware (what nodes pull on an OTA) ─────────────────────────────────────────────────
// Stored in the state directory, so the brain has no flash budget to fight: a 1.6 MB image is nothing to a disk.
struct NodeImage { bool present = false; std::string fw, md5; uint32_t size = 0; };
static NodeImage g_images[2];                                   // 0 = pwm, 1 = linear
static const char* const kImgName[2] = {"pwm", "linear"};
static std::string imgFile(int k, const char* ext) { return g_stateDir + "/node-" + kImgName[k] + ext; }
static std::string imgUrlPath(int k) { return std::string("/node-") + kImgName[k] + ".bin"; }

static void loadImages() {
    for (int k = 0; k < 2; k++) {
        std::ifstream j(imgFile(k, ".json")); if (!j) continue;
        StaticJsonDocument<256> d; if (deserializeJson(d, j)) continue;
        std::ifstream b(imgFile(k, ".bin"), std::ios::binary | std::ios::ate); if (!b) continue;
        g_images[k].fw = d["fw"] | ""; g_images[k].md5 = d["md5"] | ""; g_images[k].size = (uint32_t)b.tellg(); g_images[k].present = true;
    }
}

static topo::NodeImageView imageViewFor(dgbrain::Node& n) {
    const int k = n.session.info().capLinear > 0 ? 1 : 0;
    topo::NodeImageView v; v.present = g_images[k].present; v.fw = g_images[k].fw.c_str();
    return v;
}

static std::string nodesJson() {
    DynamicJsonDocument d(8192);
    JsonArray a = d.createNestedArray("nodes");
    for (auto& kv : g_hub->nodes()) topo::writeNodeEntry(a, kv.second->session, kv.first.c_str(), kv.first.c_str(), kv.second->name.c_str(), imageViewFor(*kv.second));
    // This board is a board too, and has no hardware of its own: the same shape the ESP32 reports.
    JsonObject self = d.createNestedObject("self");
    self["id"] = g_hub->primaryId(); self["name"] = g_hub->primaryId(); self["fw"] = "native"; self["board"] = "native";
    JsonObject sc = self.createNestedObject("caps"); sc["servos"] = 0; sc["linear"] = 0;
    std::string o; serializeJson(d, o); return o;
}

static std::string g_www, g_apiKey;

static const char* mimeOf(const std::string& p) {
    auto ends = [&](const char* e) { size_t n = std::strlen(e); return p.size() >= n && p.compare(p.size() - n, n, e) == 0; };
    if (ends(".html")) return "text/html"; if (ends(".js")) return "text/javascript"; if (ends(".css")) return "text/css";
    if (ends(".json")) return "application/json"; if (ends(".svg")) return "image/svg+xml"; if (ends(".png")) return "image/png";
    if (ends(".ico")) return "image/x-icon"; if (ends(".woff2")) return "font/woff2"; if (ends(".txt")) return "text/plain";
    return "application/octet-stream";
}

// A file under --www, or index.html for any path with no file (the app routes in the browser).
static bool readStatic(std::string path, std::string& body, std::string& mime) {
    if (g_www.empty()) return false;
    const size_t q = path.find('?'); if (q != std::string::npos) path.resize(q);
    if (path.find("..") != std::string::npos) return false;
    if (path == "/") path = "/index.html";
    for (int pass = 0; pass < 2; pass++) {
        std::string full = g_www + (pass ? "/index.html" : path);
        std::ifstream f(full, std::ios::binary);
        if (f) { std::stringstream b; b << f.rdbuf(); body = b.str(); mime = mimeOf(pass ? "/index.html" : path); return true; }
        if (path.rfind("/api/", 0) == 0) return false;
    }
    return false;
}

// ── unpaired nodes that knocked: what GET /api/nodes/discover lists ──────────────────────────────────────
// A node dials its brain, so a board nobody has paired yet announces itself with a JOIN and is turned away
// ("not-paired"). Remembering who knocked recently IS discovery — it needs no multicast, which a shop network may block.
// ── what the shared API (api/ApiCore.h) asks of this brain ────────────────────────────────────────────
static outletops::Self selfIdentity() { return outletops::Self{g_hub->primaryId(), g_hub->primaryId()}; }

// ── claiming a node that has no owner yet ───────────────────────────────────────────────────────────
// A node dials the brain that owns it, but a FRESH node has no owner and dials nobody: it waits for a brain to say HELLO.
// So pairing a new board is the one time this brain dials out — once, to hand over the claim (the HELLO carries our id), after
// which the node persists it and finds us itself (beacon, cached address, <id>.local). Run on a worker thread: it blocks.
static void claimNode(const std::string& hostIn, bool takeover) {
    std::string host = hostIn;
    if (host.find('.') == std::string::npos) host += ".local";
    namespace websocket = boost::beast::websocket;
    try {
        net::io_context io;
        tcp::resolver rs(io);
        beast::error_code ec;
        const auto results = rs.resolve(host, "80", ec);
        if (ec) { dglog::linef("[CLAIM] %s: cannot resolve (%s)", host.c_str(), ec.message().c_str()); return; }
        websocket::stream<beast::tcp_stream> w(io);
        auto step = [&](auto&& start) { ec = {}; start(); io.restart(); io.run(); return !ec; };
        beast::get_lowest_layer(w).expires_after(std::chrono::seconds(5));
        if (!step([&] { beast::get_lowest_layer(w).async_connect(results, [&](beast::error_code e, const tcp::endpoint&) { ec = e; }); }))
            { dglog::linef("[CLAIM] %s: cannot connect (%s)", host.c_str(), ec.message().c_str()); return; }
        beast::get_lowest_layer(w).expires_after(std::chrono::seconds(5));
        if (!step([&] { w.async_handshake(host, "/nodelink", [&](beast::error_code e) { ec = e; }); }))
            { dglog::linef("[CLAIM] %s: not a node (%s)", host.c_str(), ec.message().c_str()); return; }
        StaticJsonDocument<192> d;
        topo::nodelink::buildHello(d.to<JsonObject>(), g_hub->primaryId().c_str(), hostIn.c_str(), takeover);
        std::string hello; serializeJson(d, hello);
        w.text(true);
        beast::get_lowest_layer(w).expires_after(std::chrono::seconds(5));
        if (!step([&] { w.async_write(net::buffer(hello), [&](beast::error_code e, size_t) { ec = e; }); })) { dglog::linef("[CLAIM] %s: write failed", host.c_str()); return; }
        beast::flat_buffer buf;
        beast::get_lowest_layer(w).expires_after(std::chrono::seconds(8));
        if (!step([&] { w.async_read(buf, [&](beast::error_code e, size_t) { ec = e; }); })) { dglog::linef("[CLAIM] %s: no WELCOME (%s)", host.c_str(), ec.message().c_str()); return; }
        StaticJsonDocument<768> r; deserializeJson(r, beast::buffers_to_string(buf.data()));
        const bool accepted = r["accepted"] | true;
        if (accepted) dglog::linef("[CLAIM] %s is ours (%s, %s) - it will dial us now", host.c_str(), (const char*)(r["board"] | "?"), (const char*)(r["fw"] | "?"));
        else dglog::linef("[CLAIM] %s belongs to '%s' - pair it again with takeover to take it", host.c_str(), (const char*)(r["claimedBy"] | "someone else"));
        beast::error_code e2; w.next_layer().socket().shutdown(tcp::socket::shutdown_both, e2);
    } catch (const std::exception& e) { dglog::linef("[CLAIM] %s: %s", host.c_str(), e.what()); }
}

class NativeBackend : public api::Backend {
public:
    bool setToolManual(const std::string& id, bool on) override { return g_rt.setMachineManual(id, on); }
    bool setCollectorManual(const std::string& sys, bool on) override {
        std::string s = sys;
        if (s.empty()) { auto ids = g_rt.systemIds(); if (ids.empty()) return false; s = ids[0]; }
        return g_rt.setCollectorManual(s, on);
    }
    bool jog(const std::string& cid, int ch, int angle, bool detach, std::string& why) override {
        if (cid.empty() || topo::isOwnBoard(cid, "")) { why = "no servo support on this brain"; return false; }
        topo::ActuatorBus* b = g_hub->bus().busForController(cid.c_str());
        if (!b || !b->online()) { why = "that board is not linked"; return false; }
        if (!b->jog(ch, angle, detach)) { why = "the board refused the jog"; return false; }
        return true;
    }
    void resetAll() override {
        g_rt.clear(); g_topoJson.clear(); g_collectors.clear(); g_poller.sync({});
        if (!g_topoPath.empty()) std::remove(g_topoPath.c_str());
        std::vector<std::string> ids; for (auto& kv : g_hub->nodes()) ids.push_back(kv.first);
        for (auto& id : ids) g_hub->remove(id);
        savePairs();
        dglog::line("[API] reset everything requested");
    }
    void pairNode(const std::string& host, const std::string& name, bool remove, bool takeover) override {
        if (remove) g_hub->remove(host);
        else {
            auto n = g_hub->add(host, name); if (takeover) n->session.requestTakeover(); g_knocks.erase(host);
            std::thread([host, takeover] { claimNode(host, takeover); }).detach();   // a fresh node has no owner and dials nobody
        }
        savePairs();
    }
    void pauseLinks(bool p) override { g_hub->paused = p; dglog::line(p ? "[NODE] Links PAUSED \xE2\x80\x94 every link stopped, pairings kept" : "[NODE] Links resumed"); }
    std::string discoverNodes() override {
        DynamicJsonDocument d(4096); JsonArray a = d.to<JsonArray>();
        const uint32_t now = dgbrain::nowMs();
        for (auto& kv : g_knocks) {
            if (now - kv.second.atMs > 120000 || g_hub->find(kv.first)) continue;
            JsonObject o = a.createNestedObject();
            o["host"] = kv.first; o["ip"] = kv.second.ip; o["board"] = "unknown"; o["servos"] = 0;   // a JOIN carries only the id
        }
        std::string out; serializeJson(d, out); return out;
    }
    bool updateNode(const std::string& id, std::string& why) override {
        auto n = g_hub->find(id);
        if (!n) { why = "that board is not paired"; return false; }
        const int k = n->session.info().capLinear > 0 ? 1 : 0;
        if (!g_images[k].present) { why = "no firmware image is staged for that kind of board"; return false; }
        if (g_rt.collectorOn() || g_hub->bus().busy()) { why = "a tool is running - wait until the shop is quiet"; return false; }
        const char* w = "";
        if (!n->session.requestOta(imgUrlPath(k).c_str(), g_images[k].size, g_images[k].md5.c_str(), g_images[k].fw.c_str(), w)) { why = w; return false; }
        return true;
    }
};
static NativeBackend g_backend;

class HttpConn : public std::enable_shared_from_this<HttpConn> {
public:
    explicit HttpConn(tcp::socket&& s) : _stream(std::move(s)) { _parser.body_limit(4u * 1024 * 1024); }   // a node image is ~1.6 MB
    void run() {
        _stream.expires_after(std::chrono::seconds(60));
        http::async_read(_stream, _buf, _parser, [self = shared_from_this()](beast::error_code ec, size_t) {
            if (ec) return;
            self->handle();
        });
    }
private:
    struct Out { http::status st = http::status::ok; std::string body, mime = "application/json"; std::vector<std::pair<std::string, std::string>> headers; };

    void handle() {
        auto& req = _parser.get();
        if (ws::is_upgrade(req) && req.target() == "/nodelink") {
            std::string remote = beast::get_lowest_layer(_stream).socket().remote_endpoint().address().to_string();
            std::make_shared<NodeWs>(std::move(_stream), remote)->run(_parser.release());
            return;
        }
        const std::string target = std::string(req.target());
        api::Request ar; ar.method = std::string(req.method_string());
        const size_t q = target.find('?');
        ar.path = target.substr(0, q); if (q != std::string::npos) ar.query = target.substr(q + 1);
        ar.body = req.body();
        Out o;
        const bool isApi = ar.path.rfind("/api/", 0) == 0;

        // A node image is pulled by a node, with no key (it has none to send): it is public the way the app is.
        if (ar.method == "GET" && (ar.path == "/node-pwm.bin" || ar.path == "/node-linear.bin")) {
            const int k = ar.path == "/node-linear.bin" ? 1 : 0;
            std::ifstream f(imgFile(k, ".bin"), std::ios::binary);
            if (!g_images[k].present || !f) { o.st = http::status::not_found; o.body = "no image"; o.mime = "text/plain"; }
            else { std::stringstream b; b << f.rdbuf(); o.body = b.str(); o.mime = "application/octet-stream"; }
            send(o); return;
        }
        if (!isApi) {
            std::string fb, mime;
            if (ar.method == "GET" && readStatic(ar.path, fb, mime)) { o.body = fb; o.mime = mime; }
            else { o.st = http::status::not_found; o.body = "not found"; o.mime = "text/plain"; }
            send(o); return;
        }
        if (ar.path != "/api/info") {
            auto k = req.find("X-Api-Key");
            if (k == req.end() || std::string(k->value()) != g_apiKey) { o.st = http::status::unauthorized; o.body = "{\"error\":\"unauthorized\"}"; send(o); return; }
        }
        api::Response r;
        if (api::handle(ar, g_backend, r)) { o.st = (http::status)r.status; o.body = r.body; o.mime = r.type; send(o); return; }
        route(ar, req, o);
        if (!_deferred) send(o);
    }

    // The routes only this shell has: they read this process's own state.
    void route(const api::Request& ar, http::request<http::string_body>& req, Out& o) {
        const std::string& t = ar.path; const std::string& m = ar.method;
        auto err = [&](http::status s, const std::string& msg) { o.st = s; o.body = "{\"error\":\"" + msg + "\"}"; };
        // ── smart plugs: blocking HTTP to a device that may not answer, so each runs off the network thread ──
        if (t == "/api/outlets/ping" && m == "POST") {
            StaticJsonDocument<128> d; const std::string ip = deserializeJson(d, ar.body) ? "" : std::string(d["ip"] | "");
            if (ip.empty()) { err(http::status::bad_request, "missing 'ip'"); return; }
            defer([ip] { Out r; r.body = outletops::describeJson(ip.c_str(), selfIdentity()); return r; });
            return;
        }
        if (t == "/api/outlets/name" && m == "POST") {
            StaticJsonDocument<192> d;
            if (deserializeJson(d, ar.body)) { err(http::status::bad_request, "invalid JSON"); return; }
            const std::string ip = d["ip"] | "", label = d["label"] | "";
            const bool take = d["takeover"] | false;
            if (ip.empty()) { err(http::status::bad_request, "missing 'ip'"); return; }
            defer([ip, label, take] { Out r; r.body = outletops::rename(ip.c_str(), label.substr(0, 47).c_str(), take, selfIdentity()); return r; });
            return;
        }
        if (t == "/api/outlets/release" && m == "POST") {
            StaticJsonDocument<128> d; const std::string ip = deserializeJson(d, ar.body) ? "" : std::string(d["ip"] | "");
            if (ip.empty()) { err(http::status::bad_request, "missing 'ip'"); return; }
            // This brain never wrote to a plug on its own (it polls them), so there is no push target to restore.
            defer([ip] { Out r; r.body = outletops::release(ip.c_str(), selfIdentity(), false, ""); return r; });
            return;
        }
        // Taking a plug means repointing ITS push target at this brain. This brain polls its plugs and has no push endpoint
        // to point one at, so there is nothing to take: a plug someone else owns can still be sensed by polling it.
        if (t == "/api/outlets/takeover" && m == "POST") { err(http::status::not_implemented, "this brain polls its plugs; it does not repoint a plug's push target"); return; }
        if (t == "/api/outlets/save" && m == "POST") { o.body = "{\"ok\":true}"; return; }   // nothing to persist: plugs live in the layout
        if (t == "/api/outlets/sweep" && m == "POST") { g_sweep.start(g_localIp, g_plugPort, selfIdentity()); o.body = "{\"ok\":true}"; return; }
        if (t == "/api/outlets/sweep" && m == "DELETE") { g_sweep.cancel(); o.body = "{\"ok\":true}"; return; }
        if (t == "/api/outlets/sweep" && m == "GET") { o.body = g_sweep.progressJson(); return; }
        // No mDNS browser here, so "discover" is what the last sweep found; the sweep is the way to look.
        if (t == "/api/outlets/discover" && m == "GET") {
            DynamicJsonDocument d(16384); JsonArray a = d.to<JsonArray>();
            for (auto& r : g_sweep.rows()) { DynamicJsonDocument one(1024); if (!deserializeJson(one, r)) a.add(one.as<JsonObject>()); }
            serializeJson(d, o.body); return;
        }
        if (t == "/api/outlets" && m == "GET") { o.body = "{\"outlets\":[]}"; return; }
        if (t.rfind("/api/outlets", 0) == 0 && (m == "PUT" || m == "DELETE")) { o.body = "{\"ok\":true}"; return; }
        if (t == "/api/nodes") o.body = nodesJson();
        else if (t == "/api/topology" && m == "GET") { if (g_topoJson.empty()) err(http::status::not_found, "no topology configured"); else o.body = g_topoJson; }
        else if (t == "/api/topology" && m == "PUT") { if (adoptLayout(ar.body)) o.body = "{\"ok\":true}"; else err(http::status::bad_request, g_topoErr); }
        else if (t == "/api/topology" && m == "DELETE") { g_rt.clear(); g_topoJson.clear(); g_collectors.clear(); g_poller.sync({}); if (!g_topoPath.empty()) std::remove(g_topoPath.c_str()); o.body = "{\"ok\":true}"; }
        else if (t == "/api/status") {
            if (g_topoJson.empty()) err(http::status::not_found, "no topology configured");
            else { DynamicJsonDocument d(32768); g_rt.writeStatus(d.to<JsonObject>()); serializeJson(d, o.body); }
        }
        // What the layout wants watched, and what each board last said: the plan, then the reading it is judged by.
        else if (t == "/api/sensors") {
            DynamicJsonDocument d(8192); JsonArray a = d.to<JsonArray>();
            for (const topo::PlannedSensor& p : g_rt.sensorPlan()) {
                JsonObject so = a.createNestedObject();
                so["id"] = p.id; so["kind"] = p.kind == topo::PlannedSensor::Kind::Clamp ? "ct" : p.kind == topo::PlannedSensor::Kind::Plug ? "plug" : "bin";
                so["board"] = p.board;
                if (p.kind == topo::PlannedSensor::Kind::Plug) {
                    so["ip"] = p.ip; so["tasmota"] = p.tasmota;
                    topo::TopologyRuntime::NodePlugReading np;
                    if (g_rt.nodePlug(p.id, np)) { so["polledBy"] = "node"; so["reachable"] = np.reachable; so["watts"] = np.watts; }
                    else { const auto r = g_poller.read("m:" + p.id); so["polledBy"] = "brain"; so["reachable"] = r.reachable; so["watts"] = r.watts; }
                }
            }
            serializeJson(d, o.body);
        }
        else if (t == "/api/problems") { DynamicJsonDocument d(4096); g_rt.writeProblems(d.to<JsonObject>()); serializeJson(d, o.body); }
        // The brain's console since byte N, as the app's Brain log screen reads it.
        else if (t == "/api/serial" && m == "GET") {
            size_t start = 0, next = 0;
            o.body = dglog::readFrom((size_t)std::strtoull(ar.param("from").c_str(), nullptr, 10), start, next);
            o.mime = "text/plain; charset=utf-8";
            o.headers = {{"X-Serial-Start", std::to_string(start)}, {"X-Serial-Next", std::to_string(next)}, {"X-Serial-Boot", std::to_string(dglog::R().bootId)},
                         {"Cache-Control", "no-store"}, {"Access-Control-Expose-Headers", "X-Serial-Start, X-Serial-Next, X-Serial-Boot"}};
        }
        else if (t == "/api/serial" && m == "POST") err(http::status::not_implemented, "this brain has no serial console to type into");
        else if (t == "/api/linklog" && m == "GET") {
            const std::string path = dglog::R().linkLogPath + (ar.param("old").empty() ? "" : ".1");
            std::ifstream f(path, std::ios::binary);
            if (path.empty() || !f) err(http::status::not_found, ar.param("old").empty() ? "no link log yet" : "no rotated link log yet");
            else { std::stringstream b; b << f.rdbuf(); o.body = b.str(); o.mime = "application/x-ndjson"; }
        }
        // Stage the image nodes will pull: raw body, X-Fw and X-Md5 headers, checked against the same size window a node enforces.
        else if (t == "/api/node-image" && m == "POST") {
            const std::string kind = ar.param("kind");
            const int k = kind == "pwm" ? 0 : kind == "linear" ? 1 : -1;
            auto fw = req.find("X-Fw"); auto md = req.find("X-Md5");
            if (k < 0) err(http::status::bad_request, "kind must be pwm or linear");
            else if (fw == req.end() || md == req.end() || md->value().size() != 32) err(http::status::bad_request, "X-Fw and X-Md5 (32 hex) are required");
            else if (ar.body.size() < topo::nodelink::kMinOtaBytes || ar.body.size() > topo::nodelink::kMaxOtaBytes) err(http::status::payload_too_large, "not a node image (size outside what a node accepts)");
            else if (md5::hex(ar.body) != std::string(md->value())) err(http::status::bad_request, "the upload does not match X-Md5");
            else if (g_stateDir.empty()) err(http::status::service_unavailable, "this brain has no state directory to keep an image in");
            else {
                { std::ofstream b(imgFile(k, ".bin"), std::ios::binary | std::ios::trunc); b << ar.body; }
                { std::ofstream j(imgFile(k, ".json"), std::ios::trunc); j << "{\"fw\":\"" << std::string(fw->value()) << "\",\"md5\":\"" << std::string(md->value()) << "\"}"; }
                g_images[k].present = true; g_images[k].fw = std::string(fw->value()); g_images[k].md5 = std::string(md->value()); g_images[k].size = (uint32_t)ar.body.size();
                dglog::linef("[NODEIMG] the %s node image is %s (%u bytes)", kImgName[k], g_images[k].fw.c_str(), (unsigned)g_images[k].size);
                o.body = "{\"ok\":true}";
            }
        }
        // DEV ONLY: stand in for a plug reporting watts, so routing can be driven with no tool running.
        else if (t == "/api/dev/power" && m == "POST") {
            StaticJsonDocument<192> d;
            if (deserializeJson(d, ar.body) || !d["machineId"].is<const char*>()) err(http::status::bad_request, "machineId, watts");
            else { g_rt.setMachinePower(d["machineId"].as<std::string>(), d["watts"] | 0.0f); o.body = "{\"ok\":true}"; }
        }
        else if (t == "/api/info") o.body = "{\"role\":\"native\",\"id\":\"" + g_hub->primaryId() + "\",\"apiKey\":\"" + g_apiKey + "\",\"build\":\"native\",\"uptimeSec\":" + std::to_string(dglog::upMs() / 1000) + "}";
        else err(http::status::not_found, "not found");
    }

    // Run `work` on its own thread and answer when it is done. The connection is kept alive by the closure.
    void defer(std::function<Out()> work) {
        _deferred = true;
        auto self = shared_from_this();
        auto ex = _stream.get_executor();
        std::thread([self, ex, work] { Out r = work(); net::post(ex, [self, r] { self->send(r); }); }).detach();
    }

    void send(const Out& o) {
        auto res = std::make_shared<http::response<http::string_body>>(o.st, _parser.get().version());
        res->set(http::field::content_type, o.mime);
        // The API answers a question about NOW. With no cache header a browser (Safari most of all) may keep serving an
        // earlier answer, which showed a layout error that had already been fixed on the brain.
        res->set(http::field::cache_control, "no-store");
        for (auto& h : o.headers) res->set(h.first, h.second);
        res->body() = o.body; res->prepare_payload(); res->keep_alive(false);
        http::async_write(_stream, *res, [self = shared_from_this(), res](beast::error_code ec, size_t) {
            self->_stream.socket().shutdown(tcp::socket::shutdown_send, ec);
        });
    }
    beast::tcp_stream _stream;
    beast::flat_buffer _buf;
    http::request_parser<http::string_body> _parser;
    bool _deferred = false;   // an operation is running on its own thread and will answer
};

class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(net::io_context& io, tcp::endpoint ep) : _io(io), _acc(io) {
        _acc.open(ep.protocol()); _acc.set_option(net::socket_base::reuse_address(true)); _acc.bind(ep); _acc.listen();
    }
    void run() { accept(); }
private:
    void accept() {
        _acc.async_accept(net::make_strand(_io), [self = shared_from_this()](beast::error_code ec, tcp::socket s) {
            if (!ec) std::make_shared<HttpConn>(std::move(s))->run();
            self->accept();
        });
    }
    net::io_context& _io; tcp::acceptor _acc;
};

// ── the UDP beacon: "DGB1|<id>|<ip>|<port>", fast while a node is down, slow otherwise ──
static std::string guessIp() {
    try {
        net::io_context io; udp::socket s(io); s.connect(udp::endpoint(net::ip::make_address("8.8.8.8"), 53));
        return s.local_endpoint().address().to_string();
    } catch (...) { return "127.0.0.1"; }
}

static void beaconLoop(net::io_context& io, std::shared_ptr<udp::socket> sock, std::shared_ptr<net::steady_timer> t,
                       std::string ip, unsigned port, std::string bcast) {
    const uint32_t every = g_hub->anyDown() ? 5000 : 60000;
    t->expires_after(std::chrono::milliseconds(every));
    t->async_wait([&io, sock, t, ip, port, bcast](beast::error_code ec) {
        if (ec) return;
        std::string msg = "DGB1|" + g_hub->primaryId() + "|" + ip + "|" + std::to_string(port);
        beast::error_code e2;
        sock->send_to(net::buffer(msg), udp::endpoint(net::ip::make_address(bcast), nl::kBeaconPort), 0, e2);
        beaconLoop(io, sock, t, ip, port, bcast);
    });
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::string id = "dustgate", pair, ip, bcast = "255.255.255.255", stateDir;
    unsigned port = 8080, alsoPort = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--id") id = val(); else if (a == "--pair") pair = val(); else if (a == "--port") port = (unsigned)std::stoi(val());
        else if (a == "--ip") ip = val(); else if (a == "--state") stateDir = val(); else if (a == "--also-port") alsoPort = (unsigned)std::stoi(val()); else if (a == "--trace") g_trace = true; else if (a == "--www") g_www = val(); else if (a == "--plug-port") { g_plugPort = val(); plughttp::setPort(g_plugPort); } else if (a == "--key") g_apiKey = val(); else if (a == "--broadcast") bcast = val();
        else { dglog::linef("usage: dustgate-brain [--id dustgate] [--pair nodeId,nodeId,...] [--port 8080] [--also-port 80] [--state dir] [--www dir] [--plug-port 80] [--key k] [--ip a.b.c.d] [--broadcast a.b.c.255]\n"); return a == "--help" ? 0 : 2; }
    }
    std::vector<std::string> ids; std::stringstream ss(pair); std::string x;
    while (std::getline(ss, x, ',')) if (!x.empty()) ids.push_back(x);
    g_stateDir = stateDir;
    { std::random_device rd; dglog::R().bootId = rd(); }
    if (!stateDir.empty()) { dglog::R().linkLogPath = stateDir + "/linklog.txt"; loadImages(); }
    NodeHub hub(id); g_hub = &hub;
    if (!stateDir.empty()) {
        g_pairPath = stateDir + "/nodes.json";
        std::ifstream pf(g_pairPath, std::ios::binary);
        if (pf) { DynamicJsonDocument d(4096); if (!deserializeJson(d, pf)) for (JsonObject o : d.as<JsonArray>()) hub.add(o["host"] | "", o["name"] | ""); }
    }
    for (auto& nid : ids) hub.add(nid, "");
    g_rt.begin(&hub.bus());
    if (!stateDir.empty()) {
        g_topoPath = stateDir + "/topology.json";
        std::ifstream f(g_topoPath, std::ios::binary);
        if (f) { std::stringstream b; b << f.rdbuf(); if (!adoptLayout(b.str())) dglog::linef("[TOPO] stored layout refused: %s\n", g_topoErr.c_str()); }
    }
    if (ip.empty()) ip = guessIp();
    g_localIp = ip;
    if (g_apiKey.empty()) {   // persisted with the state, so a restart does not log every browser out
        const std::string kp = stateDir.empty() ? "" : stateDir + "/apikey";
        std::ifstream kf(kp); if (!kp.empty() && kf) std::getline(kf, g_apiKey);
        if (g_apiKey.empty()) {
            std::random_device rd; char b[33]; for (int i = 0; i < 32; i++) b[i] = "0123456789abcdef"[rd() % 16]; b[32] = 0; g_apiKey = b;
            if (!kp.empty()) { std::ofstream o(kp); o << g_apiKey; }
        }
    }

    net::io_context io(1);
    std::make_shared<Listener>(io, tcp::endpoint(tcp::v4(), (unsigned short)port))->run();
    // Nodes pull firmware from http://<brain ip><path> with no port, so a brain on another port also listens on 80.
    if (alsoPort) std::make_shared<Listener>(io, tcp::endpoint(tcp::v4(), (unsigned short)alsoPort))->run();
    auto sock = std::make_shared<udp::socket>(io, udp::v4()); sock->set_option(net::socket_base::broadcast(true));
    beaconLoop(io, sock, std::make_shared<net::steady_timer>(io), ip, port, bcast);
    net::steady_timer rtTimer(io);
    std::function<void()> rtTick = [&]() {
        rtTimer.expires_after(std::chrono::milliseconds(100));
        rtTimer.async_wait([&](beast::error_code ec) { if (ec) return; g_rt.update(nowMs()); feedPlugs(nowMs()); if (g_rt.loaded()) raiseDeviceProblems(nowMs()); driveCollectors(nowMs()); rtTick(); });
    };
    rtTick();
    net::signal_set sig(io, SIGINT, SIGTERM); sig.async_wait([&](beast::error_code, int) { io.stop(); });
    dglog::linef("dustgate-brain %s on %s:%u, %zu paired node(s)\n", id.c_str(), ip.c_str(), port, ids.size());
    g_poller.start();
    io.run();
    g_poller.stop();
    return 0;
}
