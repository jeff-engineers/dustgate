// native/src/PlugPoller.h — polls the plugs the brain itself is responsible for, on one worker thread.
//
// The outlet drivers (outlets/ShellyGen2Outlet, TasmotaOutlet) are the SAME code the ESP32 runs; they block for
// up to their timeout on a dead plug, so they live on a thread of their own and the network thread only ever
// reads the latest answer. That is the ESP's arrangement too (its outlet poll task).
#pragma once
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "ShellyGen2Outlet.h"
#include "TasmotaOutlet.h"

namespace dgbrain {

class PlugPoller {
public:
    struct Target { std::string key, ip; bool tasmota = false; };
    struct Reading { bool have = false; bool reachable = false; float watts = 0.0f; };

    ~PlugPoller() { stop(); }
    void start() { _run = true; _th = std::thread([this] { loop(); }); }
    void stop() { _run = false; if (_th.joinable()) _th.join(); }

    // The plugs we should be polling now. Anything not named is dropped; anything new is created.
    void sync(const std::vector<Target>& want) {
        std::lock_guard<std::mutex> g(_m);
        for (auto it = _slots.begin(); it != _slots.end();) {
            bool keep = false;
            for (const Target& t : want) if (t.key == it->first && t.ip == it->second->ip && t.tasmota == it->second->tasmota) keep = true;
            if (keep) ++it; else it = _slots.erase(it);
        }
        for (const Target& t : want) {
            if (_slots.count(t.key)) continue;
            auto s = std::make_shared<Slot>();
            s->ip = t.ip; s->tasmota = t.tasmota;
            if (t.tasmota) s->outlet.reset(new TasmotaOutlet(t.ip.c_str(), t.key.c_str()));
            else s->outlet.reset(new ShellyGen2Outlet(t.ip.c_str(), t.key.c_str()));
            _slots[t.key] = s;
        }
    }
    Reading read(const std::string& key) {
        std::lock_guard<std::mutex> g(_m);
        auto it = _slots.find(key);
        return it == _slots.end() ? Reading() : it->second->last;
    }
    // Switch a plug (a collector's own control plug). Queued; the worker does the blocking call.
    void setSwitch(const std::string& key, bool on) {
        std::lock_guard<std::mutex> g(_m);
        auto it = _slots.find(key);
        if (it != _slots.end()) { it->second->switchWanted = on ? 1 : 0; }
    }

private:
    struct Slot {
        std::string ip; bool tasmota = false;
        std::unique_ptr<SmartOutlet> outlet;
        Reading last;
        int switchWanted = -1;     // -1 nothing queued
    };
    void loop() {
        while (_run) {
            std::vector<std::pair<std::string, std::shared_ptr<Slot>>> work;
            { std::lock_guard<std::mutex> g(_m); for (auto& kv : _slots) work.push_back(kv); }
            const auto t0 = std::chrono::steady_clock::now();
            for (auto& kv : work) {
                if (!_run) return;
                Slot& s = *kv.second;
                int sw; { std::lock_guard<std::mutex> g(_m); sw = s.switchWanted; s.switchWanted = -1; }
                if (sw >= 0) s.outlet->setSwitch(sw == 1);
                const bool ok = s.outlet->poll();
                std::lock_guard<std::mutex> g(_m);
                s.last.have = true; s.last.reachable = ok && s.outlet->isReachable(); s.last.watts = s.last.reachable ? s.outlet->getPowerW() : 0.0f;
            }
            // 500 ms between passes (OUTLET_POLL_INTERVAL_MS), counted from the start of one.
            const auto spent = std::chrono::steady_clock::now() - t0;
            if (spent < std::chrono::milliseconds(500)) std::this_thread::sleep_for(std::chrono::milliseconds(500) - spent);
        }
    }
    std::mutex _m;
    std::map<std::string, std::shared_ptr<Slot>> _slots;
    std::atomic<bool> _run{false};
    std::thread _th;
};

}  // namespace dgbrain
