// native/src/main.cpp — dustgate-brain, the Linux/macOS shell around the shared brain core.
//
// STATUS (2026-10-04): step 1 of the native build. This runs the node half of the core — the SAME
// NodeSession the ESP32 runs — against real nodes: it accepts a node's JOIN on /nodelink, does the
// HELLO/WELCOME handshake, pings, and answers GET /api/nodes. There is no layout, routing or UI yet;
// those come as the API handlers move behind the platform interface (docs/brain-options.md).
//
// ONE THREAD. Everything runs on the io_context's thread, which is what makes NodeSession's "the shell
// serialises access" a non-issue here.
// Spelled out, not left to whatever Boost happens to pull in: GCC (the Pi) is stricter than the Mac's clang.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#ifndef DG_COMMIT
#define DG_COMMIT "unknown"
#endif
#include <csignal>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <deque>
#include <netdb.h>
#include <arpa/inet.h>
#include <set>
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
#include "AtomicFile.h"
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
// Boards that took our claim but are too old to dial a brain themselves (no caps.join in their WELCOME): this brain never
// dials a node, so they pair and then never link. Filled by claimNode() on its own thread, read by raiseDeviceProblems().
static std::mutex g_tooOldMu;
static std::set<std::string> g_tooOld;
static topo::TopologyRuntime g_rt;
static std::string g_topoJson, g_topoPath, g_topoErr, g_stateDir;

// ── the plugs the brain polls itself ──────────────────────────────────────────────────────────────
// Tools whose plug no node polls, and the collector's own plugs. The drivers are the ESP32's; this only decides
// WHICH plugs and hands each reading to the runtime the way the sketch's loop does.
namespace plughttp { void setPort(const std::string&); }
static dgbrain::PlugPoller g_poller;
static dgbrain::Sweep g_sweep;
static std::string g_localIp, g_plugPort = "80";
// This brain's own address is NOT fixed: a Pi takes whatever its router gives it, and the router may give it another. g_localIp is
// read from the plug workers, the beacon and the sweep, so it is only touched through localIp() / setLocalIp().
static std::mutex g_ipMu;
static std::string localIp() { std::lock_guard<std::mutex> g(g_ipMu); return g_localIp; }
static void setLocalIp(const std::string& ip) { std::lock_guard<std::mutex> g(g_ipMu); g_localIp = ip; }
static std::string g_bcast, g_ipFile;          // the beacon's address; a file to read the address from (tests: nothing else moves an address)
static bool g_ipFixed = false, g_bcastFixed = false;
static unsigned g_port = 80;
static outletops::Self selfIdentity();
static uint32_t g_plugSyncAtMs = 0;
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
        b.optional = g_rt.loaded() && topo::isOptionalBoard(g_rt.topology(), topo::controllerIdForHost(g_rt.topology(), kv.first));
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
    std::set<std::string> old; { std::lock_guard<std::mutex> g(g_tooOldMu); old = g_tooOld; }
    for (auto& kv : g_hub->nodes()) {
        const std::string key = "old:" + kv.first;
        if (old.count(kv.first) && !kv.second->session.health().linked)
            g_rt.raiseProblem(key, "board-fault", "bad", "board", kv.first,
                              "Its firmware is too old to link to this brain (it waits to be dialled). Reflash it by USB.", now);
        else g_rt.clearProblem(key);
    }
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
            g_poller.setSwitch("s:" + sys, want);           // every tick: the poller re-sends until the plug takes it
        }
        if (g_rt.collectorHasClamp(sys)) continue;          // the clamp's reading stands (pollSensors writes it)
        const bool haveSensor = !g_rt.collectorSensorOutlet(sys)["ip"].isNull();
        const auto r = g_poller.read((haveSensor ? "c:" : "s:") + sys);
        if (r.have) g_rt.setCollectorPlug(sys, r.watts, r.reachable, g_onSince[sys] ? now - g_onSince[sys] : 0);
    }
}

// ── the collector: one presser and one press-state per system that has a remote ──────────────────
// `key` names the blower and the transmitter that presses it, so a layout save can tell "the same remote" from a new one.
struct CollectorSlot { std::string sys, key; std::unique_ptr<topo::RemoteRfPresser> presser; topo::PressState ps; };
static std::vector<CollectorSlot> g_collectors;

static void rebuildPressers() {
    // A SAVE IS NOT A REBOOT (see TopologyRuntime::adopt). The press bookkeeping says whether WE started a blower, and a
    // fresh one reads every running blower as started by a person, which nothing then presses off. It is kept for the same
    // system pressed by the same transmitter (board, address, data); a different remote is a different blower.
    std::map<std::string, topo::PressState> kept;
    for (CollectorSlot& c : g_collectors) kept[c.key] = c.ps;
    g_collectors.clear();
    for (const std::string& sys : g_rt.systemIds()) {
        JsonObjectConst rf = g_rt.collectorRf(sys);
        if (rf.isNull()) continue;
        // There is no pad on this machine: the transmitter is always a paired node's, named by controllerId.
        const std::string board = rf["controllerId"] | "";
        if (board.empty() || topo::isOwnBoard(board, "")) { dglog::linef("[RF] collector %s: the layout names no board for its transmitter\n", sys.c_str()); continue; }
        const uint8_t addr = (uint8_t)(rf["address"] | (int)topo::rf::kRocklerAddress), data = (uint8_t)(rf["data"] | (int)topo::rf::kRocklerData);
        CollectorSlot c; c.sys = sys;
        c.key = sys + "|" + board + "|" + std::to_string(addr) + "|" + std::to_string(data);
        auto k = kept.find(c.key);
        if (k != kept.end()) c.ps = k->second;
        c.presser.reset(new topo::RemoteRfPresser(&g_hub->bus(), board, addr, data,
            (uint32_t)(rf["tickUs"] | (int)topo::rf::kDefaultTickUs), (uint32_t)(rf["repeats"] | (int)topo::rf::kDefaultRepeats)));
        // Said when a layout loads, so it must not read like an event: nothing is pressed here.
        dglog::linef("[RF] collector %s: its remote is keyed through board %s%s\n", sys.c_str(), board.c_str(),
                     k != kept.end() ? " (press state kept across the save)" : "");
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
    std::string out; serializeJson(d, out);
    if (!dgbrain::writeFileAtomic(g_pairPath, out, true)) dglog::linef("[STATE] could not save %s: %s\n", g_pairPath.c_str(), std::strerror(errno));
}

// persist: 0 = do not touch the file (a layout read FROM the file), 1 = save it and keep the previous copy as .bak (a person saved),
// 2 = save it WITHOUT rotating .bak (restoring from .bak: the copy beside it is the good one and must not be replaced by the damaged file).
static bool adoptLayout(const std::string& json, int persist = 1) {
    std::string err;
    const bool fromNothing = !g_rt.loaded();   // boot, or a first layout: settle it (TopologyRuntime::settleAtBoot)
    if (!g_rt.adopt(json.data(), json.size(), err)) { g_topoErr = err; return false; }
    if (fromNothing) g_rt.settleAtBoot();
    g_topoErr.clear(); g_topoJson = json;
    rebuildPressers();
    syncPlugs();
    // Atomically, keeping the last good copy beside it: a power cut mid-save must not cost the shop its layout.
    if (persist && !g_topoPath.empty() && !dgbrain::writeFileAtomic(g_topoPath, json, persist == 1))
        dglog::linef("[TOPO] could not save the layout to %s: %s - it is adopted but will be lost at a restart\n", g_topoPath.c_str(), std::strerror(errno));
    dglog::linef("[TOPO] layout adopted (%zu bytes)\n", json.size());
    return true;
}
// Forget the layout: what DELETE /api/topology does, and the first half of resetting everything.
static void clearLayout() {
    g_rt.clear(); g_topoJson.clear(); g_collectors.clear(); g_poller.sync({});
    if (!g_topoPath.empty()) std::remove(g_topoPath.c_str());
}
// At most `max` bytes, cut on a character boundary: a plug label cut mid-character (an accent, an emoji) is invalid UTF-8.
static std::string utf8Prefix(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;   // back off continuation bytes to the start of a character
    return s.substr(0, n);
}
static unsigned g_nextLinkId = 1;
struct Knock { std::string ip; uint32_t atMs; };
// Unpaired nodes that dialled in lately: GET /api/nodes/discover. Touched ONLY on the network thread — the scan that lists them
// runs on a worker, so it is handed a copy (bug search 2026-10-06: iterating this while a JOIN inserted could crash the brain).
static std::map<std::string, Knock> g_knocks;
static constexpr uint32_t kKnockKeepMs = 120000;
static constexpr size_t   kMaxKnocks   = 32;      // a LAN client inventing ids must not grow it without bound
static void noteKnock(const std::string& id, const std::string& ip, uint32_t now) {
    for (auto it = g_knocks.begin(); it != g_knocks.end();) it = (now - it->second.atMs > kKnockKeepMs) ? g_knocks.erase(it) : std::next(it);
    if (g_knocks.size() >= kMaxKnocks && !g_knocks.count(id)) return;
    g_knocks[id] = Knock{ip, now};
}
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
        if ((d["v"] | 0) != nl::kVersion) {
            // There is no "version" refuse reason, so the node hears "busy" and retries forever; say here what it is.
            static std::map<std::string, uint32_t> lastSaid;
            const uint32_t now = nowMs();
            if (!lastSaid.count(id) || now - lastSaid[id] > 60000) {
                lastSaid[id] = now;
                dglog::linef("[NODE] JOIN from %s (%s) speaks NodeLink v%d, this brain v%d - refused; reflash it\n",
                             id.c_str(), _remote.c_str(), (int)(d["v"] | 0), (int)nl::kVersion);
            }
            enqueue({false, refuseFrame("busy"), true}); return;
        }
        std::shared_ptr<Node> n = g_hub->find(id);
        if (g_hub->paused) { enqueue({false, refuseFrame("busy"), true}); return; }
        if (!n) {
            dglog::linef("[NODE] JOIN from %s (%s) - not paired, refused\n", id.c_str(), _remote.c_str());
            noteKnock(id, _remote, nowMs());
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
        // Into _hold FIRST, then write from it: the write keeps a pointer to the buffer until it completes. Writing from the
        // local and moving it afterwards only worked because a long string's move keeps its buffer; a frame short enough for
        // the small-string optimisation is copied instead, and the socket would send from a dead stack frame.
        else { _hold = std::move(it.text); _ws.text(true); _ws.async_write(net::buffer(_hold), done); }
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
        std::ifstream b(imgFile(k, ".bin"), std::ios::binary); if (!b) continue;
        std::stringstream body; body << b.rdbuf(); const std::string bin = body.str();
        // The image and its description are written one after the other: refuse a pair that disagrees (a cut between the two
        // writes) rather than serve a node an image whose checksum it will reject.
        if (md5::hex(bin) != std::string(d["md5"] | "")) { dglog::linef("[NODEIMG] the stored %s image does not match its checksum - ignored, upload it again\n", kImgName[k]); continue; }
        g_images[k].fw = d["fw"] | ""; g_images[k].md5 = d["md5"] | ""; g_images[k].size = (uint32_t)bin.size(); g_images[k].present = true;
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
// host: where a plug's push target points (our address, which is what decides "ours"); name: the owner suffix a plug carries
// and what recognises our own plug at a stale address after the brain moves.
static outletops::Self selfIdentity() { return outletops::Self{localIp(), g_hub->primaryId()}; }
// What the sweep must not knock on: our own address, but only when it is knocking on the port we serve (a test runs a fake
// plug beside the brain on 127.0.0.1, on another port). The sweep's subnet comes from the same address.
static std::string sweepSelf() { return localIp() + (g_plugPort == std::to_string(g_port) ? "" : "|other-port"); }

// ── finding boards by mDNS ───────────────────────────────────────────────────────────────────────
// "Scan for boards": a node advertises _dustgate._tcp with TXT (owner, board, servos, linear, role). Unlike a node that already
// belongs to us, a FRESH board never dials anyone, so knocks alone can never show it. The OS's own tools do the browsing
// (dns-sd on macOS, avahi-browse on Linux) because a second mDNS stack in this process would be one more thing to keep working.
// mDNS is a fast path, never a requirement: with no tool, or a network that blocks it, the answer is just the knock list,
// and pairing by typing a host name still works.
static std::string shellOut(const std::string& cmd) {
    std::string out; FILE* f = popen(cmd.c_str(), "r");
    if (!f) return out;
    char b[512]; while (std::fgets(b, sizeof(b), f)) out += b;
    pclose(f); return out;
}
static std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> v; std::stringstream ss(s); std::string x; while (ss >> x) v.push_back(x); return v;
}
static std::string resolveIp(const std::string& host) {
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; addrinfo* res = nullptr;
    if (getaddrinfo((host + ".local").c_str(), nullptr, &hints, &res) != 0 || !res) return "";
    char ip[INET_ADDRSTRLEN] = {0}; inet_ntop(AF_INET, &((sockaddr_in*)res->ai_addr)->sin_addr, ip, sizeof(ip)); freeaddrinfo(res); return ip;
}
struct Found { std::string host, ip, board, owner; int servos = 0, linear = 0; };
static std::vector<Found> mdnsBoards() {
    std::vector<Found> out;
    if (std::system("command -v dns-sd >/dev/null 2>&1") == 0) {
        const std::string b = shellOut("sh -c 'dns-sd -B _dustgate._tcp local & p=$!; sleep 2.5; kill $p' 2>/dev/null");
        std::vector<std::string> names;
        std::stringstream ss(b); std::string line;
        while (std::getline(ss, line)) {
            if (line.find(" Add ") == std::string::npos) continue;
            auto w = splitWs(line); if (w.size() >= 7) { const std::string n = w.back(); if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n); }
        }
        for (const std::string& n : names) {
            // THE NAME CAME OFF THE NETWORK and goes into a shell command, so anything that is not a plain host label is
            // skipped (bug search 2026-10-06: a device advertising `x'; cmd; '` would have run cmd as the brain's user). A
            // DustGate board's name is its hostname, so nothing real is lost. Linux's avahi path interpolates nothing.
            if (n.empty() || n.size() > 63 || !std::all_of(n.begin(), n.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '-' || c == '_'; })) continue;
            // TXT: "owner=x linear=0 servos=2 board=xiao_c5 role=secondary" — an empty owner prints as a bare word.
            const std::string l = shellOut("sh -c 'dns-sd -L " + n + " _dustgate._tcp local & p=$!; sleep 1.5; kill $p' 2>/dev/null");
            Found f; f.host = n;
            for (const std::string& t : splitWs(l)) {
                const size_t eq = t.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = t.substr(0, eq), v = t.substr(eq + 1);
                if (k == "owner") f.owner = v; else if (k == "board") f.board = v;
                else if (k == "servos") f.servos = std::atoi(v.c_str()); else if (k == "linear") f.linear = std::atoi(v.c_str());
            }
            // a bare "owner" word (empty value) means unclaimed: f.owner stays "".
            f.ip = resolveIp(n); out.push_back(f);
        }
    } else if (std::system("command -v avahi-browse >/dev/null 2>&1") == 0) {
        // =;eth0;IPv4;name;_dustgate._tcp;local;host.local;192.168.1.5;80;"owner=x" "board=y" ...
        const std::string b = shellOut("avahi-browse -rpt _dustgate._tcp 2>/dev/null");
        std::stringstream ss(b); std::string line;
        while (std::getline(ss, line)) {
            if (line.rfind("=;", 0) != 0) continue;
            std::vector<std::string> c; std::stringstream ls(line); std::string x; while (std::getline(ls, x, ';')) c.push_back(x);
            if (c.size() < 10 || c[2] != "IPv4") continue;
            Found f; f.host = c[3]; f.ip = c[7];
            for (const std::string& t : splitWs(c[9])) {
                std::string u = t; u.erase(std::remove(u.begin(), u.end(), '"'), u.end());
                const size_t eq = u.find('='); if (eq == std::string::npos) continue;
                const std::string k = u.substr(0, eq), v = u.substr(eq + 1);
                if (k == "owner") f.owner = v; else if (k == "board") f.board = v;
                else if (k == "servos") f.servos = std::atoi(v.c_str()); else if (k == "linear") f.linear = std::atoi(v.c_str());
            }
            out.push_back(f);
        }
    }
    return out;
}
// The rows the Boards screen reads (DiscoveredNode): boards found by mDNS, then any that only knocked.
static std::string discoverBoards(const std::map<std::string, Knock>& knocks) {
    DynamicJsonDocument d(8192); JsonArray a = d.to<JsonArray>();
    std::set<std::string> seen;
    for (const Found& f : mdnsBoards()) {
        if (f.host == g_hub->primaryId() || seen.count(f.host)) continue;
        seen.insert(f.host);
        JsonObject o = a.createNestedObject();
        o["host"] = f.host; o["ip"] = f.ip; o["board"] = f.board.empty() ? "unknown" : f.board; o["servos"] = f.servos;
        if (!f.owner.empty() && f.owner != g_hub->primaryId()) { o["claimedBy"] = f.owner; o["takeable"] = true; }
    }
    const uint32_t now = dgbrain::nowMs();
    for (auto& kv : knocks) {
        if (now - kv.second.atMs > kKnockKeepMs || seen.count(kv.first)) continue;
        JsonObject o = a.createNestedObject(); o["host"] = kv.first; o["ip"] = kv.second.ip; o["board"] = "unknown"; o["servos"] = 0;
    }
    std::string out; serializeJson(d, out); return out;
}

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
        // A board from before node-initiated links (2026-10-04) says nothing of caps.join and waits to be dialled, which this
        // brain never does: it would pair and then sit unlinked with no word as to why (found 2026-10-07, a planer sensor).
        const bool dials = (r["caps"]["join"] | 0) != 0;   // sent as 1 (NodeLink.h buildWelcome), and `| false` reads an int as absent
        if (accepted && !dials) {
            dglog::linef("[CLAIM] %s is ours, but its firmware (%s) is too old to dial a brain - it will never link. Reflash it by USB.",
                         host.c_str(), (const char*)(r["fw"] | "?"));
            std::lock_guard<std::mutex> g(g_tooOldMu); g_tooOld.insert(hostIn);
        } else if (accepted) {
            dglog::linef("[CLAIM] %s is ours (%s, %s) - it will dial us now", host.c_str(), (const char*)(r["board"] | "?"), (const char*)(r["fw"] | "?"));
            std::lock_guard<std::mutex> g(g_tooOldMu); g_tooOld.erase(hostIn);
        }
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
        clearLayout();
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
    bool linearGoto(const std::string& id, float mm, bool home, std::string& why) override { return g_rt.driveLinearTo(id, mm, why, home); }
    bool gateMoving() override { return g_rt.anyGateMoving(); }
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

// ── the app's push socket ────────────────────────────────────────────────────────────────────────────
// The app opens /ws for the device's legacy status (the slider's position and stops, the Wi-Fi name, whether the primary has a
// rack). This brain has no slider and no radio of its own, so it says so; without the socket the app logged a failed
// connection every 3 s for as long as the page was open.
static std::string legacyStatusJson() {
    DynamicJsonDocument d(1024);
    d["state"] = "IDLE"; d["currentStop"] = -1; d["targetStop"] = 0; d["positionSteps"] = 0; d["positionMM"] = 0;
    d["homed"] = false; d["hasLinear"] = false; d["enabled"] = true; d["endstopHome"] = false; d["farEndstop"] = false;
    d["manifoldModel"] = "custom"; d["measuredSpanSteps"] = 0; d["stepsPerMm"] = 1.0; d["ssid"] = ""; d.createNestedArray("stops");
    d["manualOverride"] = false; d["dcConfigured"] = false; d["dcOn"] = false; d.createNestedArray("outlets");
    std::string o; serializeJson(d, o); return o;
}
class StatusWs : public std::enable_shared_from_this<StatusWs> {
public:
    explicit StatusWs(beast::tcp_stream&& s) : _ws(std::move(s)), _timer(_ws.get_executor()) {}
    void run(http::request<http::string_body> req) {
        _ws.async_accept(req, [self = shared_from_this()](beast::error_code ec) {
            if (ec) return;
            beast::get_lowest_layer(self->_ws).expires_never();
            self->read(); self->push();
        });
    }
private:
    void read() {
        _ws.async_read(_buf, [self = shared_from_this()](beast::error_code ec, size_t) {
            if (ec) { self->_closed = true; self->_timer.cancel(); return; }
            self->_buf.consume(self->_buf.size()); self->read();
        });
    }
    void push() {
        if (_closed) return;
        _msg = legacyStatusJson(); _ws.text(true);
        _ws.async_write(net::buffer(_msg), [self = shared_from_this()](beast::error_code ec, size_t) {
            if (ec) { self->_closed = true; return; }
            self->_timer.expires_after(std::chrono::seconds(1));
            self->_timer.async_wait([self](beast::error_code e) { if (!e) self->push(); });
        });
    }
    ws::stream<beast::tcp_stream> _ws; net::steady_timer _timer; beast::flat_buffer _buf; std::string _msg; bool _closed = false;
};

// A Gen2 plug's Outbound WebSocket: it dials us (we pointed it here with Ws.SetConfig) and streams JSON-RPC NotifyStatus /
// NotifyFullStatus frames carrying switch:0.apower. TRUST MODEL as on the ESP32: unauthenticated because a plug cannot present a
// key; a frame is matched to a plug we are polling ONLY by its TCP source address, so it can at most move a plug already paired.
class PlugWs : public std::enable_shared_from_this<PlugWs> {
public:
    PlugWs(beast::tcp_stream&& s, std::string remote) : _ws(std::move(s)), _remote(std::move(remote)) {}
    void run(http::request<http::string_body> req) {
        _ws.async_accept(req, [self = shared_from_this()](beast::error_code ec) {
            if (ec) return;
            beast::get_lowest_layer(self->_ws).expires_never();
            g_poller.pushConnect(self->_remote);
            dglog::linef("[PLUGS] %s connected its push socket\n", self->_remote.c_str());
            self->read();
        });
    }
private:
    void read() {
        _ws.async_read(_buf, [self = shared_from_this()](beast::error_code ec, size_t) {
            if (ec) { g_poller.pushDisconnect(self->_remote); dglog::linef("[PLUGS] %s push socket closed\n", self->_remote.c_str()); return; }
            const std::string m = beast::buffers_to_string(self->_buf.data());
            self->_buf.consume(self->_buf.size());
            StaticJsonDocument<128> filter; filter["params"]["switch:0"]["apower"] = true;
            StaticJsonDocument<256> d;
            if (!deserializeJson(d, m, DeserializationOption::Filter(filter))) {
                JsonVariant ap = d["params"]["switch:0"]["apower"];
                if (!ap.isNull()) g_poller.pushPower(self->_remote, ap.as<float>());
            }
            self->read();
        });
    }
    ws::stream<beast::tcp_stream> _ws; beast::flat_buffer _buf; std::string _remote;
};

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

    // The peer's address, or "" if it reset between the upgrade and this call. The throwing overload would end the brain.
    std::string peerAddress() {
        beast::error_code ec;
        const auto ep = beast::get_lowest_layer(_stream).socket().remote_endpoint(ec);
        return ec ? std::string() : ep.address().to_string();
    }

    void handle() {
        auto& req = _parser.get();
        if (ws::is_upgrade(req) && req.target() == "/nodelink") {
            const std::string remote = peerAddress();
            std::make_shared<NodeWs>(std::move(_stream), remote)->run(_parser.release());
            return;
        }
        if (ws::is_upgrade(req) && req.target() == "/shelly-rpc") {
            const std::string remote = peerAddress();
            std::make_shared<PlugWs>(std::move(_stream), remote)->run(_parser.release());
            return;
        }
        if (ws::is_upgrade(req) && req.target() == "/ws") { std::make_shared<StatusWs>(std::move(_stream))->run(_parser.release()); return; }
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
        // A scan takes seconds (it asks the network): off the network thread, so no node link stalls behind it.
        if (ar.method == "GET" && ar.path == "/api/nodes/discover") { defer([knocks = g_knocks] { Out r; r.body = discoverBoards(knocks); return r; }); return; }
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
            defer([ip, label, take] { Out r; r.body = outletops::rename(ip.c_str(), utf8Prefix(label, 47).c_str(), take, selfIdentity()); return r; });
            return;
        }
        if (t == "/api/outlets/release" && m == "POST") {
            StaticJsonDocument<128> d; const std::string ip = deserializeJson(d, ar.body) ? "" : std::string(d["ip"] | "");
            if (ip.empty()) { err(http::status::bad_request, "missing 'ip'"); return; }
            defer([ip] {
                Out r;
                // Only a plug that reads as OURS is ever written to on the way out: one we merely polled (someone else owns it, or
                // we never got to claim it) has nothing of ours on it, and "disable its push" would silence its real owner.
                StaticJsonDocument<1024> row; deserializeJson(row, outletops::describeJson(ip.c_str(), selfIdentity()));
                const bool ours = std::string(row["claim"] | "") == "ours";
                const dgbrain::PlugPoller::Claim c = g_poller.claimOf(ip);
                r.body = outletops::release(ip.c_str(), selfIdentity(), c.pollOnly || !ours, c.restoreUrl.c_str());
                g_poller.forget(ip);
                return r;
            });
            return;
        }
        // A person was shown what stops working on the other controller (plug-claim.js takeoverWarning) and said yes: the
        // one way a plug somebody else owns is repointed at this brain. The next provisioning pass does the write.
        if (t == "/api/outlets/takeover" && m == "POST") {
            StaticJsonDocument<128> d; const std::string ip = deserializeJson(d, ar.body) ? "" : std::string(d["ip"] | "");
            if (ip.empty()) { err(http::status::bad_request, "missing 'ip'"); return; }
            g_poller.approveTakeover(ip);
            dglog::linef("[PLUGS] takeover approved for %s\n", ip.c_str());
            o.body = "{\"ok\":true}"; return;
        }
        if (t == "/api/outlets/save" && m == "POST") { o.body = "{\"ok\":true}"; return; }   // nothing to persist: plugs live in the layout
        if (t == "/api/outlets/sweep" && m == "POST") { g_sweep.start(sweepSelf(), g_plugPort, selfIdentity()); o.body = "{\"ok\":true}"; return; }
        if (t == "/api/outlets/sweep" && m == "DELETE") { g_sweep.cancel(); o.body = "{\"ok\":true}"; return; }
        if (t == "/api/outlets/sweep" && m == "GET") { o.body = g_sweep.progressJson(); return; }
        // No mDNS browser here, so "discover" is the plugs the last sweep FOUND — asked again, now. The picker finds a tool's
        // plug by its draw ("switch it on and tap Scan again"), and it used to be handed the sweep's own rows, frozen at
        // whatever each plug drew when the sweep passed it: the draw never moved, so nothing could be found that way
        // (found 2026-10-07, pairing a second collector's plug on the Pi). Each address is described in parallel, off the
        // network thread; the sweep is still how a NEW plug is found.
        if (t == "/api/outlets/discover" && m == "GET") {
            // Nothing swept since this brain started (a restart forgets the rows): sweep first, so the picker is never
            // handed an empty list for want of a button nobody knew to press. A few seconds on a /24.
            if (!g_sweep.everRan() && !g_sweep.running()) g_sweep.start(sweepSelf(), g_plugPort, selfIdentity());
            if (g_sweep.running()) {
                defer([] {
                    for (int i = 0; i < 600 && g_sweep.running(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    std::vector<std::string> rows = g_sweep.rows();
                    DynamicJsonDocument d(16384); JsonArray a = d.to<JsonArray>();
                    for (const std::string& r : rows) { DynamicJsonDocument one(1024); if (!deserializeJson(one, r)) a.add(one.as<JsonObject>()); }
                    Out out; serializeJson(d, out.body); return out;   // just swept: these readings are fresh
                });
                return;
            }
            std::vector<std::string> ips;
            for (auto& r : g_sweep.rows()) { StaticJsonDocument<1024> one; if (!deserializeJson(one, r) && one["ip"].is<const char*>()) ips.push_back(one["ip"].as<std::string>()); }
            defer([ips] {
                std::vector<std::string> rows(ips.size());
                std::vector<std::thread> asks;
                const outletops::Self self = selfIdentity();
                for (size_t i = 0; i < ips.size(); i++) asks.emplace_back([&rows, &ips, i, self] { rows[i] = outletops::describeJson(ips[i].c_str(), self); });
                for (auto& th : asks) th.join();
                DynamicJsonDocument d(16384); JsonArray a = d.to<JsonArray>();
                for (const std::string& r : rows) { DynamicJsonDocument one(1024); if (!deserializeJson(one, r)) a.add(one.as<JsonObject>()); }
                Out out; serializeJson(d, out.body); return out;
            });
            return;
        }
        if (t == "/api/outlets" && m == "GET") { o.body = "{\"outlets\":[]}"; return; }
        if (t.rfind("/api/outlets", 0) == 0 && (m == "PUT" || m == "DELETE")) { o.body = "{\"ok\":true}"; return; }
        if (t == "/api/nodes") o.body = nodesJson();
        else if (t == "/api/topology" && m == "GET") { if (g_topoJson.empty()) err(http::status::not_found, "no topology configured"); else o.body = g_topoJson; }
        else if (t == "/api/topology" && m == "PUT") { if (adoptLayout(ar.body)) o.body = "{\"ok\":true}"; else { std::string e; for (char c : g_topoErr) { if (c == '"' || c == '\\') e += '\\'; if ((unsigned char)c >= 32) e += c; } err(http::status::bad_request, e); } }
        else if (t == "/api/topology" && m == "DELETE") { clearLayout(); o.body = "{\"ok\":true}"; }
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
                         {"Cache-Control", "no-store"}};   // no CORS headers: same origin only, as on the ESP32
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
                // The image first, its description second: loadImages() checks they agree, so a cut between the two is a refused image, not a wrong one.
                const bool wrote = dgbrain::writeFileAtomic(imgFile(k, ".bin"), ar.body) &&
                    dgbrain::writeFileAtomic(imgFile(k, ".json"), "{\"fw\":\"" + std::string(fw->value()) + "\",\"md5\":\"" + std::string(md->value()) + "\"}");
                if (!wrote) { err(http::status::internal_server_error, "could not store the image"); return; }
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
        else if (t == "/api/info") o.body = "{\"role\":\"native\",\"id\":\"" + g_hub->primaryId() + "\",\"apiKey\":\"" + g_apiKey + "\",\"build\":\"native " + std::string(DG_COMMIT) + "\",\"uptimeSec\":" + std::to_string(dglog::upMs() / 1000) + "}";
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
    if (!g_ipFile.empty()) { std::ifstream f(g_ipFile); std::string l; if (f && std::getline(f, l) && !l.empty()) return l; return "127.0.0.1"; }
    try {
        net::io_context io; udp::socket s(io); s.connect(udp::endpoint(net::ip::make_address("8.8.8.8"), 53));
        return s.local_endpoint().address().to_string();
    } catch (...) { return "127.0.0.1"; }
}

// The address and the broadcast are read at each send, not captured: the beacon is how a node finds a brain that MOVED.
// Is a board the shop NEEDS down? Only those hurry the beacon: an optional board (a planer's sensor board, powered with the
// planer) is off for days at a time and would otherwise keep it at 5 s forever (docs/optional-nodes-plan.md).
static bool requiredBoardDown() {
    for (auto& kv : g_hub->nodes()) {
        if (kv.second->session.online()) continue;
        if (g_rt.loaded() && topo::isOptionalBoard(g_rt.topology(), topo::controllerIdForHost(g_rt.topology(), kv.first))) continue;
        return true;
    }
    return false;
}

static void beaconLoop(net::io_context& io, std::shared_ptr<udp::socket> sock, std::shared_ptr<net::steady_timer> t) {
    const uint32_t every = requiredBoardDown() ? 5000 : 60000;
    t->expires_after(std::chrono::milliseconds(every));
    t->async_wait([&io, sock, t](beast::error_code ec) {
        if (ec) return;
        std::string msg = "DGB1|" + g_hub->primaryId() + "|" + localIp() + "|" + std::to_string(g_port);
        beast::error_code e2;
        const auto to = net::ip::make_address(g_bcast, e2);   // a bad --broadcast skips the beacon; it must not end the brain
        if (!e2) sock->send_to(net::buffer(msg), udp::endpoint(to, nl::kBeaconPort), 0, e2);
        beaconLoop(io, sock, t);
    });
}

// "ws://<our address>[:port]/shelly-rpc": where a plug is told to push. Port 80 needs no number.
static std::string pushUrl() { return "ws://" + localIp() + (g_port == 80 ? "" : ":" + std::to_string(g_port)) + "/shelly-rpc"; }
// The subnet's own broadcast address: the all-ones one is dropped or mis-routed by some stacks (macOS often).
static std::string broadcastFor(const std::string& ip) { const size_t dot = ip.rfind('.'); return dot == std::string::npos ? "255.255.255.255" : ip.substr(0, dot) + ".255"; }

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::string id = "dustgate", pair, ip, bcast, stateDir;
    unsigned port = 80, alsoPort = 0;   // 80: a node pulls its firmware from http://<brain ip><path>, with no port
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--id") id = val(); else if (a == "--pair") pair = val(); else if (a == "--port") port = (unsigned)std::stoi(val());
        else if (a == "--ip") ip = val(); else if (a == "--ip-from-file") g_ipFile = val(); else if (a == "--state") stateDir = val(); else if (a == "--also-port") alsoPort = (unsigned)std::stoi(val()); else if (a == "--trace") g_trace = true; else if (a == "--www") g_www = val(); else if (a == "--plug-port") { g_plugPort = val(); plughttp::setPort(g_plugPort); } else if (a == "--key") g_apiKey = val(); else if (a == "--broadcast") { bcast = val(); g_bcastFixed = true; }
        else { dglog::linef("usage: dustgate-brain [--id dustgate] [--pair nodeId,nodeId,...] [--port 80] [--also-port 80] [--state dir] [--www dir] [--plug-port 80] [--key k] [--ip a.b.c.d] [--broadcast a.b.c.255]\n"); return a == "--help" ? 0 : 2; }
    }
    std::vector<std::string> ids; std::stringstream ss(pair); std::string x;
    while (std::getline(ss, x, ',')) if (!x.empty()) ids.push_back(x);
    // Everything that must survive a restart (the layout, the pairings, the API key, staged firmware) lives here. With no
    // directory the layout vanished at every restart, so the default is a real one.
    if (stateDir.empty()) {
        const char* env = std::getenv("DUSTGATE_STATE"); const char* home = std::getenv("HOME");
        stateDir = env ? env : (home ? std::string(home) + "/.dustgate" : "");
        if (!stateDir.empty()) { std::string mk = "mkdir -p '" + stateDir + "'"; (void)std::system(mk.c_str()); }
    }
    if (g_www.empty()) dglog::line("[HTTP] no --www: the API is served but there is no app. Point --www at dustgate-ui/dist/dustgate-ui/browser.");
    g_stateDir = stateDir;
    { std::random_device rd; dglog::R().bootId = rd(); }
    if (!stateDir.empty()) { dglog::R().linkLogPath = stateDir + "/linklog.txt"; loadImages(); }
    NodeHub hub(id); g_hub = &hub;
    if (!stateDir.empty()) {
        g_pairPath = stateDir + "/nodes.json";
        // The pairings, from the file or, if that is unreadable (a cut mid-save on an older build), the copy kept beside it.
        for (const std::string& path : {g_pairPath, g_pairPath + ".bak"}) {
            std::ifstream pf(path, std::ios::binary);
            DynamicJsonDocument d(4096);
            if (pf && !deserializeJson(d, pf) && d.is<JsonArray>()) {
                for (JsonObject o : d.as<JsonArray>()) hub.add(o["host"] | "", o["name"] | "");
                if (path != g_pairPath) dglog::linef("[STATE] %s was unreadable - pairings restored from the previous copy\n", g_pairPath.c_str());
                break;
            }
        }
    }
    for (auto& nid : ids) hub.add(nid, "");
    g_rt.begin(&hub.bus());
    g_rt.setSay([](const std::string& l) { dglog::line(l); });   // the [TOOL] line
    if (!stateDir.empty()) {
        g_topoPath = stateDir + "/topology.json";
        // The layout, from the file or, if it will not load (truncated by a power cut under an older build, or damaged), from the
        // last good copy kept beside it. adoptLayout() then rewrites the main file from the copy, so the damage heals.
        for (const std::string& path : {g_topoPath, g_topoPath + ".bak"}) {
            std::ifstream f(path, std::ios::binary);
            if (!f) continue;
            std::stringstream b; b << f.rdbuf();
            if (adoptLayout(b.str(), path == g_topoPath ? 0 : 2)) { if (path != g_topoPath) dglog::linef("[TOPO] %s was unreadable - restored the previous layout from %s\n", g_topoPath.c_str(), path.c_str()); break; }
            dglog::linef("[TOPO] stored layout %s refused: %s\n", path.c_str(), g_topoErr.c_str());
        }
    }
    g_ipFixed = !ip.empty();   // --ip says where this brain IS; without it the address is followed
    if (ip.empty()) {
        // At boot a Pi starts this before its WiFi has an address; "127.0.0.1" would be beaconed and pushed to plugs for the
        // life of the process. Wait (up to a minute) for a real one.
        ip = guessIp();
        for (int i = 0; i < 60 && ip == "127.0.0.1"; i++) { std::this_thread::sleep_for(std::chrono::seconds(1)); ip = guessIp(); }
        if (ip == "127.0.0.1") dglog::line("[NET] no network address after 60 s - nodes cannot find this brain. Check the Pi's WiFi.");
    }
    setLocalIp(ip);
    // The subnet's own broadcast address: the all-ones one is dropped or mis-routed by some stacks (macOS often), and the beacon
    // is what lets a node find a brain whose address changed.
    if (bcast.empty()) bcast = broadcastFor(ip);
    g_bcast = bcast;
#ifdef __APPLE__
    // A sleeping Mac is a brain that has gone away: nodes drop, nothing switches. Hold idle sleep off
    // for as long as this process lives (caffeinate exits when its -w pid does). Best effort.
    { const std::string cmd = "caffeinate -i -w " + std::to_string((long)getpid()) + " >/dev/null 2>&1 &"; if (std::system(cmd.c_str()) != 0) {} }
#endif
    if (g_apiKey.empty()) {   // persisted with the state, so a restart does not log every browser out
        const std::string kp = stateDir.empty() ? "" : stateDir + "/apikey";
        std::ifstream kf(kp); if (!kp.empty() && kf) std::getline(kf, g_apiKey);
        if (g_apiKey.empty()) {
            std::random_device rd; char b[33]; for (int i = 0; i < 32; i++) b[i] = "0123456789abcdef"[rd() % 16]; b[32] = 0; g_apiKey = b;
            if (!kp.empty()) dgbrain::writeFileAtomic(kp, g_apiKey);
        }
    }

    net::io_context io(1);
    try { std::make_shared<Listener>(io, tcp::endpoint(tcp::v4(), (unsigned short)port))->run(); }
    catch (const std::exception& e) {
        if (port == 8080) throw;
        dglog::linef("[HTTP] cannot listen on port %u (%s) - using 8080. Nodes pull firmware from port 80, so updating them will not work.", port, e.what());
        port = 8080; std::make_shared<Listener>(io, tcp::endpoint(tcp::v4(), (unsigned short)port))->run();
    }
    // Nodes pull firmware from http://<brain ip><path> with no port, so a brain on another port also listens on 80.
    if (alsoPort) std::make_shared<Listener>(io, tcp::endpoint(tcp::v4(), (unsigned short)alsoPort))->run();
    auto sock = std::make_shared<udp::socket>(io, udp::v4()); sock->set_option(net::socket_base::broadcast(true));
    g_port = port;   // after the listeners: the port may have fallen back to 8080
    beaconLoop(io, sock, std::make_shared<net::steady_timer>(io));
    net::steady_timer rtTimer(io);
    std::function<void()> rtTick = [&]() {
        rtTimer.expires_after(std::chrono::milliseconds(100));
        rtTimer.async_wait([&](beast::error_code ec) { if (ec) return; g_rt.update(nowMs()); feedPlugs(nowMs()); if (g_rt.loaded()) raiseDeviceProblems(nowMs()); driveCollectors(nowMs()); rtTick(); });
    };
    rtTick();
    net::signal_set sig(io, SIGINT, SIGTERM); sig.async_wait([&](beast::error_code, int) { io.stop(); });
    dglog::linef("dustgate-brain %s (%s) on %s:%u, %zu paired node(s)\n", id.c_str(), DG_COMMIT, ip.c_str(), port, ids.size());
    // Plugs are pointed at the port the app is served on (port 80 needs no number in the URL).
    g_poller.setPushTarget(selfIdentity(), pushUrl(), stateDir.empty() ? "" : stateDir + "/plugs.json");
    g_poller.start();
    // FOLLOW THIS BRAIN'S OWN ADDRESS. A Pi is given an address by the router and the router may give it another (a lease
    // renewal, a reboot, a new access point). The brain is told nothing, so it looks: when the address moves it beacons the
    // new one (a node redials by the beacon), points every plug it owns at the new address (a plug keeps pushing to the old
    // one until told), and says so in the log. No fixed address, and no DHCP reservation, is needed. The ESP32 does the same
    // (SmartOutletControl::checkLocalIpChange). Skipped when --ip was given, which is a statement of where the brain is.
    net::steady_timer ipTimer(io);
    std::function<void()> ipTick = [&]() {
        ipTimer.expires_after(std::chrono::seconds(5));
        ipTimer.async_wait([&](beast::error_code ec) {
            if (ec) return;
            if (!g_ipFixed) {
                const std::string now = guessIp();
                // 127.0.0.1 is "no network right now" (a WiFi drop), not an address: keep the last good one until a real one is back.
                if (now != "127.0.0.1" && now != localIp()) {
                    dglog::linef("[NET] address changed %s -> %s; nodes will find it by the beacon and plugs are being repointed\n", localIp().c_str(), now.c_str());
                    setLocalIp(now);
                    if (!g_bcastFixed) g_bcast = broadcastFor(now);
                    g_poller.retarget(selfIdentity(), pushUrl());
                }
            }
            ipTick();
        });
    };
    ipTick();
    // ONE EXCEPTION IN ONE HANDLER USED TO END THE BRAIN (bug search 2026-10-06). systemd brought it back in ~3 s and the nodes
    // relinked, which read as a flap nobody could explain. A handler that throws now costs that one request, and says so.
    for (;;) {
        try { io.run(); break; }
        catch (const std::exception& e) { dglog::linef("[ERROR] a network handler threw: %s - carrying on\n", e.what()); }
    }
    g_poller.stop();
    return 0;
}
