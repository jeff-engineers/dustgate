// native/src/Relocator.h — runs control/OutletRelocate.h's PlugRelocator on a thread of its own.
//
// The policy is the shared one (the ESP32 runs the same class on its main loop); this file is only the native shell's
// half: probing blocks on HTTP, so the policy runs here, and the network thread only ever reads a snapshot of its verdicts
// and collects the layout rewrites it asks for. Nothing in here decides anything.
#pragma once
// Spelled out, not left to whatever Boost happens to pull in: GCC (the Pi) is stricter than the Mac's clang.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <ArduinoJson.h>
#include "OutletRelocate.h"
#include "OutletOps.h"
#include "Sweep.h"

namespace dgbrain {

class Relocator {
public:
    // `self` and `startSweep` are the shell's: who we are to a plug, and how a sweep of our subnet is begun.
    Relocator(Sweep& sweep, std::function<outletops::Self()> self, std::function<bool()> startSweep)
        : _sweep(sweep), _self(std::move(self)), _startSweep(std::move(startSweep)) {}
    ~Relocator() { stop(); }
    void start() { _run = true; _th = std::thread([this] { loop(); }); }
    void stop() { _run = false; _cv.notify_all(); if (_th.joinable()) _th.join(); }

    // What the layout's plugs are doing now (network thread, every tick).
    void publish(std::vector<relocate::Plug> plugs) {
        std::lock_guard<std::mutex> g(_m);
        const bool first = !_published;
        _plugs = std::move(plugs); _published = true;
        if (first) _cv.notify_all();   // the first pass runs at once: plugs with a MAC on file wait on it to be polled
    }
    // The verdicts of the last pass. A plug the policy has not seen yet is trusted only if it has no MAC to check.
    bool trusted(const std::string& key, const std::string& mac) const {
        std::lock_guard<std::mutex> g(_m);
        auto it = _trusted.find(key);
        return it == _trusted.end() ? relocate::normMac(mac).empty() : it->second;
    }
    std::string replacedBy(const std::string& key) const {
        std::lock_guard<std::mutex> g(_m);
        auto it = _replaced.find(key);
        return it == _replaced.end() ? std::string() : it->second;
    }
    // Bumped whenever a verdict changes, so the shell re-syncs its poller then rather than on its next timer.
    uint32_t generation() const { return _gen; }
    std::vector<relocate::Rewrite> take() {
        std::lock_guard<std::mutex> g(_m);
        std::vector<relocate::Rewrite> out; out.swap(_rewrites); return out;
    }

private:
    struct NativeProbe : relocate::Probe {
        Relocator& r;
        explicit NativeProbe(Relocator& r) : r(r) {}
        relocate::Seen describe(const std::string& ip) override {
            relocate::Seen s;
            DynamicJsonDocument d(1024);
            if (deserializeJson(d, outletops::describeJson(ip.c_str(), r._self()))) return s;
            s.reachable = d["reachable"] | false; s.kind = d["kind"] | ""; s.mac = d["mac"] | "";
            return s;
        }
        bool sweepRunning() override { return r._sweep.running(); }
        bool startSweep() override { return r._startSweep(); }
        uint32_t sweepsFinished() override { return r._sweep.finished(); }
        std::vector<relocate::Found> sweepRows() override {
            std::vector<relocate::Found> out;
            for (const std::string& row : r._sweep.rows()) {
                StaticJsonDocument<1024> d;
                if (deserializeJson(d, row)) continue;
                out.push_back({d["ip"] | "", relocate::normMac(d["mac"] | ""), d["pickable"] | false, d["kind"] | ""});
            }
            return out;
        }
    };
    void loop() {
        relocate::PlugRelocator policy(8);   // a thread of its own: a boot's worth of plugs is checked in one pass
        NativeProbe probe(*this);
        while (_run) {
            std::vector<relocate::Plug> plugs;
            {
                std::unique_lock<std::mutex> g(_m);
                _cv.wait_for(g, std::chrono::seconds(5), [this] { return !_run || (_published && !_ranOnce); });
                if (!_run) return;
                if (!_published) continue;
                plugs = _plugs; _ranOnce = true;
            }
            std::vector<std::string> log;
            const uint32_t now = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            std::vector<relocate::Rewrite> rw = policy.tick(plugs, probe, now, &log);
            for (const std::string& l : log) plughttp::log(l);
            std::lock_guard<std::mutex> g(_m);
            bool changed = false;
            for (const relocate::Plug& p : plugs) {
                const bool t = policy.trusted(p.key);
                const std::string by = policy.replacedBy(p.key);
                auto ti = _trusted.find(p.key);
                if (ti == _trusted.end() || ti->second != t) changed = true;
                _trusted[p.key] = t;
                if (by.empty()) _replaced.erase(p.key); else _replaced[p.key] = by;
            }
            for (relocate::Rewrite& r : rw) _rewrites.push_back(std::move(r));
            if (changed) _gen++;
        }
    }
    Sweep& _sweep;
    std::function<outletops::Self()> _self;
    std::function<bool()> _startSweep;
    mutable std::mutex _m;
    std::condition_variable _cv;
    std::vector<relocate::Plug> _plugs;
    std::map<std::string, bool> _trusted;
    std::map<std::string, std::string> _replaced;
    std::vector<relocate::Rewrite> _rewrites;
    bool _published = false, _ranOnce = false;
    std::atomic<uint32_t> _gen{0};
    std::atomic<bool> _run{false};
    std::thread _th;
};

}  // namespace dgbrain
