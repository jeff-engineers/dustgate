---
name: flash
description: Flash DustGate firmware or UI to a real ESP32 at the bench, or run against real hardware — covers dev.sh flash/flash-node/provision/monitor/ports and the traps around them. Use when asked to flash, deploy to the device, provision WiFi, watch serial, or point the UI at real hardware.
---

# Flashing DustGate hardware

Everything goes through `dev.sh`. It's a deliberately thin wrapper over
PlatformIO/esptool — prefer it over raw commands, because it handles the board
identification and the topology backup that raw commands don't.

## Before you flash anything

**Confirm with the user first.** Flashing writes to physical hardware and can
erase the saved shop. Say which board and which mode you're about to use, and
wait for a yes.

Check what's attached and which role each board is pinned to:

```bash
bash dev.sh ports
```

`/dev/cu.*` paths are **not stable** — the same DevKitC has shown up as
`cu.usbserial-110` and `cu.usbserial-1110` in one afternoon, because macOS
derives the suffix from USB topology. Boards are identified by USB serial number
instead, pinned once into `.dustgate-ports`:

```bash
bash dev.sh ports --pin
```

If a flash targets the wrong board, that's the cause. Re-pin rather than
guessing at a path. One-shot override: `DUSTGATE_PORT=/dev/cu.xxx`.

## The trap: flashing the filesystem erases the shop

`topology.json` shares the LittleFS partition with the Angular bundle, so any
filesystem flash wipes the user's saved shop layout. `deploy.sh` copies it off
the device first and writes it back at the end — backups land in
`.dustgate-backups/`, restorable by hand with `bash tools/restore-topology.sh`.

This only works if you let `dev.sh` do it. **Never** flash the filesystem with a
raw `pio run -t uploadfs`. If a flash is interrupted mid-way, check
`.dustgate-backups/` before re-flashing — a second run can back up the
already-erased state over the good backup.

## Primary board

```bash
bash dev.sh flash                 # UI build + firmware + filesystem + provision
bash dev.sh flash --fw            # firmware only — skips the filesystem, so the shop is safe
bash dev.sh flash --ui            # UI + filesystem only
bash dev.sh flash --no-provision
bash dev.sh flash --no-topology-backup   # only if the user explicitly says so
```

Prefer `--fw` when the change is firmware-only. It's faster and can't touch the
saved shop.

WiFi/hostname come from `tools/.env`, overridable per-flash:

```bash
bash dev.sh flash --host shop --ssid Shop-WiFi   # prompts for the password, hidden
bash dev.sh flash --ask                          # prompt for all three, prefilled
```

Overrides apply to that flash only; `--save` writes them to `tools/.env`.
**Don't use `--pass SECRET`** — it lands in shell history. Use `--ssid` alone
and let it prompt, or `--ask`.

To change WiFi/key/hostname without reflashing:

```bash
bash dev.sh provision --host shop --ssid Shop-WiFi
```

## Secondary node

A node is a dumb actuator bank: up to four servo valves, no web UI, no stepper,
no plug polling. The primary sends it already-resolved angles.

```bash
bash dev.sh flash-node            # QT Py ESP32-S3 (default)
bash dev.sh flash-node c5 dustgate-node-c5
```

The C5 rides the pioarduino platform and builds against its own
`PLATFORMIO_CORE_DIR` (`~/.platformio-pioarduino`); `dev.sh` sets it via
`use_core_for_env`. The first build there downloads ~7.6 GB — warn the user
before starting one. Nothing in `~/.platformio` is touched. Its pin map in
`firmware/wiring/xiao-c5.md` is **unverified against hardware** — flag that when
flashing a C5.

(`dev.sh` itself still prints an older warning about "swapping the core in and
out". That text is stale; the mechanism is isolation.)

## After flashing

```bash
bash dev.sh monitor               # serial, primary
bash dev.sh monitor node          # or: monitor c5
bash dev.sh live [host]           # ng serve with hot reload, proxied to the real device
```

`dev.sh live` (default host `dustgate.local`) is the fast loop for UI work
against real hardware — no reflash per change.

To certify a real device against the contract instead of the mock:

```bash
node shared/device-model/conformance.js http://<device-ip> <api-key> --force
node shared/device-model/nodelink-conformance.js ws://<node-ip>/nodelink
```

## When it's weird

A corrupted partition table produces symptoms that look like firmware bugs —
boot loops, mounting failures, a device that flashes clean and does nothing:

```bash
bash dev.sh erase                 # full chip erase, then re-flash
```

This erases the shop too. Confirm with the user and check the backup first.
