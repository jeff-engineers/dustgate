// native/src/PlugPoller.h — polls the plugs the brain itself is responsible for, on one worker thread.
//
// The outlet drivers (outlets/ShellyGen2Outlet, TasmotaOutlet) are the SAME code the ESP32 runs; they block for
// up to their timeout on a dead plug, so they live on a thread of their own and the network thread only ever
// reads the latest answer. That is the ESP's arrangement too (its outlet poll task).
#pragma once
// Spelled out, not left to whatever Boost happens to pull in: GCC (the Pi) is stricter than the Mac's clang.
#include <cstdint>
#include <utility>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <set>
#include <ArduinoJson.h>
#include "ShellyGen2Outlet.h"
#include "TasmotaOutlet.h"
#include "Provision.h"

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
    // Who "we" are to a plug, and the address a plug should push to. Until this is set the poller only polls: a brain
    // with nowhere for a plug to push has nothing to claim a plug for. `statePath` keeps what a takeover displaced, so
    // unpairing after a restart can still hand the plug back.
    void setPushTarget(const outletops::Self& self, const std::string& wsUrl, const std::string& statePath = "") {
        std::lock_guard<std::mutex> g(_m);
        _self = self; _wsUrl = wsUrl; _statePath = statePath;
        if (statePath.empty()) return;
        std::ifstream f(statePath, std::ios::binary);
        DynamicJsonDocument d(2048);
        if (f && !deserializeJson(d, f)) for (JsonPair kv : d.as<JsonObject>()) _prevUrl[kv.key().c_str()] = kv.value().as<std::string>();
    }
    // A plug's outbound WebSocket arrived, spoke, or went away (the /shelly-rpc endpoint calls these from the network thread).
    void pushConnect(const std::string& ip) {
        std::lock_guard<std::mutex> g(_m);
        for (auto& kv : _slots) if (kv.second->ip == ip) { kv.second->pushed = true; kv.second->heardMs = nowMs(); }
    }
    void pushPower(const std::string& ip, float watts) {
        std::lock_guard<std::mutex> g(_m);
        for (auto& kv : _slots) if (kv.second->ip == ip) {
            Slot& s = *kv.second;
            s.pushed = true; s.heardMs = nowMs();
            s.last.have = true; s.last.reachable = true; s.last.watts = watts;
        }
    }
    void pushDisconnect(const std::string& ip) {
        std::lock_guard<std::mutex> g(_m);
        for (auto& kv : _slots) if (kv.second->ip == ip) {
            Slot& s = *kv.second;
            s.pushed = false; s.last.reachable = false; s.last.watts = 0.0f;   // unknown until the next poll
        }
    }
    // A person was shown what stops working on the other controller and said yes. The ONLY way a plug someone else owns
    // gets written. Remembered for an address with no slot yet (the plug is picked after the approval) and consumed
    // when the write lands.
    void approveTakeover(const std::string& ip) {
        std::lock_guard<std::mutex> g(_m);
        _approved.insert(ip);
        for (auto& kv : _slots) if (kv.second->ip == ip) { kv.second->provisioned = false; kv.second->pollOnly = false; kv.second->provisionAtMs = 0; }
    }
    // What unpairing needs to know about a plug: was it only ever polled, and whose push target did we displace.
    struct Claim { bool pollOnly = false; std::string restoreUrl; };
    Claim claimOf(const std::string& ip) {
        std::lock_guard<std::mutex> g(_m);
        Claim c;
        for (auto& kv : _slots) if (kv.second->ip == ip && kv.second->pollOnly) c.pollOnly = true;
        auto it = _prevUrl.find(ip); if (it != _prevUrl.end()) c.restoreUrl = it->second;
        return c;
    }
    void forget(const std::string& ip) {
        std::lock_guard<std::mutex> g(_m);
        if (_prevUrl.erase(ip)) saveLocked();
        _approved.erase(ip);
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
        bool pushed = false;             // its outbound WebSocket is up: it tells us, so we do not ask
        bool provisioned = false, pollOnly = false;
        uint32_t heardMs = 0;            // last frame or good poll — a "connected" socket gone quiet is polled anyway
        uint32_t provisionAtMs = 0;      // next time to try claiming it
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
                provision(s);
                bool quiet;
                { std::lock_guard<std::mutex> g(_m); quiet = !s.pushed || nowMs() - s.heardMs > kPushQuietMs; }
                // A plug that pushes is not polled: that is the whole saving. But a socket that is "up" and silent for
                // half a minute is polled once anyway — Shelly only speaks on a CHANGE, so a quiet socket is normal and a
                // hung plug looks exactly the same.
                if (!quiet) continue;
                const bool ok = s.outlet->poll();
                std::lock_guard<std::mutex> g(_m);
                s.last.have = true; s.last.reachable = ok && s.outlet->isReachable(); s.last.watts = s.last.reachable ? s.outlet->getPowerW() : 0.0f;
                if (ok) s.heardMs = nowMs();
            }
            // 500 ms between passes (OUTLET_POLL_INTERVAL_MS), counted from the start of one.
            const auto spent = std::chrono::steady_clock::now() - t0;
            if (spent < std::chrono::milliseconds(500)) std::this_thread::sleep_for(std::chrono::milliseconds(500) - spent);
        }
    }
    static constexpr uint32_t kPushQuietMs = 30000, kProvisionRetryMs = 15000;
    static uint32_t nowMs() { return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    // Claim a plug for this brain, if we have somewhere for it to push. Off the network thread: it is blocking HTTP.
    void provision(Slot& s) {
        outletops::Self self; std::string url; bool takeover;
        {
            std::lock_guard<std::mutex> g(_m);
            if (_wsUrl.empty() || s.provisioned || s.pollOnly || nowMs() < s.provisionAtMs) return;
            self = _self; url = _wsUrl; takeover = _approved.count(s.ip) > 0;
        }
        const outletops::ProvisionResult r = outletops::provisionPlug(*s.outlet, self, url, takeover);
        std::lock_guard<std::mutex> g(_m);
        if (r.state == outletops::Provision::Pending) { s.provisionAtMs = nowMs() + kProvisionRetryMs; plughttp::log("[PLUGS] " + s.ip + " not claimed yet: " + r.why); return; }
        if (r.state == outletops::Provision::PollOnly) { s.pollOnly = true; plughttp::log("[PLUGS] " + s.ip + " is " + r.why + " - polling it, NOT repointing its push target"); return; }
        s.provisioned = true; s.provisionAtMs = 0;
        if (takeover) _approved.erase(s.ip);                 // the approval has nothing left to authorize
        if (!r.previousPushUrl.empty()) { _prevUrl[s.ip] = r.previousPushUrl; saveLocked(); }
        plughttp::log("[PLUGS] " + s.ip + " now pushes to " + url);
    }
    void saveLocked() {
        if (_statePath.empty()) return;
        DynamicJsonDocument d(2048); JsonObject o = d.to<JsonObject>();
        for (auto& kv : _prevUrl) o[kv.first] = kv.second;
        std::ofstream f(_statePath, std::ios::binary | std::ios::trunc); serializeJson(d, f);
    }
    outletops::Self _self; std::string _wsUrl, _statePath;
    std::map<std::string, std::string> _prevUrl;   // plug address -> the push target a takeover displaced
    std::set<std::string> _approved;
    std::mutex _m;
    std::map<std::string, std::shared_ptr<Slot>> _slots;
    std::atomic<bool> _run{false};
    std::thread _th;
};

}  // namespace dgbrain
