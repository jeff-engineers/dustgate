# UI review — quality of life and consistency, 2026-11-23

A discovery pass over `dustgate-ui/`, done by driving the app at phone width (375×812, the browser pane's mobile
emulation) against `tools/mock-api.js` and against the in-app demo (`?demo`), plus reading the templates. No app code
changed. Written against `fb6cb1b`.

Findings are grouped by zone and **ordered by what they cost a woodworker using the app**, worst first, with
`[C]`/`[H]`/`[M]`/`[L]` tags (`[C]` here means the app is unusable or says something false at the moment it matters).
Work it with the `burndown` skill: one commit per fix, tick the finding off in the same commit, keep the numbering.

**Out of scope, on purpose:**
- **Saves that are refused.** A parallel session is making a save never blocked (warn instead): the canvas `save()` and
  import checks, the demo brain's `putTopology`, the mock's 400, and the raw validator text in `shop-ready.ts` /
  Live's `checkReady()`. Findings below that touch those places say so and stop there.
- Decided design constraints (root `CLAUDE.md`) and settled register entries are not relitigated. Where a finding
  rubs against one (D-72's "Devices" tray, D-84's "the brain's own reason"), it names it and asks.

**What emulation could not settle:** iOS long-press, real focus styles, and real finger sizes. Tap-target sizes
below were measured in the DOM, not tried on a phone.

**One thing seen while testing, not an app bug:** something PUT a different layout (shop "star") into the mock on
`:3000` mid-session. Two sessions sharing one mock port will keep doing that.

Zones:
1. [Words a person reads](#1-words-a-person-reads)
2. [Failures, empty and offline states](#2-failures-empty-and-offline-states)
3. [The same thing done the same way](#3-the-same-thing-done-the-same-way)
4. [Dead ends and stranded edits](#4-dead-ends-and-stranded-edits)
5. [Phone use](#5-phone-use)
6. [The demo](#6-the-demo)

## 1. Words a person reads

### 1.1 [H] Validator JSON shown raw on Boards
**Where:** Boards (`boards/board-setup.component.ts:990` `message()` → `failureReason()`); seen in the demo on open.
**What:** Opening Boards in the demo printed `That didn't go through — invalid topology: [{"code":"controller",
"message":"system \"s2\": board \"ShopVac node\" is set up as a servo board…","ref":"dustgate-slider-1"}].` D-84 passes
"the brain's own reason" through, and when that reason is a validator array the person gets JSON.
**Fix:** In `services/failure.ts`, turn a body that carries a validator `errors`/issues array into the same woodworker
sentence the canvas guide bar uses (`wip-message.ts`), with the raw body going to the console only. One place fixes
every screen.

### 1.2 [H] "HTTP 500" and "refused" in every screen's failure line
**Where:** `services/failure.ts:55` and `:64` (`refused it (HTTP ${status})`); seen on Tools, Gates, Plugs and the canvas
with the brain stopped.
**What:** With the brain down the dev proxy answers 500, and every screen said "the shop brain refused it (HTTP 500)":
a code, and the wrong story. Nothing refused anything; nothing answered. The same text appears in production for any
5xx without a body.
**Fix:** Drop the status code from the sentence (console only), and treat a 5xx with no body, a 502/503/504 and a proxy
error as "didn't answer", per D-84.

### 1.3 [H] Plugs are named by hostname and MAC, and their addresses are on screen
**Where:** canvas tray chips (`build/build.component.html:479`, `o.name || o.hostname`); the plug picker
(`tools/outlet-picker.component.ts:79`, `:152` — every row's second line is `hostname · IP`); Tools list pills
("ShellyPlugUSG4-0F0E99229E66 · above 50 W", "shellyplugus-tablesaw"); Plugs rows (`Shelly · 192.168.87.30 · pushes
to this brain`); the drag ghost (`html:564`); the chip rename sheet header (`html:644`).
**What:** An unnamed plug reads as `ShellyPlugUSG4-84E6D7047170` — a MAC — in the tray, the picker and the Tools list.
Every picker row and every Plugs row also prints its IP. The rule is no IPs or MACs in what a person reads.
**Fix:** One display-name helper for a plug (its own name, else the tool it is on, else "Unnamed plug" plus its live
draw), used by every screen; IP, hostname, kind and "pushes to this brain" go behind developer mode. Typing an
address stays as the fallback (that is an input, not a display).

### 1.4 [H] Boards rows carry chip names, IPs, hostnames twice and firmware strings
**Where:** `boards/board-setup.component.ts:329` (scan rows: `qtpy_s3 · 192.168.87.61 · 4 servo channels`);
`subtitle()` at `:835-848` (`servo valves · dustgate-node-1 · 0 of 2 gates · fw 1.0.0-mock`).
**What:** A freshly paired board is called `dustgate-node-1`, and its subtitle repeats `dustgate-node-1`. The scan
list shows the build target (`devkitc`, `xiao_c5`) and the IP, which the comment at `:841` says is a developer's word.
"servo valves" is a third name for gates.
**Fix:** Scan rows: hostname as the name (it is what the board's own screen shows) and nothing else but what it
drives. Paired rows: drop the host when the name is still the host, move `fw …` and the host to developer mode, say
"gates" not "servo valves".

### 1.5 [M] The brain has five names
**Where:** Live "Can't reach the **controller**" (`live/live.component.ts:391`); landing "No answer from the
**controller**" (`landing/landing.component.ts:82`); canvas/Tools/Gates "the **shop brain**"; Settings and log "the
**brain**"; Boards "**This board** · primary — runs the app"; canvas glyph "**Shop Brain**" (default name,
`build.component.ts:5858`) with role "PRIMARY".
**What:** One thing, five words, and "controller" is not in the Glossary. On a Pi the brain is not a board at all,
so "This board" is wrong there.
**Fix:** "the brain" everywhere a person reads it (Glossary), and the Boards row for an ESP32 primary named by its
board name with "runs the app" as its subtitle.

### 1.6 [M] "Device" for the sensing setup on a tool
**Where:** canvas tool menu `Set up this device` / `Device` (`build/build.component.ts:2833`); the sheet header
`Tool: New tool — Device setup` (`tools/element-outlet-config.component.ts:76`); the tray title `Devices`
(`build.component.html:470`); `servo-calibration.component.ts:450` "This device doesn't have the servo jog endpoint
yet"; the build stamp's `device …` (`app.component.ts:133`).
**What:** Jeff: a tool is a tool, never a device. "Set up this device" on a tool's menu reads as calling the tool a
device. The tray title was decided in D-72 (a clamp is not an outlet) — worth asking whether "Plugs & clamps" keeps
D-72's point without the word.
**Fix:** Menu: "How it's sensed" (or "Plug or clamp"); sheet header: "Tool: Table Saw — how DustGate sees it run";
tray: "Plugs & clamps" if Jeff agrees (D-72); jog error: "This board's firmware is too old to move a gate from here —
update it from Boards"; stamp: "brain".

### 1.7 [M] Plug and outlet used for the same thing on the same sheet
**Where:** canvas tool sheet: question "Is it on a smart **plug**?", badge "No **outlet**", picker "Which **outlet** is
this one's?" (`element-outlet-config.component.ts:76-104`, `outlet-picker.component.ts:69`); Live "pair an **outlet** to
automate" (`live.component.ts:885`), "**Outlet** offline" (`:792`); chip sheet "**Outlet** name" (`html:645`); the page
is called **Plugs**.
**What:** D-83 allows "outlet" for the plug, but one sheet using both words for one plug reads as two things.
**Fix:** Pick "plug" (the page's name) for everything a person reads, keeping "outlet" out of the UI entirely; code
identifiers keep their names, as D-83 did.

### 1.8 [M] Four phrasings of "no plug, you switch it yourself"
**Where:** Live "Manual · pair an outlet to automate"; Tools "Switched on by hand" / "No outlet paired" / "No outlet
paired yet"; canvas menu note "switched by hand"; sheet "No, I switch it on myself"; collector "started by hand".
**Fix:** One phrase for the state ("Switched on by hand") and one for the fix ("Pair a plug"), shared from a
constant like `pairHint`.

### 1.9 [L] "pipe" in the guide bar, "Duct" everywhere else; "click" on a phone
**Where:** `build/build.component.ts:5365`, `:5370`, `:5375`.
**What:** "drag it to run more pipe", "click a dot on a run to branch" — the menu one tap later says **Duct**, and
this is a phone app.
**Fix:** "duct" and "tap".

### 1.10 [L] Board glyph says PRIMARY / SECONDARY
**Where:** `build/build.component.html:346`.
**What:** Developer words on the canvas; Boards says neither.
**Fix:** Drop SECONDARY (a board is a board); show "BRAIN" on an ESP32 primary; keep NOT PAIRED.

### 1.11 [L] Smaller jargon
- Boards clamp line "idle, 0.3x trip" — say "idle" alone; the ratio goes to developer mode.
- Settings "Keeps WiFi, the app key and gate calibration" — "app key" means nothing to a woodworker; "Keeps WiFi and
  gate calibration".
- Tray tooltip "a few seconds on the Pi, about a minute on an ESP32 brain" (`html:547`) — "a minute or so".
- Settings "Shop **L**ayout →" (`settings.component.ts:207`, `:475`) — "Shop layout", as everywhere else.

## 2. Failures, empty and offline states

### 2.1 [H] A failed load shows the empty state as well as the error
**Where:** Tools (`tools/tool-setup.component.ts:297` — `!anythingToSetUp` with no error check); Gates
(`gates/gate-list.component.ts:158`); Plugs (`plugs/plugs.component.ts:140`).
**What:** With the brain stopped: "No tools to set up yet. Draw your plumbing first." above "Couldn't load your
tools…"; same on Gates and Plugs ("No plugs are paired to a tool yet"). D-84: a list that failed to load is not an
empty list — and "draw your plumbing first" sends someone to redraw a shop that exists.
**Fix:** Gate each empty state on "loaded and no error", and say "Can't reach the brain — showing nothing until it
answers. Trying again…" instead.

### 2.2 [H] Boards says nothing when the brain is down
**Where:** `boards/board-setup.component.ts` — `refreshLinks()` failures go to `QuietPoll` only (`:858-866`).
**What:** With the brain stopped, Boards still showed "This board" with its green dot and no message. The screen a
person opens to ask "is it connected?" says yes.
**Fix:** After two failed polls, grey every dot and show the same "Can't reach the brain" bar Live has.

### 2.3 [M] Settings waits forever for a brain that is not there
**Where:** `settings/settings.component.ts:400` ("Reading the shop's setting…"), `:373`.
**What:** With the brain stopped the fail-closed field sits empty under "Reading the shop's setting…" indefinitely,
and its Save is live.
**Fix:** On failure say "Can't reach the brain"; disable that Save with the reason beside it.

### 2.4 [M] A canvas that could not load still draws a starter shop
**Where:** `build/build.component.ts:5259` and the seed at `:5858`.
**What:** "Couldn't load your layout" sits above a drawn collector, a "Shop Brain" board and a tray saying "nothing
answered". Saving is off (D-84), but the drawing looks like your shop was wiped.
**Fix:** Draw nothing but the message and Try again while `loadFailed` is set.

### 2.5 [M] Live said "No shop configured yet" over a saved layout
**Where:** `live/live.component.ts:580-586` (the `noShop` template keys on `tools.length`).
**What:** Seen once against the mock right after a layout was saved: "No shop configured yet — Set up your shop →".
It cleared on reload. A shop whose tools have not loaded YET reads as a shop that does not exist, and the button
offers to start over.
**Fix:** A distinct loading state ("Reading your shop…") until the first `getTopology()` answers; "No shop configured
yet" only on a 404.

### 2.6 [L] Every problem on Live links to the brain log first
**Where:** `live/live.component.ts` problems block (`routerLink="/boards/log"` on every row).
**What:** The serial log is a bench screen; for a woodworker the useful link is Boards or Plugs, which only some codes
get.
**Fix:** Show the log link only in developer mode; give every problem a "what to open" link.

## 3. The same thing done the same way

### 3.1 [H] Canvas and Boards disagree about what a board drives
**Where:** canvas glyph count `portUsedCount/portCapacity` (`build/build.component.ts:4129`) vs Boards `subtitle()`
(`boards/board-setup.component.ts:843`); scan rows `{{ n.servos }} servo channels` (`:329`).
**What:** Against the mock the same brain read **0/6** on the canvas (bus) and **"servo valves · 0 of 2 gates"** on
Boards; the bus board `dustgate-wall` read "servo valves · 0 of 2 gates" on Boards though it reported a bus of 6; and
a board offered as "4 servo channels" in the scan becomes "0 of 2 gates" once paired. Three numbers for one board's
room.
**Fix:** One `drivesOf()`/capacity function shared by both screens (`board-drives.ts` is the natural home), and the
scan row says what the board will hold in the same words ("2 gates" / "6 bus servos").

### 3.2 [H] Four ways to confirm something destructive
**Where:** native `window.confirm` — Unpair (`board-setup.component.ts:539`, `build.component.ts:3951`), Take over
(`board-setup:517`), Update (`:774`, `:823`), Leave without saving (`build.component.ts:5465`), Import (`:5687`), discard
tool edits (`tool-setup.component.ts:842`); in-app sheet — Clear shop and delete-a-system (`removalAsk`,
`build.component.html:611`); inline panel — Plugs Release / Take over… (`plugs.component.ts:143-149`); tap-again
button — Settings resets (`settings.component.ts:292-323`).
**What:** Taking over a board is a browser dialog; taking over a plug is an inline panel; resetting is "Tap again".
Native dialogs on a phone also show the page's address as the title.
**Fix:** One in-app confirm sheet (title, one sentence of consequence, a red verb button, Cancel), used everywhere —
the canvas `removalAsk` sheet is already the right shape.

### 3.3 [M] Back and navigation differ on every page
**Where:** Tools and Gates: round ← plus a title; Boards and Plugs: small "Your boards"/"YOUR PLUGS" captions, no back,
a full-width "Shop layout →" at the bottom; Add a servo and the servo page: "← Your boards" text; Brain log: "‹ Boards";
canvas: "Done" to Live.
**What:** Every screen is reached and left differently. Live's nav reaches Shop layout, Tools, Gates, Boards, Settings
but not **Plugs**; the canvas menu reaches Find boards, Plugs, Settings but not Tools or Gates; Settings reaches Shop
Layout and the log only.
**Fix:** One page header (← + title) on every non-canvas screen, back going to where you came from; the same five
or six destinations from Live, the canvas menu and Settings, Plugs included.

### 3.4 [M] Gates are listed by kind, so four rows read "Ball valve"
**Where:** Gates list (`gates/gate-list.component.ts:128-129`, `name: sel.name || sel.id`, sub `kind · where`).
**What:** In the demo every row's title is "Ball valve" and its subtitle starts "Ball valve · …". To recalibrate the
table saw's gate you read the board name in the small print. Add a servo shows `g.name || g.id` too.
**Fix:** Title a gate by the tools it serves ("Table Saw", "Router Table & Jointer") when its name is empty or equals
its kind; the kind goes in the subtitle once.

### 3.5 [M] Removing a plug from a tool is three verbs
**Where:** Tools/canvas sheet "Remove outlet" / "Remove" (`tools/paired-outlet-row.component.ts:116`, `:155`); Plugs
"Release" (`plugs.component.ts:131`); Boards "Unpair" for the board equivalent.
**What:** Same act — this tool stops using this plug, and the plug is handed back — named differently per screen.
**Fix:** "Release" for a plug everywhere (the Plugs page's word), "Unpair" stays for boards (D-64).

### 3.6 [M] Gate setup is five verbs
**Where:** Gates "Recalibrate" / "Run setup again" (slider) / "Test"; servo page "Set positions" / "Test move"; Add a
servo "Set its positions"; Settings "Reset gate calibration — Start over" / "Re-run setup afterward".
**Fix:** "Set positions" and "Test" everywhere; Settings: "Forget every gate's positions".

### 3.7 [M] Two looks for "Look for Tasmota plugs"
**Where:** canvas tray "Look for Tasmota plugs" / "Look for new" (`build.component.html:548`); Plugs "Sweep network".
**Fix:** One label ("Look for more plugs") and one tooltip.

### 3.8 [M] The same plug is named differently on Tools and Plugs
**Where:** demo: Tools shows the Table Saw's plug as "shellyplugus-tablesaw", Plugs shows it as "Table Saw".
**Fix:** Falls out of 1.3's one display-name helper.

### 3.9 [L] Disabled Save looks like enabled Save
**Where:** canvas header Save (`button.primary:disabled`).
**What:** At phone size a disabled Save is a slightly dimmer gold; "is it saved?" is answered by squinting.
**Fix:** Disabled Save goes outlined grey with the word "Saved"; dirty Save is the solid accent.

### 3.10 [L] Boards and the canvas pair a board without asking its name
**Where:** `board-setup.component.ts:511` `add()`.
**What:** Every new board is called `dustgate-node-N` until someone finds Rename; the canvas then shows that host.
**Fix:** After Add, open the rename field in place, prefilled, with "e.g. Back wall".

## 4. Dead ends and stranded edits

### 4.1 [H] Tapping outside a sheet throws away what you entered
**Where:** every canvas sheet backdrop — `build.component.html:572`, `:585`, `:611`, `:638`, and the outlet/collector
sheet (`(pointerdown)="… = null"`).
**What:** Pick a plug, set the threshold, then brush the dimmed canvas above the sheet: the sheet closes, nothing
saved, no question. On a phone the backdrop is most of the screen.
**Fix:** Backdrop closes only an untouched sheet; a touched one asks (with the 3.2 sheet) or ignores the tap.

### 4.2 [M] Done leaves without saving
**Where:** `build.component.html:24` (`routerLink="/shop"`), guard `canLeave()` at `build.component.ts:5463`.
**What:** "Done" with unsaved changes gives a browser dialog with OK/Cancel to *lose* them; there is no "Save and
leave". Most people expect Done to keep what they did.
**Fix:** Done saves and then leaves (once saves never block, this is safe); the leave prompt offers Save / Discard /
Keep editing.

### 4.3 [M] Paired boards don't appear on the canvas until a layout is saved
**Where:** `board-setup.component.ts:528` `syncLayoutControllers()` is a no-op with no saved layout.
**What:** Pair three boards on a fresh shop, tap "Shop layout →": only the brain is drawn. They appear after the first
Save. Someone pairing first (the order the Boards header comment says is fine) thinks pairing failed.
**Fix:** The canvas draws every paired board from `/api/nodes` even before a save, the way it draws the default
collector.

### 4.4 [M] Disabled controls with no reason
- Servo page: Test move / Set positions / Replace are disabled when no gate holds the servo (`!sel`,
  `boards/servo-page.component.ts`) with nothing saying why — say "Give this servo to a gate first" and link Add a servo.
- Canvas tray "Match" (`build.component.html:516`, `canMatch()`) is disabled with no hint; it also gives no clue what
  it matches — label "Match by name" and a title when disabled ("No plug name matches a tool").
- Boards "Update" disabled during Update all — fine, it says so in the banner.

### 4.5 [L] Live lists a tool "Collecting" while nothing can start the collector
**Where:** Live, mock layout with a collector that has no plug: Bandsaw "Collecting", collector "Manual · pair an
outlet…".
**What:** The gate opened, but no air is moving unless someone switches the blower on. "Collecting" says it is.
**Fix:** With a manual collector that is off, the tool chip says "Gate open — switch the collector on".

### 4.6 [L] A tool sheet's Save also saves every other canvas edit
**Where:** `onConfigured()` / `onOutletConfigured()` call `save()` (`build.component.ts:1917-1928`).
**What:** Fine in practice, but a half-drawn run is committed by a Save that looked like it was for one plug.
**Fix:** Say so on the sheet button ("Save (and the layout)") or save the sheet into the draft only and leave the header
Save for the layout.

## 5. Phone use

### 5.1 [H] The canvas page scrolls sideways at 375 px
**Where:** the Tasmota sweep row (`build.component.html:545-549`): its button does not wrap or shrink.
**What:** `app-build` is 388 px wide in a 373 px host, so the whole page slides 15 px: the title, the tray's "Devices"
and the menu button are clipped at the left edge as soon as anything scrolls.
**Fix:** Let `.sweep` wrap (or the label shrink to "Look for more"), and give `app-build` `overflow-x: clip`.

### 5.2 [H] The canvas tool sheet's yes/no overflows at phone width
**Where:** `tools/element-outlet-config.component.ts` `.q` / `.yesno` styles.
**What:** At 375 px "Is it on a smart plug?" wraps one word per line and "No, I switch it on myself" runs off the
sheet's right edge, half hidden.
**Fix:** Stack the question above the two buttons, and let the buttons wrap.

### 5.3 [M] The piece badge is a 17 px target at the phone's starting zoom
**Where:** `build.component.html:283-284` (hit `r=14`, drawn `r=7`, scaled with the canvas).
**What:** The badge is the only tap that opens a tool's or gate's menu (a tap on the body only selects it). At the
60 % zoom a phone opens at, its hit area measures 17 × 17 px; the collector's badge sat half off the left edge.
**Fix:** Size the hit circle in screen pixels (divide by zoom, min 44 px), or let a tap on a selected piece open its
menu.

### 5.4 [M] Boards and Plugs buttons are 31 px tall, Rename beside Unpair
**Where:** `board-setup.component.ts` `button.act` (padding 7 px 12 px), the same on Plugs.
**Fix:** 44 px minimum height and a wider gap before Unpair/Release.

### 5.5 [L] The rightmost board on the canvas is cut off at the starting zoom
**Where:** canvas fit-to-width on first open.
**What:** Against the mock, the third board drew past the right edge at 60 %.
**Fix:** Include boards in the initial fit (`vp.reveal`), as pieces are.

## 6. The demo

### 6.1 [C] The demo shop fails validation, so its Live view is dead
**Where:** `services/demo-topology.ts:139` (`drives: "linear"`, a value removed by the bus-chain work); seen at
`/?demo#/shop`.
**What:** The public demo opens on "Shop layout incomplete — Dust collection: bad drives "linear" (servo|bus) (and 1
more) Nothing can be switched on until it's sorted out." — raw validator text, and nothing on the Live list can be
tapped. The canvas meanwhile says "1 sliding gate needs a bus board and this shop has none", and Boards dumps JSON
(1.1). Nothing in the UI suites validates the demo seed.
**Fix:** Make the ShopVac board a bus board in the seed, and add a spec that runs `validateShop` over
`demo-topology.ts` so the seed cannot drift again. (The raw text on Live is the parallel session's `shop-ready.ts`
item.)

### 6.2 [L] Demo data that confuses
**Where:** `demo-topology.ts`.
**What:** The Miter Saw's plug is named "shellyplugus-bandsaw"; boards are named after gates ("Planer gate", "Table
saw gate") so the Gates list reads "Ball valve · Planer gate · channel 1".
**Fix:** Name demo plugs after their tools and boards after where they hang ("Planer wall", "Back wall").
