// =============================================================================
// control/DeviceProblems.h — the problems a person has to see about BOARDS and PLUGS, raised the same way on every brain.
//
// They lived in firmware.ino's raiseDeviceProblems(), walking the ESP32's own link and plug-slot tables. A native brain
// has neither table and would have shown a shop with a dead board and no complaint. This takes the two facts the rule
// needs — is each board linked, is each plug answering — as plain views, and does the raising.
//
// Raised every pass and refreshed in place, cleared the moment the cause is gone: a problem that outlives its cause is
// as bad as one that never appeared. Boards and plugs only — the RF press raises its own where it happens.
//
// PURE. test_deviceproblems.cpp.
// =============================================================================
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "TopologyRuntime.h"

namespace topo {

static const uint32_t kBoardOfflineAfterMs = 20000;
static const uint32_t kPlugDownAfterMs     = 120000;

struct BoardView {
    std::string host;
    bool        linked = false;
    bool        refused = false;       // the board answered, and belongs to another controller
    uint32_t    downForMs = 0;
    const char* moveFault = nullptr;   // why its last move did not finish cleanly, or null
};

struct PlugView {
    std::string key;     // stable per plug: "plug:<slot or machine>"
    std::string name;    // what the person calls it
    std::string ip;
    bool        reachable = false;
};

class DeviceProblems {
public:
    void update(TopologyRuntime& rt, const std::vector<BoardView>& boards, const std::vector<PlugView>& plugs, uint32_t now) {
        for (const BoardView& b : boards) {
            const std::string key = "board:" + b.host;
            if (!b.linked && !b.refused && b.downForMs >= kBoardOfflineAfterMs) {
                char t[160];
                std::snprintf(t, sizeof(t), "Not linked for %lus. Its gates stay where they were; tools on it cannot open a gate.",
                              (unsigned long)(b.downForMs / 1000));
                rt.raiseProblem(key, "board-offline", "bad", "board", b.host, t, now - b.downForMs);
            } else if (b.refused) {
                rt.raiseProblem(key, "board-offline", "bad", "board", b.host,
                                "The board refused this controller \xE2\x80\x94 it is paired to another one.", now);
            } else {
                rt.clearProblem(key);
            }
            // A move that did not finish: the link may be fine again by now, which is exactly why this is its own entry —
            // the gate is still in an unknown place.
            const std::string mkey = "move:" + b.host;
            if (b.moveFault) {
                rt.raiseProblem(mkey, "move-failed", "bad", "board", b.host, b.moveFault, now);
                // ...and the gate it was moving gets sent again, ONCE per fault: the fault stays up until the next move
                // finishes, so acting on the level would re-send for as long as it did.
                if (_faulted.insert(b.host).second) rt.reassertBoard(b.host);
            } else {
                rt.clearProblem(mkey);
                _faulted.erase(b.host);
            }
        }
        // A paired plug that stops answering is usually a new address (DHCP), not a dead plug. The tool it senses is
        // silently never "on" meanwhile.
        std::map<std::string, uint32_t> still;
        for (const PlugView& p : plugs) {
            if (p.reachable) { rt.clearProblem(p.key); continue; }
            auto it = _since.find(p.key);
            const uint32_t since = it == _since.end() ? (now ? now : 1) : it->second;
            still[p.key] = since;
            if (now - since < kPlugDownAfterMs) continue;
            char t[160];
            std::snprintf(t, sizeof(t), "No answer from %s for %lus. It may have a new address \xE2\x80\x94 a tool on it is not being sensed.",
                          p.ip.c_str(), (unsigned long)((now - since) / 1000));
            rt.raiseProblem(p.key, "plug-unreachable", "warn", "plug", p.name, t, since);
        }
        _since.swap(still);   // a plug no longer listed (unpaired) stops being tracked, and its problem is cleared below
    }
    // A plug that left the list takes its problem with it.
    void forget(TopologyRuntime& rt, const std::string& key) { rt.clearProblem(key); _since.erase(key); }

private:
    std::map<std::string, uint32_t> _since;
    std::set<std::string> _faulted;   // boards whose current moveFault has already been acted on
};

}  // namespace topo
