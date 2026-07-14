# DustGate — Requirements & Architecture

Motorized blast gate manifold controller. A rack-and-pinion linear actuator selects which dust
collection port is open based on which shop tool is in use. The system is largely self-configuring
via an AI-powered setup agent accessible from a browser on the local network.

---

## 1. Hardware (decided)

| Component | Selection | Notes |
|-----------|-----------|-------|
| MCU | Adafruit ESP32-S2 Feather (#5000) | Replaced STM32 — better WiFi, larger ecosystem |
| Stepper driver | Adafruit TMC2209 Breakout (#6121) | UART current control + optional StallGuard |
| Motor | LDO-42STH48-2004MAH (NEMA 17) | 1.8° step, 2A, matched to TMC2209 |
| Drive | 15-tooth pinion + 20T/4.145mm pitch rack | ~51.47 steps/mm at 16× microstep |
| Smart outlets | Shelly Plug US Gen 4 (~$21 ea., us.shelly.com) | Fully local REST API, 1800W/15A, no cloud required |
| Home endstop | NC mechanical limit switch on D10 | Fail-safe: open wire reads as triggered |


### Planned carrier PCB (Task 5)
- ESP32-S2 Feather + BTT TMC2209 StepStick on 2.54mm headers
- Screw terminals for motor, endstop
- CNC-millable FR4 (no soldermask required)
- Replaces current breadboard/breakout assembly

---

## 2. Motion System

- **Rack-and-pinion** linear actuator, up to `NUM_STOPS` selectable stop positions (compile-time max, currently 16); the runtime-active count (≤ max) is separately configurable via `/api/config/gates` or Settings without recompiling
- **Stop 0** = home/disabled position
- **Homing:** drive toward NC limit switch at `HOMING_SPEED_STEPS_PER_SEC`, back off `HOME_BACKOFF_STEPS` after trigger, zero position
- **Positioning:** step-counted moves from home; stop distances trained by setup agent or computed from geometry constants
- **Stop distance storage:** EEPROM/NVS via `Preferences`; `clearcal` command erases stale calibration
- **Measured geometry:** gate 1 at step −51 from home; gate-to-gate ≈ 4270 steps (82.9mm × 51.47 steps/mm)
- **No enable/disable concept:** the system always runs; only e-stop (software-only — no physical e-stop button) halts motion. `home` re-homes and clears any latched e-stop; the system warns once if position commands arrive before homing.
- **Idle power-off:** if no move/home command arrives for `IDLE_TIMEOUT_SEC_DEFAULT` (3600s default, runtime-configurable via `POST /api/config/idle-timeout` or the Settings screen), the stepper driver is fully disabled and the position marked unknown, forcing a rehome on the next move instead of sitting energized indefinitely.

---

## 3. Control Modes

Exactly one control mode is active at compile time (`config.h`). The HTTP API runs *alongside* any control mode when `ENABLE_HTTP_API` is defined.

### 3a. Serial Debug (`CONTROL_SERIAL_DEBUG`)
- Human-readable serial commands: `home`, `1`–`7`, `jog <mm>`, `estop`, `status`, `clearcal`, `wifireset`, `help`
- Commands reserved for setup agent (code present, disabled at runtime): `train`, `autotune`, `sgthrs`, `homespeed`

### 3b. Smart Outlet (`CONTROL_SMART_OUTLET`)
- See Section 5 below

### 3c. HTTP API (`ENABLE_HTTP_API`)
- See Section 6 below

---

## 4. WiFi Provisioning

- On first boot (no stored credentials): ESP32 starts AP `DustGate-Setup`, serves captive portal at `http://192.168.4.1`
- Portal collects: WiFi SSID, WiFi password, Anthropic API key (optional, enables setup agent)
- Credentials stored in NVS namespace `wifi_creds` (keys: `ssid`, `pass`)
- Anthropic key stored in separate NVS namespace `agent_cfg` (key: `claude_key`) — survives `wifireset`
- `wifireset` serial command: erases WiFi credentials, reboots into portal (Anthropic key preserved)
- Developer override: hardcode `WIFI_STA_SSID` / `WIFI_STA_PASS` in `config.h` to bypass portal entirely
- On connection failure after stored credentials: falls back to portal
- Implementation: header-only `WiFiProvisioner` namespace, no external libraries (ESP32-core `WebServer` + `Preferences`)

---

## 5. Smart Outlet Control (`CONTROL_SMART_OUTLET`)

### Polling
- FreeRTOS task on Core 0, polls all configured outlets every `OUTLET_POLL_INTERVAL_MS` (500ms)
- HTTP request timeout: `OUTLET_HTTP_TIMEOUT_MS` (400ms) — shorter than poll interval
- Shelly Gen 1: `GET http://<ip>/status` → `meters[0].power`
- Shelly Gen 2: `GET http://<ip>/rpc/Switch.GetStatus?id=0` → `apower`
- Extensible base class `SmartOutlet` — new outlet types add a subclass, no changes to poll task

### Gate selection logic
- Any outlet exceeding its wattage threshold is "active"
- **Highest-wattage active outlet wins** (most recently powered tool dominates)
- ON debounce: `OUTLET_ON_DEBOUNCE_MS` (1000ms) — prevents false triggers from inrush
- OFF debounce: `OUTLET_OFF_DEBOUNCE_MS` (3000ms) — slack for tools with mechanical coast-down
- All outlets idle → return to home (stop 0)

### Manual override
- `setManualOverride(int stop)` on `SmartOutletControl`: overrides outlet selection until an outlet has a genuine OFF→ON transition
- **Edge-triggered, not level-triggered:** a tool that was already running before the manual move does *not* immediately re-clobber the override just because it's still "active" — only a fresh power-on clears it. (An earlier level-triggered version had this bug: moving manually while another gate's tool kept running would snap back within one poll tick.)
- Accessible via HTTP API (`POST /api/move` while in outlet mode)

### Outlet discovery (mDNS)
- `GET /api/outlets/discover` scans mDNS for `_http._tcp` services, filters to hostnames containing "shelly", and probes each match (Gen2 first, then Gen1) for reachability/power/generation — see `linear_actuator/utils/MdnsQuery.h`
- Retries the mDNS query a few times (`DISCOVER_MDNS_ATTEMPTS`) and merges by IP, since UDP responses are lossy
- Also fetches the outlet's own app-assigned name when available — Gen1 via `/settings`, Gen2 via `Switch.GetConfig?id=0` (falling back to `Sys.GetConfig`) — see `linear_actuator/outlets/ShellyDeviceName.h`
- Lets the setup wizard's "Scan for outlets" list replace manual IP entry in most cases; manual entry remains as a fallback
- **Must run on the main loop task**, not a spawned FreeRTOS task — ESP32's mDNS responder isn't safe to call concurrently with its own hostname-advertising; doing so from a separate task previously corrupted the heap and crashed the device
- The mDNS hostname is persisted alongside the IP (`o<N>_host` in NVS) so an outlet can re-resolve its IP after a DHCP lease change instead of going silently unreachable

### Per-outlet configuration (NVS)
- Stored in namespace `outlets`: generation, IP, mDNS hostname, name, stop index, threshold watts
- Managed by setup agent (or the manual wizard) via `OutletConfig` namespace + `SmartOutletControl::configureOutlet()` / `saveAll()`

### 240V tools
- Plug-in smart outlets are 120V only
- 240V tools (large table saw, planer, etc.) require a separate detection method or a fixed gate assignment

---

## 6. HTTP REST + WebSocket API (`ENABLE_HTTP_API`)

Runs alongside any control mode. Built on ESPAsyncWebServer + ArduinoJson v6.

### Authentication
- API key auto-generated on first boot using `esp_fill_random()`, stored in NVS
- All requests must include header: `X-Api-Key: <key>`
- Key printed to serial on boot; also retrievable via the Angular setup UI
- CORS: `Access-Control-Allow-Origin: *` for Angular dev server (localhost:4200)

### Thread safety
- Async handlers (Core 0) write only to mutex-protected `_pendingCmd` flags
- Main loop (Core 1) drains commands via `consume*()` methods — no direct motor access from handlers

### REST Endpoints

| Method | Path | Body / Params | Action |
|--------|------|---------------|--------|
| GET | `/api/info` | — | Unauthenticated bootstrap: API key, gate count, version, orientation, idle timeout |
| GET | `/api/status` | — | Full system status JSON |
| POST | `/api/home` | — | Home the actuator |
| POST | `/api/move` | `{"stop": N}` | Move to stop N (0 = home) |
| POST | `/api/jog` | `{"mm": ±F}` | Relative jog in mm |
| POST | `/api/estop` | — | Software emergency stop (no physical e-stop button exists) |
| POST | `/api/enable` / `/api/disable` | — | Legacy/vestigial — routes exist but `isEnabled()` is hardcoded `true`; the system always runs |
| POST | `/api/clearcal` | — | Erase calibration, gate count, and outlet config — "Start Over" |
| GET | `/api/outlets/discover` | — | Scan mDNS for Shelly outlets (see Section 5) |
| POST | `/api/outlets/ping` | `{"ip": "..."}` | One-shot reachability check by IP (FreeRTOS task, non-blocking) |
| PUT | `/api/outlets/:slot` | `{"gen","ip","host","name","stop","threshold"}` | Configure/update outlet slot |
| DELETE | `/api/outlets/:slot` | — | Remove outlet slot |
| POST | `/api/outlets/save` | — | Persist outlet config to NVS |
| PUT | `/api/dustcollector` | `{"gen","ip","host"}` | Assign the dust collector's switchable plug |
| DELETE | `/api/dustcollector` | — | Unassign the dust collector plug |
| POST | `/api/dustcollector/switch` | `{"on": bool}` | Manual dashboard on/off |
| POST | `/api/config/orientation` | `{"homeOnRight": bool}` | Persist visual orientation |
| POST | `/api/config/motor` | `{"invertDirection": bool}` | Flip homing direction |
| POST | `/api/config/gates` | `{"numGates": N}` | Set active gate count |
| POST | `/api/config/idle-timeout` | `{"seconds": N}` | Set idle power-off timeout (0 = never) |
| POST | `/api/wifi/reset` | — | Erase WiFi credentials, reboot into setup portal |
| POST | `/api/agent/chat` | `{"messages": [...]}` | Stateless Claude API proxy (see Section 7) |
| PUT | `/api/agent/key` | `{"key": "sk-ant-..."}` | Update Anthropic key in NVS — no UI entry point (removed deliberately; LAN-served page shouldn't expose it), only serial `provision` or the captive portal |

### WebSocket (`ws://<ip>/ws`)
- Push-only: server sends status JSON when system state changes
- Change detection via fingerprint struct (not full JSON string comparison) — avoids spurious pushes on floating sensor noise
- Fields that trigger a push: `stateName`, `currentStop`, `targetStop`, `homed`, `enabled`, `endstopHome`, `numActiveStops`, and (outlet mode) each outlet's name/ip/stop mapping, plus dust collector `dcOn`/`dcConfigured`
- These last two were both bugs found and fixed in practice: outlet config changes and dust-collector on/off toggles were originally excluded from the fingerprint, so the UI could show stale outlet lists or a stale DC switch state until some *other* field happened to change too
- Floating-point fields (positionSteps) do not trigger pushes alone

### Status JSON shape
```json
{
  "state": "IDLE",
  "currentStop": 1,
  "targetStop": 1,
  "positionSteps": -51,
  "homed": true,
  "enabled": true,
  "endstopHome": false,
  "outlets": [
    { "slot": 0, "name": "Table Saw", "ip": "192.168.1.101", "stop": 1,
      "powerW": 0.0, "active": false, "reachable": true, "thresholdW": 30.0 }
  ]
}
```

---

## 7. Setup Agent

### Architecture: ESP32 as stateless Claude API proxy

The ESP32 does **not** run or maintain the AI agent. It acts as a thin HTTPS forwarder.

```
Angular (browser)                  ESP32                        Anthropic API
─────────────────    POST /api/agent/chat    ──────────────    POST /v1/messages
  conversation    ──────────────────────────►  forward body  ──────────────────►
  history (full)  ◄──────────────────────────  forward resp  ◄──────────────────
                        (stateless)
```

- Angular holds the full conversation history and accumulates tool results
- ESP32 receives `{"messages": [...], "tools": [...]}` in the request body, adds auth headers (`x-api-key`, `anthropic-version`), POSTs to `https://api.anthropic.com/v1/messages`, and streams the response back
- HTTPS: `WiFiClientSecure` with `setInsecure()` for now  
  **TODO before any cloud/public deployment: validate Anthropic root CA cert instead of skipping verification**
- Anthropic API key read from NVS (`WiFiProvisioner::getAnthropicKey()`) — never sent to the browser

### Claude tool use loop (Angular side)
1. Angular sends current messages array to `/api/agent/chat`
2. Claude returns `tool_use` blocks → Angular executes each against the ESP32 REST API
3. Angular appends `tool_result` blocks to conversation, sends again
4. Repeat until Claude returns `stop_reason: "end_turn"`
5. Final assistant message displayed to user

### Setup agent capabilities (tools exposed to Claude)
- `home` — home the actuator
- `move_to_stop` — move to stop N
- `jog` — jog ±mm
- `get_status` — read current state
- `configure_outlet` — assign outlet slot: IP, name, stop index, watt threshold
- `delete_outlet` — remove outlet slot
- `save_outlet_config` — persist to NVS
- `ping_outlet` — check reachability
- `set_stop_count` — update NUM_STOPS equivalent (requires recompile note — or dynamic config via NVS)
- `set_homespeed` — tune homing speed
- `set_sgthrs` — tune StallGuard threshold (if sensorless mode ever re-enabled)

### Setup agent tasks (conversational flow)
1. Confirm number of stops / gates
2. For each stop: jog to position, confirm distance, assign stop index
3. For each outlet: enter IP, confirm reachability (ping), assign to stop, set watt threshold
4. Save configuration, verify by cycling through all stops
5. Optionally tune debounce timers

---

## 8. Angular Front-End

Source lives in `dustgate-ui/`. Served from ESP32 LittleFS flash.

### Views (hash routing via `withHashLocation()` — no server-side redirects needed)

| Route | Description |
|-------|-------------|
| `/#/` | **Dashboard** — interactive manifold visualizer (tap a gate to move, tap home, tap dust collector to toggle), gear icon → Settings |
| `/#/setup` | **Guided (AI) setup** — Claude agentic loop, tool-call progress pills, back arrow → dashboard. Kept polished as a demo feature but expected to be excluded from the final release build. |
| `/#/setup/manual` | **Manual setup wizard** — step through gate positions and outlet assignment without the AI; the primary supported setup path |
| `/#/settings` | **Settings** — idle power-off timeout, home orientation, motor direction, gate count, port size (client-side only), forget-WiFi, reset-calibration, and links to both setup wizards. Consolidates settings that used to be wizard-only or not reachable post-setup at all. |

- Gate/tool names pull from `status.outlets[].name` via WebSocket — no hardcoding
- Dust collector toggle drives a Shelly smart plug via `/api/dustcollector/switch`
- The Anthropic API key has no UI entry point (deliberately removed from Settings — see Section 6) — only settable via serial `provision` or the WiFi captive portal

### API key bootstrap

The app calls `GET /api/info` (unauthenticated) on first load to get the API key. No user entry required. The endpoint is only reachable on the local network.

### Deploy workflow (production)

```bash
# 1. Install dependencies (first time only)
cd dustgate-ui
npm install

# 2. Build and copy to ESP32 data directory
bash deploy.sh
# This runs `ng build`, gzips JS/CSS (ESPAsyncWebServer serves .gz automatically),
# and copies everything to ../linear_actuator/data/

# 3. Upload filesystem image to ESP32
cd ..
pio run --target uploadfs
# Then flash the firmware as normal: pio run --target upload
```

### Development workflow

```bash
# 1. Set the ESP32's IP address in dustgate-ui/proxy.conf.json
#    (change the "target" values from 192.168.1.100 to your device's IP)

# 2. Start the dev server — proxies /api and /ws to the real device
cd dustgate-ui
npm start
# Open http://localhost:4200 in a browser
```

### File locations

| Path | Purpose |
|------|---------|
| `dustgate-ui/` | Angular source |
| `dustgate-ui/proxy.conf.json` | Dev proxy — set ESP32 IP here |
| `dustgate-ui/deploy.sh` | Build + gzip + copy script |
| `linear_actuator/data/` | LittleFS image content (generated by deploy.sh) |

---

## 9. Output — Dust Collector

- Controlled by a dedicated switchable Shelly smart plug over WiFi (no local wiring)
- Turns on automatically when the actuator is at a non-home stop (a tool is running) and off at home
- Can also be toggled manually from the dashboard
- Configured via `PUT /api/dustcollector` with `{"gen":2,"ip":"192.168.1.x"}`

---

## 10. Implementation Status

| Task | Status | Notes |
|------|--------|-------|
| Motion system (homing, multi-stop, jog) | ✅ Done | Tested with clearcal fix |
| Serial debug control | ✅ Done | `help`, `home`, `1–7`, `jog`, `estop`, `status`, `wifireset` |
| Shelly outlet polling | ✅ Done | Gen1 + Gen2, debounce, FreeRTOS task |
| WiFi provisioning (captive portal) | ✅ Done | Collects WiFi creds + Anthropic API key |
| HTTP REST + WebSocket API | ✅ Done | Auth, WS fingerprint push, non-blocking ping, `/api/agent/chat` proxy |
| `setManualOverride()` on SmartOutletControl | ✅ Done | Edge-triggered — clears only on a fresh OFF→ON transition, not just "still on" |
| Wire `HttpApiServer` into `linear_actuator.ino` | ✅ Done | estop/home/move/jog/clearcal/outlets all wired |
| Angular front-end | ✅ Done | Dashboard, manual wizard, AI setup chat, Settings screen — served from LittleFS |
| LittleFS static serving + `/api/info` | ✅ Done | Auto-serves .gz, bootstrap key endpoint |
| Dust collector (Shelly plug) | ✅ Done | Auto + manual dashboard toggle via `/api/dustcollector`; scan-first discovery |
| Manual setup wizard | ✅ Done | `/#/setup/manual` — primary supported setup path, no AI dependency |
| Settings screen | ✅ Done | `/#/settings` — idle timeout, orientation, motor direction, gate count, port size, forget-WiFi, reset-calibration |
| Outlet mDNS discovery | ✅ Done | "Scan for outlets" — replaces manual IP entry as the primary path |
| Idle power-off | ✅ Done | Driver disables after inactivity; forces rehome on next use |
| Physical e-stop button | ❌ Removed | Hardware deemed too low-power to need one; software e-stop only |
| Carrier PCB (KiCad) | ⏳ Planned | Task 5 |

---

## 11. Key Config Parameters (`config.h`)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `NUM_STOPS` | compile-time max | Number of gate positions (runtime-active count is separately configurable up to this max, via `/api/config/gates` or Settings) |
| `MICROSTEPS` | 16 | TMC2209 microstep divisor |
| `TMC2209_CURRENT_MA` | 800 | UART run current (mA) |
| `TMC2209_HOLD_CURRENT_MA` | 75 | UART hold current (mA) — kept low so the motor stays cool between moves |
| `HOME_DIRECTION_DEFAULT` | 1 | Step direction toward home endstop (runtime override via `/api/config/motor`) |
| `HOME_BACKOFF_STEPS` | 50 | Steps to back off after endstop triggers |
| `MAX_SPEED_STEPS_PER_SEC` | 2000 | Normal move speed |
| `HOMING_SPEED_STEPS_PER_SEC` | 500 | Homing speed |
| `IDLE_TIMEOUT_SEC_DEFAULT` | 3600 | Seconds of inactivity before the driver powers off (0 = never); runtime override via `/api/config/idle-timeout` or Settings |
| `OUTLET_POLL_INTERVAL_MS` | 500 | Shelly poll rate |
| `OUTLET_HTTP_TIMEOUT_MS` | 400 | Per-request timeout for outlet HTTP calls (must stay under the poll interval) |
| `OUTLET_ON_DEBOUNCE_MS` | 1000 | ON debounce |
| `OUTLET_OFF_DEBOUNCE_MS` | 3000 | OFF debounce |
| `OUTLET_DEFAULT_THRESHOLD_W` | 5.0 | Watts threshold for "tool on" |
| `DISCOVER_MDNS_ATTEMPTS` | 3 | mDNS query retries for outlet discovery (UDP is lossy) |
| `DISCOVER_MDNS_TIMEOUT_MS` | 400 | Per-attempt mDNS query timeout (bypasses ESPmDNS's hardcoded 3000ms — see `MdnsQuery.h`) |
| `DISCOVER_MDNS_RETRY_DELAY_MS` | 150 | Delay between discovery attempts |
| `API_KEY_BYTES` | 8 | RNG bytes for auto-generated API key |
| `WIFI_PORTAL_SSID` | "DustGate-Setup" | Setup AP name |

---

## 12. Build Environment

- **PlatformIO** (preferred) — `platformio.ini` at project root
- **Arduino IDE** — also supported; install libraries manually
- Board: `adafruit_feather_esp32s2`
- Framework: Arduino
- Key libraries: TMCStepper, AccelStepper, ArduinoJson v6, AsyncTCP, ESPAsyncWebServer
- Build flag: `-DARDUINO_USB_CDC_ON_BOOT=1` (native USB CDC Serial)

---

*For wiring diagrams see `linear_actuator/WIRING.md`.*  
*Original seed requirements preserved in `requirements.txt`.*
