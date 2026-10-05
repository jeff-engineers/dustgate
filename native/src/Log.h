// native/src/Log.h — the brain's console: stdout, plus a ring the app reads at GET /api/serial (the ESP32 keeps the
// same, utils/SerialLog.h), plus the link log (utils/LinkLog.h) as JSON Lines in the state directory.
#pragma once
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <random>
#include <string>

namespace dglog {

struct Ring {
    std::mutex m;
    std::string text;          // the last kMax bytes
    size_t      base = 0;      // the byte offset of text[0] since boot
    uint32_t    bootId = 0;
    std::string linkLogPath;   // "" = not kept
    uint32_t    bootNo = 0;
    static constexpr size_t kMax = 32 * 1024;
};
inline Ring& R() { static Ring r; return r; }

inline uint32_t upMs() {
    static const auto t0 = std::chrono::steady_clock::now();
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

// "HH:MM:SS.mmmZ " — the shape the ESP32's log lines carry, which the app's log screen parses.
inline std::string stamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{}; gmtime_r(&t, &tm);
    char b[32]; std::snprintf(b, sizeof(b), "%02d:%02d:%02d.%03dZ ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000));
    return b;
}

inline void line(const std::string& s) {
    const std::string out = stamp() + s + "\n";
    std::fwrite(out.data(), 1, out.size(), stdout); std::fflush(stdout);
    Ring& r = R(); std::lock_guard<std::mutex> g(r.m);
    r.text += out;
    if (r.text.size() > Ring::kMax) { const size_t drop = r.text.size() - Ring::kMax; r.text.erase(0, drop); r.base += drop; }
}
inline void linef(const char* fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); std::vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    std::string s = b; while (!s.empty() && s.back() == '\n') s.pop_back();
    line(s);
}

// What GET /api/serial?from=N answers: the text from the cursor, and the cursor to ask with next.
inline std::string readFrom(size_t from, size_t& start, size_t& next, size_t maxBytes = 8192) {
    Ring& r = R(); std::lock_guard<std::mutex> g(r.m);
    start = from < r.base ? r.base : from;                 // the ring wrapped past the caller
    const size_t end = r.base + r.text.size();
    if (start > end) start = end;
    size_t n = end - start; if (n > maxBytes) n = maxBytes;
    next = start + n;
    return r.text.substr(start - r.base, n);
}

// One link-log event: {"ts":..,"up":..,"boot":..,"ev":"..","node":"..", <extra>}\n — the ESP32's format.
inline void linkEvent(const char* ev, const char* node, const char* extra) {
    std::string l = "{\"ts\":" + std::to_string((unsigned long)std::time(nullptr)) + ",\"up\":" + std::to_string(upMs()) +
                    ",\"boot\":" + std::to_string(R().bootNo) + ",\"ev\":\"" + (ev ? ev : "?") + "\"";
    if (node && *node) l += std::string(",\"node\":\"") + node + "\"";
    if (extra && *extra) l += std::string(",") + extra;
    l += "}";
    line("[LINKLOG] " + l);
    Ring& r = R(); std::lock_guard<std::mutex> g(r.m);
    if (!r.linkLogPath.empty()) { std::ofstream f(r.linkLogPath, std::ios::app); f << l << "\n"; }
}

}  // namespace dglog
