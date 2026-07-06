# Manual Setup Wizard — Requirements

**Route:** `/setup/manual`  
**Purpose:** Full gate-positioning and outlet configuration without AI assistance.  
**Scope:** Equivalent to the AI wizard — covers gate count, homing direction, gate positioning, and Shelly outlet assignment.

---

## Navigation & Entry

- A second button on the dashboard ("Manual Setup") opens `/setup/manual` alongside the existing "Setup Assistant" (AI) button.
- A back arrow on every screen returns to the previous step with a confirmation prompt if the step has unsaved changes.
- The manifold visualizer strip is pinned at the top of every screen (same as the AI wizard), showing placeholder text until gate count is set.

---

## Phase 1 — Initial Configuration

Three sequential screens. No device calls until Phase 2.

### 1.1 Gate Count
- Number stepper, range 1–16, no default pre-filled (user must set explicitly).
- Calls `POST /api/config/gates` on "Next" and updates the visualizer to show *N* gate columns.

### 1.2 Unit System
- Toggle: **mm** (default) / **inches**.
- Session-only — not persisted to the device or NVS.
- Selection propagates to all jog buttons and position readouts for the remainder of the wizard.

### 1.3 Home Side
- Two large tap targets: **Left** / **Right** (with a small diagram indicating which end has the endstop).
- Calls `POST /api/config/orientation` with `homeOnRight` immediately on tap.
- Visualizer updates orientation in real time.

---

## Phase 2 — Homing

### 2.1 Home Now
- Instruction: "The actuator will move until it hits the endstop. Keep hands clear."
- Single **Home** button → calls `POST /api/home`.
- Button disables and a spinner appears while `status.state === 'HOMING'`.

### 2.2 Direction Confirmation
- Shown automatically once status returns to `IDLE` after homing.
- Prompt: "Did the actuator move **toward** the endstop or **away** from it?"
- Two buttons:
  - **Toward — correct ✓** → proceed to Phase 3.
  - **Away — wrong ✗** → call `POST /api/config/motor` with `{ invertDirection: true }`, show "Direction corrected. Homing again…", auto-trigger a second home, then proceed once that completes.

---

## Phase 3 — Gate Positioning

One screen per gate (Gate 1 through Gate *N*), navigated in order.

### Jog Controls
- Two directional buttons (← toward home / → away from home) for each increment.
- Metric increments: **1 mm · 5 mm · 10 mm · 25 mm · 50 mm**
- Imperial increments: **1/16" · 1/4" · 1" · 2"**  
  *(imperial amounts are converted to mm before calling `POST /api/jog`)*
- Current position readout updates live from `status.positionSteps` (converted to the selected unit).

### Saving a Gate
- **"Save as Gate X"** button calls `POST /api/setstop` with `{ index: X }`.
- On success the visualizer highlights gate *X* and the wizard advances to gate *X+1*.

### Equal-Spacing Offer (gates 3+)
- Triggered once Gate 2 is saved, only if *N* ≥ 3.
- Card shows: "Gate spacing: **Ymm (Z")** — apply this interval to the remaining *N−2* gates?"
  - Spacing = `gate2_mm − gate1_mm`.
  - Projected positions: `gate1_mm + i × spacing` for gates 3 through *N*.
- **Apply** → actuator moves to each projected position in sequence; user reviews and may jog-trim before tapping "Save" on each gate.  
  *(Auto-saves are NOT done silently — each gate gets its own review screen.)*
- **Set manually** → continue with full jog UI for each remaining gate.

---

## Phase 4 — Outlet Configuration

One screen per gate (Gate 1 through Gate *N*), navigated in order.

### Per-Gate Form
| Field | Type | Notes |
|---|---|---|
| Tool name | Text input | e.g. "Bandsaw". Required to proceed. |
| Shelly generation | Toggle | Gen 1 / Gen 2 |
| IP address | Text input | Numeric keyboard hint; validated as IPv4 before ping. |
| Ping | Button | Calls `POST /api/outlets/ping`. Shows ✓ reachable + current watts, or ✗ error. |
| Wattage threshold | Number input | Default 5 W. Optional — leave blank to use default. |

- **"Save outlet"** → calls `POST /api/outlets/configure` (equivalent to the AI wizard's `configure_outlet` tool).
- **"Skip — no outlet"** → marks gate as physical-only, no Shelly assigned. Gate will never trigger automatic dust collection; user can still route manually from the dashboard.

---

## Phase 5 — Review & Save

### 5.1 Summary Screen
- Table listing each gate: position (mm or "), tool name (or *No outlet*), IP.
- Edit pencil per row → deep-links back to that gate's Phase 3 or Phase 4 screen.
- **"Save configuration"** → calls `POST /api/outlets/save` to persist outlet assignments to device flash.

### 5.2 Completion
- Success confirmation.
- **"Go to dashboard"** → navigates to `/`.

---

## Cross-Cutting Concerns

### State
- All wizard state is held in component memory (no `localStorage`).
- Gate positions are written to the device incrementally as each gate is saved (not batched).
- Outlet configs are staged in the component and flushed together in Phase 5.

### Visualizer
- Shows placeholder until gate count is set (same `isReady` gate as the AI wizard).
- Highlights the gate currently being positioned in Phase 3.
- Updates in real time via the WebSocket during jog and home operations.

### Error Handling
- Jog/move failures show an inline error with a retry button; the wizard does not advance.
- Ping failure shows an inline error; the user can edit the IP and retry without losing other form data.
- If the device disconnects mid-wizard, show a reconnecting overlay and resume when the WS reconnects.

### Back Navigation
- Phase 1 screens: back is free (no device state has been written yet).
- Phase 2+: back shows "Going back won't undo device changes — are you sure?" before returning.
- A **"Start over"** button (same as the AI wizard — resets device + clears wizard state) is accessible from the header on every screen.

---

## API Surface

All calls use the existing `ApiService` methods or trivial additions:

| Operation | Method |
|---|---|
| Set gate count | `api.setNumGates(n)` |
| Set home side | `api.setOrientation(homeOnRight)` |
| Invert motor direction | `api.setMotorDirection(invert)` |
| Home | `api.home()` |
| Jog | `api.jog(mm)` |
| Save stop position | `api.saveStop(index)` |
| Ping outlet | `api.pingOutlet(gen, ip)` |
| Configure outlet | `api.configureOutlet(cmd)` |
| Save outlet config | `api.saveOutletConfig()` |
| Watch status | `api.status$` (WebSocket) |
| Reset (start over) | `api.resetSetup()` + `api.refreshInfo()` |

No new firmware endpoints are required.

---

## Out of Scope (for this wizard)

- Autotune / StallGuard threshold calibration (legacy WiFiControl feature — no equivalent in the current firmware API).
- Editing previously saved stop positions after setup is complete (dashboard can handle re-running the wizard).
