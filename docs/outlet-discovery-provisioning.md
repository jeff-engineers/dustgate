# Outlet discovery & provisioning — design doc

**Status:** draft (2026-07) — **not implemented.** No code changed. This is a
proposal for replacing the current four-step manual outlet setup with a flow
driven entirely by Shelly's documented local APIs.
**Scope:** the smart-outlet control path (`CONTROL_SMART_OUTLET`) on v1. The
transport/discovery decisions here should carry forward to v2 nodes.
**Companion docs:** [`v2-architecture-rfc.md`](v2-architecture-rfc.md),
[`manual-setup-wizard-requirements.md`](manual-setup-wizard-requirements.md).
Any API/model surface added here goes through the canonical-model + conformance
discipline in [`../shared/device-model/README.md`](../shared/device-model/README.md).

---

## 1. Motivation

Adding one smart outlet today takes four manual steps across three different
interfaces:

1. Provision the plug onto WiFi **using the Shelly mobile app**.
2. Assign it a **static IP** (via the app or the router).
3. **Discover** it from the Angular setup wizard hosted on the ESP32.
4. **Tie it to a specific tool/gate** by hand.

Every step is a place users get stuck, and steps 1–2 require software we don't
control and can't support. All four are avoidable: Shelly exposes local APIs
that cover provisioning, discovery, and push status. Nothing here needs the
Shelly cloud, the mobile app, or a firmware fork on the plug — it is all
implementable in our existing C++ firmware.

## 2. How it works today

| Concern | Current implementation |
|---|---|
| Discovery | mDNS `PTR` query for **`_http._tcp`** ([`utils/MdnsQuery.h:29`](../linear_actuator/utils/MdnsQuery.h)) |
| Shelly identification | Substring match `hostname.indexOf("shelly")` ([`linear_actuator.ino:921`](../linear_actuator/linear_actuator.ino)) |
| Generation detection | Probe Gen2 RPC, fall back to Gen1 on failure ([`linear_actuator.ino:957-966`](../linear_actuator/linear_actuator.ino)) |
| Addressing | Static IP, with mDNS hostname re-resolve on failure (`ShellyGen2Outlet::reresolve()`, [`outlets/ShellyGen2Outlet.cpp:18`](../linear_actuator/outlets/ShellyGen2Outlet.cpp)) |
| Status | HTTP **poll** every `OUTLET_POLL_INTERVAL_MS` (500 ms) with `OUTLET_HTTP_TIMEOUT_MS` (400 ms) per outlet |
| Discovery robustness | 3 mDNS attempts × 400 ms, merged by IP (`DISCOVER_MDNS_*` in `config.h`) |

### Known weaknesses

- **The hostname match is brittle.** `indexOf("shelly")` fails as soon as a user
  renames the device in the Shelly app — a normal thing to do, and a silent
  failure when it happens.
- **Probing costs a timeout per device.** Every Gen1 device (and every
  unreachable one) eats a full Gen2 timeout before the fallback.
- **We probe unrelated hosts.** `_http._tcp` returns every HTTP responder on the
  LAN — printers, NAS, TVs — all of which get filtered only by hostname string.
- **The poll loop does not scale.** `SMART_OUTLET_COUNT` is 7; at a 400 ms
  worst-case timeout that is **2.8 s of blocking work against a 500 ms
  interval**. One slow or offline plug stalls the main loop. The retry/timeout
  tuning in `MdnsQuery.h` exists precisely because this has already caused
  watchdog resets and stale-request crashes.
- **Static IPs are a band-aid** for unreliable discovery, but they are exposed to
  the user as a *requirement*.

## 3. What Shelly actually offers (local APIs)

Confirmed against Shelly's technical documentation (see §8):

| Capability | Mechanism | Gen1 | Gen2+ |
|---|---|---|---|
| Dedicated discovery service | mDNS **`_shelly._tcp`** | ✗ | ✔ |
| Generation in discovery | TXT record `gen=2` / `gen=3` (on both `_shelly._tcp` and `_http._tcp`) | ✗ | ✔ |
| Provisioning without the app | `WiFi.SetConfig` RPC at `192.168.33.1` in AP mode | ✔ (`/settings/sta`) | ✔ |
| Push status | Outbound WebSocket (`NotifyStatus`) | ✗ (CoIoT instead) | ✔ |
| Push status (broker) | MQTT | ✔ | ✔ |

## 4. Proposal

Five changes. (1), (2) and (5) are independently shippable; (3) and (4) pair
naturally because both configure the device during provisioning.

### 4.1 Discover via `_shelly._tcp` and trust the TXT records

Query `_shelly._tcp` as the primary service, and read the `gen` TXT record
instead of probing for it.

- Removes the `indexOf("shelly")` hostname hack entirely — renaming a device no
  longer breaks discovery.
- Removes the Gen2-probe-then-Gen1-fallback: `gen` is known before any HTTP
  request, so we construct the right outlet class immediately.
- Stops probing unrelated HTTP responders.

Keep a secondary `_http._tcp` pass for **Gen1** devices, which do not advertise
`_shelly._tcp`. Classification becomes: present in `_shelly._tcp` → Gen2+ (read
`gen`); present only in `_http._tcp` with no `gen` TXT → candidate Gen1, probe as
today.

The ESP-IDF `mdns_query_ptr()` already used in `MdnsQuery.h` returns TXT data via
`r->txt` / `r->txt_count`, so this is contained to that helper plus the
discovery block in `linear_actuator.ino`.

**Note:** with generation known up front, the retry/merge logic can likely
relax — but keep the multi-attempt merge until measured on real hardware. mDNS
over UDP is genuinely lossy and that logic was added for a real reason.

### 4.2 Demote static IP to an optional override

`reresolve()` already re-resolves by mDNS hostname on poll failure, so the
machinery for DHCP-addressed outlets exists. Once discovery is reliable, store
the hostname as the identity and let DHCP assign the address.

Keep static IP as an **advanced override**, not a setup step: some consumer APs
filter multicast or enable client isolation, which is the real reason static IPs
became the default path. The escape hatch must remain — it just should not be
step 2 of the happy path.

### 4.3 Provision the plug from the ESP32 — no Shelly app

Shelly AP-mode provisioning is a single RPC call:

```
http://192.168.33.1/rpc/WiFi.SetConfig?config={"sta":{"ssid":"...","pass":"...","enable":true}}
```

Proposed flow, driven from the setup wizard:

1. `WiFi.scanNetworks()` → offer SSIDs matching `Shelly*`.
2. ESP32 connects to the selected (open) Shelly AP.
3. GET `WiFi.SetConfig` with **the home WiFi credentials already stored in NVS**
   by `WiFiProvisioner`.
4. *(with §4.4)* GET `Ws.SetConfig` to point the plug at our WebSocket endpoint.
5. ESP32 disconnects, rejoins home WiFi.
6. Plug joins the network; it appears via `_shelly._tcp` within seconds.

The user plugs in the outlet, clicks "Add outlet," and is done. **They never
open the Shelly app and never type their WiFi password a second time** — the
ESP32 already has it.

**Design constraints:**

- **The ESP32 leaves the network for ~20–40 s** while associated with the Shelly
  AP — and it is the host of the Angular UI. The wizard needs a
  "provisioning… reconnecting" state that polls until the device returns. The
  captive-portal flow already establishes this UX shape; reuse it.
- **We forward the user's WiFi password to the plug.** It is already in NVS, so
  there is no new storage exposure, but the UI should say plainly what is
  happening rather than doing it silently.
- **Failure recovery matters.** If step 3 or 5 fails, the ESP32 must reliably
  fall back to the stored home credentials rather than stranding itself on a
  Shelly AP. Treat "rejoin home WiFi" as the guaranteed final action of the
  sequence, including on every error path.

### 4.4 Replace polling with Outbound WebSocket (Gen2+)

Gen2 devices can be configured to dial **out** to a WebSocket server, delivering
a full status snapshot on connect and `NotifyStatus` on every change. We already
run `ESPAsyncWebServer` with a `/ws` endpoint — add a second path (e.g.
`/shelly-ws`) for plugs.

Configure once via `Ws.SetConfig` during provisioning (§4.3 step 4), so it costs
the user no extra steps.

Benefits:
- Eliminates the 2.8 s-vs-500 ms polling problem outright.
- Sub-second reaction instead of 500 ms poll granularity + `OUTLET_ON_DEBOUNCE_MS`.
- Offline plugs cost nothing instead of stalling the loop.

MQTT is the documented alternative and would also work, but it requires running
or depending on a broker. Shelly's own guidance is that WebSocket suits
real-time state without a broker, which matches this architecture — **choose
Outbound WebSocket.**

The debounce logic (`OUTLET_ON_DEBOUNCE_MS` / `OUTLET_OFF_DEBOUNCE_MS`) stays —
it exists to filter motor inrush and mechanical coast-down, which is a physical
concern independent of transport.

### 4.5 Auto-map the tool by watching for the power spike

Not a protocol change — a wizard inversion. Instead of asking the user to
associate an IP with a tool, prompt: *"Turn on your table saw now."* Whichever
outlet crosses its threshold gets bound to that gate.

This removes the most confusing step in the current flow, uses data we already
collect, and becomes markedly snappier under §4.4.

## 5. Gen1 compatibility

Gen1 devices support neither `_shelly._tcp` nor Outbound WebSocket. Their push
mechanism is CoIoT (CoAP multicast), which is a separate implementation.

**Proposed position:** Gen2+ is the supported path (the reference device is a
Shelly Plug US G4). Gen1 keeps the existing `_http._tcp` discovery and HTTP
polling as a legacy fallback. Do **not** invest in CoIoT unless real Gen1 demand
appears.

## 6. Suggested sequencing

| Phase | Contents | Rationale |
|---|---|---|
| 1 | §4.1 discovery + §4.2 static-IP demotion | Contained, removes a real bug and a setup step |
| 2 | §4.5 auto-map by power spike | Pure UX, no protocol work, large clarity win |
| 3 | §4.3 ESP32 provisioning + §4.4 Outbound WebSocket | Headline UX win; both configure the device at provisioning time |

Phases 1 and 2 are independently shippable and do not block each other.

## 7. Open questions

- Does `_shelly._tcp` discovery alone prove reliable enough on real consumer
  APs to drop the 3× retry merge, or does the lossy-UDP retry stay?
- How long does the ESP32 actually take to leave the LAN, provision a plug, and
  rejoin? This sets the UI reconnect timeout and needs measuring on hardware.
- Should the WebSocket endpoint authenticate plugs, or is LAN-local trust
  acceptable? (Current HTTP API uses an API key; plugs dialing in are a new
  inbound surface.)
- Does `Ws.SetConfig` survive a plug factory-reset / firmware update, or does
  the device need re-provisioning?
- Mixed fleets: what does the UI show when a user has both Gen1 (polled) and
  Gen2 (push) outlets with different latency characteristics?

## 8. References

- [Shelly mDNS](https://shelly-api-docs.shelly.cloud/gen2/General/mDNS/) ·
  [Discovering Shelly devices via mDNS (KB)](https://kb.shelly.cloud/knowledge-base/kbsa-discovering-shelly-devices-via-mdns)
- [Outbound Websocket](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/Ws/) ·
  [Notifications / NotifyStatus](https://shelly-api-docs.shelly.cloud/gen2/General/Notifications/) ·
  [MQTT](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/Mqtt/)
- [WiFi component / `WiFi.SetConfig`](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/WiFi/)
