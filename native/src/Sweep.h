// native/src/Sweep.h — the subnet sweep: knock on port 80 of every address in the brain's /24 and describe whatever
// answers as a smart plug. The ESP32 does this for Tasmotas (they advertise nothing); a brain with no mDNS browser
// needs it for Shelly too, so each hit is probed as either (outlets/OutletOps.h tries both). Runs on its own threads.
#pragma once
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "OutletOps.h"

namespace dgbrain {

class Sweep {
public:
    ~Sweep() { cancel(); join(); }
    bool running() const { return _running; }
    void start(const std::string& localIp, const std::string& port, outletops::Self self) {
        if (_running) return;
        join();
        { std::lock_guard<std::mutex> g(_m); _rows.clear(); }
        _scanned = 0; _cancel = false; _running = true; _everRan = true;
        const size_t dot = localIp.rfind('.');
        const std::string prefix = dot == std::string::npos ? "192.168.1." : localIp.substr(0, dot + 1);
        _th = std::thread([this, prefix, port, self] { run(prefix, port, self); });
    }
    void cancel() { _cancel = true; }
    std::string progressJson() {
        DynamicJsonDocument d(16384);
        d["running"] = _running.load(); d["scanned"] = _scanned.load(); d["total"] = 254; d["everRan"] = _everRan.load();
        d["cancelled"] = _cancelled.load();
        d["finishedAgoMs"] = (_everRan && !_running) ? (uint32_t)(nowMs() - _finishedAt) : 0;
        JsonArray f = d.createNestedArray("found");
        std::lock_guard<std::mutex> g(_m);
        for (auto& r : _rows) { DynamicJsonDocument one(1024); if (!deserializeJson(one, r)) f.add(one.as<JsonObject>()); }
        std::string out; serializeJson(d, out); return out;
    }
    std::vector<std::string> rows() { std::lock_guard<std::mutex> g(_m); return _rows; }

private:
    static uint32_t nowMs() {
        return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void join() { if (_th.joinable()) _th.join(); }
    static bool knock(const std::string& ip, const std::string& port) {
        // An async connect with a deadline: a blocking one waits out the OS timeout on every empty address.
        namespace net = boost::asio; namespace beast = boost::beast;
        try {
            net::io_context io; beast::tcp_stream s(io);
            beast::error_code ec = boost::asio::error::would_block;
            s.expires_after(std::chrono::milliseconds(250));
            s.async_connect(net::ip::tcp::endpoint(net::ip::make_address(ip), (unsigned short)std::stoi(port)), [&](beast::error_code e) { ec = e; });
            io.run();
            return !ec;
        } catch (...) { return false; }
    }
    void run(const std::string& prefix, const std::string& port, outletops::Self self) {
        std::atomic<int> next{1};
        auto worker = [&] {
            for (;;) {
                const int n = next++;
                if (n > 254 || _cancel) return;
                const std::string ip = prefix + std::to_string(n);
                if (knock(ip, port)) {
                    // Phase 2, now that someone is home: the real probe, with its own generous timeouts.
                    DynamicJsonDocument d(1024);
                    outletops::describe(d.to<JsonObject>(), ip.c_str(), nullptr, false, OUTLET_SHELLY, 0, self);
                    if (d["reachable"] | false) { std::string row; serializeJson(d, row); std::lock_guard<std::mutex> g(_m); _rows.push_back(row); plughttp::log("[SWEEP] found " + ip); }
                }
                _scanned++;
            }
        };
        std::vector<std::thread> pool;
        for (int i = 0; i < 32; i++) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
        _cancelled = _cancel.load(); _finishedAt = nowMs(); _running = false;
    }
    std::mutex _m; std::vector<std::string> _rows;
    std::atomic<bool> _running{false}, _cancel{false}, _everRan{false}, _cancelled{false};
    std::atomic<int> _scanned{0}; uint32_t _finishedAt = 0;
    std::thread _th;
};

}  // namespace dgbrain
