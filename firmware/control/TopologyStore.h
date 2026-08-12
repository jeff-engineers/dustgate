// =============================================================================
// TopologyStore.h — Topology persistence on LittleFS.
//
// Owns /topology.json on the same LittleFS ("ffat") partition that serves the
// Angular bundle. The primary controller is the single source of truth: the UI
// PUTs a validated topology, we persist it, and the controller layer (Stage 3)
// reads it back to drive actuators.
//
//   • save()  parses + runs a MINIMAL structural check, then writes atomically
//             (temp file + rename) so a truncated upload can't corrupt the live
//             document.
//   • load()  returns the raw JSON string for GET (streamed straight back).
//
// The authoritative validator is validateTopology() in the UI/shared model
// (shared/device-model/topology.js) — it runs before the UI ever PUTs. The
// device check here is a cheap last line of defence against garbage/truncation,
// NOT a re-implementation of the full schema rules.
//
// Device-only (depends on LittleFS) — unlike TopologyRouter.h it does not
// host-compile, and nothing in the conformance tests includes it.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

namespace topo {

static const char* kTopologyPath = "/topology.json";
static const char* kTopologyTmp  = "/topology.json.tmp";

// Upper bound on a stored topology. A big shop (~30 elements w/ servo states +
// outlet blocks + ui layout) serialises well under this; deserializeJson caps
// its own allocation, so an oversized body is rejected cleanly rather than
// exhausting the heap.
static const size_t kMaxTopologyBytes = 24 * 1024;

class TopologyStore {
public:
    // Note whether a topology is already persisted. Assumes LittleFS is mounted
    // (HttpApiServer::begin() mounts it before routes go live). Safe to skip;
    // load()/save()/exists() each touch the FS directly.
    bool begin() {
        _present = LittleFS.exists(kTopologyPath);
        return true;
    }

    bool exists() const { return LittleFS.exists(kTopologyPath); }

    // Read the stored JSON verbatim (empty String if none / read error).
    String load() const {
        File f = LittleFS.open(kTopologyPath, "r");
        if (!f) return String();
        String out = f.readString();
        f.close();
        return out;
    }

    // Validate + persist a freshly-uploaded body. On success the temp file is
    // renamed over the live one (atomic). On any failure the live file is left
    // untouched and `err` explains why.
    bool save(const uint8_t* data, size_t len, String& err) {
        if (len == 0)                 { err = "empty body"; return false; }
        if (len > kMaxTopologyBytes)  { err = "topology too large"; return false; }

        DynamicJsonDocument doc(kMaxTopologyBytes);
        DeserializationError je = deserializeJson(doc, data, len);
        if (je) { err = String("invalid JSON: ") + je.c_str(); return false; }

        if (!validateMinimal(doc.as<JsonVariantConst>(), err)) return false;

        // Atomic replace: write temp, then rename over the live file.
        LittleFS.remove(kTopologyTmp);
        File f = LittleFS.open(kTopologyTmp, "w");
        if (!f) { err = "cannot open temp file"; return false; }
        size_t wrote = f.write(data, len);
        f.close();
        if (wrote != len) {
            LittleFS.remove(kTopologyTmp);
            err = "short write (out of space?)";
            return false;
        }
        // LittleFS.rename atomically replaces an existing destination, so we do
        // NOT remove the live file first — that would open a window where a
        // reader sees no topology at all.
        if (!LittleFS.rename(kTopologyTmp, kTopologyPath)) {
            LittleFS.remove(kTopologyTmp);
            err = "rename failed";
            return false;
        }
        _present = true;
        return true;
    }

    // Cheap structural gate — NOT the full schema. Rejects obvious garbage and
    // truncation so the device never persists something the controller can't
    // parse. The UI's validateTopology() remains authoritative.
    static bool validateMinimal(JsonVariantConst t, String& err) {
        if (!t.is<JsonObjectConst>())          { err = "not an object"; return false; }
        if (!t["elements"].is<JsonArrayConst>())    { err = "elements not an array"; return false; }
        if (!t["controllers"].is<JsonArrayConst>()) { err = "controllers not an array"; return false; }

        int collectors = 0;
        for (JsonObjectConst e : t["elements"].as<JsonArrayConst>()) {
            const char* type = e["type"];
            if (type && strcmp(type, "collector") == 0) collectors++;
        }
        if (collectors != 1) { err = "exactly one collector required"; return false; }

        int primaries = 0;
        for (JsonObjectConst c : t["controllers"].as<JsonArrayConst>()) {
            const char* role = c["role"];
            if (role && strcmp(role, "primary") == 0) primaries++;
        }
        if (primaries != 1) { err = "exactly one primary controller required"; return false; }

        return true;
    }

private:
    bool _present = false;
};

} // namespace topo
