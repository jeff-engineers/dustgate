# DustGate

This project is a work in progress and is not considered complete or ready for use. Use at your own risk.
F
Automated dust collection manifold for a woodworking shop. A motorized rack-and-pinion linear actuator selects which blast gate is open based on which tool is running — no switches, no manual intervention.

Each tool plugs into a [Shelly smart outlet](https://us.shelly.com). When a tool draws power above a configurable wattage threshold, the actuator moves to that tool's blast gate automatically. When all tools are off, it returns to the home (closed) position. An AI-powered setup assistant walks you through configuration from a phone browser.

---

## Hardware

| Part | Source | Notes |
|------|--------|-------|
| Adafruit ESP32-S2 Feather | [Adafruit #5000](https://www.adafruit.com/product/5000) | Main controller |
| Adafruit TMC2209 Breakout | [Adafruit #6121](https://www.adafruit.com/product/6121) | Stepper driver |
| LDO-42STH48-2004MAH (NEMA 17) | Various | Stepper motor |
| Rack & pinion | Various | 20T rack, 15T pinion, 4.145mm pitch |
| NC mechanical limit switch | Various | Home endstop on D10 |
| Shelly Plug US (one per tool) | [us.shelly.com](https://us.shelly.com) | ~$21 each, Gen 4 recommended |
| 12–24V DC power supply (≥2A) | Various | Motor power |

For wiring details see [`linear_actuator/WIRING.md`](linear_actuator/WIRING.md).

---

## Shelly Smart Plug Setup

Do this before first boot of DustGate.

**1. Add each plug to your WiFi network**

Download the Shelly app (iOS / Android) and follow the in-app pairing flow for each plug. You only need to do this once per plug.

**2. Assign static IP addresses**

This is important — DustGate polls outlets by IP. If a plug gets a new IP from DHCP the mapping breaks.

In your router's admin panel, find the "DHCP reservations" or "static leases" section. Locate each Shelly by its MAC address (shown in the Shelly app under Device Info) and pin it to a fixed address, e.g.:

```
Bandsaw      → 192.168.1.101
Router Table → 192.168.1.102
Drill Press  → 192.168.1.103
```

**3. Confirm local control is enabled**

In the Shelly app go to each device → Settings → make sure "Local control" is on. It's on by default. Cloud access is not required.

**4. Verify reachability**

From any browser on your home network, visit:

```
http://<plug-ip>/rpc/Switch.GetStatus?id=0
```

You should get a JSON response containing `"apower": 0.0` (watts currently drawn). If you see that, the plug is ready.

> **Generation note:** Shelly Plug US Gen 4 is a Gen 2 device (uses the `/rpc/` API). When the DustGate setup assistant asks for the generation, answer **2**.

> **240V tools:** Plug-in Shelly outlets are 120V/15A only. Large table saws, planers, etc. cannot use this method — assign them a fixed gate or detect them separately.

---

## Software Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- [Node.js](https://nodejs.org/) 18+ and npm (for the web UI)
- An Anthropic API key (`sk-ant-...`) if you want the AI setup assistant

---

## Build & Flash

### 1. Clone / open the project

Open the project folder in VS Code with the PlatformIO extension installed.

### 2. Configure `config.h`

Open `linear_actuator/config.h`. At minimum:

```cpp
// Set the number of blast gates in your shop (1–7)
#define NUM_STOPS  4

// Enable smart outlet control and the HTTP API
#define CONTROL_SMART_OUTLET
#define ENABLE_HTTP_API
```

For developer / fixed-network builds you can hardcode WiFi credentials:

```cpp
#define WIFI_STA_SSID  "your-network-name"
#define WIFI_STA_PASS  "your-password"
```

Leave those commented out for end-user deployments (the setup portal handles it).

### 3. Flash the firmware

```bash
pio run --target upload
```

### 4. Build and upload the web UI

```bash
cd dustgate-ui
npm install          # first time only
bash deploy.sh       # builds Angular app, gzips assets, copies to linear_actuator/data/
cd ..
pio run --target uploadfs
```

---

## First Boot

1. **Power on the device.** Open a serial monitor (`pio device monitor`) to see boot output.

2. **Connect to the setup network.** If no WiFi credentials are stored, the ESP32 creates a hotspot:

   ```
   SSID:     DustGate-Setup
   Password: (none)
   ```

   Connect your phone or laptop to this network, then open **http://192.168.4.1** in a browser.

3. **Fill in the setup form:**
   - Your home WiFi SSID and password
   - Your Anthropic API key (optional — enables the AI setup assistant)

4. **Save & Connect.** The device reboots and joins your home network. The IP address is printed to serial:

   ```
   [WiFi] Connected. IP: 192.168.1.42
   [WiFi] Web UI:       http://192.168.1.42
   [WiFi] Setup assistant available at  http://192.168.1.42/#/setup
   ```

5. **Open the web UI** at the IP shown. You'll land on the dashboard.

---

## Setup Assistant

On first run, the dashboard will say "Not configured" — no tools have been mapped yet.

Tap the **⚙ gear icon** to open the setup assistant. It's a chat interface powered by Claude. It will:

1. Home the actuator to establish a reference position
2. Walk you through each blast gate position — jogging the actuator to align it, then asking what tool is connected there ("Bandsaw", "Router Table", etc. — whatever you call it)
3. Ask for the IP address of the Shelly outlet for each tool and confirm it's reachable
4. Save the configuration

When setup is complete, tap the back arrow to return to the dashboard. Your tool buttons will appear.

---

## Daily Use

- **Automatic mode:** just turn on a tool. DustGate detects power draw within ~1 second and moves the gate. Turn the tool off and the gate returns home after a 3-second coast-down delay.
- **Manual override:** tap any tool button on the dashboard to move the gate manually. Automatic mode resumes the next time a tool is detected.
- **HOME button:** closes all gates (moves to home position).
- **Dust collector toggle:** planned — currently a UI placeholder.

---

## Reconfiguration

To add, remove, or reassign outlets: tap the **⚙ gear icon** on the dashboard and chat with the setup assistant. Changes take effect immediately and are saved to flash.

To reset WiFi credentials (e.g. new router): type `wifireset` in the serial monitor, or use the `wifireset` command in the serial debug interface. The Anthropic API key is preserved across WiFi resets.

---

## Development

To work on the web UI against a live device:

1. Set the ESP32's IP in `dustgate-ui/proxy.conf.json` (change the `target` values).
2. Run the dev server:
   ```bash
   cd dustgate-ui
   npm start
   ```
3. Open http://localhost:4200 — API calls proxy to the real device.

---

## Project Structure

```
linear_actuator/         Firmware (Arduino / PlatformIO)
  config.h               All compile-time settings
  linear_actuator.ino    Main sketch + state machine
  api/                   HTTP REST + WebSocket server
  control/               Control input modes (serial, outlet, rotary)
  feedback/              Homing and position feedback
  motor/                 TMC2209 stepper driver
  outlets/               Shelly outlet polling
  training/              Calibration storage
  utils/                 WiFi provisioning, motion math
  data/                  LittleFS filesystem image (generated — don't edit)
  WIRING.md              Wiring reference

dustgate-ui/             Web UI (Angular 17)
  src/app/
    dashboard/           Operational view (tool buttons)
    setup/               AI setup chat
    services/            API + Claude services
  deploy.sh              Build → compress → copy to data/
  proxy.conf.json        Dev proxy (set ESP32 IP here)

platformio.ini           PlatformIO build config
REQUIREMENTS.md          Architecture decisions and spec
```

---

## Limitations & Known Issues

- HTTPS to the Anthropic API uses `setInsecure()` (no certificate validation). Acceptable for local network use; must be addressed before any cloud deployment.
- The dust collector relay output (pin A4) is implemented in firmware but the UI toggle is not yet wired to the API.
- 240V tools cannot use Shelly plug-in outlets.
- `NUM_STOPS` is set at compile time — changing the gate count requires a reflash.
