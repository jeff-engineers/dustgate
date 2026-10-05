// native/src/fakeplug.cpp — a Shelly Gen2 plug on loopback: `fakeplug <port> <powerfile> [ws-server] [name]`.
//
// HTTP: /rpc/Switch.GetStatus answers the number in <powerfile> as apower, so a test runs a tool by writing a file.
// Ownership: Ws.GetConfig / Ws.SetConfig and Switch.GetConfig / SetConfig keep their state in <powerfile>.ws and
// <powerfile>.name, the way a plug keeps it in flash. Start it with a [ws-server] that is not ours to play a plug somebody
// else already owns.
// Push: whenever the stored Ws server is enabled it dials it and streams NotifyFullStatus / NotifyStatus (switch:0.apower)
// whenever the number in the file changes — what a real plug's Outbound WebSocket does. <powerfile>.polls counts GetStatus
// requests, so a test can see that a pushing plug is not being polled.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
namespace beast = boost::beast; namespace http = beast::http; namespace net = boost::asio; namespace websocket = beast::websocket; using tcp = net::ip::tcp;

static std::string g_file;
static std::mutex g_m;

static std::string readLine(const std::string& path) { std::string s; std::ifstream f(path); std::getline(f, s); return s; }
static double readPower() { double w = 0; std::ifstream f(g_file); f >> w; return w; }
// The stored push config: "<enable 0/1> <url>".
static void ws(bool& enable, std::string& url) {
    std::lock_guard<std::mutex> g(g_m);
    const std::string l = readLine(g_file + ".ws"); enable = !l.empty() && l[0] == '1'; url = l.size() > 2 ? l.substr(2) : "";
}

// "ws://host:port/path" -> pieces.
static bool split(const std::string& url, std::string& host, std::string& port, std::string& path) {
    if (url.rfind("ws://", 0) != 0) return false;
    std::string r = url.substr(5); const size_t sl = r.find('/'); path = sl == std::string::npos ? "/" : r.substr(sl); r = r.substr(0, sl);
    const size_t c = r.find(':'); host = r.substr(0, c); port = c == std::string::npos ? "80" : r.substr(c + 1);
    return true;
}

static void pusher() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        bool en; std::string url; ws(en, url);
        std::string host, port, path;
        if (!en || !split(url, host, port, path)) continue;
        try {
            net::io_context io; tcp::resolver rs(io); websocket::stream<tcp::socket> w(io);
            net::connect(w.next_layer(), rs.resolve(host, port));
            w.handshake(host + ":" + port, path);
            double last = -1; bool first = true;
            for (;;) {
                bool en2; std::string url2; ws(en2, url2);
                if (!en2 || url2 != url) break;                    // repointed or disabled: let go
                const double p = readPower();
                if (first || p != last) {
                    w.text(true);
                    w.write(net::buffer(std::string("{\"src\":\"fake\",\"method\":\"") + (first ? "NotifyFullStatus" : "NotifyStatus") +
                                        "\",\"params\":{\"ts\":1,\"switch:0\":{\"id\":0,\"apower\":" + std::to_string(p) + "}}}"));
                    last = p; first = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            beast::error_code ec; w.close(websocket::close_code::normal, ec);
        } catch (...) {}
    }
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: fakeplug <port> <powerfile> [ws-server] [name]\n"); return 2; }
    g_file = argv[2];
    if (argc > 3 && *argv[3]) std::ofstream(g_file + ".ws") << "1 " << argv[3];
    if (argc > 4) std::ofstream(g_file + ".name") << argv[4];
    std::thread(pusher).detach();
    net::io_context io; tcp::acceptor acc(io, tcp::endpoint(tcp::v4(), (unsigned short)std::atoi(argv[1])));
    for (;;) {
        tcp::socket s(io); acc.accept(s);
        try {
            beast::flat_buffer b; http::request<http::string_body> req; http::read(s, b, req);
            std::string body; const std::string t = std::string(req.target());
            if (t.rfind("/rpc/Switch.GetStatus", 0) == 0) {
                { std::lock_guard<std::mutex> g(g_m); std::ofstream(g_file + ".polls", std::ios::app) << "x"; }
                body = "{\"id\":0,\"source\":\"test\",\"output\":true,\"apower\":" + std::to_string(readPower()) + ",\"voltage\":120.0}";
            } else if (t.rfind("/rpc/Switch.Set", 0) == 0 && t.rfind("/rpc/Switch.SetConfig", 0) != 0) {
                std::ofstream(g_file + ".sw") << t; body = "{\"was_on\":true}";
            } else if (t.rfind("/rpc/Switch.GetConfig", 0) == 0) {
                const std::string n = readLine(g_file + ".name");
                body = "{\"id\":0,\"name\":" + (n.empty() ? std::string("null") : "\"" + n + "\"") + "}";
            } else if (t.rfind("/rpc/Ws.GetConfig", 0) == 0) {
                bool en; std::string url; ws(en, url);
                body = std::string("{\"server\":\"") + url + "\",\"enable\":" + (en ? "true" : "false") + ",\"ssl_ca\":\"*\"}";
            } else if (req.method() == http::verb::post && t == "/rpc") {
                const std::string b2 = req.body();
                if (b2.find("Ws.SetConfig") != std::string::npos) {
                    const size_t sv = b2.find("\"server\":\""); std::string url;
                    if (sv != std::string::npos) { const size_t e = b2.find('"', sv + 10); url = b2.substr(sv + 10, e - sv - 10); }
                    const bool en = b2.find("\"enable\":true") != std::string::npos;
                    std::lock_guard<std::mutex> g(g_m); std::ofstream(g_file + ".ws") << (en ? "1 " : "0 ") << url;
                } else {
                    // Switch.SetConfig {"name":"..."}: remember it, the way a plug would.
                    const size_t at = b2.find("\"name\":\"");
                    if (at != std::string::npos) { const size_t e = b2.find('"', at + 8); std::ofstream(g_file + ".name") << b2.substr(at + 8, e - at - 8); }
                }
                body = "{\"result\":{\"restart_required\":false}}";
            } else body = "{}";
            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/json"); res.body() = body; res.prepare_payload(); res.keep_alive(false);
            http::write(s, res);
        } catch (...) {}
    }
}
