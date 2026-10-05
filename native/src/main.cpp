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
#include "NodeHub.h"
#include "CollectorDriver.h"
#include "NodeStatus.h"
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
static std::string g_topoJson, g_topoPath, g_topoErr;

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
        if (board.empty() || topo::isOwnBoard(board, "")) { std::printf("[RF] collector %s: the layout names no board for its transmitter\n", sys.c_str()); continue; }
        CollectorSlot c; c.sys = sys;
        c.presser.reset(new topo::RemoteRfPresser(&g_hub->bus(), board,
            (uint8_t)(rf["address"] | (int)topo::rf::kRocklerAddress), (uint8_t)(rf["data"] | (int)topo::rf::kRocklerData),
            (uint32_t)(rf["tickUs"] | (int)topo::rf::kDefaultTickUs), (uint32_t)(rf["repeats"] | (int)topo::rf::kDefaultRepeats)));
        std::printf("[RF] collector %s pressed by RF through board %s\n", sys.c_str(), board.c_str());
        g_collectors.push_back(std::move(c));
    }
}

struct StdoutHooks : topo::DriverHooks { void say(const std::string& l) override { std::printf("%s\n", l.c_str()); } };
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
    if (!g_topoPath.empty()) { std::ofstream f(g_topoPath, std::ios::binary | std::ios::trunc); f << json; }
    std::printf("[TOPO] layout adopted (%zu bytes)\n", json.size());
    return true;
}
static unsigned g_nextLinkId = 1;

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
        if (_node) { _node->session.onFrame(m.data(), m.size()); return; }
        StaticJsonDocument<192> d;
        if (deserializeJson(d, m) || std::strcmp(d["t"] | "", "JOIN") != 0) return;
        const std::string id = d["nodeId"] | "";
        if ((d["v"] | 0) != nl::kVersion) { enqueue({false, refuseFrame("busy"), true}); return; }
        std::shared_ptr<Node> n = g_hub->find(id);
        if (g_hub->paused) { enqueue({false, refuseFrame("busy"), true}); return; }
        if (!n) {
            std::printf("[NODE] JOIN from %s (%s) - not paired, refused\n", id.c_str(), _remote.c_str());
            enqueue({false, refuseFrame("not-paired"), true}); return;
        }
        if (!n->session.onAttach()) {
            std::printf("[NODE] JOIN from %s - already linked, refused as a duplicate\n", id.c_str());
            enqueue({false, refuseFrame("duplicate"), true}); return;
        }
        _node = n; _linkId = g_nextLinkId++; n->linkId = _linkId;
        _attachedMs = _lastOnlineMs = nowMs(); _lastPingMs = nowMs();
        std::printf("[NODE] %s dialled in from %s\n", id.c_str(), _remote.c_str());
        enqueue({false, n->session.helloFrame(), false});
    }

    void tick() {
        _timer.expires_after(std::chrono::milliseconds(200));
        _timer.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (ec || self->_closing) return;
            if (self->_node && (self->_node->removed || g_hub->paused)) { self->enqueue({false, "", true}); self->_node->session.onDown(); }
            else if (self->_node) {
                for (int i = 0; i < 4; i++) { std::string f; if (!self->_node->session.nextFrame(f)) break; self->enqueue({false, f, false}); }
                const uint32_t now = nowMs();
                if (now - self->_lastPingMs >= nl::kPingIntervalMs) { self->_lastPingMs = now; self->enqueue({true, "", false}); }
                if (self->_node->session.online()) self->_lastOnlineMs = now;
                else if (now - self->_lastOnlineMs > 2 * nl::kPongTimeoutMs) {
                    std::printf("[NODE] %s silent - closing its socket\n", self->_node->id.c_str());
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
            std::printf("[NODE] %s link closed\n", _node->id.c_str());
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
static std::string nodesJson() {
    DynamicJsonDocument d(8192);
    JsonArray a = d.createNestedArray("nodes");
    for (auto& kv : g_hub->nodes()) topo::writeNodeEntry(a, kv.second->session, kv.first.c_str(), kv.first.c_str(), kv.second->name.c_str(), topo::NodeImageView());
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

class HttpConn : public std::enable_shared_from_this<HttpConn> {
public:
    explicit HttpConn(tcp::socket&& s) : _stream(std::move(s)) {}
    void run() {
        _stream.expires_after(std::chrono::seconds(30));
        http::async_read(_stream, _buf, _req, [self = shared_from_this()](beast::error_code ec, size_t) {
            if (ec) return;
            self->handle();
        });
    }
private:
    void handle() {
        if (ws::is_upgrade(_req) && _req.target() == "/nodelink") {
            std::string remote = beast::get_lowest_layer(_stream).socket().remote_endpoint().address().to_string();
            std::make_shared<NodeWs>(std::move(_stream), remote)->run(std::move(_req));
            return;
        }
        http::status st = http::status::ok; std::string body;
        const std::string t = std::string(_req.target());
        const bool isApi = t.rfind("/api/", 0) == 0;
        if (isApi && t != "/api/info") {
            auto k = _req.find("X-Api-Key");
            if (k == _req.end() || std::string(k->value()) != g_apiKey) { respond(http::status::unauthorized, "{\"error\":\"unauthorized\"}", "application/json"); return; }
        }
        if (!isApi) {
            std::string fb, mime;
            if (_req.method() == http::verb::get && readStatic(t, fb, mime)) respond(http::status::ok, fb, mime);
            else respond(http::status::not_found, "not found", "text/plain");
            return;
        }
        if (t == "/api/nodes") body = nodesJson();
        else if (t == "/api/topology" && _req.method() == http::verb::get) {
            if (g_topoJson.empty()) { st = http::status::not_found; body = "{\"error\":\"no topology configured\"}"; } else body = g_topoJson;
        }
        else if (t == "/api/topology" && _req.method() == http::verb::put) {
            if (adoptLayout(_req.body())) body = "{\"ok\":true}";
            else { st = http::status::bad_request; body = "{\"error\":\"" + g_topoErr + "\"}"; }
        }
        else if (t == "/api/topology" && _req.method() == http::verb::delete_) {
            g_rt.clear(); g_topoJson.clear(); g_collectors.clear(); if (!g_topoPath.empty()) std::remove(g_topoPath.c_str()); body = "{\"ok\":true}";
        }
        else if (t == "/api/nodes/pair" && _req.method() == http::verb::post) {
            StaticJsonDocument<256> d;
            const char* host = nullptr;
            if (!deserializeJson(d, _req.body())) host = d["host"] | (const char*)nullptr;
            if (!host || !*host) { st = http::status::bad_request; body = "{\"error\":\"missing 'host'\"}"; }
            else {
                const std::string h = host;
                if (d["remove"] | false) g_hub->remove(h);
                else { auto n = g_hub->add(h, d["name"] | ""); if (d["takeover"] | false) n->session.requestTakeover(); }
                savePairs(); body = "{\"ok\":true}";
            }
        }
        else if (t == "/api/nodes/pause" && _req.method() == http::verb::post) {
            StaticJsonDocument<64> d;
            if (deserializeJson(d, _req.body()) || !d["paused"].is<bool>()) { st = http::status::bad_request; body = "{\"error\":\"need {\\\"paused\\\": true|false}\"}"; }
            else { g_hub->paused = d["paused"].as<bool>(); body = "{\"ok\":true}"; }
        }
        else if (t == "/api/status") {
            if (g_topoJson.empty()) { st = http::status::not_found; body = "{\"error\":\"no topology configured\"}"; }
            else { DynamicJsonDocument d(32768); g_rt.writeStatus(d.to<JsonObject>()); serializeJson(d, body); }
        }
        else if (t == "/api/problems") { DynamicJsonDocument d(4096); g_rt.writeProblems(d.to<JsonObject>()); serializeJson(d, body); }
        // DEV ONLY: stand in for a plug reporting watts, so routing can be driven with no tool running.
        else if (t == "/api/dev/power" && _req.method() == http::verb::post) {
            StaticJsonDocument<192> d;
            if (deserializeJson(d, _req.body()) || !d["machineId"].is<const char*>()) { st = http::status::bad_request; body = "{\"error\":\"machineId, watts\"}"; }
            else { g_rt.setMachinePower(d["machineId"].as<std::string>(), d["watts"] | 0.0f); body = "{\"ok\":true}"; }
        }
        else if (t == "/api/info") body = "{\"role\":\"native\",\"id\":\"" + g_hub->primaryId() + "\",\"apiKey\":\"" + g_apiKey + "\",\"build\":\"native\"}";
        else { st = http::status::not_found; body = "{\"error\":\"not found\"}"; }
        respond(st, body, "application/json");
    }
    void respond(http::status st, const std::string& body, const std::string& mime) {
        auto res = std::make_shared<http::response<http::string_body>>(st, _req.version());
        res->set(http::field::content_type, mime);
        res->body() = body; res->prepare_payload(); res->keep_alive(false);
        http::async_write(_stream, *res, [self = shared_from_this(), res](beast::error_code ec, size_t) {
            self->_stream.socket().shutdown(tcp::socket::shutdown_send, ec);
        });
    }
    beast::tcp_stream _stream;
    beast::flat_buffer _buf;
    http::request<http::string_body> _req;
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
    unsigned port = 8080;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--id") id = val(); else if (a == "--pair") pair = val(); else if (a == "--port") port = (unsigned)std::stoi(val());
        else if (a == "--ip") ip = val(); else if (a == "--state") stateDir = val(); else if (a == "--www") g_www = val(); else if (a == "--key") g_apiKey = val(); else if (a == "--broadcast") bcast = val();
        else { std::printf("usage: dustgate-brain [--id dustgate] [--pair nodeId,nodeId,...] [--port 8080] [--state dir] [--www dir] [--key k] [--ip a.b.c.d] [--broadcast a.b.c.255]\n"); return a == "--help" ? 0 : 2; }
    }
    std::vector<std::string> ids; std::stringstream ss(pair); std::string x;
    while (std::getline(ss, x, ',')) if (!x.empty()) ids.push_back(x);
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
        if (f) { std::stringstream b; b << f.rdbuf(); if (!adoptLayout(b.str())) std::printf("[TOPO] stored layout refused: %s\n", g_topoErr.c_str()); }
    }
    if (ip.empty()) ip = guessIp();
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
    auto sock = std::make_shared<udp::socket>(io, udp::v4()); sock->set_option(net::socket_base::broadcast(true));
    beaconLoop(io, sock, std::make_shared<net::steady_timer>(io), ip, port, bcast);
    net::steady_timer rtTimer(io);
    std::function<void()> rtTick = [&]() {
        rtTimer.expires_after(std::chrono::milliseconds(100));
        rtTimer.async_wait([&](beast::error_code ec) { if (ec) return; g_rt.update(nowMs()); driveCollectors(nowMs()); rtTick(); });
    };
    rtTick();
    net::signal_set sig(io, SIGINT, SIGTERM); sig.async_wait([&](beast::error_code, int) { io.stop(); });
    std::printf("dustgate-brain %s on %s:%u, %zu paired node(s)\n", id.c_str(), ip.c_str(), port, ids.size());
    io.run();
    return 0;
}
