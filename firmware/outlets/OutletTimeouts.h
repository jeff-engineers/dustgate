// =============================================================================
// outlets/OutletTimeouts.h — how long to wait on a smart plug. Lived in config.h, which is all Arduino;
// the outlet classes are shared with the native brain and must not drag a board's pin map in with them.
// config.h includes this, so every existing user is unchanged.
// =============================================================================
#pragma once

// HTTP request timeout per outlet — must be shorter than OUTLET_POLL_INTERVAL_MS
// to avoid stalling the poll loop when a device is offline
#define OUTLET_HTTP_TIMEOUT_MS      400

// Timeout for RPC config writes (Ws.SetConfig / Switch.SetConfig / Sys.SetConfig).
// These persist to the plug's flash and can take far longer than a status read,
// so they get a generous window. Only runs at provisioning time (device add /
// boot), never on the fast poll path, so a long value is safe here.
#define OUTLET_RPC_WRITE_TIMEOUT_MS 3000

// Reachability probe timeout for the provisioning path. Unlike the 400ms poll
// probe (which must fit inside the poll interval), provisioning runs rarely and
// off the motor loop, so it can afford to wait for a marginal plug to answer its
// first request instead of failing it and deferring for a whole retry cycle.
#define OUTLET_PROVISION_PROBE_TIMEOUT_MS 2000

#include <cstdint>

// How often each plug is asked for its power (ms). Both brains' pollers (SmartOutletControl, native PlugPoller).
#ifndef OUTLET_POLL_INTERVAL_MS
#define OUTLET_POLL_INTERVAL_MS     500
#endif

// How often to retry claiming a plug that is not yet claimed (e.g. briefly unreachable at boot). Each retry only
// fast-probes the unclaimed plugs; a plug already pushing is skipped, so this is cheap.
#define OUTLET_PROVISION_RETRY_MS   15000

// A plug whose push socket is up is not polled — that is the whole saving — but one that has said nothing for this long
// is polled anyway: a Shelly only speaks on a CHANGE, so a quiet socket is normal, and a hung plug looks exactly the same.
#define OUTLET_PUSH_QUIET_MS        30000

// THE rule for "ask this plug now?", in one place for both brains. The ESP32 skipped a push-connected plug for ever, so a
// plug that hung with its socket up read its last value until a reboot; the native brain had the quiet fallback alone
// (found 2026-10-18, sweeping the two brains for drift). A plug a node polls is never asked here — the caller skips it.
inline bool outletPollDue(bool pushConnected, uint32_t msSinceHeard) {
    return !pushConnected || msSinceHeard > OUTLET_PUSH_QUIET_MS;
}

