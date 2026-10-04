// =============================================================================
// control/RfDefaults.h — the measured numbers for keying the Rockler remote, in one
// place that touches no hardware.
//
// They were static members of RfCollectorPresser, which is Arduino-only, and a presser
// on a NODE (RemoteRfPresser) needs the same defaults to fill a PRESS frame. Two copies
// of a number measured on a bench against a real receiver is exactly how one drifts, so
// both now read these. Where each came from is in RfCollectorPresser.h.
// =============================================================================
#pragma once
#include <cstdint>

namespace topo { namespace rf {
static const uint8_t  kRocklerAddress = 0b01011110;
static const uint8_t  kRocklerData    = 0b1110;
static const uint32_t kDefaultTickUs  = 270;
static const uint16_t kDefaultRepeats = 24;
}}  // namespace topo::rf
