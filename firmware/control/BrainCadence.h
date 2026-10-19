// =============================================================================
// control/BrainCadence.h — how often a brain does the things it does on a clock, and what its hourly line says.
//
// Both brains beacon and both keep a link log. The numbers and the line used to be written into each shell (firmware.ino,
// native/src/main.cpp) and agreed only by luck; the native brain had no hourly line at all, so a quiet stretch in its log
// read the same as a brain that had stopped writing (found 2026-10-18, sweeping the two brains for drift). One home now,
// held by tools/check-brain-parity.js and test_braincadence.cpp.
//
// Pure — STL only.
// =============================================================================
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace topo {

// The beacon ("DGB1|<id>|<ip>|<port>", NodeLink.h kBeaconPort): quick while a board the shop needs is down, so it finds
// us fast; a slow heartbeat otherwise, for a board that joins later or reboots. An optional board does not hurry it.
static constexpr uint32_t kBeaconFastMs = 5000;
static constexpr uint32_t kBeaconSlowMs = 60000;
inline uint32_t beaconEveryMs(bool requiredBoardDown) { return requiredBoardDown ? kBeaconFastMs : kBeaconSlowMs; }

// The link log's "still here" line: once an hour, every paired board and whether it is linked.
static constexpr uint32_t kHourlyLogMs = 3600000;
inline std::string hourlyNodesField(const std::vector<std::pair<std::string, bool>>& boards) {
    std::string out;
    for (const auto& b : boards) { if (!out.empty()) out += ' '; out += b.first + (b.second ? ":up" : ":down"); }
    return out;
}

}  // namespace topo
