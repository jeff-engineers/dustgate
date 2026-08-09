import { AfterViewInit, Component, ElementRef, HostListener, OnDestroy, OnInit, ViewChild } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { RouterLink } from '@angular/router';
import { DomSanitizer, SafeHtml } from '@angular/platform-browser';
import { ApiService, Topology, TopologyStatus } from '../services/api.service';
import { validateTopology, airflowIssues, redundantSelectors, type AirflowIssue } from '@topology';
import { SelectorConfigComponent } from '../gates/selector-config.component';
import { ElementOutletConfigComponent } from '../tools/element-outlet-config.component';
import {
  AnyElement, ConfigurableSelector,
  isCalibrated, isLinearSelector, isServoSelector,
} from '../gates/selector-types';
import {
  type Glyph, type Pt, type SceneNode,
  CELL, GATE_PAD, PAD, TOOL_HALF, UNIT_H,
  halfH as glyphHalfH, halfW as glyphHalfW, ptSegDist, segBoxHit,
} from './routing/geometry';
import { type RoutedDuct, type Scene, Router, sceneBounds } from './routing/router';

// ── Layout model ──────────────────────────────────────────────────────────────
// The canvas is pure presentation: each element gets a grid cell (col,row). The
// topology tree stays presentation-free; positions ride along in the doc under
// `ui.layout` — an unknown key the model + validator ignore, so it round-trips
// through PUT/GET. Every mutation keeps the topology valid by construction: new
// selectors are added with all outlets CAPPED (role 'blocked', which needs no
// child), and attaching a tool flips one cap to a tool branch.
//
// A "unit" selector (sliding gate = linear, manifold = servoManifold) renders as
// one horizontal UNIT spanning N cells, with an outlet on the bottom of each
// cell. Its tools LOCK into the cell directly below their outlet (drag the whole
// unit; tools are select-only); the trunk to the collector enters from the LEFT.

type SelKind = 'linear' | 'servoGate' | 'servoManifold';

interface Cell { col: number; row: number; }
interface RawEl { [k: string]: unknown; }
interface Branch { id: string; opensState: string; role: string; }

interface NodeVM {
  id: string; glyph: Glyph; name: string;
  col: number; row: number; branchCount: number; live: boolean;
  isUnit: boolean; span: number; openIndex: number;
  /** Setup state of a gate: '' for anything that needs none (tools, the collector),
   *  'todo' until someone has measured it on the hardware, 'done' after. */
  setup: '' | 'todo' | 'done';
  /** This gate isolates nothing the shop wouldn't isolate without it — advisory,
   *  derived fresh on every mutation, so it clears itself the moment it stops
   *  being true. See redundantSelectors() in shared/device-model/topology.js. */
  redundant: boolean;
  dragX?: number; dragY?: number;
}
interface DuctVM { childId: string; live: boolean; open: boolean; }
/** A tee point on a run. `axis` is the direction the run travels here, so a new
 *  leg can be sent off perpendicular to it. */
interface BDot { x: number; y: number; childId: string; col: number; row: number; axis: 'h' | 'v'; }
interface ODot { x: number; y: number; parentId: string; branchId?: string; cell: Cell; }

type Fitting = SelKind | 'tool' | 'duct';
type MenuKind = Fitting | 'cap' | 'uncap' | 'delete' | 'configure' | 'outlet';

const FITTINGS: Array<{ kind: Fitting; label: string }> = [
  { kind: 'duct',          label: 'Duct' },          // lay bare pipe; populate the open end later
  { kind: 'tool',          label: 'Tool' },
  { kind: 'linear',        label: 'Sliding gate' },
  { kind: 'servoGate',     label: 'Ball valve' },
  { kind: 'servoManifold', label: 'Manifold' },
];

/** One row of the context menu: always present, greyed (with a reason) where it
 *  doesn't apply — the list never changes shape between add points. */
interface MenuOption { kind: MenuKind; label: string; enabled: boolean; note?: string; }

const isUnitKind = (kind: unknown): boolean => kind === 'linear' || kind === 'servoManifold';
/** Undo depth. Snapshots are a few KB of JSON each — this is cheap. */
const HISTORY_MAX = 60;
/** Grid cells a fitting takes up: a unit is one horizontal bar N cells wide. */
const spanFor = (kind: MenuKind): number => kind === 'linear' ? 4 : kind === 'servoManifold' ? 2 : 1;

@Component({
  selector: 'app-build',
  standalone: true,
  imports: [CommonModule, FormsModule, RouterLink, SelectorConfigComponent, ElementOutletConfigComponent],
  styles: [`
    :host { display: flex; flex-direction: column; height: 100dvh; height: 100vh; overflow: hidden; }
    /* Wraps rather than clips: the row already overflowed a phone-width viewport
       before Boards was added, silently pushing Save off the edge. */
    .bar { display: flex; flex-wrap: wrap; align-items: center; gap: 10px 12px; padding: 12px 16px;
           border-bottom: 1px solid var(--border); flex-shrink: 0; }
    .bar .title { font-size: 15px; font-weight: 600; flex: 1 0 auto; }
    .bar button { background: var(--surface); border: 1px solid var(--border); color: var(--text); border-radius: var(--radius); padding: 9px 14px; font-size: 14px; }

    /* Contextual guidance bar — pinned under the toolbar. */
    .guide { display: flex; align-items: center; gap: 10px; padding: 8px 16px; border-bottom: 1px solid var(--border);
      flex-shrink: 0; font-size: 13px; line-height: 1.4; color: var(--muted); background: var(--surface); }
    .guide span { flex: 1; }
    .guide.warn { color: var(--danger); background: color-mix(in srgb, var(--danger) 12%, var(--surface)); }
    .guide.ok   { color: var(--success); }
    .guide .cap { background: var(--danger); border: none; color: #fff; border-radius: 7px; padding: 5px 10px; font-size: 12px; flex-shrink: 0; }
    .bar button.primary { background: var(--accent); border-color: var(--accent); color: #1a1200; font-weight: 600; }
    .bar button:disabled { opacity: 0.45; }

    .stage { flex: 1; display: flex; min-height: 0; }
    .canvas-wrap { flex: 1; overflow: auto; background: var(--bg); position: relative; }
    svg.canvas { display: block; touch-action: none; }

    .duct { stroke: var(--border-strong, #3a3a3a); stroke-width: 6; fill: none; stroke-linecap: round; stroke-linejoin: round; }
    .duct.live { stroke: var(--success); }
    /* The knockout that makes a crossing legible: 5px of canvas either side of the
       duct on top. It runs the whole length, so every duct sits in a thin clear
       corridor through the dot grid — deliberate, and it costs nothing to draw. */
    .duct-casing { stroke: var(--bg); stroke-width: 16; fill: none; stroke-linecap: round; stroke-linejoin: round; }
    /* Branch points: a subtle dot at each grid step on a duct; click to branch there. */
    .bdot { cursor: cell; }
    .bdot-dot { fill: var(--muted); opacity: 0.5; transition: r .1s, opacity .1s, fill .1s; }
    .bdot:hover .bdot-dot { fill: var(--accent); opacity: 1; r: 6; }
    /* Output add-dots: a hollow ⊕ ring at each free output — add a run there. */
    .odot { cursor: cell; }
    .odot-ring { fill: var(--bg); stroke: var(--muted); stroke-width: 1.5; transition: r .1s, stroke .1s; }
    .odot-plus { stroke: var(--muted); stroke-width: 1.5; stroke-linecap: round; transition: stroke .1s; }
    .odot:hover .odot-ring { stroke: var(--accent); r: 7; }
    .odot:hover .odot-plus { stroke: var(--accent); }
    /* Open run — bare pipe dead-ending at an unpopulated end. The run itself stays a
       plain pipe; only the LAST stretch is a dashed accent + accent end-dot, so the
       "unfinished" signal sits at the end and doesn't drown the branch dots along it. */
    .open-stub { stroke: var(--accent); stroke-width: 6; fill: none; stroke-linecap: round; stroke-dasharray: 6 5; }
    .open-end .dot { fill: var(--accent); }
    .open-end .end-ring { fill: none; stroke: var(--accent); stroke-width: 1.5; opacity: 0.6; }
    .node { cursor: grab; }
    .node.dragging { cursor: grabbing; }
    /* Live drop feedback: the target cell, and the ghost itself when it's refused. */
    .target { fill: color-mix(in srgb, var(--accent) 10%, transparent); stroke: var(--accent);
              stroke-width: 1.5; stroke-dasharray: 5 4; pointer-events: none; }
    .target.bad { fill: color-mix(in srgb, var(--danger) 12%, transparent); stroke: var(--danger); }
    .node.dragging.bad .body, .node.dragging.bad .unit { stroke: var(--danger); stroke-width: 2.5; }
    .node.dragging.bad { opacity: 0.75; }
    .node .body, .node .unit { fill: var(--surface); stroke: var(--border-strong, #444); stroke-width: 1.5; }
    .node.sel .body, .node.sel .unit { stroke: var(--accent); stroke-width: 2.5; }
    .node.live .body, .node.live .unit { stroke: var(--success); }
    .glabel { fill: var(--text); font-size: 12.5px; text-anchor: middle; font-weight: 500; }
    .gsub   { fill: var(--muted); font-size: 10.5px; text-anchor: middle; }
    .gsub.redundant { fill: var(--accent); opacity: 0.85; }
    /* The selected piece's name is edited WHERE IT IS DRAWN — a foreignObject input
       sitting exactly on top of its label, matching it in size and weight, so the
       text doesn't jump when it becomes editable. It replaced a text field in the
       floating inspector, which showed the same name twice: once on the node and
       again in a box above it, with no cue which one you were changing. */
    /* Absolutely positioned INSIDE the scrolling canvas, in board coordinates, so it
       travels with the board. It used to be position:fixed at a screen point computed
       from getScreenCTM() — which nothing recomputes on scroll, so the field drifted
       off its glyph as soon as you panned. */
    .nameedit { position: absolute; z-index: 20; transform: translate(-50%, -50%);
                box-sizing: border-box; background: transparent; border: none;
                border-radius: 0; color: var(--text); caret-color: var(--accent);
                font-size: 12.5px; font-weight: 500; font-family: inherit; text-align: center;
                padding: 1px 2px 2px; outline: none; cursor: text; }
    /* A name is editable, so it gets the I-beam rather than the body's grab hand. */
    .glabel { cursor: text; }
    .stroke { stroke: var(--muted); } .node.live .stroke { stroke: var(--success); }
    .fillmuted { fill: var(--muted); } .node.live .fillmuted { fill: var(--success); }
    .puck { fill: var(--border-strong, #555); } .node.live .puck { fill: var(--success); }

    .menu, .inspector { position: fixed; z-index: 20; background: var(--surface); border: 1px solid var(--border-strong, #444); border-radius: 12px; box-shadow: 0 8px 24px rgba(0,0,0,0.5); }
    .menu { padding: 6px; min-width: 150px; }
    .menu button { display: flex; align-items: center; gap: 10px; width: 100%; background: none; border: none; color: var(--text); padding: 9px 12px; border-radius: 8px; font-size: 14px; text-align: left; }
    .menu button:hover:not(:disabled) { background: var(--bg); }
    /* Invalid here — kept in place (greyed) so the list is the same everywhere. */
    .menu button:disabled { opacity: 0.4; cursor: default; }
    .menu button .note { margin-left: auto; padding-left: 10px; font-size: 11.5px; color: var(--muted); }
    .menu svg { width: 18px; height: 18px; color: var(--muted); flex-shrink: 0; }
    .menu-sect { font-size: 10.5px; letter-spacing: 0.06em; text-transform: uppercase; color: var(--muted); padding: 6px 12px 3px; }
    .menu-sep { height: 1px; background: var(--border); margin: 5px 8px; }
    .backdrop { position: fixed; inset: 0; z-index: 15; }

    /* Floats just above the selected element (see inspectorPos). */
    .inspector { transform: translate(-50%, calc(-100% - 12px)); padding: 8px 10px; display: flex; align-items: center; gap: 8px; white-space: nowrap; }
    .inspector input { background: var(--bg); border: 1px solid var(--border); color: var(--text); border-radius: 8px; padding: 7px 9px; font-size: 14px; width: 140px; }
    .inspector .meta { font-size: 12px; color: var(--muted); display: flex; align-items: center; gap: 7px; }
    .inspector .step { width: 26px; height: 26px; border-radius: 7px; background: var(--bg); border: 1px solid var(--border); color: var(--text); font-size: 15px; padding: 0; }
    .inspector .step:disabled { opacity: 0.35; }
    /* The config sheet floats over the canvas rather than replacing it — you keep your
       bearings on which gate you're setting up. */
    .sheet-bg { position: fixed; inset: 0; background: rgba(0,0,0,0.5); z-index: 25; }
    .sheet { position: fixed; z-index: 26; left: 50%; top: 50%; transform: translate(-50%, -50%);
             width: min(440px, calc(100vw - 28px)); max-height: calc(100vh - 40px); overflow-y: auto; }
    /* Destructive menu row reads red, like the (−) badge. */
    .menu button.danger { color: var(--danger); }
    .menu button.danger svg { color: var(--danger); }
    .node .cap { fill: var(--muted); }
    .node .fan { opacity: 0.55; }
    /* Remove badge — replaces the old cap/delete dialog. */
    .todo { fill: var(--accent); stroke: var(--surface); stroke-width: 2; }
    .done { fill: var(--success); stroke: var(--surface); stroke-width: 2; }
    .tick { fill: none; stroke: var(--bg); stroke-width: 2; stroke-linecap: round; stroke-linejoin: round; }
    .todo-hit { cursor: pointer; }
    .rm { cursor: pointer; }
    .rm-bg { fill: var(--danger); stroke: var(--surface); stroke-width: 2; }
    .rm-line { stroke: #fff; stroke-width: 2.2; stroke-linecap: round; }
    .rm.off { cursor: default; opacity: 0.35; }
  `],
  template: `
    <div class="bar">
      <span class="title">Shop layout</span>
      <button routerLink="/shop">Done</button>
      <input #fileInput type="file" accept="application/json,.json" hidden (change)="onImportFile($event)"/>
      <button (click)="importClick(fileInput)">Import</button>
      <button (click)="exportShop()" [disabled]="!hasShop">Export</button>
      <button (click)="undo()" [disabled]="!canUndo" title="Undo (Ctrl/Cmd-Z)">Undo</button>
      <button (click)="redo()" [disabled]="!canRedo" title="Redo (Ctrl/Cmd-Shift-Z)">Redo</button>
      <button (click)="autoArrange()">Auto-arrange</button>
      <!-- Where a second ESP32 gets added. Sits next to Gates because that's the
           order the work happens in: a gate can't be told which board drives it
           until the board exists. -->
      <button routerLink="/boards">Boards</button>
      <button class="primary" (click)="save()" [disabled]="!dirty || saving">{{ saving ? 'Saving…' : 'Save' }}</button>
    </div>

    <!-- Contextual guidance / status — fixed under the toolbar. Always present; its
         message follows what you're doing: onboarding → progress → problems + fixes. -->
    <div class="guide" [class.warn]="guide.kind === 'warn'" [class.ok]="guide.kind === 'ok'">
      <span>{{ guide.text }}</span>
      <button class="cap" *ngIf="guide.cap" (click)="capAndSave()">Cap them</button>
    </div>

    <div class="stage">
      <div class="canvas-wrap" #wrap>
        <svg #svg class="canvas" *ngIf="nodes.length"
             [attr.viewBox]="'0 0 ' + vbW + ' ' + vbH" [attr.width]="vbW" [attr.height]="vbH">
          <defs>
            <pattern id="bdots" width="27" height="27" patternUnits="userSpaceOnUse"><circle cx="1" cy="1" r="1" fill="var(--border)"/></pattern>
          </defs>
          <rect x="0" y="0" [attr.width]="vbW" [attr.height]="vbH" fill="url(#bdots)" (pointerdown)="deselect()"/>

          <!-- The cell a drag is over. Red while the drop would be refused, so the
               reason in the guide bar has something to point at. -->
          <rect *ngIf="hoverCell" class="target" [class.bad]="!!dropBlocked"
                [attr.x]="PAD + hoverCell.col * CELL - CELL / 2" [attr.y]="PAD + hoverCell.row * CELL - CELL / 2"
                [attr.width]="CELL" [attr.height]="CELL" rx="6"/>

          <!-- Each run is stroked twice: a fat casing in the canvas colour, then the
               duct. Later ducts therefore punch a clean gap through earlier ones where
               they cross, which is the whole crossing treatment — no hop geometry. -->
          <g *ngFor="let d of ducts">
            <path class="duct-casing" [attr.d]="ductD(d.childId)"/>
            <path class="duct" [class.live]="d.live" [attr.d]="ductD(d.childId)"/>
          </g>
          <path *ngFor="let d of openDucts()" class="open-stub" [attr.d]="openStubD(d.childId)"/>
          <g *ngFor="let bd of branchDots()" class="bdot" (pointerdown)="onBranchDotDown($event, bd)">
            <circle [attr.cx]="bd.x" [attr.cy]="bd.y" r="14" fill="transparent"/>
            <circle class="bdot-dot" [attr.cx]="bd.x" [attr.cy]="bd.y" r="3.5"/>
          </g>
          <!-- Output add-dots: hollow rings at each free output (add a run there). -->
          <g *ngFor="let od of outputDots()" class="odot" (pointerdown)="onODotDown($event, od)">
            <circle [attr.cx]="od.x" [attr.cy]="od.y" r="14" fill="transparent"/>
            <circle class="odot-ring" [attr.cx]="od.x" [attr.cy]="od.y" r="5"/>
            <line class="odot-plus" [attr.x1]="od.x - 2.5" [attr.y1]="od.y" [attr.x2]="od.x + 2.5" [attr.y2]="od.y"/>
            <line class="odot-plus" [attr.x1]="od.x" [attr.y1]="od.y - 2.5" [attr.x2]="od.x" [attr.y2]="od.y + 2.5"/>
          </g>

          <g *ngFor="let n of nodes" class="node"
             [class.sel]="n.id === selectedId" [class.live]="n.live" [class.dragging]="n.id === dragId"
             [class.bad]="n.id === dragId && !!dropBlocked"
             [attr.transform]="'translate(' + nx(n) + ',' + ny(n) + ')'" (pointerdown)="startDrag($event, n)">
            <ng-container [ngSwitch]="n.glyph">
              <g *ngSwitchCase="'collector'"><circle class="body" r="30"/><path class="stroke" fill="none" stroke-width="3" stroke-linecap="round" d="M0 -19 a19 19 0 1 0 6 2 l-6 17"/><circle class="fillmuted" r="3.5"/></g>
              <g *ngSwitchCase="'ballvalve'"><circle class="body" r="22"/><line class="stroke" x1="0" y1="-22" x2="0" y2="22" stroke-width="4"/></g>
              <g *ngSwitchCase="'junction'" [class.open-end]="isOpenEnd(n.id)">
                <circle fill="transparent" r="16"/>
                <circle *ngIf="isOpenEnd(n.id)" class="end-ring" r="10"/>
                <rect *ngIf="isCap(n.id)" class="cap" x="-14" y="-5" width="28" height="10" rx="3"/>
                <circle *ngIf="!isCap(n.id)" class="fillmuted dot" r="6"/>
              </g>
              <g *ngSwitchCase="'slidingGate'">
                <rect class="unit" [attr.x]="-GATE_PAD" [attr.y]="-UNIT_H/2" [attr.width]="unitW(n)" [attr.height]="UNIT_H" rx="9"/>
                <rect class="puck" [attr.x]="n.openIndex * CELL - 15" [attr.y]="-UNIT_H/2 + 6" width="30" height="14" rx="4"/>
                <line *ngFor="let x of outletXs(n)" class="stroke" [attr.x1]="x" [attr.y1]="UNIT_H/2" [attr.x2]="x" [attr.y2]="UNIT_H/2 + 12" stroke-width="4"/>
              </g>
              <!-- Manifold = a rounded rotary body: one input (hub, top, over outlet 0) fanning to each output. -->
              <g *ngSwitchCase="'manifold'">
                <rect class="unit" [attr.x]="-GATE_PAD" [attr.y]="-UNIT_H/2" [attr.width]="unitW(n)" [attr.height]="UNIT_H" [attr.rx]="UNIT_H/2"/>
                <line *ngFor="let x of outletXs(n)" class="stroke fan" x1="0" [attr.y1]="-UNIT_H/2 + 3" [attr.x2]="x" [attr.y2]="UNIT_H/2 - 3" stroke-width="2.5"/>
                <circle class="puck" cx="0" [attr.cy]="-UNIT_H/2 + 3" r="6"/>
                <line *ngFor="let x of outletXs(n)" class="stroke" [attr.x1]="x" [attr.y1]="UNIT_H/2" [attr.x2]="x" [attr.y2]="UNIT_H/2 + 12" stroke-width="4"/>
              </g>
              <g *ngSwitchCase="'tool'"><rect class="body" x="-38" y="-24" width="76" height="48" rx="11"/></g>
            </ng-container>
            <!-- Hidden while selected — the editable field (below, outside the SVG)
                 takes its place, in the same spot. -->
            <!-- Hidden only while the editable field is actually on top of it. The
                 old test was "is this selected", but selection and the field are not
                 the same thing — a piece stays selected behind its menu, where the
                 field is suppressed, so the name vanished entirely for that beat. -->
            <text *ngIf="n.glyph !== 'junction' && !isEditingName(n)"
                  class="glabel" [attr.x]="labelX(n)" [attr.y]="labelY(n)">{{ n.name }}</text>
            <text *ngIf="n.glyph === 'tool'" class="gsub" y="42">{{ toolAuto(n.id) ? 'auto' : 'manual' }}</text>
            <!-- A gate that isolates nothing. Stated on the piece, in the same place
                 a tool says auto/manual, because it's a property of the piece — not
                 an error, so it never turns the guide bar red. -->
            <text *ngIf="n.redundant" class="gsub redundant"
                  [attr.x]="labelX(n)" [attr.y]="redundantY(n)">redundant</text>
            <!-- Setup state of a gate, so an unfinished shop reads at a glance rather
                 than only when the Live view refuses to run. -->
            <!-- Tap target for configuring this gate. The dot already SAID a gate
                 needed setting up; making it the button is what let the separate
                 /gates screen go away. Radius 14 hit area over a 7px dot — a
                 fingertip on a phone is nowhere near 7px. -->
            <g *ngIf="n.setup" class="todo-hit"
               [attr.transform]="'translate(' + todoX(n) + ',' + todoY(n) + ')'"
               (pointerdown)="onSetupDot($event, n)">
              <title>{{ n.setup === 'done' ? 'Set up — tap to adjust' : 'Tap to set up' }}</title>
              <circle r="14" fill="transparent"/>
              <circle [class.todo]="n.setup === 'todo'" [class.done]="n.setup === 'done'" r="7"/>
              <path *ngIf="n.setup === 'done'" class="tick" d="M-3.2 0 L-1 2.4 L3.2 -2.4"/>
            </g>
            <!-- Remove: a red (−) on the selected piece, in place of the old dialog.
                 Devices only — run ends delete from their own menu. -->
            <g *ngIf="n.id === selectedId && n.glyph !== 'collector' && n.glyph !== 'junction'" class="rm" [class.off]="!canDelete(n)"
               [attr.transform]="'translate(' + rmX(n) + ',' + rmY(n) + ')'"
               (pointerdown)="onRemove($event, n)">
              <title>{{ deleteHint(n) }}</title>
              <circle class="rm-bg" r="11"/>
              <line class="rm-line" x1="-5" y1="0" x2="5" y2="0"/>
            </g>
          </g>
        </svg>

        <!-- The selected piece's name, edited exactly where it's drawn. A real input
             rather than an SVG <foreignObject>: one inside a foreignObject takes focus
             but never receives keystrokes here, so the field looked live and swallowed
             everything typed into it. It lives inside the scrolling canvas and is
             placed in BOARD coordinates, so it stays on its glyph when you pan. -->
        <input *ngIf="namedPiece() as np" class="nameedit" placeholder="Name"
               [style.left.px]="namePos().left" [style.top.px]="namePos().top" [style.width.px]="nameW(np)"
               [ngModel]="np.name" (ngModelChange)="rename(np.id, $event)"
               (keydown.enter)="blurName($event)"/>
      </div>
    </div>

    <div class="backdrop" *ngIf="menu" (pointerdown)="closeMenu()"></div>
    <!-- ONE context menu for every add point (open end, free outlet, mid-run branch).
         Always the same five fittings + Cap; whatever doesn't apply here is greyed
         with the reason, so the list never shifts under you. -->
    <div class="menu" *ngIf="menu" [style.left.px]="menu.x" [style.top.px]="menu.y">
      <div class="menu-sect">{{ menuTitle }}</div>
      <button *ngFor="let o of menuOptions" [disabled]="!o.enabled" (click)="choose(o.kind)"
              [class.danger]="o.kind === 'delete'" [title]="o.note || ''">
        <span [innerHTML]="iconFor(o.kind)"></span>{{ o.label }}
        <span class="note" *ngIf="o.note">{{ o.note }}</span>
      </button>
    </div>

    <!-- Outlet count, anchored right above the selected unit (not a bottom bar).
         This is ALL the inspector is now: how many outlets a sliding gate has, which
         is the one property with nowhere else to live. Setting a piece up — a gate's
         calibration, a tool's or the collector's plug — is the badge and the tap
         menu, so it doesn't also need a button floating over the thing you're
         looking at. Hidden while a menu is open so the two never stack. -->
    <div class="inspector" *ngIf="inspectedPiece() as ins" [style.left]="inspectorPos().left" [style.top]="inspectorPos().top">
      <span class="meta" *ngIf="ins.glyph === 'slidingGate'">
        <button class="step" (click)="changeOutlets(ins.id, -2)" [disabled]="!canRemoveOutlets(ins)">−</button>
        {{ ins.branchCount }} outlets
        <button class="step" (click)="changeOutlets(ins.id, 2)">+</button>
      </span>
      <span class="meta" *ngIf="ins.glyph === 'manifold'">2 outlets</span>
    </div>

    <!-- Configuring one gate — a sheet over the canvas, so the layout stays put. -->
    <div class="sheet-bg" *ngIf="configuring" (pointerdown)="configuring = null"></div>
    <div class="sheet" *ngIf="configuring as cfg">
      <app-selector-config [sel]="cfg" [topo]="topoDoc"
                           (saved)="onConfigured($event)" (cancelled)="configuring = null">
      </app-selector-config>
    </div>

    <!-- Pairing a tool (or the collector) with its plug — same sheet as a gate. -->
    <div class="sheet-bg" *ngIf="outletTool" (pointerdown)="outletTool = null"></div>
    <div class="sheet" *ngIf="outletTool as ot">
      <app-element-outlet-config [element]="ot" [mode]="outletMode"
                                 [excludeIps]="outletExcludeIps"
                                 [excludeReason]="outletExcludeReason"
                                 (saved)="onOutletConfigured($event)" (cancelled)="outletTool = null">
      </app-element-outlet-config>
    </div>
  `,
})
export class BuildComponent implements OnInit, AfterViewInit, OnDestroy {
  @ViewChild('svg') svgRef?: ElementRef<SVGSVGElement>;
  @ViewChild('wrap') wrapRef?: ElementRef<HTMLDivElement>;

  nodes: NodeVM[] = [];
  ducts: DuctVM[] = [];
  selectedId: string | null = null;
  dragId: string | null = null;
  dirty = false;
  saving = false;
  saveError = '';
  saveNote = '';
  /** Structural gap that stops the controller accepting the doc (draft kept here). */
  wip = '';
  /** The gate whose config sheet is open, or null. */
  configuring: ConfigurableSelector | null = null;
  /** The tool whose smart plug is being paired, as an editable copy. */
  outletTool: RawEl | null = null;
  outletMode: 'sensor' | 'switch' = 'sensor';
  outletExcludeIps: string[] = [];
  outletExcludeReason: Record<string, string> = {};
  private past: string[] = [];
  private future: string[] = [];
  private lastTag: string | null = null;
  airflowErrors: AirflowIssue[] = [];
  vbW = 400; vbH = 300;
  /** Solves every duct on the board, memoized on the scene — see routing/router.ts. */
  private readonly router = new Router();
  /** Why the cell under a drag won't take the piece, or '' while it will. Shown live
   *  in the guidance bar so a refused drop is never a silent one. */
  dropBlocked = '';
  /** Snapped cell under the pointer mid-drag; drives the target-cell highlight. */
  hoverCell: Cell | null = null;
  menu: { x: number; y: number; branch?: BDot; end?: string; convert?: string;
          addOutput?: { parentId: string; branchId?: string; cell: Cell } } | null = null;
  /** The one option list, resolved once when the menu opens (see openMenu). */
  menuOptions: MenuOption[] = [];
  menuTitle = '';
  readonly CELL = CELL; readonly UNIT_H = UNIT_H; readonly GATE_PAD = GATE_PAD; readonly PAD = PAD;

  private icons: Record<string, SafeHtml> = {};

  private topo: Topology | null = null;
  private cells = new Map<string, Cell>();
  private parentOf = new Map<string, string>();
  private outletOf = new Map<string, { unitId: string; index: number }>();  // tool → its unit outlet
  private byId = new Map<string, NodeVM>();
  private grab = { dx: 0, dy: 0 };
  private counter = 0;
  private moveH = (e: PointerEvent) => this.onMove(e);
  private upH = (e: PointerEvent) => this.onUp(e);
  private bdrag: { bd: BDot; x0: number; y0: number; moved: boolean } | null = null;
  private bMove = (e: PointerEvent) => this.onBDotMove(e);
  private bUp = (e: PointerEvent) => this.onBDotUp(e);
  private odrag: { od: ODot; x0: number; y0: number; moved: boolean } | null = null;
  private oMove = (e: PointerEvent) => this.onODotMove(e);
  private oUp = (e: PointerEvent) => this.onODotUp(e);

  constructor(private api: ApiService, sanitizer: DomSanitizer) {
    const svg = (inner: string): SafeHtml =>
      sanitizer.bypassSecurityTrustHtml(`<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round">${inner}</svg>`);
    this.icons = {
      duct:          svg('<path d="M4 12h16" stroke-dasharray="3 3"/><circle cx="20" cy="12" r="2.5" fill="currentColor" stroke="none"/>'),
      tool:          svg('<rect x="5" y="7" width="14" height="10" rx="2"/>'),
      linear:        svg('<rect x="3" y="9" width="18" height="7" rx="2"/><line x1="8" y1="16" x2="8" y2="20"/><line x1="14" y1="16" x2="14" y2="20"/>'),
      servoGate:     svg('<circle cx="12" cy="12" r="7"/><line x1="12" y1="5" x2="12" y2="19"/>'),
      servoManifold: svg('<circle cx="12" cy="12" r="7"/><path d="M8 15l4-4 4 4"/>'),
      cap:           svg('<path d="M6 12h9"/><rect x="15" y="8" width="3" height="8" rx="1" fill="currentColor" stroke="none"/>'),
      uncap:         svg('<path d="M5 12h9"/><circle cx="18" cy="12" r="2.5"/>'),
      delete:        svg('<path d="M6 7h12M10 7V5h4v2M9 7l1 12h4l1-12"/>'),
      configure:     svg('<path d="M4 8h10M18 8h2M4 16h4M12 16h8"/><circle cx="16" cy="8" r="2"/><circle cx="10" cy="16" r="2"/>'),
      // A wall socket, for the smart-plug row.
      outlet:        svg('<rect x="4" y="4" width="16" height="16" rx="3"/><circle cx="9.5" cy="11" r="1.1" fill="currentColor" stroke="none"/><circle cx="14.5" cy="11" r="1.1" fill="currentColor" stroke="none"/><path d="M9 16h6"/>'),
    };
  }

  async ngOnInit(): Promise<void> {
    let loaded: Topology | null = null;
    // Wait for the API key first — a 401 here is indistinguishable from "fresh device"
    // below, and would quietly replace a saved shop with a blank canvas.
    try { await this.api.whenReady(); } catch { /* device unreachable; fall through */ }
    try { loaded = JSON.parse(JSON.stringify(await this.api.getTopology())) as Topology; }
    catch { loaded = null; }   // 404 (fresh device) / no device reachable → start fresh
    // The canvas needs a collector to build outward from — everything else hangs
    // off it. A shop with none is unusable (empty canvas), so seed a blank one on
    // a fresh system. Left un-dirty: it's the default starting point, not an edit,
    // so Save stays disabled until the user actually adds a gate.
    this.topo = loaded && this.elems(loaded).some(e => e['type'] === 'collector')
      ? loaded
      : this.blankTopology();
    this.buildGraph(this.topo);
    const saved = this.savedLayout(this.topo);
    if (saved) for (const [id, c] of Object.entries(saved)) this.cells.set(id, c);
    else this.autoLayoutInto(this.cells);
    this.syncNodes();
    try { this.applyLive(await this.api.getV2Status()); } catch { /* not running */ }
  }
  /** The wrap's size isn't known until the view exists — size the board to it then
   *  (deferred a tick so we're not writing bindings mid-check). */
  ngAfterViewInit(): void { setTimeout(() => this.recomputeExtent()); }

  @HostListener('window:resize')
  onResize(): void { this.recomputeExtent(); }

  ngOnDestroy(): void {
    this.detachDrag();
    window.removeEventListener('pointermove', this.bMove);
    window.removeEventListener('pointerup', this.bUp);
    window.removeEventListener('pointermove', this.oMove);
    window.removeEventListener('pointerup', this.oUp);
  }

  iconFor(kind: string): SafeHtml { return this.icons[kind]; }

  // ── geometry ────────────────────────────────────────────────────────────────
  nx(n: NodeVM): number { return n.dragX ?? (PAD + n.col * CELL); }
  ny(n: NodeVM): number { return n.dragY ?? (PAD + n.row * CELL); }
  unitW(n: NodeVM): number { return (n.span - 1) * CELL + 2 * GATE_PAD; }
  outletXs(n: NodeVM): number[] { return Array.from({ length: n.span }, (_, i) => i * CELL); }
  isUnitChild(id: string): boolean { return this.outletOf.has(id); }

  /** The scene the router solves: every glyph at its resolved position, every duct
   *  with the endpoint it hangs off. Built fresh each call — it's cheap, and the
   *  Router memoizes on a hash of it, so nothing re-solves unless something moved. */
  private scene(): Scene {
    const nodes: SceneNode[] = this.nodes.map(n => ({
      id: n.id, glyph: n.glyph, isUnit: n.isUnit, span: n.span,
      x: this.routeX(n), y: this.routeY(n),
    }));
    const ducts = this.ducts.map(d => ({
      childId: d.childId,
      parentId: this.parentOf.get(d.childId),
      outlet: this.outletOf.get(d.childId),
    }));
    return { nodes, ducts, bounds: sceneBounds(nodes) };
  }

  /** Where the router thinks a node is. A dragged glyph tracks the pointer visually,
   *  but routes off the SNAPPED cell — so a run re-solves when the pointer crosses a
   *  cell boundary, not on every pixel of travel. That alone is most of why dragging
   *  used to degrade the further you went. */
  private routeX(n: NodeVM): number {
    return n.dragX == null ? PAD + n.col * CELL : PAD + Math.max(0, Math.round((n.dragX - PAD) / CELL)) * CELL;
  }
  private routeY(n: NodeVM): number {
    return n.dragY == null ? PAD + n.row * CELL : PAD + Math.max(0, Math.round((n.dragY - PAD) / CELL)) * CELL;
  }

  /** Solved routes for the whole board. During a drag every duct NOT attached to the
   *  dragged node is frozen at its committed path, so the rest of the picture holds
   *  still while the pointer moves. */
  private routes(): ReadonlyMap<string, RoutedDuct> {
    return this.router.routes(this.scene(), this.frozenDucts());
  }
  private frozenDucts(): ReadonlySet<string> | undefined {
    if (!this.dragId) return undefined;
    const moving = this.dragId;
    const frozen = new Set<string>();
    for (const d of this.ducts) {
      if (d.childId === moving || this.parentOf.get(d.childId) === moving || this.outletOf.get(d.childId)?.unitId === moving) continue;
      frozen.add(d.childId);
    }
    return frozen;
  }

  /** The route the whole app reads: the drawn path, branch dots, the hit target and
   *  the device-crossing check all come through here, so they can't disagree. */
  private ductPoints(childId: string): Pt[] {
    return this.routes().get(childId)?.pts ?? [];
  }

  /** True when a duct is boxed in and only has the straight-dogleg fallback to show. */
  private ductBoxedIn(childId: string): boolean {
    const r = this.routes().get(childId);
    return !!r && !r.ok;
  }

  private halfW(n: NodeVM): number { return glyphHalfW(this.sceneNode(n)); }
  /** Approximate half-height of a glyph, for anchoring badges to its edges. */
  private halfH(n: NodeVM): number { return glyphHalfH(this.sceneNode(n)); }
  private sceneNode(n: NodeVM): SceneNode {
    return { id: n.id, glyph: n.glyph, isUnit: n.isUnit, span: n.span, x: this.nx(n), y: this.ny(n) };
  }

  /** Plain ortho path (no hops) — used for the fat invisible hit target. */
  ductPath(childId: string): string {
    const p = this.ductPoints(childId); if (!p.length) return '';
    return 'M ' + p.map(pt => `${pt.x} ${pt.y}`).join(' L ');
  }

  /** Visible path, with corners arced (radius CORNER_R) so two ducts whose corners
   *  land near the same point curve apart with a gap instead of overlapping into an X.
   *
   *  Crossings are NOT drawn here any more. The old version bumped a horizontal over
   *  every vertical it met, which meant walking every other duct's route from inside
   *  this one — O(n²) per frame — for a 5px arc that read as a wobble on a 6px line.
   *  A crossing is now a gap punched by the casing stroke (see .duct-casing), which
   *  needs no geometry at all. */
  ductD(childId: string): string {
    const pts = this.ductPoints(childId); if (!pts.length) return '';
    if (pts.length === 1) return `M ${pts[0].x} ${pts[0].y}`;
    const CORNER_R = 12;
    const dist = (p: Pt, q: Pt) => Math.hypot(q.x - p.x, q.y - p.y);
    const toward = (from: Pt, to: Pt, r: number) => {
      const L = dist(from, to) || 1; return { x: from.x + (to.x - from.x) / L * r, y: from.y + (to.y - from.y) / L * r };
    };
    const n = pts.length;
    let d = `M ${pts[0].x} ${pts[0].y}`;
    for (let i = 1; i < n; i++) {
      if (i === n - 1) { d += ` L ${pts[i].x} ${pts[i].y}`; break; }
      const r = Math.min(CORNER_R, dist(pts[i - 1], pts[i]) / 2, dist(pts[i], pts[i + 1]) / 2);
      const before = toward(pts[i], pts[i - 1], r), after = toward(pts[i], pts[i + 1], r);
      d += ` L ${before.x} ${before.y} Q ${pts[i].x} ${pts[i].y} ${after.x} ${after.y}`;
    }
    return d;
  }

  openDucts(): DuctVM[] { return this.ducts.filter(d => d.open); }

  /** Short dashed accent path covering only the LAST stretch of an open run, so the
   *  "bare / unfinished" cue lives at the end instead of along the whole pipe. */
  openStubD(childId: string): string {
    const pts = this.ductPoints(childId); if (pts.length < 2) return '';
    const end = pts[pts.length - 1], prev = pts[pts.length - 2];
    const dx = end.x - prev.x, dy = end.y - prev.y;
    const len = Math.hypot(dx, dy) || 1;
    const stub = Math.min(30, len);
    return `M ${end.x - (dx / len) * stub} ${end.y - (dy / len) * stub} L ${end.x} ${end.y}`;
  }

  /** Top-right corner of the glyph, in the node's own (translated) coordinates. */
  rmX(n: NodeVM): number { return n.isUnit ? (n.span - 1) * CELL + GATE_PAD : this.halfW(n); }
  rmY(n: NodeVM): number { return -this.halfH(n); }
  /** The setup badge sits on the opposite corner from the (−) so the two never overlap
   *  on a selected gate. A unit's group origin is its FIRST outlet, not its centre —
   *  hence the same asymmetry rmX has, mirrored. */
  todoX(n: NodeVM): number { return n.isUnit ? -GATE_PAD : -this.halfW(n); }
  todoY(n: NodeVM): number { return -this.halfH(n); }
  /** The red (−): remove this piece. Swallows the event so it doesn't start a drag. */
  onRemove(evt: PointerEvent, n: NodeVM): void {
    evt.preventDefault(); evt.stopPropagation();
    if (!this.canDelete(n)) return;
    this.selectedId = n.id;
    this.deleteSelected();
  }

  labelX(n: NodeVM): number { return n.isUnit ? (n.span - 1) * CELL / 2 : 0; }
  /** Below the glyph AND below its add-dot, which sits at halfH + 18 — the two used
   *  to land on top of each other on a ball valve. */
  redundantY(n: NodeVM): number { return this.halfH(n) + 34; }
  labelY(n: NodeVM): number { return n.glyph === 'tool' ? 4 : (n.isUnit ? -UNIT_H / 2 - 9 : -34); }
  toolAuto(id: string): boolean { return !!(this.elem(id)?.['sensor'] as RawEl | undefined)?.['outlet']; }
  /** Does this piece have a plug paired — sensed for a tool, switched for the
   *  collector? Drives the one button's label for both. */
  /** Plug paired? Sensed for a tool, switched for the collector — one question,
   *  which is why the badge can be one badge. */
  private hasPlugEl(el: RawEl): boolean {
    const branch = (el['type'] === 'collector' ? el['control'] : el['sensor']) as RawEl | undefined;
    return !!branch?.['outlet'];
  }
  /** A junction with no children and not capped = an unpopulated open duct end. */
  isOpenEnd(id: string): boolean { const e = this.elem(id); return e?.['type'] === 'junction' && !e['capped'] && this.childrenOf(id).length === 0; }
  /** A junction the user has explicitly sealed. */
  isCap(id: string): boolean { return !!this.elem(id)?.['capped']; }
  /** Seal an open end so it's a finished terminal, not a leaking open run. */
  capEnd(id: string): void {
    const el = this.elem(id); if (!el || el['type'] !== 'junction') return;
    el['capped'] = true; el['name'] = 'Cap'; this.afterMutation(id);
  }
  /** Take the cap back off — the end is live pipe again, ready to build on. */
  uncapEnd(id: string): void {
    const el = this.elem(id); if (!el || el['type'] !== 'junction') return;
    delete el['capped']; el['name'] = 'Open end'; this.afterMutation(id);
  }
  /** Delete a specific element (the menu's Delete, for ends that have no (−)). */
  private removeAt(id: string): void {
    if (!this.topo) return;
    this.removeElement(id);
    this.selectedId = null; this.dirty = true; this.saveError = '';
    this.buildGraph(this.topo); this.syncNodes(); this.refreshHandles();
  }
  private openEndCount(): number { return this.ducts.filter(d => d.open).length; }

  // ── selection / handles ───────────────────────────────────────────────────────
  inspected(): NodeVM | null { return this.nodes.find(n => n.id === this.selectedId) ?? null; }
  /** What the inspector shows: a named piece, never a run end, never behind a menu. */
  /** The inspector only appears when it has something in it — and that is now only
   *  an outlet count, so only the units have one. Everything else moved out: the
   *  name onto the node, setup onto the badge and the tap menu. Anything else
   *  selected would float an empty box over itself. */
  inspectedPiece(): NodeVM | null {
    const n = this.inspected();
    if (!n || this.menu) return null;
    return (n.glyph === 'slidingGate' || n.glyph === 'manifold') ? n : null;
  }

  /** The piece whose name is editable right now. Junctions have no name, and the
   *  field hides behind a menu the same way the inspector does. */
  namedPiece(): NodeVM | null {
    const n = this.inspected();
    return n && n.glyph !== 'junction' && !this.menu ? n : null;
  }

  /** Is the editable field currently sitting on this piece's label? The drawn label
   *  hides exactly when this is true, so the two can never both be absent. */
  isEditingName(n: NodeVM): boolean { return this.namedPiece()?.id === n.id; }

  /** Width of the in-place name field, sized to the piece it sits on. */
  nameW(n: NodeVM): number {
    if (n.glyph === 'tool') return 74;      // just inside the 76-wide body
    return n.isUnit ? 150 : 118;
  }

  /** Client-space centre of the selected piece's LABEL, so the editable field lands
   *  on the text it replaces rather than near it. The glyph's own y is the text
   *  baseline; back off ~4px to get its visual middle. */
  /** Where the name field sits, in BOARD pixels — the same coordinate space the SVG
   *  is drawn in, since it renders 1:1 at vbW x vbH inside the scrolling wrapper.
   *  Deliberately not screen coordinates: those go stale the moment the user pans. */
  namePos(): { left: number; top: number } {
    const n = this.namedPiece();
    if (!n) return { left: -9999, top: -9999 };
    return { left: this.nx(n) + this.labelX(n), top: this.ny(n) + this.labelY(n) - 4 };
  }

  /** Enter commits by leaving the field; the value is already saved per keystroke. */
  blurName(e: Event): void { (e.target as HTMLElement | null)?.blur(); }

  /** The doc, for the config sheet's bindings only — `configuring` is never set unless
   *  a topology is loaded, so the non-null assertion holds. */
  get topoDoc(): Topology { return this.topo!; }

  /** Open the gate config straight from its dot, without selecting-then-tapping.
   *  Must swallow the event: the node group under it starts a drag on pointerdown,
   *  so without this a tap on the dot moves the gate instead of configuring it. */
  onSetupDot(evt: PointerEvent, n: NodeVM): void {
    evt.preventDefault(); evt.stopPropagation();
    this.selectedId = n.id;
    // The badge is now the handle for the whole piece, not just its setup: it opens
    // the same menu the body used to, which already carried setup alongside the
    // conversions. That leaves the body free to mean "rename me".
    this.openMenu(evt.clientX, evt.clientY, { convert: n.id });
  }

  configure(id: string): void {
    const el = this.elems(this.topo!).find(e => e['id'] === id) as AnyElement | undefined;
    if (isServoSelector(el) || isLinearSelector(el)) this.configuring = el as unknown as ConfigurableSelector;
  }

  /** Open the smart-plug sheet for one tool. Edits a COPY, so cancelling leaves the
   *  doc untouched — same contract as the gate sheet. */
  configureOutlet(id: string): void {
    const el = this.elems(this.topo!).find(e => e['id'] === id) as RawEl | undefined;
    if (!el || (el['type'] !== 'tool' && el['type'] !== 'collector')) return;
    // A tool's plug is a sensor (we watch its draw); the collector's is a switch
    // (we command it). Same picker, different field — see the sheet's header.
    this.outletMode = el['type'] === 'collector' ? 'switch' : 'sensor';
    this.outletTool = JSON.parse(JSON.stringify(el)) as RawEl;
    // Computed once, on open, rather than from the template: a getter would hand
    // the child freshly-allocated arrays on every change-detection pass.
    const ex = this.outletExcludes(id);
    this.outletExcludeIps = ex.ips;
    this.outletExcludeReason = ex.reason;
  }

  /** Plugs that can't be picked for `toolId`, and why. One physical outlet driving
   *  two tools would make the routing brain believe two machines started at once;
   *  the collector's own switch is off-limits for the obvious reason. */
  private outletExcludes(me: string): { ips: string[]; reason: Record<string, string> } {
    const ips: string[] = [];
    const reason: Record<string, string> = {};
    for (const e of this.elems(this.topo!)) {
      const el = e as RawEl;
      if (el['type'] === 'collector') {
        // …unless the collector IS what's being configured: its own plug has to
        // stay pickable, or re-opening the sheet greys out the current choice.
        if (el['id'] === me) continue;
        const dc = ((el['control'] as RawEl | undefined)?.['outlet'] as RawEl | undefined)?.['ip'] as string | undefined;
        if (dc) { ips.push(dc); reason[dc] = 'reserved — dust collector'; }
        continue;
      }
      if (el['type'] !== 'tool' || el['id'] === me) continue;
      const ip = ((el['sensor'] as RawEl | undefined)?.['outlet'] as RawEl | undefined)?.['ip'] as string | undefined;
      if (ip) { ips.push(ip); reason[ip] = `already paired with ${(el['name'] as string) || 'another tool'}`; }
    }
    return { ips, reason };
  }

  /** Splice the paired tool back in. Shares onConfigured's path deliberately: a
   *  sensor change is a topology edit like any other and gets the same validation,
   *  history entry and save. */
  onOutletConfigured(updated: RawEl): void {
    if (!this.topo) return;
    const els = this.elems(this.topo);
    const i = els.findIndex(e => e['id'] === updated['id']);
    if (i < 0) return;
    this.pushHistory(updated['id'] as string);
    els[i] = updated as unknown as (typeof els)[number];
    this.outletTool = null;
    this.dirty = true;
    this.buildGraph(this.topo); this.syncNodes(); this.refreshHandles();
    void this.save();
  }

  /** Fold the configured gate back into the doc and persist through the normal save,
   *  so it goes through the same validation and work-in-progress handling. */
  onConfigured(updated: ConfigurableSelector): void {
    if (!this.topo) return;
    const els = this.elems(this.topo);
    const i = els.findIndex(e => e['id'] === updated.id);
    if (i < 0) return;
    this.pushHistory(updated.id);
    els[i] = updated as unknown as (typeof els)[number];
    this.configuring = null;
    this.dirty = true;
    this.buildGraph(this.topo); this.syncNodes(); this.refreshHandles();
    void this.save();
  }

  /** Client-space position for the floating inspector — centered just above the node. */
  inspectorPos(): { left: string; top: string } {
    const n = this.inspected(); const svg = this.svgRef?.nativeElement;
    const m = svg?.getScreenCTM();
    if (!n || !svg || !m) return { left: '-9999px', top: '-9999px' };
    const pt = svg.createSVGPoint();
    pt.x = this.nx(n) + this.labelX(n);
    // Clear the name field as well as the glyph. Every piece except a tool draws its
    // label ABOVE the body, which is now an input box sitting in that gap — without
    // this the inspector landed on top of it and clipped the name being typed.
    const nameGap = n.glyph === 'tool' ? 0 : 24;
    pt.y = this.ny(n) - (n.isUnit ? UNIT_H / 2 : (n.glyph === 'tool' ? TOOL_HALF : 22)) - nameGap;
    const s = pt.matrixTransform(m);
    return { left: `${s.x}px`, top: `${s.y}px` };
  }

  /** Delete removes the selected element; Ctrl/Cmd-Z steps history. Both ignored
   *  while a text field has focus (that's the browser's own undo). */
  @HostListener('window:keydown', ['$event'])
  onKeydown(e: KeyboardEvent): void {
    const tag = (document.activeElement?.tagName ?? '').toUpperCase();
    if (tag === 'INPUT' || tag === 'TEXTAREA') return;
    const key = e.key.toLowerCase();
    if (e.metaKey || e.ctrlKey) {
      if (key === 'z' && !e.shiftKey) { e.preventDefault(); this.undo(); return; }
      if ((key === 'z' && e.shiftKey) || key === 'y') { e.preventDefault(); this.redo(); return; }
      return;
    }
    if (e.key !== 'Delete' && e.key !== 'Backspace') return;
    const n = this.inspected();
    if (n && this.canDelete(n)) { e.preventDefault(); this.deleteSelected(); }
  }

  /** The + side-handles were retired in favor of duct-first drawing; this stays as a
   *  no-op hook so the many selection/mutation call sites don't each need editing. */
  private refreshHandles(): void { /* handles retired */ }

  deselect(): void { this.selectedId = null; this.closeMenu(); }

  // ── drag (reposition) / tap ───────────────────────────────────────────────────
  startDrag(evt: PointerEvent, n: NodeVM): void {
    evt.preventDefault(); evt.stopPropagation();
    this.selectedId = n.id; this.menu = null;
    this.dragId = n.id;
    const pt = this.toSvg(evt);
    this.grab = { dx: pt.x - this.nx(n), dy: pt.y - this.ny(n) };
    window.addEventListener('pointermove', this.moveH);
    window.addEventListener('pointerup', this.upH);
  }
  private onMove(evt: PointerEvent): void {
    const n = this.byId.get(this.dragId ?? ''); if (!n) return;
    const pt = this.toSvg(evt);
    n.dragX = pt.x - this.grab.dx; n.dragY = pt.y - this.grab.dy;
    // Validity is re-checked only when the pointer crosses into a new cell — the
    // check walks every duct, and the answer can't change inside one cell anyway.
    const col = Math.max(0, Math.round((n.dragX - PAD) / CELL));
    const row = Math.max(0, Math.round((n.dragY - PAD) / CELL));
    if (col === this.hoverCell?.col && row === this.hoverCell?.row) return;
    this.hoverCell = { col, row };
    this.dropBlocked = (col === n.col && row === n.row) ? '' : this.placeBlockedBy(n, col, row);
  }
  private onUp(evt: PointerEvent): void {
    const n = this.byId.get(this.dragId ?? '');
    if (n && n.dragX != null && n.dragY != null) {
      const col = Math.max(0, Math.round((n.dragX - PAD) / CELL)), row = Math.max(0, Math.round((n.dragY - PAD) / CELL));
      if (this.canPlace(n, col, row) && (col !== n.col || row !== n.row)) {
        this.pushHistory(null);
        n.col = col; n.row = row; this.cells.set(n.id, { col, row });
        this.dirty = true;
      }
      n.dragX = undefined; n.dragY = undefined;
      this.recomputeExtent();
    } else if (n && n.glyph === 'junction') {
      // A tap (no drag) on a run end — open or capped — opens its menu: what goes
      // here, plus cap/reopen/delete. Ends carry no inspector or (−) of their own.
      this.openMenu(evt.clientX, evt.clientY, { end: n.id });
    }
    // A tap on the body of a piece now does one thing: select it, which puts the
    // editable name on its label. What the piece IS — its kind, its setup, its plug —
    // moved to the badge, because tapping the name to rename it and tapping it to
    // open a menu were the same gesture, and the menu won.
    this.dragId = null; this.hoverCell = null; this.dropBlocked = '';
    this.detachDrag();
  }
  private detachDrag(): void {
    window.removeEventListener('pointermove', this.moveH);
    window.removeEventListener('pointerup', this.upH);
  }
  private toSvg(evt: PointerEvent): { x: number; y: number } {
    const svg = this.svgRef?.nativeElement; if (!svg) return { x: 0, y: 0 };
    const pt = svg.createSVGPoint(); pt.x = evt.clientX; pt.y = evt.clientY;
    const m = svg.getScreenCTM(); if (!m) return { x: 0, y: 0 };
    const r = pt.matrixTransform(m.inverse()); return { x: r.x, y: r.y };
  }

  private canPlace(n: NodeVM, col: number, row: number): boolean {
    return this.placeBlockedBy(n, col, row) === '';
  }

  /** Why `n` can't stand at (col,row) — '' when it can. The wording is what the user
   *  sees in the guidance bar mid-drag, so it names the thing in the way, not the
   *  predicate that failed. */
  private placeBlockedBy(n: NodeVM, col: number, row: number): string {
    const occ = new Map<string, NodeVM>();
    for (const m of this.nodes) {
      if (m.id === n.id) continue;
      if (m.isUnit) for (let i = 0; i < m.span; i++) occ.set((m.col + i) + ',' + m.row, m);
      else occ.set(m.col + ',' + m.row, m);
    }
    const cells: Cell[] = n.isUnit ? Array.from({ length: n.span }, (_, i) => ({ col: col + i, row })) : [{ col, row }];
    const hit = cells.map(c => occ.get(c.col + ',' + c.row)).find(m => m);
    if (hit) {
      return n.isUnit
        ? `${this.pieceLabel(n)} needs ${n.span} free cells in a row — ${hit.name} is in the way.`
        : `${hit.name} is already in that cell.`;
    }
    // No duct may cross a device. Test at the CANDIDATE position (so the moved node's
    // own ducts reroute), then require every device to be clear of every foreign duct —
    // this catches both "device lands on a duct" and "moved duct now runs through
    // another device". Restore position afterwards.
    const sc = n.col, sr = n.row, dx = n.dragX, dy = n.dragY;
    n.col = col; n.row = row; n.dragX = undefined; n.dragY = undefined;
    let blocker: NodeVM | null = null;
    let boxedIn = false;
    for (const m of this.nodes) {
      if (m.glyph === 'junction' || m.glyph === 'collector') continue;   // devices only
      if (this.deviceCrossed(m)) { blocker = m; break; }
    }
    if (!blocker) boxedIn = this.ducts.some(d => this.ductBoxedIn(d.childId));
    n.col = sc; n.row = sr; n.dragX = dx; n.dragY = dy;
    this.router.invalidate();
    if (blocker) return `A duct would have to run through ${blocker.name} to get there.`;
    if (boxedIn) return 'No room for the duct to reach it there.';
    return '';
  }

  /** What to call a piece in a sentence. */
  private pieceLabel(n: NodeVM): string {
    return n.glyph === 'slidingGate' ? 'A sliding gate' : n.glyph === 'manifold' ? 'A manifold' : n.name;
  }
  /** True only if a foreign duct actually runs through m's glyph box (tight margin).
   *  Because routing already skirts devices with clearance, this fires only when a run
   *  is truly boxed in and can't get around — the real "can't place here" case. */
  private deviceCrossed(m: NodeVM): boolean {
    const M = 2;
    const x0 = (m.isUnit ? this.nx(m) - GATE_PAD : this.nx(m) - this.halfW(m)) - M;
    const x1 = (m.isUnit ? this.nx(m) + (m.span - 1) * CELL + GATE_PAD : this.nx(m) + this.halfW(m)) + M;
    const box = { x0, y0: this.ny(m) - this.halfH(m) - M, x1, y1: this.ny(m) + this.halfH(m) + M };
    for (const d of this.ducts) {
      if (d.childId === m.id || this.parentOf.get(d.childId) === m.id) continue;
      const pts = this.ductPoints(d.childId);
      for (let i = 0; i < pts.length - 1; i++) if (segBoxHit(pts[i], pts[i + 1], box)) return true;
    }
    return false;
  }
  /** True if the grid cell centre lies on some OTHER run's duct (within ~0.4 cell) —
   *  ducts connected to `selfId` are exempt (they're meant to reach this device). */
  private cellOnDuct(col: number, row: number, selfId: string): boolean {
    const cx = PAD + col * CELL, cy = PAD + row * CELL, thresh = CELL * 0.4;
    for (const d of this.ducts) {
      if (d.childId === selfId || this.parentOf.get(d.childId) === selfId) continue;
      const pts = this.ductPoints(d.childId);
      for (let i = 0; i < pts.length - 1; i++)
        if (ptSegDist(cx, cy, pts[i], pts[i + 1]) < thresh) return true;
    }
    return false;
  }
  // ── the one context menu ──────────────────────────────────────────────────────
  /** Open the context menu at a click point and resolve its options ONCE, so the
   *  room/validity checks (which walk every duct) don't re-run on every change
   *  detection pass while the menu is up. */
  private openMenu(x: number, y: number, ctx: Partial<NonNullable<BuildComponent['menu']>>): void {
    // A gate keeps its selection so the name/outlet controls are still there once
    // the menu is dismissed; every other context shows the menu alone.
    if (!ctx.convert) this.selectedId = null;
    this.menu = { x, y, ...ctx };
    this.menuTitle = ctx.convert ? this.convertTitle(ctx.convert)
                   : ctx.end ? (this.isCap(ctx.end) ? 'This capped end' : 'At the end of this run')
                   : ctx.branch ? 'Add on this run'
                   : 'Add here';
    this.menuOptions = this.resolveOptions();
  }
  closeMenu(): void { this.menu = null; this.menuOptions = []; }

  /**
   * The cells a fitting of `kind` would need in this context — the single source of
   * truth for both the room check below and the placement that follows, so what the
   * menu promises is exactly where the thing lands. Empty = needs no cell.
   */
  private targetCells(kind: MenuKind, m: NonNullable<BuildComponent['menu']>): { cells: Cell[]; span: number; selfId: string; occ: Set<string> } | null {
    if (kind === 'cap' || kind === 'uncap' || kind === 'delete') return null;
    if (m.end) {
      // The fitting TAKES OVER the end's cell (the 1-child end then collapses away),
      // so the end itself mustn't count as the thing blocking it.
      return { cells: [this.cells.get(m.end) ?? { col: 0, row: 0 }], span: spanFor(kind), selfId: m.end,
               occ: this.occupiedExcept(new Set([m.end])) };
    }
    if (m.addOutput) return { cells: [m.addOutput.cell], span: spanFor(kind), selfId: m.addOutput.parentId, occ: this.cellOccupied() };
    if (m.branch) {
      // A gate splices INTO the run at the dot; a duct tees a leg off to the side.
      const occ = this.cellOccupied();
      if (kind === 'duct') {
        const leg = this.legCellFor(m.branch, m.branch.childId);
        return leg ? { cells: [leg], span: 1, selfId: m.branch.childId, occ } : null;
      }
      return { cells: [{ col: m.branch.col, row: m.branch.row }], span: spanFor(kind), selfId: m.branch.childId, occ };
    }
    return null;
  }

  /** Every option, every time — with the ones that don't apply here greyed and
   *  labelled why:
   *   • Duct — nothing to lay at an END (you drag the end to run more pipe).
   *   • Tool — terminates a run, so never spliced INTO one mid-way.
   *   • Cap — only seals a terminal; you can't cap the middle of a run.
   *   • anything needing a cell — greyed "(no room)" when that exact cell is taken. */
  private resolveOptions(): MenuOption[] {
    const m = this.menu; if (!m) return [];
    if (m.convert) return this.convertOptions(m.convert);
    const mid = !!m.branch;
    const opts: MenuOption[] = FITTINGS.map(f => {
      let enabled = true, note: string | undefined;
      if (f.kind === 'duct' && m.end)    { enabled = false; note = 'drag the end'; }
      else if (f.kind === 'tool' && mid) { enabled = false; note = 'ends a run'; }
      if (enabled) {
        const t = this.targetCells(f.kind, m);
        // An add-dot may point off the negative edge (growing left or up); that's
        // legal — normalizeCells slides the board under it once the piece lands.
        const neg = !!m.addOutput;
        if (!t || !t.cells.every(c => this.roomAt(c.col, c.row, t.span, t.selfId, t.occ, neg))) { enabled = false; note = 'no room'; }
      }
      return { kind: f.kind, label: f.label, enabled, note };
    });
    if (mid) {
      opts.push({ kind: 'cap', label: 'Cap', enabled: false, note: 'not mid-run' });
      return opts;
    }
    if (m.end) {
      // A run end also carries its own housekeeping: seal it, reopen it, or bin it.
      opts.push(this.isCap(m.end)
        ? { kind: 'uncap', label: 'Reopen this end', enabled: true }
        : { kind: 'cap', label: 'Cap this end', enabled: true });
      opts.push({ kind: 'delete', label: 'Delete', enabled: true });
    } else {
      opts.push({ kind: 'cap', label: 'Cap this outlet', enabled: true });
    }
    return opts;
  }

  /** Menu heading for a tap on a placed piece — named after what you tapped. */
  private convertTitle(id: string): string {
    const n = this.byId.get(id);
    if (n?.glyph === 'collector') return 'Change this collector';
    if (n?.glyph === 'tool')      return 'Change this tool';
    return 'Change this gate';
  }

  /** What a tap on a placed piece offers. Gates: setup + the other kinds. Tools and
   *  the collector: their smart outlet, which is the only thing about them that
   *  isn't already said by where they sit. */
  private convertOptions(id: string): MenuOption[] {
    const n = this.byId.get(id); if (!n) return [];
    if (n.glyph === 'tool' || n.glyph === 'collector') {
      const paired = n.setup === 'done';
      return [{
        kind: 'outlet',
        label: paired ? 'Smart outlet' : 'Set up smart outlet',
        enabled: true,
        note: paired ? undefined : (n.glyph === 'collector' ? 'started by hand' : 'switched by hand'),
      }];
    }
    // Tapping a gate is how you get at it, so its setup lives here alongside the
    // conversions — the floating inspector is only reachable right after placing one.
    const opts: MenuOption[] = n.setup
      ? [{ kind: 'configure', label: n.setup === 'todo' ? 'Set up this gate' : 'Gate setup', enabled: true,
           note: n.setup === 'todo' ? 'not done yet' : undefined }]
      : [];
    return opts.concat(this.gateTypes(n).map(t => ({
      kind: t.kind, label: t.current ? `${t.label} (current)` : t.label,
      enabled: t.enabled, note: t.note,
    })));
  }

  /** Route a chosen option to the right primitive for the context it was opened in. */
  choose(kind: MenuKind): void {
    const m = this.menu; if (!m || !this.topo) return;
    if (!this.menuOptions.find(o => o.kind === kind)?.enabled) return;
    this.pushHistory(null);
    if (m.convert) {
      const id = m.convert; this.closeMenu();
      if (kind === 'configure') this.configure(id);
      else if (kind === 'outlet') this.configureOutlet(id);
      else this.convertKind(id, kind as SelKind);
      return;
    }
    if (m.end) {
      const id = m.end; this.closeMenu();
      if (kind === 'cap') this.capEnd(id);
      else if (kind === 'uncap') this.uncapEnd(id);
      else if (kind === 'delete') this.removeAt(id);
      else this.fillEnd(id, kind as SelKind | 'tool');
      return;
    }
    if (m.addOutput) { this.addAtOutput(kind); return; }
    if (m.branch) {
      const bd = m.branch; this.closeMenu();
      if (kind === 'duct') this.branchDuct(bd, 'duct');
      else this.insertInline(bd.childId, kind as SelKind, { col: bd.col, row: bd.row });
    }
  }

  // ── add / mutate ──────────────────────────────────────────────────────────────
  /** Turn an existing open end into a tool or a gate. A tool terminates the run; a
   *  gate/manifold/slide splices in where the end was (the 1-child open end then
   *  collapses onto its parent) and seeds ONE continuation open end so the run keeps
   *  going. The gate's OTHER outlets stay blocked → they surface as add-dots. */
  private fillEnd(endId: string, kind: SelKind | 'tool'): void {
    if (!this.topo) return;
    const at = this.cells.get(endId) ?? { col: 0, row: 0 };
    if (kind === 'tool') {
      const t = this.addTool(endId); if (!t) return;    // child of the end; the end then collapses away
      this.cells.set(t, { col: at.col, row: at.row });
      this.afterMutation(t);
      return;
    }
    const selId = this.addSelector(endId, kind); if (!selId) return;
    this.cells.set(selId, { col: at.col, row: at.row });
    // One continuation so the run keeps going — but only if the cell straight below
    // is actually free. If it isn't, skip it: the outlet stays a blocked add-dot,
    // which beats refusing the whole gate over a byproduct.
    const below = { col: at.col, row: at.row + 1 };
    if (this.roomAt(below.col, below.row, 1, endId, this.occupiedExcept(new Set([endId])))) {
      const cont = this.addOpenEnd(selId);            // other outlets remain add-dots
      if (cont) this.placeAt(cont, below);
    }
    this.afterMutation(selId);
  }
  /** Run a bare duct from a node to a fresh OPEN END (a childless junction) — the
   *  "lay pipe first, populate later" primitive. For a selector parent it takes the
   *  first free outlet; use addOpenEndOn to target a specific one. */
  private addOpenEnd(parentId: string): string | null {
    const p = this.elem(parentId);
    if (p && p['type'] === 'selector') {
      const b = (p['branches'] as Branch[]).find(x => x.role === 'blocked');
      if (!b) return null;
      return this.addOpenEndOn(parentId, b.id);
    }
    return this.addOpenEndOn(parentId);
  }
  /** Open end off a specific output: a named selector outlet (branchId → role feed)
   *  or, with no branchId, straight off a collector/junction. */
  private addOpenEndOn(parentId: string, branchId?: string): string | null {
    if (!this.topo) return null;
    const els = this.elems(this.topo);
    const j: RawEl = { id: this.newId('j'), type: 'junction', name: 'Open end' };
    els.push(j);
    const duct: RawEl = { child: j['id'], parent: parentId };
    if (branchId) {
      const b = (this.elem(parentId)?.['branches'] as Branch[] | undefined)?.find(x => x.id === branchId);
      if (!b || b.role !== 'blocked') { els.pop(); return null; }
      b.role = 'feed'; duct['parentBranch'] = branchId;
    }
    this.ductsRaw().push(duct);
    return j['id'] as string;
  }

  // ── output add-dots: a dot at every FREE output (collector always; each blocked
  // selector outlet). Click → menu (add a branch/gate/valve/manifold/cap/tool there);
  // drag → tee out a passive open-end leg. This is how you add more runs off the
  // collector or off a gate/manifold/slider now that the + handles are gone.
  outputDots(): ODot[] {
    const out: ODot[] = [];
    if (this.dragId || this.bdrag || this.odrag) return out;
    for (const n of this.nodes) {
      const el = this.elem(n.id);
      if (n.glyph === 'collector') {
        // One ⊕ per free side, each aimed at a cell that is genuinely free. The old
        // single dot was drawn to the right but committed to (col+1, row+1) — under
        // the 4-wide main gate in the demo layout, so every menu item read "no room"
        // while (1,0) sat empty beside it.
        const hw = this.halfW(n), hh = this.halfH(n);
        const sides: Array<{ dx: number; dy: number; x: number; y: number }> = [
          { dx: 1, dy: 0, x: this.nx(n) + hw + 16, y: this.ny(n) },
          { dx: -1, dy: 0, x: this.nx(n) - hw - 16, y: this.ny(n) },
          { dx: 0, dy: 1, x: this.nx(n), y: this.ny(n) + hh + 16 },
        ];
        for (const s of sides) {
          const cell = this.firstFreeCellToward(n, s.dx, s.dy, 1, n.id);
          if (cell) out.push({ x: s.x, y: s.y, parentId: n.id, cell });
        }
      } else if (el?.['type'] === 'selector') {
        const branches = (el['branches'] as Branch[]) ?? [];
        branches.forEach((b, i) => {
          if (b.role !== 'blocked') return;
          const x = n.isUnit ? this.nx(n) + i * CELL : this.nx(n);
          const y = this.ny(n) + this.halfH(n) + 18;
          out.push({ x, y, parentId: n.id, branchId: b.id, cell: { col: n.col + (n.isUnit ? i : 0), row: n.row + 1 } });
        });
      }
    }
    return out;
  }
  onODotDown(evt: PointerEvent, od: ODot): void {
    evt.preventDefault(); evt.stopPropagation();
    this.odrag = { od, x0: evt.clientX, y0: evt.clientY, moved: false };
    window.addEventListener('pointermove', this.oMove);
    window.addEventListener('pointerup', this.oUp);
  }
  private onODotMove(evt: PointerEvent): void {
    if (!this.odrag) return;
    if (Math.hypot(evt.clientX - this.odrag.x0, evt.clientY - this.odrag.y0) > 8) this.odrag.moved = true;
  }
  private onODotUp(evt: PointerEvent): void {
    window.removeEventListener('pointermove', this.oMove);
    window.removeEventListener('pointerup', this.oUp);
    const d = this.odrag; this.odrag = null; if (!d) return;
    if (d.moved) {
      if (!this.roomAt(d.od.cell.col, d.od.cell.row, 1, d.od.parentId)) return;   // nowhere to put it
      this.pushHistory(null);
      const endId = this.addOpenEndOn(d.od.parentId, d.od.branchId);   // passive branch off the output
      if (endId) { this.placeAt(endId, d.od.cell); this.afterMutation(endId); }
    } else {
      this.openMenu(evt.clientX, evt.clientY, { addOutput: { parentId: d.od.parentId, branchId: d.od.branchId, cell: d.od.cell } });
    }
  }
  /** Add-dot menu → put a fitting straight onto a free output. Reuses the open-end
   *  path: seed an open end on the output, then fill it (or cap / leave as a duct). */
  private addAtOutput(kind: MenuKind): void {
    const m = this.menu; if (!m?.addOutput) return;
    const { parentId, branchId, cell } = m.addOutput; this.closeMenu();
    const endId = this.addOpenEndOn(parentId, branchId); if (!endId) return;
    this.placeAt(endId, cell);
    if (kind === 'duct') { this.afterMutation(endId); return; }
    if (kind === 'cap') { this.capEnd(endId); return; }
    this.fillEnd(endId, kind as SelKind | 'tool');
  }
  /** Put it exactly where the user asked. Nothing searches for a better cell —
   *  the menu has already greyed out anything that wouldn't fit here. */
  private placeAt(id: string, cell: Cell): void { this.cells.set(id, cell); }

  /** True if a `span`-wide fitting fits EXACTLY at (col,row): on the board, every
   *  cell of its footprint free, and no other run's duct passing through. */
  /** The first cell out from `n` in direction (dx,dy) with room for `span` — instead
   *  of a fixed offset that might land on something. Scans a few cells so growth
   *  past a wide neighbour still finds the gap beyond it. Cells off the negative
   *  edge are allowed here; {@link normalizeCells} shifts the board afterwards. */
  private firstFreeCellToward(n: NodeVM, dx: number, dy: number, span: number, selfId: string): Cell | null {
    const occ = this.cellOccupied();
    const start = dx > 0 ? n.span : 1;   // clear a unit's own width before scanning right
    for (let i = start; i <= start + 5; i++) {
      const cell = { col: n.col + dx * i, row: n.row + dy * i };
      if (this.roomAt(cell.col, cell.row, span, selfId, occ, true)) return cell;
    }
    return null;
  }

  /** Shift every cell so the board starts at (0,0) again. Growth to the left or up
   *  produces negative cells for one beat; rather than teach the whole editor about
   *  a negative quadrant, the board slides back under them. Relative positions are
   *  untouched, so nothing re-routes differently. */
  private normalizeCells(): void {
    if (!this.cells.size) return;
    let minCol = Infinity, minRow = Infinity;
    for (const c of this.cells.values()) { minCol = Math.min(minCol, c.col); minRow = Math.min(minRow, c.row); }
    if (minCol === 0 && minRow === 0) return;
    if (!isFinite(minCol) || !isFinite(minRow)) return;
    for (const c of this.cells.values()) { c.col -= minCol; c.row -= minRow; }
    for (const n of this.nodes) { n.col -= minCol; n.row -= minRow; }
    this.router.invalidate();
  }

  private roomAt(col: number, row: number, span: number, selfId: string, occ = this.cellOccupied(), allowNegative = false): boolean {
    if (!allowNegative && (col < 0 || row < 0)) return false;
    for (let i = 0; i < Math.max(1, span); i++) {
      if (occ.has((col + i) + ',' + row)) return false;
      if (this.cellOnDuct(col + i, row, selfId)) return false;
    }
    return true;
  }

  /** Where a tee's new leg goes: PERPENDICULAR to the run it taps off — a
   *  horizontal run drops down (or up), a vertical one goes right (or left).
   *  Never onto another duct. Null → no room, and the option greys out. */
  private legCellFor(bd: BDot, selfId: string): Cell | null {
    const occ = this.cellOccupied();
    const tries: Cell[] = bd.axis === 'h'
      ? [{ col: bd.col, row: bd.row + 1 }, { col: bd.col, row: bd.row - 1 }]
      : [{ col: bd.col + 1, row: bd.row }, { col: bd.col - 1, row: bd.row }];
    return tries.find(c => this.roomAt(c.col, c.row, 1, selfId, occ)) ?? null;
  }

  // ── branch off a tube ─────────────────────────────────────────────────────────
  // A branch dot is a tee point on a run. CLICK it → menu (splice a gate, or add a
  // leg). CLICK-DRAG → tee in a PASSIVE branch (a plain open-end leg) you can then
  // extend or populate — no gate forced on you.
  onBranchDotDown(evt: PointerEvent, bd: BDot): void {
    evt.preventDefault(); evt.stopPropagation();
    this.bdrag = { bd, x0: evt.clientX, y0: evt.clientY, moved: false };
    window.addEventListener('pointermove', this.bMove);
    window.addEventListener('pointerup', this.bUp);
  }
  private onBDotMove(evt: PointerEvent): void {
    if (!this.bdrag) return;
    if (Math.hypot(evt.clientX - this.bdrag.x0, evt.clientY - this.bdrag.y0) > 8) this.bdrag.moved = true;
  }
  private onBDotUp(evt: PointerEvent): void {
    window.removeEventListener('pointermove', this.bMove);
    window.removeEventListener('pointerup', this.bUp);
    const d = this.bdrag; this.bdrag = null; if (!d) return;
    if (d.moved) {
      this.pushHistory(null);
      this.branchDuct(d.bd, 'duct');            // passive open-end leg, perpendicular to the run
    } else {
      this.openMenu(evt.clientX, evt.clientY, { branch: d.bd });
    }
  }

  /** Tee a new leg off a run at the clicked dot. The tee lands exactly on that dot;
   *  the leg goes perpendicular to the run (legCellFor). No cell → nothing to do,
   *  and the menu would already have greyed the option. */
  private branchDuct(bd: BDot, kind: Fitting): void {
    if (!this.topo) return;
    const legCell = this.legCellFor(bd, bd.childId); if (!legCell) return;
    const duct = this.ductsRaw().find(d => d['child'] === bd.childId); if (!duct) return;
    const parentId = duct['parent'] as string; const parentEl = this.elem(parentId);
    let junctionId: string;
    // Reuse the upstream tee ONLY if it is standing on the cell you clicked. It used
    // to be reused wherever it was, so branching off a long run that already had a
    // wye anywhere upstream hung the new leg on that wye instead — the open end
    // landed in the right cell, but fed from the wrong place, and the route drew
    // itself from there. Harmless when a run had one dot; wrong now that every cell
    // on a run is a branch point.
    const parentCell = this.cells.get(parentId);
    if (parentEl && parentEl['type'] === 'junction' && parentCell && parentCell.col === bd.col && parentCell.row === bd.row) {
      junctionId = parentId;                          // already a tee right here — just add a leg
    } else {
      const j: RawEl = { id: this.newId('wye'), type: 'junction', name: 'Wye' };
      this.elems(this.topo).push(j);
      const upDuct: RawEl = { child: j['id'], parent: parentId };
      if (duct['parentBranch']) {                     // was on a selector outlet → the branch now feeds the wye
        upDuct['parentBranch'] = duct['parentBranch'];
        const b = (parentEl?.['branches'] as Branch[] | undefined)?.find(x => x.id === duct['parentBranch']);
        if (b) b.role = 'feed';
      }
      this.ductsRaw().push(upDuct);
      duct['parent'] = j['id']; delete duct['parentBranch'];   // original element now hangs off the wye
      junctionId = j['id'] as string;
      this.placeAt(junctionId, { col: bd.col, row: bd.row });  // the tee sits where you clicked
    }
    const newId = this.addJunctionChild(junctionId, kind); if (!newId) return;
    this.placeAt(newId, legCell);
    this.afterMutation(newId);
  }

  /** Splice a gate INTO the run at the clicked point: the downstream reconnects to
   *  the new gate's FIRST outlet (so a manifold becomes a real 2-way — one leg used,
   *  one free; a ball valve is a plain inline on/off). Remaining outlets stay
   *  capped-but-available, never dead. */
  private insertInline(childId: string, kind: SelKind, cell?: Cell): string | null {
    if (!this.topo) return null;
    const duct = this.ductsRaw().find(d => d['child'] === childId); if (!duct) return null;
    const parentId = duct['parent'] as string;
    const channel = this.freeServoChannel();
    const sel = this.makeSelector(kind, channel);
    this.elems(this.topo).push(sel);
    // The gate inherits the child's upstream link (incl. a parent selector's outlet).
    const upDuct: RawEl = { child: sel['id'], parent: parentId };
    if (duct['parentBranch']) {
      upDuct['parentBranch'] = duct['parentBranch'];
      const pb = (this.elem(parentId)?.['branches'] as Branch[] | undefined)?.find(x => x.id === duct['parentBranch']);
      if (pb) pb.role = 'feed';
    }
    this.ductsRaw().push(upDuct);
    // Downstream reconnects to the gate's first outlet (tool if it's a tool, else feed).
    const first = (sel['branches'] as Branch[])[0];
    const childEl = this.elem(childId);
    first.role = childEl?.['type'] === 'tool' ? 'tool' : 'feed';
    duct['parent'] = sel['id']; duct['parentBranch'] = first.id;
    const cc = this.cells.get(childId) ?? { col: 0, row: 0 };
    this.placeAt(sel['id'] as string, cell ?? { col: cc.col, row: cc.row });
    this.afterMutation(sel['id'] as string);
    return sel['id'] as string;
  }

  /** Grid-snapped branch points along every duct: a dot at each cell step on both
   *  vertical and horizontal segments. Clicking one branches the run right there. */
  branchDots(): BDot[] {
    const out: BDot[] = [];
    if (this.dragId || this.bdrag || this.odrag) return out;   // hide while dragging
    const seen = new Set<string>();
    const push = (x: number, y: number, childId: string, axis: 'h' | 'v') => {
      const col = Math.round((x - PAD) / CELL), row = Math.round((y - PAD) / CELL);
      const key = col + ',' + row;
      if (seen.has(key)) return;                      // one dot per cell even where ducts overlap
      seen.add(key); out.push({ x, y, childId, col, row, axis });
    };
    for (const d of this.ducts) {
      const pts = this.ductPoints(d.childId);
      // A unit (sliding gate / manifold) is fed at its TOP. Branching off that last
      // drop would grow a run out of the gate's top, which reads as a second inlet —
      // confusing. No dots there; the rest of the feed run stays branchable.
      const last = this.byId.get(d.childId)?.isUnit ? pts.length - 2 : -1;
      for (let i = 0; i < pts.length - 1; i++) {
        if (i === last) continue;
        const a = pts[i], b = pts[i + 1];
        if (Math.abs(a.x - b.x) < 0.5) {              // vertical segment
          const lo = Math.min(a.y, b.y), hi = Math.max(a.y, b.y);
          for (let r = Math.ceil((lo - PAD) / CELL); r <= Math.floor((hi - PAD) / CELL); r++) {
            const y = PAD + r * CELL; if (y > lo + 18 && y < hi - 18) push(a.x, y, d.childId, 'v');
          }
        } else if (Math.abs(a.y - b.y) < 0.5) {       // horizontal segment
          const lo = Math.min(a.x, b.x), hi = Math.max(a.x, b.x);
          for (let c = Math.ceil((lo - PAD) / CELL); c <= Math.floor((hi - PAD) / CELL); c++) {
            const x = PAD + c * CELL; if (x > lo + 18 && x < hi - 18) push(x, a.y, d.childId, 'h');
          }
        }
      }
    }
    return out;
  }
  private addJunctionChild(junctionId: string, kind: SelKind | 'tool' | 'duct'): string | null {
    if (!this.topo) return null;
    if (kind === 'tool') {
      const tool: RawEl = { id: this.newId('tool'), type: 'tool', name: 'New tool' };
      this.elems(this.topo).push(tool);
      this.ductsRaw().push({ child: tool['id'], parent: junctionId });
      return tool['id'] as string;
    }
    if (kind === 'duct') {
      const j: RawEl = { id: this.newId('j'), type: 'junction', name: 'Open end' };
      this.elems(this.topo).push(j);
      this.ductsRaw().push({ child: j['id'], parent: junctionId });
      return j['id'] as string;
    }
    const channel = this.freeServoChannel();
    const sel = this.makeSelector(kind, channel);
    this.elems(this.topo).push(sel);
    this.ductsRaw().push({ child: sel['id'], parent: junctionId });
    return sel['id'] as string;
  }
  private cellOccupied(): Set<string> {
    const occ = new Set<string>();
    for (const [id, c] of this.cells) {
      const el = this.elem(id);
      const span = el && isUnitKind(el['kind']) ? Math.max(1, (el['branches'] as unknown[] | undefined)?.length ?? 1) : 1;
      for (let i = 0; i < span; i++) occ.add((c.col + i) + ',' + c.row);
    }
    return occ;
  }
  // ── undo / redo ───────────────────────────────────────────────────────────────
  // Whole-state snapshots (topology + cell positions). The doc is small and every
  // mutation already rebuilds the graph, so restoring a snapshot is the same work
  // as any other edit — no need for per-action inverse operations.

  /** Snapshot now, and remember what produced it so keystroke-by-keystroke edits
   *  (renaming) collapse into one undo step instead of dozens. */
  private snapshot(): string {
    return JSON.stringify({ topo: this.topo, cells: [...this.cells] });
  }
  private pushHistory(tag: string | null): void {
    if (!this.topo) return;
    if (tag && tag === this.lastTag && this.past.length) return;   // coalesce a run of the same edit
    this.past.push(this.snapshot());
    if (this.past.length > HISTORY_MAX) this.past.shift();
    this.future.length = 0;                                        // a fresh edit forks the timeline
    this.lastTag = tag;
  }
  /** Run a mutation with an undo point in front of it. */
  private mutate<T>(tag: string | null, fn: () => T): T {
    this.pushHistory(tag);
    return fn();
  }
  get canUndo(): boolean { return this.past.length > 0; }
  get canRedo(): boolean { return this.future.length > 0; }
  undo(): void { this.step(this.past, this.future); }
  redo(): void { this.step(this.future, this.past); }
  private step(from: string[], to: string[]): void {
    const snap = from.pop(); if (!snap || !this.topo) return;
    to.push(this.snapshot());
    const state = JSON.parse(snap) as { topo: Topology; cells: [string, Cell][] };
    this.topo = state.topo;
    this.cells = new Map(state.cells);
    this.lastTag = null;                       // never coalesce across an undo
    this.selectedId = null; this.closeMenu();
    this.dirty = true; this.saveError = ''; this.wip = ''; this.saveNote = '';
    this.airflowErrors = [];
    this.buildGraph(this.topo); this.syncNodes(); this.refreshHandles();
  }

  private afterMutation(selectId: string): void {
    if (!this.topo) return;
    this.dirty = true; this.saveError = ''; this.airflowErrors = []; this.saveNote = ''; this.wip = '';
    this.normalizeCells();
    this.collapsePassThroughJunctions();
    this.buildGraph(this.topo); this.syncNodes();
    this.selectedId = selectId; this.refreshHandles();
  }

  private childDucts(id: string): RawEl[] { return this.ductsRaw().filter(d => d['parent'] === id); }

  /** A junction is only meaningful as a tee (≥2 legs) or an OPEN END (0 legs). One
   *  with exactly one child is a redundant pass-through — reconnect that child to
   *  the grandparent and drop the junction. Keeps populated ends / chained runs
   *  clean instead of leaving stray dots. Capped ends are terminals — never touched. */
  private collapsePassThroughJunctions(): void {
    if (!this.topo) return;
    for (let guard = 0; guard < 100; guard++) {
      const junc = this.elems(this.topo).find(e =>
        e['type'] === 'junction' && !e['capped'] &&
        this.childDucts(e['id'] as string).length === 1 &&
        this.ductsRaw().some(d => d['child'] === e['id']));
      if (!junc) break;
      const jid = junc['id'] as string;
      const inDuct = this.ductsRaw().find(d => d['child'] === jid)!;
      const outDuct = this.childDucts(jid)[0];
      const grandparentId = inDuct['parent'] as string;
      outDuct['parent'] = grandparentId;
      if (inDuct['parentBranch']) {
        outDuct['parentBranch'] = inDuct['parentBranch'];   // child inherits the outlet
        const gp = this.elem(grandparentId);
        if (gp && gp['type'] === 'selector') {
          const b = (gp['branches'] as Branch[]).find(x => x.id === inDuct['parentBranch']);
          const childEl = this.elem(outDuct['child'] as string);
          if (b) b.role = childEl?.['type'] === 'tool' ? 'tool' : 'feed';
        }
      } else {
        delete outDuct['parentBranch'];
      }
      (this.topo as { elements: RawEl[] }).elements = this.elems(this.topo).filter(e => e['id'] !== jid);
      (this.topo as { ducts: RawEl[] }).ducts = this.ductsRaw().filter(d => d !== inDuct);
      this.cells.delete(jid);
    }
  }

  /** Lowest PWM channel not already spoken for. Counting existing gates would reuse a
   *  channel after a delete, and the schema rejects two gates sharing one. */
  /** The board a newly-drawn gate is assigned to: the primary, by id rather than
   *  by the literal 'primary' — /boards mints controller ids from hostnames. */
  private defaultControllerId(): string {
    const cs = (this.topo?.['controllers'] as RawEl[] | undefined) ?? [];
    const primary = cs.find(c => c['role'] === 'primary');
    return (primary?.['id'] as string) ?? 'primary';
  }

  /** First unused servo channel ON A GIVEN BOARD.
   *
   *  Channels are per-board: two gates on different boards can both sit on
   *  channel 0. Searching the whole shop (as this used to) starts handing out
   *  channel 4 as soon as a second board exists, which validateTopology rejects
   *  — so drawing a fifth gate silently produced an unsaveable layout. */
  private freeServoChannel(controllerId = this.defaultControllerId()): number {
    const taken = new Set(
      this.elems(this.topo!)
        .filter(e => e['type'] === 'selector' && e['kind'] !== 'linear'
                  && (e['controllerId'] ?? this.defaultControllerId()) === controllerId)
        .map(e => (e['servo'] as RawEl | undefined)?.['channel'] as number),
    );
    for (let ch = 0; ch < 4; ch++) if (!taken.has(ch)) return ch;
    return 0;                          // over budget; validation reports it on save
  }

  private addSelector(parentId: string, kind: SelKind): string | null {
    if (!this.topo) return null;
    const els = this.elems(this.topo);
    const sel = this.makeSelector(kind, this.freeServoChannel()); els.push(sel);
    const duct: RawEl = { child: sel['id'], parent: parentId };
    const p = this.elem(parentId);
    if (p && p['type'] === 'selector') {
      const b = (p['branches'] as Branch[]).find(x => x.role === 'blocked');
      if (!b) { els.pop(); return null; }
      b.role = 'feed'; duct['parentBranch'] = b.id;
    }
    this.ductsRaw().push(duct);
    return sel['id'] as string;
  }
  private addTool(parentId: string): string | null {
    if (!this.topo) return null;
    const tool: RawEl = { id: this.newId('tool'), type: 'tool', name: 'New tool' };
    this.elems(this.topo).push(tool);
    const duct: RawEl = { child: tool['id'], parent: parentId };
    const p = this.elem(parentId);
    if (p && p['type'] === 'selector') {
      const b = (p['branches'] as Branch[]).find(x => x.role === 'blocked');
      if (!b) { this.elems(this.topo).pop(); return null; }
      b.role = 'tool'; duct['parentBranch'] = b.id;
    }
    this.ductsRaw().push(duct);
    return tool['id'] as string;
  }
  private makeSelector(kind: SelKind, channel: number): RawEl {
    const base: RawEl = { id: this.newId('sel'), type: 'selector', name: this.defaultName(kind), controllerId: this.defaultControllerId(), kind };
    if (kind === 'linear') {
      // Outlet positions are seeded at the nominal Rockler pitch so the canvas has
      // something to draw, but `linear.calibration` is left out on purpose — only the
      // reference sweep can measure the rail, and inventing a span would look set up
      // while driving the carriage to the wrong place. See /gates.
      base['states'] = [{ id: 'home', isClosed: true, positionMm: 0 }]; base['branches'] = [];
      this.appendLinearOutlets(base, 4);
    } else if (kind === 'servoGate') {
      // The offsets are the valve's design geometry (a quarter turn to shut), so they
      // seed fine. referenceAngle is a per-BUILD measurement, so it is deliberately
      // absent until someone calibrates: an invented one would look configured while
      // driving the ball to the wrong place. See /gates.
      base['states'] = [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }];
      base['branches'] = [{ id: 'b1', opensState: 'open', role: 'blocked' }];
      base['servo'] = { channel, detented: true };
    } else {
      base['states'] = [{ id: 'left', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 80 }, { id: 'right', isClosed: false, offsetDeg: 161 }];
      base['branches'] = [{ id: 'mL', opensState: 'left', role: 'blocked' }, { id: 'mR', opensState: 'right', role: 'blocked' }];
      base['servo'] = { channel, detented: true };
    }
    return base;
  }
  /** Append `count` non-closed states + blocked branches to a linear selector. */
  private appendLinearOutlets(sel: RawEl, count: number): void {
    const states = sel['states'] as RawEl[]; const branches = sel['branches'] as Branch[];
    const start = branches.length;
    for (let i = start + 1; i <= start + count; i++) {
      states.push({ id: 's' + i, isClosed: false, positionMm: Math.round(i * 82.9 * 10) / 10 });
      branches.push({ id: 'b' + i, opensState: 's' + i, role: 'blocked' });
    }
  }
  private defaultName(kind: SelKind): string { return kind === 'linear' ? 'Sliding gate' : kind === 'servoGate' ? 'Ball valve' : 'Manifold'; }

  // ── outlet-count chooser (sliding gate only) ─────────────────────────────────
  canRemoveOutlets(n: NodeVM): boolean {
    const b = (this.elem(n.id)?.['branches'] as Branch[] | undefined) ?? [];
    return b.length > 2 && b.slice(-2).every(x => x.role === 'blocked');
  }
  changeOutlets(id: string, delta: number): void {
    const el = this.elem(id); if (!el || el['kind'] !== 'linear' || !this.topo) return;
    this.pushHistory(null);
    if (delta > 0) {
      this.appendLinearOutlets(el, delta);
    } else {
      const branches = el['branches'] as Branch[]; const states = el['states'] as RawEl[];
      for (let k = 0; k < -delta; k++) {
        if (branches.length <= 2) break;
        const last = branches[branches.length - 1];
        if (last.role !== 'blocked') break;   // never drop a used outlet
        branches.pop();
        const si = states.findIndex(s => s['id'] === last.opensState);
        if (si >= 0) states.splice(si, 1);
      }
    }
    this.afterMutation(id);
  }

  // ── gate type swap ────────────────────────────────────────────────────────────
  isGate(n: NodeVM): boolean { return n.glyph === 'ballvalve' || n.glyph === 'slidingGate' || n.glyph === 'manifold'; }
  private kindOf(id: string): SelKind | null {
    const k = this.elem(id)?.['kind'];
    return k === 'linear' || k === 'servoGate' || k === 'servoManifold' ? k : null;
  }
  /** Outlets a fresh gate of this kind offers. A sliding gate grows in PAIRS (the
   *  Rockler manifolds it's built on ship in pairs), so it can take a wide fork. */
  private capacityOf(kind: SelKind, legs = 0): number {
    if (kind === 'servoGate') return 1;
    if (kind === 'servoManifold') return 2;
    return Math.max(4, legs + (legs % 2));
  }
  /** The three gate kinds for the inspector's swap control: which one this already
   *  is, and for the others whether the swap can actually happen here — a narrower
   *  gate can't hold the legs it has, and a wider one needs the cells beside it. */
  gateTypes(n: NodeVM): Array<{ kind: SelKind; label: string; current: boolean; enabled: boolean; note?: string }> {
    const cur = this.kindOf(n.id);
    const legs = this.childDucts(n.id).length;
    const at = this.cells.get(n.id) ?? { col: 0, row: 0 };
    const occ = this.occupiedExcept(new Set([n.id]));
    // `note` is the bare reason — the menu shows the label alongside it.
    return (['servoGate', 'servoManifold', 'linear'] as SelKind[]).map(kind => {
      const label = this.defaultName(kind);
      if (kind === cur) return { kind, label, current: true, enabled: false };
      const cap = this.capacityOf(kind, legs);
      if (legs > cap)
        return { kind, label, current: false, enabled: false, note: `only ${cap} outlet${cap === 1 ? '' : 's'}` };
      const span = isUnitKind(kind) ? cap : 1;
      for (let i = 0; i < span; i++)
        if (occ.has((at.col + i) + ',' + at.row))
          return { kind, label, current: false, enabled: false, note: 'no room beside it' };
      return { kind, label, current: false, enabled: true };
    });
  }
  /** Swap the mechanism, keep the plumbing: same id, name, cell and upstream feed —
   *  the legs below just move onto the new gate's outlets. */
  convertKind(id: string, kind: SelKind): void {
    const el = this.elem(id), n = this.byId.get(id);
    if (!el || !n || !this.topo) return;
    if (!this.gateTypes(n).find(t => t.kind === kind)?.enabled) return;
    const was = this.kindOf(id);
    const legs = this.childDucts(id);
    const channel = ((el['servo'] as RawEl | undefined)?.['channel'] as number | undefined)
      ?? this.freeServoChannel();
    const fresh = this.makeSelector(kind, channel);
    fresh['id'] = id;
    // A name the user chose survives; an untouched default follows the new kind.
    fresh['name'] = was && el['name'] === this.defaultName(was) ? this.defaultName(kind) : el['name'];
    if (kind === 'linear') {
      const need = this.capacityOf(kind, legs.length), have = (fresh['branches'] as Branch[]).length;
      if (need > have) this.appendLinearOutlets(fresh, need - have);
    }
    const branches = fresh['branches'] as Branch[];
    legs.forEach((d, i) => {
      const b = branches[i]; if (!b) return;
      d['parentBranch'] = b.id;
      b.role = this.elem(d['child'] as string)?.['type'] === 'tool' ? 'tool' : 'feed';
    });
    const els = this.elems(this.topo);
    els[els.findIndex(e => e['id'] === id)] = fresh;
    this.afterMutation(id);
  }

  rename(id: string, name: string): void {
    const el = this.elem(id); if (!el) return;
    this.pushHistory('rename:' + id);   // a whole typed name undoes as one step
    el['name'] = name; const n = this.byId.get(id); if (n) n.name = name; this.dirty = true;
  }
  /** Anything with at most ONE thing below it can go: the run heals into plain duct
   *  (see removeElement). Only a real fork — two or more legs — has to be thinned
   *  first, because there's no single run left to splice back together. */
  canDelete(n: NodeVM): boolean {
    if (n.glyph === 'collector') return false;
    if (n.glyph === 'tool') return true;
    return this.childrenOf(n.id).length <= 1;
  }
  /** Why the (−) is greyed out, for its tooltip. */
  deleteHint(n: NodeVM): string {
    if (n.glyph === 'collector') return 'The collector stays';
    return this.canDelete(n) ? 'Remove (or press Delete)' : 'Remove its other legs first';
  }
  deleteSelected(): void {
    const n = this.inspected(); if (!n || !this.canDelete(n) || !this.topo) return;
    this.pushHistory(null);
    this.removeElement(n.id);
    this.selectedId = null; this.dirty = true; this.saveError = '';
    this.buildGraph(this.topo); this.syncNodes(); this.refreshHandles();
  }
  /** Take an element out of the run. With exactly one leg below, the run doesn't
   *  break: that leg reconnects to the parent and what was a gate becomes plain
   *  pipe again — deleting a ball valve mid-run just gives you back the duct. */
  private removeElement(id: string): void {
    if (!this.topo) return;
    const inDuct = this.ductsRaw().find(d => d['child'] === id);
    const outDucts = this.childDucts(id);
    const parentId = inDuct?.['parent'] as string | undefined;
    const parentBranch = inDuct?.['parentBranch'] as string | undefined;
    const pBranch = parentBranch
      ? (this.elem(parentId ?? '')?.['branches'] as Branch[] | undefined)?.find(x => x.id === parentBranch)
      : undefined;

    if (outDucts.length === 1 && parentId) {
      const out = outDucts[0];
      out['parent'] = parentId;
      delete out['parentBranch'];
      if (pBranch) {                                  // it hung off an outlet → the leg inherits it
        out['parentBranch'] = pBranch.id;
        pBranch.role = this.elem(out['child'] as string)?.['type'] === 'tool' ? 'tool' : 'feed';
      }
      (this.topo as { ducts: RawEl[] }).ducts = this.ductsRaw().filter(d => d !== inDuct);
    } else {
      if (pBranch) pBranch.role = 'blocked';          // nothing below → the outlet frees up
      (this.topo as { ducts: RawEl[] }).ducts = this.ductsRaw().filter(d => d['child'] !== id && d['parent'] !== id);
    }
    (this.topo as { elements: RawEl[] }).elements = this.elems(this.topo).filter(e => e['id'] !== id);
    this.cells.delete(id);
    // Removing a leg can leave the tee it hung off as a 1-child pass-through —
    // collapse it so the run heals and the branch point becomes addable again.
    this.collapsePassThroughJunctions();
  }

  // ── actions ───────────────────────────────────────────────────────────────────
  get hasShop(): boolean { return !!this.topo; }
  autoArrange(): void {
    this.pushHistory(null);
    this.autoLayoutInto(this.cells); this.syncNodes(); this.refreshHandles(); this.dirty = true;
  }

  /** The topology doc plus the current node positions, ready to PUT or download. */
  private docWithLayout(): Record<string, unknown> {
    const layout: Record<string, Cell> = {};
    for (const [id, c] of this.cells) layout[id] = { col: c.col, row: c.row };
    return { ...(this.topo as Record<string, unknown>), ui: { layout } };
  }

  /** Live always-open leaks (tools with no gate on their path to the collector). */
  private liveLeaks(): AirflowIssue[] {
    if (!this.topo) return [];
    try { return airflowIssues(this.topo as Topology); } catch { return []; }
  }

  /** Names of gates still missing their calibration, in canvas order. Gates only:
   *  tools and the collector share the badge but an unpaired one is a MANUAL
   *  piece, not an unfinished one — nagging about it would be wrong. */
  unconfiguredGates(): string[] {
    return this.nodes
      .filter(n => n.setup === 'todo' && n.glyph !== 'tool' && n.glyph !== 'collector')
      .map(n => n.name || n.id);
  }

  /**
   * The one contextual line in the guide bar. Priority: a failed save first, then
   * live airflow problems (with a fix + Cap action), then a save confirmation,
   * then onboarding (empty shop) → progress nudge.
   */
  get guide(): { text: string; kind: 'info' | 'warn' | 'ok'; cap?: boolean } {
    // Mid-drag, the reason a drop won't work outranks everything: it's about the
    // gesture in progress, and saying it before the finger lifts is the whole point.
    if (this.dropBlocked) return { text: this.dropBlocked, kind: 'warn' };
    if (this.saveError) return { text: this.saveError, kind: 'warn' };
    if (this.wip) return { text: `Work in progress — saved here, but the controller won’t take it yet: ${this.wip}.`, kind: 'warn' };

    // Below the real problems, above the general nudges: a redundant gate is a
    // working shop with a part in it that isn't doing anything. Worth saying once,
    // as an offer rather than a demand — select it and the (−) is right there.
    const spare = this.nodes.filter(n => n.redundant);
    if (spare.length && !this.liveLeaks().length) {
      const names = spare.map(n => n.name).join(', ');
      return {
        // True whether it duplicates a gate downstream or simply has nothing under
        // it yet — in both cases the shop shuts off exactly the same without it.
        text: spare.length === 1
          ? `${names} isn’t changing what the shop can shut off — you can delete it, or keep it as a manual shut-off.`
          : `${spare.length} gates aren’t changing what the shop can shut off: ${names}. Any of them can go, or stay as manual shut-offs.`,
        kind: 'info',
      };
    }

    const leaks = this.liveLeaks();
    if (leaks.length) {
      const open = leaks.filter(l => l.kind === 'always-open');
      const shared = leaks.filter(l => l.kind === 'co-open');
      // Ungated tools are the root cause when both show up, so they lead.
      if (open.length) {
        const names = open.map(l => l.name).join(', ');
        const one = open.length === 1;
        return {
          kind: 'warn', cap: true,
          text: `${names} can’t be selected — there’s no gate between ${one ? 'it' : 'them'} and the collector, so suction leaks there. The shop stays off until that’s fixed: add a gate on the path, delete the tool, or`,
        };
      }
      // With a pair, both ends are flagged and naming them twice reads badly; a lone
      // flag means the other leg is the ungated one, so that one is worth naming.
      const one = shared.length === 1;
      const names = shared.map(l => l.name).join(', ');
      const partners = (shared[0].with ?? []).map(w => w.name).join(', ');
      return {
        kind: 'warn', cap: true,
        text: one
          ? `${names} can’t be selected on its own — ${partners} share${(shared[0].with ?? []).length === 1 ? 's' : ''} its outlet with no gate in between, so running it pulls air through ${partners} too. The shop stays off until that’s fixed: put a gate on that leg, move it to a free outlet, or`
          : `${names} can’t be selected on their own — they share an outlet, so opening one opens the others and suction leaks. The shop stays off until that’s fixed: put a gate on each leg, move them to free outlets, or`,
      };
    }

    // Gates nobody has shown the positions to. Sits below the leaks (a leak is a
    // structural mistake; this is just unfinished work) but above the open-end
    // nudge, because the shop can't run until it's done. This is the whole reason
    // the separate /gates pass could go away: the canvas already knows, and the
    // orange dot on each gate is the thing you tap to fix it.
    const unset = this.unconfiguredGates();
    if (unset.length) {
      const one = unset.length === 1;
      return {
        kind: 'warn',
        text: `${unset.join(', ')} ${one ? 'still needs' : 'still need'} setting up — tap the orange dot on ${one ? 'it' : 'each'} to show ${one ? 'it' : 'them'} where the valve positions are.`,
      };
    }

    if (this.saveNote) return { text: this.saveNote, kind: 'ok' };

    const ends = this.openEndCount();
    if (ends) {
      const one = ends === 1;
      return {
        kind: 'info',
        text: `${ends} open ${one ? 'end' : 'ends'} — drag ${one ? 'it' : 'one'} to run more pipe, or tap ${one ? 'it' : 'one'} to drop a tool or gate there.`,
      };
    }

    if (this.nodes.length <= 1)
      return { text: 'Drag the open end out to run pipe, then tap it to add your first gate or tool.', kind: 'info' };

    return {
      kind: 'info',
      text: this.dirty
        ? 'Drag open ends to run pipe; tap an end for a gate or tool; click a dot on a run to branch. Save when you’re done.'
        : 'Saved. Add more, or open the Live view to try it out.',
    };
  }

  /**
   * Save never blocks on an unfinished shop — you can stop mid-build and come back.
   * Problems are flagged as work-in-progress instead, and the Live view refuses to
   * drive anything until they're cleared (see LiveViewComponent.ready).
   *
   * The one thing we can't do is PUT a structurally broken doc: the controller
   * rejects it with 400. That draft stays here, intact, and the guide bar says so.
   */
  async save(): Promise<void> {
    if (!this.topo || this.saving) return;
    const doc = this.docWithLayout();
    const v = validateTopology(doc);
    this.saving = true; this.saveError = ''; this.saveNote = ''; this.wip = '';
    try {
      this.topo = doc as Topology;                     // keep the draft either way
      if (!v.ok) {
        this.wip = v.errors[0]?.message ?? 'incomplete layout';
        return;                                        // still dirty — retry once it's whole
      }
      await this.api.putTopology(doc as Topology);
      this.dirty = false;
      this.airflowErrors = this.liveLeaks();
      this.saveNote = this.airflowErrors.length ? '' : 'Saved.';
    } catch {
      this.saveError = 'Couldn’t reach the controller — your layout is kept here.';
    } finally { this.saving = false; }
  }

  /** Bypass: put a closed gate above each always-open tool, then save. */
  async capAndSave(): Promise<void> {
    if (!this.topo) return;
    const n = this.capAlwaysOn();
    this.buildGraph(this.topo); this.syncNodes(); this.refreshHandles();
    this.airflowErrors = [];
    await this.save();
    if (!this.saveError && !this.airflowErrors.length) {
      this.saveNote = `Put a closed gate on ${n} leaking outlet${n === 1 ? '' : 's'} — wire a servo to each, or delete the tool.`;
    }
  }
  private capAlwaysOn(): number {
    if (!this.topo) return 0;
    const leaks = airflowIssues(this.topo);
    // A co-open pair is only fixed once EVERY tool sharing the outlet is behind its
    // own gate — gating one still leaves the other open when the first one runs — so
    // the partners named in the issue get capped too.
    const targets = new Set<string>();
    for (const iss of leaks) {
      targets.add(iss.id);
      for (const w of iss.with ?? []) targets.add(w.id);
    }
    if (targets.size) this.pushHistory(null);
    for (const id of targets) {
      const duct = this.ductsRaw().find(d => d['child'] === id); if (!duct) continue;
      const parentId = duct['parent'] as string;
      const parentBranch = duct['parentBranch'] as string | undefined;
      const channel = this.freeServoChannel();
      const gate = this.makeSelector('servoGate', channel);
      (gate['branches'] as Branch[])[0].role = 'tool';
      this.elems(this.topo).push(gate);
      duct['parent'] = gate['id']; duct['parentBranch'] = (gate['branches'] as Branch[])[0].id;   // tool now behind the gate
      // The gate takes the tool's old spot — including its outlet on a parent
      // selector, which now feeds a sub-network rather than a tool directly.
      const up: RawEl = { child: gate['id'], parent: parentId };
      if (parentBranch) {
        up['parentBranch'] = parentBranch;
        const pb = (this.elem(parentId)?.['branches'] as Branch[] | undefined)?.find(b => b.id === parentBranch);
        if (pb) pb.role = 'feed';
      }
      this.ductsRaw().push(up);
      // The gate takes the tool's cell and the tool moves down to the first free
      // row — capping a shared outlet inserts several gates at once, so "one row
      // down" alone would stack tools on top of each other.
      const tcell = this.cells.get(id) ?? { col: 0, row: 0 };
      this.cells.set(gate['id'] as string, { col: tcell.col, row: tcell.row });
      let row = tcell.row + 1;
      const taken = new Set([...this.cells].filter(([k]) => k !== id).map(([, c]) => c.col + ',' + c.row));
      while (taken.has(tcell.col + ',' + row)) row++;
      this.cells.set(id, { col: tcell.col, row });
    }
    this.dirty = true;
    return targets.size;
  }

  /** Download the whole shop (topology + layout) as a JSON file the user can keep. */
  exportShop(): void {
    if (!this.topo) return;
    const json = JSON.stringify(this.docWithLayout(), null, 2);
    const blob = new Blob([json], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url; a.download = this.fileName();
    a.click();
    URL.revokeObjectURL(url);
  }
  private fileName(): string {
    const raw = ((this.topo as { name?: string } | null)?.name ?? 'shop').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
    return `${raw || 'shop'}.json`;
  }

  importClick(input: HTMLInputElement): void {
    if (this.dirty && !window.confirm('Discard unsaved changes and import a shop file?')) return;
    input.click();
  }
  async onImportFile(evt: Event): Promise<void> {
    const input = evt.target as HTMLInputElement;
    const file = input.files?.[0];
    input.value = '';                       // let the same file be re-picked later
    if (!file) return;
    let doc: unknown;
    try { doc = JSON.parse(await file.text()); }
    catch { this.saveError = 'That file isn’t valid JSON.'; return; }
    const v = validateTopology(doc);
    if (!v.ok) { this.saveError = 'Not a valid shop file: ' + (v.errors[0]?.message ?? 'invalid topology'); return; }
    this.saving = true; this.saveError = '';
    try {
      this.pushHistory(null);                 // an import is undoable like any other edit
      await this.api.putTopology(doc as Topology);
      this.topo = JSON.parse(JSON.stringify(doc)) as Topology;
      this.cells.clear();
      this.buildGraph(this.topo);
      const saved = this.savedLayout(this.topo);
      if (saved) for (const [id, c] of Object.entries(saved)) this.cells.set(id, c);
      else this.autoLayoutInto(this.cells);
      this.selectedId = null; this.dirty = false;
      this.syncNodes();
      try { this.applyLive(await this.api.getV2Status()); } catch { /* not running */ }
    } finally { this.saving = false; }
  }

  // ── graph helpers ─────────────────────────────────────────────────────────────
  private elems(t: Topology): RawEl[] { return ((t as { elements?: RawEl[] }).elements) ?? []; }

  /** A fresh shop: a primary controller, a collector, and one bare open run off it —
   *  an immediate anchor to draw/tap from (duct-first: there's always a run end to
   *  pull pipe from or drop the first fitting onto). */
  private blankTopology(): Topology {
    return {
      schemaVersion: 1,
      name: 'My Shop',
      controllers: [{ id: 'primary', role: 'primary', name: 'Shop Brain', board: 'devkitc' }],
      elements: [
        { id: 'dc', type: 'collector', name: 'Dust collector' },
        { id: 'end0', type: 'junction', name: 'Open end' },
      ],
      ducts: [{ child: 'end0', parent: 'dc' }],
    } as unknown as Topology;
  }
  private ductsRaw(): RawEl[] { return ((this.topo as { ducts?: RawEl[] } | null)?.ducts) ?? []; }
  private elem(id: string): RawEl | undefined { return this.topo ? this.elems(this.topo).find(e => e['id'] === id) : undefined; }
  private newId(prefix: string): string { let id: string; do { id = `${prefix}${++this.counter}`; } while (this.elem(id)); return id; }

  private buildGraph(t: Topology): void {
    this.parentOf.clear(); this.outletOf.clear();
    const ducts = ((t as { ducts?: RawEl[] }).ducts) ?? [];
    this.ducts = ducts.map(d => {
      const child = d['child'] as string, parent = d['parent'] as string;
      this.parentOf.set(child, parent);
      const pel = this.elem(parent);
      if (pel && isUnitKind(pel['kind']) && d['parentBranch']) {
        const idx = (pel['branches'] as Branch[]).findIndex(b => b.id === d['parentBranch']);
        if (idx >= 0) this.outletOf.set(child, { unitId: parent, index: idx });
      }
      return { childId: child, live: false, open: false };
    });
    // A run is "open" when it dead-ends at an unpopulated junction — bare pipe
    // waiting for a tool or gate. Second pass: parentOf is fully built now.
    for (const d of this.ducts) {
      const cel = this.elem(d.childId);
      d.open = cel?.['type'] === 'junction' && !cel['capped'] && this.childrenOf(d.childId).length === 0;
    }
  }
  private glyphFor(e: RawEl): Glyph {
    if (e['type'] === 'collector') return 'collector';
    if (e['type'] === 'tool') return 'tool';
    if (e['type'] === 'junction') return 'junction';
    switch (e['kind']) { case 'servoGate': return 'ballvalve'; case 'servoManifold': return 'manifold'; default: return 'slidingGate'; }
  }
  private syncNodes(): void {
    if (!this.topo) return;
    // Derived, never stored: a gate that stops being redundant stops being flagged
    // on the very next mutation, with nothing to keep in sync.
    let redundant = new Set<string>();
    try { redundant = new Set(redundantSelectors(this.topo as Topology).map(r => r.id)); } catch { /* mid-edit doc */ }
    this.nodes = this.elems(this.topo).map(e => {
      const id = e['id'] as string, c = this.cells.get(id) ?? { col: 0, row: 0 };
      const branchCount = (e['branches'] as unknown[] | undefined)?.length ?? 0;
      const glyph = this.glyphFor(e);
      const isUnit = glyph === 'slidingGate' || glyph === 'manifold';
      const el = e as AnyElement;
      const configurable = isServoSelector(el) || isLinearSelector(el);
      // One badge, two meanings — because to the person building the shop it's
      // the same question either way: is this piece finished? A gate's answer is
      // its calibration; a tool's and the collector's is whether a plug is paired.
      // Orange on an unpaired tool is not a scold: it's the "I'm switched on by
      // hand" state, which is a legitimate place to stop.
      const setup: NodeVM['setup'] =
        configurable ? (isCalibrated(el as unknown as ConfigurableSelector) ? 'done' : 'todo')
        : (glyph === 'tool' || glyph === 'collector') ? (this.hasPlugEl(e) ? 'done' : 'todo')
        : '';
      return { id, glyph, name: (e['name'] as string) || id, col: c.col, row: c.row, branchCount, isUnit, span: isUnit ? Math.max(1, branchCount) : 1, live: false, openIndex: 0, setup, redundant: redundant.has(id) };
    });
    this.byId = new Map(this.nodes.map(n => [n.id, n]));
    this.recomputeExtent();
  }
  private occupiedExcept(exclude: Set<string>): Set<string> {
    const occ = new Set<string>();
    for (const n of this.nodes) {
      if (exclude.has(n.id)) continue;
      if (n.isUnit) for (let i = 0; i < n.span; i++) occ.add((n.col + i) + ',' + n.row);
      else occ.add(n.col + ',' + n.row);
    }
    return occ;
  }
  /** The board is at least the whole visible area and grows past it as the layout
   *  does — so there's always empty grid to build onto, and it scrolls only once
   *  the shop genuinely outgrows the screen. */
  private recomputeExtent(): void {
    const maxCol = Math.max(1, ...this.nodes.map(n => n.col + n.span - 1));
    const maxRow = Math.max(1, ...this.nodes.map(n => n.row));
    const wrap = this.wrapRef?.nativeElement;
    this.vbW = Math.max(PAD * 2 + maxCol * CELL + CELL, wrap?.clientWidth ?? 0);
    this.vbH = Math.max(PAD * 2 + maxRow * CELL + CELL, wrap?.clientHeight ?? 0);
  }
  private childrenOf(id: string): string[] { const out: string[] = []; for (const [c, p] of this.parentOf) if (p === id) out.push(c); return out; }
  private canAddChild(id: string): boolean {
    const el = this.elem(id); if (!el) return false;
    if (el['type'] === 'collector') return true;
    if (el['type'] === 'junction') return true;   // a tee/open end always takes another leg
    if (el['type'] === 'selector') return (el['branches'] as Branch[]).some(b => b.role === 'blocked');
    return false;
  }
  /** Top-down auto-layout: collector on top, flow downward; unit selectors span N cells with tools below. */
  private autoLayoutInto(target: Map<string, Cell>): void {
    if (!this.topo) return;
    const root = this.elems(this.topo).find(e => e['type'] === 'collector'); if (!root) return;
    target.clear();
    let cursor = 0;
    const widthOf = (id: string): number => {
      const el = this.elem(id);
      if (isUnitKind(el?.['kind'])) return Math.max(1, (el!['branches'] as unknown[]).length);
      const kids = this.childrenOf(id).filter(k => !this.isUnitChild(k));
      if (!kids.length) return 1;
      return Math.max(1, kids.reduce((s, k) => s + widthOf(k), 0));
    };
    const place = (id: string, row: number): void => {
      const el = this.elem(id);
      if (isUnitKind(el?.['kind'])) { target.set(id, { col: cursor, row }); cursor += widthOf(id); return; }
      const kids = this.childrenOf(id).filter(k => !this.isUnitChild(k));
      if (!kids.length) { target.set(id, { col: cursor, row }); cursor += 1; return; }
      const start = cursor;
      for (const k of kids) place(k, row + 1);
      // A lone unit child: sit directly above its FIRST outlet (col of the unit) so the
      // trunk drops straight in. Otherwise center over the children's span.
      if (kids.length === 1 && isUnitKind(this.elem(kids[0])?.['kind'])) {
        target.set(id, { col: target.get(kids[0])?.col ?? start, row });
      } else {
        target.set(id, { col: Math.floor((start + cursor - 1) / 2), row });
      }
    };
    place(root['id'] as string, 0);
    for (const [toolId, o] of this.outletOf) { const uc = target.get(o.unitId); if (uc) target.set(toolId, { col: uc.col + o.index, row: uc.row + 1 }); }
  }
  private savedLayout(t: Topology): Record<string, Cell> | null { return (t as { ui?: { layout?: Record<string, Cell> } }).ui?.layout ?? null; }
  private applyLive(status: TopologyStatus): void {
    const reach = status.reachable ?? {}, actuators = status.actuators ?? {}, liveSet = new Set<string>();
    for (const [toolId, ok] of Object.entries(reach)) {
      if (!ok) continue; let cur: string | undefined = toolId;
      while (cur) { liveSet.add(cur); cur = this.parentOf.get(cur); }
    }
    for (const n of this.nodes) {
      n.live = liveSet.has(n.id);
      if (n.isUnit) {
        const state = actuators[n.id]; const branches = (this.elem(n.id)?.['branches'] as Branch[]) ?? [];
        const idx = branches.findIndex(b => b.opensState === state);
        n.openIndex = idx >= 0 ? idx : 0;
      }
    }
    for (const d of this.ducts) d.live = liveSet.has(d.childId);
  }
}
