import { Component, OnInit, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { RouterLink } from '@angular/router';
import { ApiService, Topology, TopologyStatus } from '../services/api.service';
import { airflowIssues } from '@topology';
import { collectorPlugState } from '@topology-device';
import { validateShop } from '@shop';
import { configurableSelectorsOf, isCalibrated } from '../gates/selector-types';
import { type ShopDoc, machinesOf, portsOf, primaryPortOf, systemViews,
         systemsInLayoutOrder, toShop } from '../services/shop-doc';

/** What a tool row's chip says. One axis: is air reaching this tool? */
type ToolChip = 'collecting' | 'nosuction' | 'waiting' | 'nogate' | 'idle';

// One tool row's static identity (from the topology) merged with its live state
// (from /api/status). `collecting` is the routing winner — the single tool
// with a clear open path to the collector right now — as opposed to `on`, which
// only means it's drawing / requesting power (it may have been out-voted by a
// more-recently-started tool sharing the same gate).
interface ToolRow {
  id: string;
  name: string;
  auto: boolean;          // has a smart outlet → senses its own power
  on: boolean;            // drawing / requested power
  collecting: boolean;    // won a clear path — the green one
  /** The device says a person threw this on, rather than a plug noticing it.
   *  Firmware-only; the mock drives a hand-run through synthetic watts and
   *  cannot tell, so absent means "don't know", never "no". */
  manual: boolean;
  /** This machine has no port in any system — no gate stands between it and the
   *  trunk. A layout fault, not a runtime one. */
  orphan: boolean;
  /** A supplemental port lost its path while the primary kept one: the overarm
   *  guard is shut but the cabinet is collecting. Still one row and one chip (a
   *  machine is ONE box) — this rides the subtitle instead. */
  partial: boolean;
}

/**
 * One airflow system: its blower, and the tools that breathe through it.
 *
 * The view used to be one collector card over one flat list, which had nothing to
 * say about a two-system shop — the card read "2 dust collectors" and the switch
 * on it stopped every tool in the building. A blower is per system and so is the
 * decision to shut one down, so the page is per system too.
 *
 * `id` is '' for the orphan group: machines with no port in any system. They are
 * a layout fault the readiness check already reports, but they must still be
 * LISTED — they were visible in the flat list, and quietly dropping a tool from
 * the page is a worse answer than showing it can't collect.
 */
interface SystemGroup {
  id: string;
  /** The collector ELEMENT's id — its subtitle links to its pairing panel.
   *  Empty for the orphan group, and for a system drawn without a collector. */
  collectorId: string;
  name: string;
  tools: ToolRow[];
  on: boolean;
  coasting: boolean;
  /** Running because a person switched it on, not because a tool is drawing. */
  manual: boolean;
  /** The tool currently winning this system's air, for the subtitle. */
  activeName: string;
  /** Running with nothing open. The firmware publishes this per system; the mock
   *  has no analogue, so it is simply never true in demo. It is the one hard rule
   *  this project has, and a card showing green through it would be a lie. */
  deadHead: boolean;
  /** No switchable plug named for this blower — a legitimate shop ("I start that
   *  one by hand"), but the card must not offer a switch that cannot work. */
  noPlug: boolean;
  /**
   * What the plug reported back, judged against what we commanded.
   *
   *   'running'      commanded on, current flowing — the only green we've earned
   *   'notStarting'  commanded on, past spin-up, drawing nothing
   *   'unknown'      no plug reading at all, or the plug isn't answering
   *
   * A device that doesn't poll its collector plug answers 'unknown' for
   * everything, which is exactly what this page knew before it did.
   */
  plug: ReturnType<typeof collectorPlugState>;
  /** Last plug reading, for the subtitle — showing the number is what lets
   *  someone judge the claim instead of taking it on faith. */
  plugWatts: number;
}

/** What a collector card's chip says. Same axis: is this thing moving air? */
type CollectorChip = 'collecting' | 'byhand' | 'coasting' | 'deadhead'
                   | 'notstarting' | 'plugoffline'
                   | 'blocked' | 'noplug' | 'idle';

const POLL_MS = 2000;

/**
 * The "Live view" — the daily driver. A plain list of tools: what's
 * collecting reads at a glance, everything else is one tap away. Auto tools
 * sense their own power; every tool (auto included) is manually overridable,
 * because sometimes you just need to run the collector to clear a clog.
 *
 * In demo mode DemoApiService seeds a topology so it has something to show.
 * Drives tools through `simTool` (simulated power draw), which is the same
 * lever the real firmware's /api/sim/tool exposes.
 */
@Component({
  selector: 'app-live',
  standalone: true,
  imports: [CommonModule, RouterLink],
  styles: [`
    :host {
      display: block;
      max-width: 460px;
      margin: 0 auto;
      padding: 14px 12px 28px;
      min-height: 100dvh;
      min-height: 100vh;
    }
    .top {
      display: flex; align-items: center; justify-content: space-between;
      padding: 4px 8px 14px;
    }
    .shop { font-size: 14px; color: var(--muted); }
    .dot  { width: 8px; height: 8px; border-radius: 50%; background: var(--muted); }
    .dot.live { background: var(--success); }

    /* collector card */
    .collector {
      display: flex; align-items: center; gap: 12px;
      background: var(--surface); border: 1px solid var(--border);
      border-radius: var(--radius); padding: 16px; margin-bottom: 18px;
    }
    .collector.running { border-color: var(--success); }
    .collector.warn { border-color: rgba(240,165,0,0.55); }
    .collector.warn .cyc { color: var(--accent); }
    .collector.warn .c-sub { color: var(--accent); }
    .collector.bad { border-color: rgba(217,68,68,0.55); }
    .collector.bad .cyc { color: var(--danger); }
    .collector.bad .c-sub { color: var(--danger); }
    .cyc {
      width: 44px; height: 44px; border-radius: 50%; flex-shrink: 0;
      display: flex; align-items: center; justify-content: center;
      background: var(--bg); color: var(--muted);
    }
    .collector.running .cyc { color: var(--success); }
    .cyc svg { width: 24px; height: 24px; }
    .c-body { flex: 1; min-width: 0; }
    .c-name { font-size: 16px; font-weight: 600; }
    .c-sub  { font-size: 13px; color: var(--muted); margin-top: 2px; display: block; }
    .collector.running .c-sub { color: var(--success); }
    /* ── the setup line IS the way to fix it ──────────────────────────────
       When the only thing wrong is that no outlet is paired, that line says so
       and goes there. It used to be two lines — the fact, then "Pair an outlet
       →" under it — which is one thought spending two lines on a page whose
       whole argument is that a row gets one. Underlined, because underline is
       the only "this is a link" signal that survives a thumb: there is no hover
       on the phone this page is read on. */
    a.c-sub, .r-src a {
      text-decoration: underline; text-underline-offset: 2px; text-decoration-thickness: 1px;
    }
    /* Accent, the same colour the standalone "Pair an outlet →" link wore: the
       one thing on an otherwise quiet row worth reaching for. The collector's
       line already inherits it from the card's warn state; a tool row has no
       such state, so it is set here. */
    .r-src a { color: var(--accent); }
    a.c-sub:active, .r-src a:active { opacity: 0.65; }

    .label {
      font-size: 12px; color: var(--muted); letter-spacing: 0.06em;
      text-transform: uppercase; padding: 0 8px 8px;
    }

    /* Systems are separated by space, not by a rule: the collector card already
       reads as a header, and a divider on top of it is one boundary too many. */
    .sys + .sys { margin-top: 26px; }
    .orphan {
      font-size: 13.5px; color: var(--accent); background: rgba(240,165,0,0.10);
      border-radius: var(--radius); padding: 12px 14px; margin-bottom: 18px;
      line-height: 1.45;
    }
    .rows-empty { font-size: 13px; color: var(--muted); padding: 6px 8px; }

    /* tool rows */
    .rows { display: flex; flex-direction: column; gap: 10px; }
    /* THE WRAPPER CARRIES THE CARD, and the row's TEXT sits outside its button.
       Tapping the row hand-runs the tool, and one line of that row is a link to
       the pairing screen — a link cannot be nested in a button. So the button is
       a transparent hit area stretched over the row, drawn under the text, and
       the link lifts itself back above it. Everything that draws the card
       (surface, border, radius, the state washes) lives on the wrapper. */
    .rowwrap {
      background: var(--surface); border: 1px solid var(--border);
      border-radius: var(--radius); transition: border-color 0.12s;
    }
    .row {
      position: relative;
      display: flex; align-items: center; gap: 12px;
      border-radius: var(--radius); padding: 14px 16px;
      text-align: left; width: 100%; color: inherit;
    }
    .hit {
      position: absolute; inset: 0; width: 100%;
      background: none; border: 0; border-radius: var(--radius); padding: 0;
    }
    .hit:focus-visible { outline: 2px solid var(--accent); outline-offset: -3px; }
    /* Above the hit area, and only this. Everything else on the row is text the
       button is meant to swallow the tap for. */
    .r-src a { position: relative; z-index: 1; }
    .rowwrap.collecting { background: rgba(60,190,110,0.10); border-color: var(--success); }
    /* Orange gets a border only, no wash: green has to stay the loudest thing on
       the page, and a shop mid-transition can have several rows waiting at once. */
    .rowwrap.waiting { border-color: rgba(240,165,0,0.55); }
    .r-body { flex: 1; min-width: 0; }
    .r-name { font-size: 16px; font-weight: 500; }
    .r-src { font-size: 12.5px; color: var(--muted); margin-top: 2px; }
    .rowwrap.collecting .r-name { color: var(--success); }
    /* A tool with no gate can't be hand-run — the row is inert, and reads that
       way rather than looking like a control that ignores you. The dimming is on
       the wrapper, the deadening on the button: an inert row may still carry a
       link, and a link is not a control this page has any reason to refuse. */
    .rowwrap.inert { opacity: 0.62; }
    .rowwrap.inert .hit { pointer-events: none; }
    /* An unfinished layout locks the row's CONTROL. It cannot lock the row
       wholesale any more: the greying is opacity, opacity applies to children,
       and the one child that must stay lit is the link to the screen that
       finishes the layout. So the name and the chip grey; a line that is a link
       does not. */
    .rowwrap.locked .hit { pointer-events: none; }
    .rowwrap.locked .r-name,
    .rowwrap.locked .chip { opacity: 0.45; filter: grayscale(1); }
    .rowwrap.locked .r-src { opacity: 0.45; }
    .rowwrap.locked .r-src.pair { opacity: 1; }

    /* ── the chip ────────────────────────────────────────────────────────
       This replaces the switch that used to sit here. That switch was a SPAN
       mirroring "on" while the whole row carried the tap — an indicator drawn as
       a control, and drawn as the same control the collector card uses for real.
       Worse, it could only say two things, and the state worth saying is the
       third: a tool drawing power that lost the gate to something more recent.

       Colour is the severity axis, the words carry the specific. It is checked
       against the board LEDs (firmware/utils/StatusLed.h): green = working and
       red = broken agree across the pixel, this page and the bin light on the
       cyclone. Orange deliberately diverges — motion on a pixel, "no air here"
       on a chip — which is safe only because this page renders no transients, so
       the two can never describe the same moment. Every chip carries text and a
       dot as well as colour: a dim shop and a colour-blind woodworker both break
       a colour-only encoding. See docs/mockups/shop-status-chips.html. */
    .chip {
      font-size: 12px; font-weight: 600; padding: 5px 12px; border-radius: 999px;
      flex-shrink: 0; white-space: nowrap;
      display: inline-flex; align-items: center; gap: 6px;
    }
    .chip::before {
      content: ''; width: 7px; height: 7px; border-radius: 50%;
      background: currentColor; flex: none;
    }
    .chip.go   { color: var(--success); background: rgba(60,190,110,0.16); }
    .chip.wait { color: var(--accent);  background: rgba(240,165,0,0.15); }
    .chip.bad  { color: var(--danger);  background: rgba(217,68,68,0.15); }
    /* Idle is the absence of a story: hollow, so a shelf of sleeping tools stays
       quiet and the row that matters carries all the colour on the page. */
    .chip.off  { color: var(--muted); background: transparent; border: 1px solid var(--border); }
    .chip.off::before { background: transparent; border: 1px solid var(--muted); }

    /* The card keeps its switch — unlike a tool row it is a real button that does
       what it looks like — so the chip stacks above it. */
    .ccol { display: flex; flex-direction: column; align-items: flex-end; gap: 7px; flex-shrink: 0; }

    /* toggle */
    .sw {
      width: 46px; height: 28px; border-radius: 999px; flex-shrink: 0;
      background: var(--bg); border: 1px solid var(--border); position: relative;
      transition: background 0.14s, border-color 0.14s;
    }
    .sw::after {
      content: ''; position: absolute; top: 3px; left: 3px;
      width: 20px; height: 20px; border-radius: 50%; background: var(--muted);
      transition: transform 0.14s, background 0.14s;
    }
    .sw.on { background: var(--success); border-color: var(--success); }
    .sw.on::after { transform: translateX(18px); background: #fff; }

    .setup {
      display: flex; align-items: center; justify-content: center; gap: 6px;
      color: var(--muted); font-size: 13px; text-decoration: none;
      padding: 22px 0 4px;
    }
    .setup svg { width: 15px; height: 15px; }

    .ctlerr {
      font-size: 13px; color: var(--danger); margin: 0 8px 10px; line-height: 1.5;
    }
    /* Three across, not two. At 375px a 14px label plus an 18px icon no longer
       fits, and "Shop layout" wrapped to two lines while its neighbours stayed on
       one — a ragged row. Everything here is a notch smaller so all three labels
       stay single-line on the narrowest phone we target. */
    /* A GRID, not a flex row: four destinations at 13px with an icon each are
       wider than a phone, and flex:1 on a nowrap label cannot shrink below its
       own text — so the last one hung off the right edge of a 375px screen (found
       2026-08-22, once /gates made it four).
       auto-fit rather than a fixed 2x2: this column is capped at 460px, so four
       entries land two-by-two at every width, but the fifth entry — or the fourth
       going away again — re-flows on its own instead of needing this rule edited
       to match. */
    .nav { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
           gap: 8px; margin-top: 18px; }
    .nav a { display: flex; align-items: center; justify-content: center; gap: 7px;
             padding: 12px 6px; border: 1px solid var(--border); border-radius: var(--radius);
             color: var(--muted); text-decoration: none; font-size: 13px; white-space: nowrap; }
    .nav a svg { width: 17px; height: 17px; flex: none; }
    .empty {
      text-align: center; color: var(--muted); padding: 48px 20px;
    }
    .empty a { color: var(--accent); }

    /* Layout isn't finished — nothing here may drive a gate or the collector. */
    .incomplete {
      display: flex; flex-direction: column; gap: 10px;
      background: color-mix(in srgb, var(--danger) 12%, var(--surface));
      border: 1px solid var(--danger); color: var(--danger);
      border-radius: var(--radius); padding: 14px 16px; margin-bottom: 18px;
      font-size: 13.5px; line-height: 1.45;
    }
    .incomplete a {
      align-self: flex-start; color: #fff; background: var(--danger);
      border-radius: 8px; padding: 6px 12px; font-size: 13px;
      text-decoration: none; font-weight: 600;
    }
    /* Applied to the CONTROLS, not to the cards around them.
       It used to grey out the whole collector card and the whole tool list, which
       was fine while everything in there drove hardware. It no longer is: those
       cards now carry links to the setup screens, and an unfinished layout is
       precisely when someone needs them. A link switches nothing on. */
    .locked { opacity: 0.45; pointer-events: none; filter: grayscale(1); }
  `],
  template: `
    <div class="top">
      <span class="shop">{{ shopName }}</span>
      <span class="dot" [class.live]="collectorOn"></span>
    </div>

    <ng-container *ngIf="tools.length; else noShop">
      <!-- An unfinished shop must not drive hardware: everything below is inert
           until the layout is whole (see the ready check). -->
      <div class="incomplete" *ngIf="!ready">
        <span>Shop layout incomplete — {{ notReadyReason }} Nothing can be switched on until it’s sorted out.</span>
        <a [routerLink]="fixLink">Finish setup →</a>
      </div>

      <p class="ctlerr" *ngIf="error">{{ error }}</p>

      <!-- One block per airflow system, in the order the shop layout draws them
           top to bottom. A blower and the tools that breathe through it belong
           together: which collector a tool runs is the single most useful thing
           this page knows about it, and a flat list threw it away. -->
      <div class="sys" *ngFor="let g of groups">
        <div class="collector" *ngIf="g.id"
             [class.running]="collectorChipTone(g) === 'go'"
             [class.warn]="collectorChipTone(g) === 'wait'"
             [class.bad]="collectorChipTone(g) === 'bad'">
          <span class="cyc">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
                 stroke-linecap="round" stroke-linejoin="round">
              <path d="M13 5a7 7 0 1 0 6 7"/><path d="M12 8a4 4 0 1 1-4 4"/>
              <circle cx="12" cy="12" r="1.2" fill="currentColor" stroke="none"/>
            </svg>
          </span>
          <div class="c-body">
            <div class="c-name">{{ g.name }}</div>
            <!-- The same line, twice: as a link when the fix is a screen away,
                 as plain text otherwise. A collector's outlet could be paired
                 nowhere but the build canvas until 2026-08-25 — so the one thing
                 this card cannot do anything about was also the one thing it
                 wouldn't tell you how to fix. -->
            <a class="c-sub" *ngIf="g.noPlug && g.collectorId"
               [routerLink]="['/tools']" [queryParams]="{ el: g.collectorId }"
               [title]="'Pair a smart outlet with ' + g.name + ', so DustGate can start it'"
               >{{ collectorSub(g) }}</a>
            <div class="c-sub" *ngIf="!(g.noPlug && g.collectorId)">{{ collectorSub(g) }}</div>
          </div>
          <div class="ccol">
            <!-- State, on the same axis as the tool rows: is this thing moving
                 air? Note there is no "Waiting" here — a blower is never
                 out-voted, it runs or it doesn't, so orange goes to Blocked. -->
            <span class="chip" [class]="'chip ' + collectorChipTone(g)">{{ collectorChipText(g) }}</span>
            <!-- Starts and stops THIS system's blower. Stop-only until 2026-08-22,
                 which made an idle card's switch a control that did nothing. Off
                 still stops this system's TOOLS — it used to stop every tool in the
                 shop, which on a two-system layout reached across and switched off
                 a machine in the other half of the building.
                 Hidden with no outlet paired: there is genuinely nothing to switch,
                 and a dead control is what this whole change is about removing. -->
            <button class="sw" *ngIf="!g.noPlug" [class.on]="g.on" [class.locked]="!ready"
                    [attr.aria-label]="(g.on ? 'Stop ' : 'Run ') + g.name"
                    [title]="collectorSwitchTitle(g)"
                    (click)="toggleCollector(g)"></button>
          </div>
        </div>

        <!-- No card, because there is no collector to card. Named rather than
             silently mixed in: these tools cannot collect, and the readiness
             banner above says why. -->
        <div class="orphan" *ngIf="!g.id">{{ g.name }}</div>

        <div class="label">Tools</div>
        <div class="rows">
          <!-- The ROW is a button — tapping it hand-runs the tool — but its text
               is no longer INSIDE that button, because one line of the text is a
               link and a button cannot contain one. The button is a transparent
               hit area under the text instead; it carries its own label, since it
               no longer has any of its own. -->
          <div class="rowwrap" *ngFor="let t of g.tools"
               [class.collecting]="toolChipTone(t, g) === 'go'"
               [class.waiting]="toolChipTone(t, g) === 'wait'"
               [class.locked]="!ready"
               [class.inert]="t.orphan">
            <div class="row">
              <button class="hit"
                      [disabled]="t.orphan"
                      (click)="toggle(t)"
                      [attr.aria-pressed]="t.on"
                      [attr.aria-label]="(t.on ? 'Stop ' : 'Run ') + t.name"></button>
              <div class="r-body">
                <div class="r-name">{{ t.name }}</div>
                <!-- Not shown as a link on an orphan — its subtitle is about the
                     LAYOUT, and an outlet would not help it. -->
                <div class="r-src" *ngIf="!pairable(t)">{{ sourceLine(t, g) }}</div>
                <div class="r-src pair" *ngIf="pairable(t)">{{ sourceLead(t)
                  }}<a [routerLink]="['/tools']" [queryParams]="{ el: t.id }"
                       [title]="'Pair a smart outlet with ' + t.name + ', so it starts collection on its own'"
                       >{{ pairHint }}</a>{{ sourceTail(t, g) }}</div>
              </div>
              <span class="chip" [class]="'chip ' + toolChipTone(t, g)">{{ toolChipText(t, g) }}</span>
            </div>
          </div>
          <div class="rows-empty" *ngIf="!g.tools.length">Nothing plumbed into this one yet.</div>
        </div>
      </div>

      <!-- The way out of the Live view. Since / forwards straight here, the
           switcher has to live on the page it leaves — and that includes
           Settings, which had a route and a back button but nothing anywhere in
           the app pointing at it. -->
      <div class="nav">
        <a routerLink="/build">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
               stroke-linecap="round" stroke-linejoin="round">
            <path d="M4 8h9M17 8h3"/><circle cx="15" cy="8" r="2"/>
            <path d="M4 16h4M12 16h8"/><circle cx="10" cy="16" r="2"/>
          </svg>
          Shop layout
        </a>
        <a routerLink="/tools">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
               stroke-linecap="round" stroke-linejoin="round">
            <rect x="4" y="4" width="16" height="16" rx="3"/>
            <circle cx="9.5" cy="11" r="1.1" fill="currentColor" stroke="none"/>
            <circle cx="14.5" cy="11" r="1.1" fill="currentColor" stroke="none"/>
            <path d="M9 16h6"/>
          </svg>
          Tools
        </a>
        <!-- Gate calibration used to be reachable only by finding the gate on the
             build canvas and tapping it — a layout tool, opened with a wrench in
             your other hand. Recalibrating a knocked valve is a shop-floor errand,
             so it gets an entry where the app already puts "go somewhere else". -->
        <a routerLink="/gates">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
               stroke-linecap="round" stroke-linejoin="round">
            <!-- A butterfly valve seen edge-on: a duct, and a disc across it at an
                 angle. The gate glyph the canvas uses is a dot on a line, which at
                 17px in a nav bar is indistinguishable from the layout icon. -->
            <path d="M4 7h16M4 17h16"/>
            <path d="M8 16.5L16 7.5"/>
            <circle cx="12" cy="12" r="1.5" fill="currentColor" stroke="none"/>
          </svg>
          Gates
        </a>
        <a routerLink="/settings">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
               stroke-linecap="round" stroke-linejoin="round">
            <!-- A cog, not a sunburst: the first draft was a hub plus eight
                 detached rays, which at 17px read as a sun. The outer ring is what
                 makes it a gear — the ticks are teeth crossing a rim, not rays. -->
            <circle cx="12" cy="12" r="7.4"/>
            <circle cx="12" cy="12" r="2.9"/>
            <path d="M12 2.8v2.3M12 18.9v2.3M2.8 12h2.3M18.9 12h2.3
                     M5.5 5.5l1.6 1.6M16.9 16.9l1.6 1.6M5.5 18.5l1.6-1.6M16.9 7.1l1.6-1.6"/>
          </svg>
          Settings
        </a>
      </div>
    </ng-container>

    <ng-template #noShop>
      <div class="empty">
        <p>No shop configured yet.</p>
        <p><a routerLink="/build">Set up your shop →</a></p>
        <!-- The nav above is inside the has-tools branch, so without this a device
             with no layout — the one most likely to need Forget WiFi — has no way
             to reach Settings at all. -->
        <p><a routerLink="/settings">Settings</a></p>
      </div>
    </ng-template>
  `,
})
export class LiveViewComponent implements OnInit, OnDestroy {
  shopName = 'The Shop';
  /** One block per airflow system, in shop-layout order. */
  groups: SystemGroup[] = [];
  /** Every tool across every system, flat. Kept because the shop-wide questions —
   *  is anything running, is the layout ready — are still shop-wide. */
  tools: ToolRow[] = [];
  collectorOn = false;
  /** The blower is still on, but only to finish clearing the ducts — every tool is
   *  already off. Firmware has always published this (TopologyRuntime::writeStatus);
   *  until now the view ignored it and showed "Collecting · " with nothing after the
   *  separator, since there's no active tool left to name. */
  collectorCoasting = false;
  activeName = '';
  /** False while the saved layout is unfinished — the build canvas lets you save a
   *  work-in-progress shop, so this view is where that gets enforced: no gate and
   *  no collector moves until it's whole. */
  ready = true;
  notReadyReason = '';
  /** Last control failure, shown inline. A tap that silently does nothing is the
   *  exact failure mode this view just had. */
  error = '';
  /** Where "Finish setup" goes — the gate pass when that's what's missing, the canvas
   *  otherwise. Sending someone to the layout to fix a calibration would just confuse. */
  fixLink = '/build';

  private poll: ReturnType<typeof setInterval> | null = null;
  private busy = false;

  constructor(private api: ApiService) {}

  async ngOnInit(): Promise<void> {
    try {
      await this.api.whenReady();          // else the first fetch 401s and reads as "no shop"
      const topo = toShop(await this.api.getTopology());
      if (topo) this.parseTopology(topo as unknown as Topology);
    } catch {
      this.tools = []; // no topology → empty state
      return;
    }
    await this.refresh();
    this.poll = setInterval(() => { void this.refresh(); }, POLL_MS);
  }

  ngOnDestroy(): void {
    if (this.poll) clearInterval(this.poll);
  }

  /**
   * The line under the blower's name: ONLY what the chip beside it cannot carry.
   *
   * The chip says the state now, so repeating it here ("Collecting" over
   * "Collecting · Table Saw") spends the one line that could have said something
   * useful on saying the same word twice. What is left is the detail: which tool
   * has the air, why a blower is running with nothing on it, what to do about a
   * fault.
   */
  collectorSub(g: SystemGroup): string {
    // Not "no outlet paired" — that names the state and stops. The one line
    // this card has says the state AND the fix in the same breath, and IS the
    // way there (the template links it), which is what a second line of link
    // text used to be spent on.
    if (g.noPlug) return 'Manual · ' + this.pairHint;
    // The one hard rule this project has. Said plainly, because a blower running
    // into a sealed system is the thing you stop what you are doing to fix.
    if (g.deadHead) return 'Running with nothing open — stop it';
    // Show the reading. The number is what lets someone tell "the breaker went"
    // from "the app is confused", and it is the first thing they would ask for.
    if (this.collectorChip(g) === 'notstarting') {
      return 'Switched on, drawing ' + Math.round(g.plugWatts) + ' W — check its switch and breaker';
    }
    if (this.collectorChip(g) === 'plugoffline') return "Its outlet isn't answering — it may still be running";
    if (!this.ready && g.tools.some(t => t.on)) {
      const asking = g.tools.find(t => t.on)?.name ?? 'A tool';
      return asking + ' is asking — the layout isn\'t finished';
    }
    if (!g.on) return 'Nothing is asking for it';
    // Both of these are said out loud because a blower running with every tool
    // off otherwise reads as a stuck relay, and someone goes looking for a fault
    // instead of waiting the few seconds out.
    if (g.coasting) return 'Every tool is off — clearing the ducts';
    if (g.manual && !g.activeName) return 'No tool is asking — you started it';
    return g.activeName || 'Running';
  }

  /**
   * The collector card's switch, which is a SWITCH now.
   *
   * It was stop-only: tapping an idle blower did nothing at all, while looking
   * exactly like a control that would start it (reported 2026-08-22). Running the
   * blower on its own is a real errand — clear a clog, sweep up, prove a run —
   * and the routing brain has always been built to allow it: idle-hold keeps a
   * path open precisely so a hand start can't dead-head.
   *
   * OFF still means "stop what is running here". With tools running that is the
   * tools, because a blower switched off under a running saw would just come back
   * on the next poll; with only a hand-run to stop, it is the hand-run.
   */
  async toggleCollector(g: SystemGroup): Promise<void> {
    if (this.busy || !this.ready) return;
    this.busy = true;
    this.error = '';
    try {
      if (!g.on) {
        await this.api.setCollectorManual(true, g.id);
      } else {
        // Tools first: stopping them is what "off" means while any are running,
        // and clearing the hand-run as well leaves nothing behind holding it on.
        for (const t of g.tools) if (t.on) await this.api.setToolManual(t.id, false);
        if (g.manual) await this.api.setCollectorManual(false, g.id);
      }
      await this.refresh(true);
    } catch {
      this.error = "Couldn't reach the controller — nothing was switched.";
    } finally {
      this.busy = false;
    }
  }

  /**
   * The collector card's state, same axis rephrased for a blower: is it moving
   * air?
   *
   * There is deliberately no "Waiting" — a blower is never out-voted, it runs or
   * it doesn't. Orange goes to `blocked` instead, which carries the same feeling:
   * something wants air and isn't getting it.
   *
   * NOT here, because the device cannot know them yet: "not starting" (commanded
   * on, plug reporting no current) and "plug offline". Collector plugs are
   * switched, never polled — SmartOutletControl.h is explicit that they are
   * "switchable Shelly outlets… rather than power sensors" — so `collectorOn`
   * is what we COMMANDED, not what is happening. Until the plug is polled for
   * watts this card cannot tell a running blower from a tripped breaker, and it
   * must not pretend otherwise by inventing a chip for it.
   */
  collectorChip(g: SystemGroup): CollectorChip {
    if (g.noPlug)   return 'noplug';
    if (g.deadHead) return 'deadhead';
    // The plug disagreeing with the command outranks everything below: a blower
    // we think is collecting while nothing draws is the worst thing this page
    // can get wrong, and it looks identical to working unless we say so.
    if (g.plug === 'notStarting') return 'notstarting';
    // Unreachable is NOT an accusation — the blower may well be running. But it
    // is only worth mentioning while we are asking it to run; an idle plug we
    // can't reach is tomorrow's problem.
    if (g.plug === 'unknown' && g.on && !g.coasting) return 'plugoffline';
    // Asked for, and can't be given: a tool is drawing here while the layout is
    // unfinished. Only worth saying when something actually wants air — an idle
    // shop mid-setup is just idle.
    if (!this.ready && g.tools.some(t => t.on)) return 'blocked';
    if (g.coasting) return 'coasting';
    if (!g.on)      return 'idle';
    return (g.manual && !g.activeName) ? 'byhand' : 'collecting';
  }

  collectorChipText(g: SystemGroup): string {
    switch (this.collectorChip(g)) {
      case 'notstarting':  return 'Not starting';
      case 'plugoffline':  return 'Outlet offline';
      case 'noplug':     return 'No outlet';
      case 'deadhead':   return 'Dead-headed';
      case 'blocked':    return 'Blocked';
      case 'coasting':   return 'Coasting';
      case 'byhand':     return 'Running by hand';
      case 'collecting': return 'Collecting';
      default:           return 'Idle';
    }
  }

  collectorChipTone(g: SystemGroup): string {
    switch (this.collectorChip(g)) {
      case 'deadhead':
      case 'notstarting':
      case 'plugoffline':       return 'bad';
      case 'blocked':
      case 'noplug':            return 'wait';
      case 'byhand':
      case 'collecting':        return 'go';
      default:                  return 'off';
    }
  }

  collectorSwitchTitle(g: SystemGroup): string {
    if (!g.on) return `Run ${g.name} by hand — opens a gate first, then starts the blower`;
    if (g.manual && !g.activeName) return `Stop ${g.name}`;
    return `Switch off every tool running on ${g.name}`;
  }

  // ── chips ────────────────────────────────────────────────────────────────
  //
  // ONE question: is air reaching this tool? Auto-vs-manual is a settings fact
  // and lives in the subtitle — mixing it into this axis is exactly what made
  // the switch that used to sit here unreadable.

  /**
   * @returns the tool row's state on the air-reaching-it axis.
   *
   * Winning the routing vote is NOT the same as getting air. A tool can hold a
   * wide-open path to a blower that is not turning — and rendering that green
   * would be the same lie the collector card used to tell, just moved one row
   * down. So a confirmed dead blower strips every tool under it back to
   * "No suction".
   *
   * Only a CONFIRMED one. An unreachable plug leaves the blower's state unknown,
   * and a shop whose collector has no plug at all starts it by hand and has air
   * — neither is an accusation, and neither changes this row.
   */
  toolChip(t: ToolRow, g?: SystemGroup): ToolChip {
    if (t.orphan)      return 'nogate';
    if (!t.on)         return 'idle';
    if (!t.collecting) return 'waiting';
    return g?.plug === 'notStarting' ? 'nosuction' : 'collecting';
  }

  toolChipText(t: ToolRow, g?: SystemGroup): string {
    switch (this.toolChip(t, g)) {
      case 'nogate':     return 'No gate';
      case 'collecting': return 'Collecting';
      case 'nosuction':  return 'No suction';
      case 'waiting':    return 'Waiting';
      default:           return 'Idle';
    }
  }

  toolChipTone(t: ToolRow, g?: SystemGroup): string {
    switch (this.toolChip(t, g)) {
      case 'collecting': return 'go';
      case 'nosuction':
      case 'waiting':
      case 'nogate':     return 'wait';
      default:           return 'off';
    }
  }

  /**
   * The line under the name: how this tool is driven, and — when it is running
   * and getting nothing — WHY.
   *
   * Naming the winner is the point. "Waiting" alone leaves you hunting the list
   * for whatever took the gate; naming it makes the next action obvious, because
   * the answer is always "go and switch that one off".
   */
  sourceLine(t: ToolRow, g?: SystemGroup): string {
    return this.sourceHow(t) + this.sourceTail(t, g);
  }

  /**
   * Whether this row's line is a LINK — the unpaired case, where the whole
   * reason it says anything is that a screen elsewhere fixes it.
   *
   * Same condition the standalone "Pair an outlet →" link carried before the
   * two collapsed into one line: paired tools have nothing to fix, and an
   * orphan's problem is the layout, which an outlet would not help.
   */
  pairable(t: ToolRow): boolean { return !t.auto && !t.orphan; }

  /** The words the link itself is made of, so the two renderings can't drift. */
  readonly pairHint = 'pair an outlet to automate';

  /** What comes before the link on a pairable row. */
  sourceLead(t: ToolRow): string {
    return (t.manual ? 'Switched on by hand' : 'Manual') + ' · ';
  }

  /**
   * How it came on. `manual` is the device's own account and beats our guess
   * from the layout; absent (the mock never sends it) falls back to whether a
   * plug exists at all, which is what this line has always said.
   *
   * "Manual" alone says what, never why — and the why is a setup fact the reader
   * can act on: nothing is paired, so nothing can sense this tool starting. Say
   * the FIX rather than the fact, in the line that's already there. Said on
   * every unpaired row, not just an unexpected one.
   */
  private sourceHow(t: ToolRow): string {
    if (t.orphan) return 'Not plumbed into a system yet';
    if (this.pairable(t)) return this.sourceLead(t) + this.pairHint;
    return t.manual ? 'Switched on by hand'
                    : (t.on ? 'Auto · sensing power' : 'Auto');
  }

  /**
   * What follows the "how", whether or not the how is a link — so the link can
   * be one element in the middle of the line and this can be the text after it.
   */
  sourceTail(t: ToolRow, g?: SystemGroup): string {
    if (t.orphan) return '';

    if (t.collecting) {
      // Its gate is open and the blower is not turning: the fault is upstairs,
      // so say so here rather than leaving someone to check this tool.
      if (g?.plug === 'notStarting') return ' · ' + g.name + " isn't running";
      // A machine is ONE box however many ports it has, so a lost overarm does
      // not get its own row or its own chip — but it is still worth saying.
      return t.partial ? ' · second port is shut' : '';
    }
    if (!t.on) return '';

    const winner = g?.activeName;
    if (winner && winner !== t.name) return ' · ' + winner + ' has the gate';
    // On, not collecting, and nothing else won either: the blower for this
    // system isn't running yet, or the layout won't let it.
    return ' · no clear path to the collector';
  }

  async toggle(t: ToolRow): Promise<void> {
    // An orphan has no gate to open, so there is nothing to hand-run into. The
    // row is already inert in the template; this is the backstop.
    if (this.busy || !this.ready || t.orphan) return;
    this.busy = true;
    this.error = '';
    try {
      await this.api.setToolManual(t.id, !t.on);
      // Optimistic, then confirmed: the device answers the POST before its main
      // loop has routed, so the authoritative state comes from the refresh.
      t.on = !t.on;
      await this.refresh(true);
    } catch {
      this.error = 'Couldn\'t reach the controller — nothing was switched.';
    } finally {
      this.busy = false;
    }
  }

  private async refresh(force = false): Promise<void> {
    if (this.busy && !force) return;
    try {
      this.applyStatus(await this.api.getStatus());
    } catch { /* transient — keep last known state */ }
  }

  private applyStatus(status: TopologyStatus): void {
    const st = status.tools ?? {};
    // `machines`, not `reachable`. `reachable` is keyed by PORT id, so looking a
    // MACHINE up in it returns undefined for any tool whose port id differs from
    // its own — which is every shop-schema layout. Every row read as "not
    // collecting", forever. `machines` is the rolled-up per-machine verdict the
    // device has published all along, and it distinguishes a lost supplemental
    // port (partial) from a lost primary (stripped), which a boolean can't.
    const verdict = status.machines ?? {};
    const reach = status.reachable ?? {};
    for (const t of this.tools) {
      t.on = !!st[t.id]?.active;
      t.manual = st[t.id]?.manual === true;
      const v = verdict[t.id];
      if (v) {
        t.collecting = v.status !== 'stripped';
        t.partial    = v.status === 'partial';
      } else {
        // No verdict for this machine: it wasn't in the routing pass at all,
        // which for an active tool means it got nothing. Fall back to the port
        // map for the one shape where the ids DO line up (a v1 topology).
        t.collecting = reach[t.id] === true;
        t.partial = false;
      }
    }
    this.collectorOn = !!status.collectorOn;
    this.collectorCoasting = !!status.collectorCoasting;
    this.activeName = this.tools.find(t => t.collecting)?.name ?? '';

    // Per-blower truth if the device sends it — both the firmware and the model
    // always have. A device that doesn't falls back to the shop-wide pair, which
    // is exactly right for a one-system shop and the best available guess
    // anywhere else; the alternative is showing nothing.
    const per = status.systems;
    for (const g of this.groups) {
      const s = per?.[g.id];
      g.on = s ? !!s.collectorOn : this.collectorOn;
      g.coasting = s ? !!s.coasting : this.collectorCoasting;
      g.manual = !!s?.manual;
      // Firmware-only; the mock has no analogue, so this is simply never true in
      // demo rather than being faked into one.
      g.deadHead = s?.deadHeadRisk === true;
      // The verdict lives in the shared model, not here — the same function the
      // mock and the demo use, so all three agree by construction.
      g.plug = collectorPlugState(s?.plug, g.on);
      g.plugWatts = s?.plug?.watts ?? 0;
      g.activeName = g.tools.find(t => t.collecting)?.name ?? '';
    }
  }

  /** The shop has to be structurally whole, free of always-open leaks, AND have every
   *  servo gate calibrated before it may run. The first two are what the build canvas
   *  reports as work-in-progress; the third is the /gates pass. An uncalibrated gate is
   *  as unsafe as a leak — we'd be driving to positions nobody has checked. */
  private checkReady(topo: Topology): void {
    let reason = '';
    this.fixLink = '/build';
    try {
      const v = validateShop(topo);
      if (!v.ok) reason = v.errors[0]?.message ?? 'the layout is incomplete.';
      else {
        // Per system — a leak is a statement about one blower's ducts, and
        // airflowIssues handed a whole shop would find no elements at the root
        // and report all-clear. See systemViews().
        const leaks = systemViews(topo as unknown as ShopDoc).flatMap(view => airflowIssues(view));
        const unset = configurableSelectorsOf(topo).filter(s => !isCalibrated(s));
        const open = leaks.filter(l => l.kind === 'always-open');
        const shared = leaks.filter(l => l.kind === 'co-open');
        if (open.length) {
          const names = open.map(l => l.name).join(', ');
          reason = `${names} ${open.length === 1 ? 'has' : 'have'} no gate between ${open.length === 1 ? 'it' : 'them'} and the collector, so suction would leak there.`;
        } else if (shared.length) {
          const names = shared.map(l => l.name).join(', ');
          reason = shared.length === 1
            ? `${names} shares an outlet with ${(shared[0].with ?? []).map(w => w.name).join(', ')} with no gate in between, so running it would pull air through them too.`
            : `${names} share an outlet with no gate between them, so running one would pull air through the others.`;
        } else if (unset.length) {
          const names = unset.map(s => s.name || s.id).join(', ');
          reason = `${names} ${unset.length === 1 ? "hasn't" : "haven't"} been set up yet — no one has shown ${unset.length === 1 ? 'it' : 'them'} where the valve positions are.`;
          // The canvas, not a separate pass: each unset gate wears an orange dot
          // there, and tapping it opens the same configurator.
          this.fixLink = '/build';
        }
      }
    } catch { reason = 'the layout could not be read.'; }
    this.ready = !reason;
    this.notReadyReason = reason;
  }

  private parseTopology(topo: Topology): void {
    this.checkReady(topo);
    const doc = topo as unknown as ShopDoc;
    this.shopName = doc.name ?? 'The Shop';

    // The list is of MACHINES, not ports. This view answers "what is running",
    // and what runs is a machine — a table saw with a cabinet port and an overarm
    // secondary port is one row with one switch, not two. The status blob keys its `tools`
    // map by machine id for the same reason, so these ids line up with it.
    this.tools = machinesOf(doc).map(m => ({
      id: m.id,
      name: m.name || m.id,
      auto: !!m.sensor?.outlet,
      on: false,
      collecting: false,
      manual: false,
      // Set in buildGroups, which is where "has a port in a system" is already
      // being decided in order to place the row.
      orphan: false,
      partial: false,
    }));

    this.groups = this.buildGroups(doc);
  }

  /**
   * Split the tools across their systems, in the order the build canvas draws
   * them.
   *
   * ORDER comes from `ui.layout` — the canvas stripes its systems by the topmost
   * row any of their pieces stands on (systemRowBands()), so reading the same
   * saved cells and sorting the same way is what makes this page and that one
   * agree. Document order is the fallback when a shop has never been laid out;
   * it is what the canvas would auto-layout from anyway.
   *
   * A machine belongs to the system of its PRIMARY port. A secondary port's run
   * may cross the seam — it is the one thing allowed to — but the tool itself
   * lives in one system, and that is the one whose blower it starts.
   */
  private buildGroups(doc: ShopDoc): SystemGroup[] {
    // The ordering lives in shop-doc now: /tools and /gates group by system too,
    // and three screens deriving canvas order from `ui.layout` separately is three
    // chances to disagree with the drawing they are all describing.
    const order = systemsInLayoutOrder(doc);

    const byId = new Map<string, SystemGroup>();
    for (const s of order) {
      const dc = (s.elements as Array<Record<string, unknown>>).find(e => e['type'] === 'collector');
      byId.set(s.id, {
        id: s.id,
        collectorId: (dc?.['id'] as string) ?? '',
        name: (dc?.['name'] as string) || (s.name as string) || 'Dust collector',
        tools: [], on: false, coasting: false, manual: false, activeName: '',
        deadHead: false, plug: 'noplug', plugWatts: 0,
        // A system with no plug means "I start that collector by hand" — a
        // legitimate shop the firmware explicitly supports (see the collector
        // slot loop in firmware.ino). The card must say so rather than offer a
        // switch that cannot do anything.
        noPlug: !((dc?.['control'] as Record<string, unknown> | undefined)?.['outlet']),
      });
    }

    // Machines with no port anywhere. A layout fault the readiness banner already
    // explains — but they were visible in the flat list this replaces, and a tool
    // that silently vanishes off the page is worse than one shown as unable to
    // collect.
    const orphans: SystemGroup = {
      id: '', collectorId: '',
      name: 'Not connected to a collector — finish the layout to run these.',
      tools: [], on: false, coasting: false, manual: false, activeName: '',
      deadHead: false, noPlug: true, plug: 'noplug', plugWatts: 0,
    };

    for (const t of this.tools) {
      const primary = primaryPortOf(doc, t.id);
      const sysId = primary
        ? portsOf(doc, t.id).find(p => p.port === primary)?.systemId
        : undefined;
      const group = byId.get(sysId ?? '');
      // No system owns it → no gate between it and the trunk. The row still gets
      // listed (a tool that silently vanishes is worse than one shown as unable
      // to collect) but it wears the chip that says so and can't be hand-run.
      t.orphan = !group;
      (group ?? orphans).tools.push(t);
    }

    const groups = order.map(s => byId.get(s.id) as SystemGroup);
    if (orphans.tools.length) groups.push(orphans);
    return groups;
  }
}
