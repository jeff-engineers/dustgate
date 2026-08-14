# DustGate — Requirements & Architecture

Motorized blast gate manifold controller. A rack-and-pinion linear actuator selects which dust
collection port is open based on which shop tool is in use. The shop is laid out once from a
browser on the local network, and the controller routes from that layout.

---

## 1. Hardware (decided)

| Component | Selection | Notes |
|-----------|-----------|-------|
| MCU | Adafruit ESP32-S2 Feather (#5000) | Replaced STM32 — better WiFi, larger ecosystem |
| Stepper driver | Adafruit TMC2209 Breakout (#6121) | UART current control |
| Motor | LDO-42STH48-2004MAH (NEMA 17) | 1.8° step, 2A, matched to TMC2209 |
| Drive | 15-tooth pinion + 20T/4.145mm pitch rack | ~51.47 steps/mm at 16× microstep |
| Smart outlets | Shelly Plug US Gen 4 (~$21 ea., us.shelly.com) | Fully local REST API, 1800W/15A, no cloud required |
| Home endstop | NC mechanical limit switch on D10 | Fail-safe: open wire reads as triggered |
| Far endstop | NC mechanical limit switch on D11 (required) | Over-travel safety + reference for self-calibration; wired identically (NC, HIGH = triggered) |


### Planned carrier PCB (Task 5)
- ESP32-S2 Feather + BTT TMC2209 StepStick on 2.54mm headers
- Screw terminals for motor, both endstops
- CNC-millable FR4 (no soldermask required)
- Replaces current breadboard/breakout assembly

---

## 2. Motion System

- **Rack-and-pinion** linear actuator, up to `NUM_STOPS` selectable stop positions (compile-time max, currently 16); the runtime-active count (≤ max) is separately configurable via `/api/config/gates` or Settings without recompiling
- **Stop 0** = home/disabled position
- **Homing:** drive toward the near NC limit switch at `HOMING_SPEED_STEPS_PER_SEC`, back off `HOME_BACKOFF_STEPS` after trigger, zero position
- **Dual-endstop self-calibration:** a **reference sweep** (near endstop → far endstop) measures the step span, derives `steps/mm` empirically per unit, and places gates by **proportion of the measured span** — immune to per-unit mechanical variance. For a known manifold (Rockler 2.5") gate positions are computed from a stored profile; `custom` falls back to manual jog. Also provides over-travel safety, lost-step detection, and auto motor-direction detection. See [`docs/dual-endstop-calibration.md`](docs/dual-endstop-calibration.md). *(Model + mock + conformance implemented; firmware foundation done, sweep motion pending hardware.)*
- **Positioning:** step-counted moves from home; stop distances from the sweep, manual calibration, or geometry constants
- **Port roles:** each gate has a role — `tool | unassigned | blocked` (`feed` reserved for v2). Blocked ports are never move targets; this lets the actuator become a node in the v2 topology graph (see [`docs/architecture-rfc.md`](docs/architecture-rfc.md))
- **Stop distance storage:** EEPROM/NVS via `Preferences`; `CalibrationData` v2 also stores per-port roles, manifold model, and measured span; `clearcal` erases stale calibration
- **Measured geometry (2.5"):** symmetric — trigger-to-trigger span = 84.9mm at 2 gates, gate-to-gate pitch = 82.9mm → trigger→gate offset = 1mm/side. Backoff (`HOME_BACKOFF_STEPS` ≈ 1mm) cancels out of the pitch but must be added back to the home→far sweep count when deriving steps/mm (these feed the manifold profile / proportional placement)
- **No enable/disable concept:** the system always runs; only e-stop (software-only — no physical e-stop button) halts motion. `home` re-homes and clears any latched e-stop; the system warns once if position commands arrive before homing.
- **Idle power-off:** if no move/home command arrives for `IDLE_TIMEOUT_SEC_DEFAULT` (3600s default, runtime-configurable via `POST /api/config/idle-timeout` or the Settings screen), the stepper driver is fully disabled and the position marked unknown, forcing a rehome on the next move instead of sitting energized indefinitely.

---

## 3. Control Modes

Exactly one control mode is active at compile time (`config.h`). The HTTP API runs *alongside* any control mode when `ENABLE_HTTP_API` is defined.

### 3a. Serial Debug (`CONTROL_SERIAL_DEBUG`)
- Human-readable serial commands: `home`, `1`–`7`, `jog <mm>`, `estop`, `status`, `clearcal`, `wifireset`, `help`

### 3b. Smart Outlet (`CONTROL_SMART_OUTLET`)
- See Section 5 below

### 3c. HTTP API (`ENABLE_HTTP_API`)
- See Section 6 below

---

## 4. WiFi Provisioning

- On first boot (no stored credentials): ESP32 starts AP `DustGate-Setup`, serves captive portal at `http://192.168.4.1`
- Portal collects: WiFi SSID, WiFi password
- Credentials stored in NVS namespace `wifi_creds` (keys: `ssid`, `pass`, `host`)
- `wifireset` serial command: erases WiFi credentials, reboots into portal
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
- `GET /api/outlets/discover` scans mDNS for `_http._tcp` services, filters to hostnames containing "shelly", and probes each match (Gen2 first, then Gen1) for reachability/power/generation — see `firmware/utils/MdnsQuery.h`
- Retries the mDNS query a few times (`DISCOVER_MDNS_ATTEMPTS`) and merges by IP, since UDP responses are lossy
- Also fetches the outlet's own app-assigned name when available — Gen1 via `/settings`, Gen2 via `Switch.GetConfig?id=0` (falling back to `Sys.GetConfig`) — see `firmware/outlets/ShellyDeviceName.h`
- Lets the Tools screen's "Scan for outlets" list replace manual IP entry in most cases; manual entry remains as a fallback
- **Must run on the main loop task**, not a spawned FreeRTOS task — ESP32's mDNS responder isn't safe to call concurrently with its own hostname-advertising; doing so from a separate task previously corrupted the heap and crashed the device
- The mDNS hostname is persisted alongside the IP (`o<N>_host` in NVS) so an outlet can re-resolve its IP after a DHCP lease change instead of going silently unreachable

### Per-outlet configuration (NVS)
- Stored in namespace `outlets`: generation, IP, mDNS hostname, name, stop index, threshold watts
- Managed during setup via `OutletConfig` namespace + `SmartOutletControl::configureOutlet()` / `saveAll()`

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
| GET | `/api/motion` | — | Full system status JSON |
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
| POST | `/api/dustcollector/switch` | `{"on": bool}` | Manual on/off |
| POST | `/api/config/orientation` | `{"homeOnRight": bool}` | Persist visual orientation |
| POST | `/api/config/motor` | `{"invertDirection": bool}` | Flip homing direction |
| POST | `/api/config/gates` | `{"numGates": N}` | Set active gate count |
| POST | `/api/config/idle-timeout` | `{"seconds": N}` | Set idle power-off timeout (0 = never) |
| POST | `/api/wifi/reset` | — | Erase WiFi credentials, reboot into setup portal |

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

## 7. Angular Front-End

Source lives in `dustgate-ui/`. Served from ESP32 LittleFS flash.

### Views (hash routing via `withHashLocation()` — no server-side redirects needed)

| Route | Description |
|-------|-------------|
| `/#/` | **Landing** — reads the stored layout and forwards: finished shop → `/shop`, unfinished → `/build` |
| `/#/shop` | **Live view** — the daily driver. A list of tools showing which one is collecting; every tool is manually overridable |
| `/#/build` | **Build canvas** — the setup. Place the collector, run duct, attach gates and tools. Gate badges open calibration in place |
| `/#/tools` | **Tools** — tag each tool with its smart outlet (identify-by-power) and threshold, or mark it manual |
| `/#/boards` | **Boards** — discover and pair secondary nodes, assign gates to them |
| `/#/settings` | **Settings** — idle power-off timeout, home orientation, motor direction, gate count, port size (client-side only), forget-WiFi, reset-calibration |

- Tool names come from the topology; live state comes from the status endpoint
- Dust collector toggle drives a Shelly smart plug via `/api/dustcollector/switch`

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
# and copies everything to ../firmware/data/

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
| `firmware/data/` | LittleFS image content (generated by deploy.sh) |

---

## 8. Output — Dust Collector

- Controlled by a dedicated switchable Shelly smart plug over WiFi (no local wiring)
- Turns on automatically when the actuator is at a non-home stop (a tool is running) and off at home
- Can also be toggled manually from the Live view
- Configured via `PUT /api/dustcollector` with `{"gen":2,"ip":"192.168.1.x"}`

---

## 9. Implementation Status

| Task | Status | Notes |
|------|--------|-------|
| Motion system (homing, multi-stop, jog) | ✅ Done | Tested with clearcal fix |
| Serial debug control | ✅ Done | `help`, `home`, `1–7`, `jog`, `estop`, `status`, `wifireset` |
| Shelly outlet polling | ✅ Done | Gen1 + Gen2, debounce, FreeRTOS task |
| WiFi provisioning (captive portal) | ✅ Done | Collects WiFi creds + hostname |
| HTTP REST + WebSocket API | ✅ Done | Auth, WS fingerprint push, non-blocking ping |
| `setManualOverride()` on SmartOutletControl | ✅ Done | Edge-triggered — clears only on a fresh OFF→ON transition, not just "still on" |
| Wire `HttpApiServer` into `firmware.ino` | ✅ Done | estop/home/move/jog/clearcal/outlets all wired |
| Angular front-end | ✅ Done | Live view, Build canvas, Tools, Boards, Settings — served from LittleFS |
| LittleFS static serving + `/api/info` | ✅ Done | Auto-serves .gz, bootstrap key endpoint |
| Dust collector (Shelly plug) | ✅ Done | Auto + manual toggle via `/api/dustcollector`; scan-first discovery |
| Settings screen | ✅ Done | `/#/settings` — idle timeout, orientation, motor direction, gate count, port size, forget-WiFi, reset-calibration |
| Outlet mDNS discovery | ✅ Done | "Scan for outlets" — replaces manual IP entry as the primary path |
| Idle power-off | ✅ Done | Driver disables after inactivity; forces rehome on next use |
| Physical e-stop button | ❌ Removed | Hardware deemed too low-power to need one; software e-stop only |
| Carrier PCB (KiCad) | ⏳ Planned | Task 5 |

---

## 10. Key Config Parameters (`config.h`)

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

## 11. Build Environment

- **PlatformIO** (preferred) — `platformio.ini` at project root
- **Arduino IDE** — also supported; install libraries manually
- Board: `adafruit_feather_esp32s2`
- Framework: Arduino
- Key libraries: TMCStepper, AccelStepper, ArduinoJson v6, AsyncTCP, ESPAsyncWebServer
- Build flag: `-DARDUINO_USB_CDC_ON_BOOT=1` (native USB CDC Serial)

---

*For wiring diagrams see `firmware/WIRING.md` and the per-board files in `firmware/wiring/`.*  
*Original seed requirements preserved in `requirements.txt`.*
