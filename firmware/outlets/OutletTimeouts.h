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
