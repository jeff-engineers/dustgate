import { AfterViewInit, Component, ElementRef, HostListener, OnDestroy, OnInit, ViewChild } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { RouterLink } from '@angular/router';
import { DomSanitizer, SafeHtml } from '@angular/platform-browser';
import { ApiService, Topology, TopologyStatus } from '../services/api.service';
import { validateTopology, airflowIssues, type AirflowIssue } from '@topology';
import { SelectorConfigComponent } from '../gates/selector-config.component';
import {
  AnyElement, ConfigurableSelector,
  isCalibrated, isLinearSelector, isServoSelector,
} from '../gates/selector-types';

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

type Glyph = 'collector' | 'slidingGate' | 'ballvalve' | 'manifold' | 'junction' | 'tool';
type SelKind = 'linear' | 'servoGate' | 'servoManifold';
const CELL = 108;
const PAD = 64;
const UNIT_H = 46;
const GATE_PAD = 0.42 * CELL;
const TOOL_HALF = 24;

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
  dragX?: number; dragY?: number;
}
interface DuctVM { childId: string; live: boolean; open: boolean; }
/** A tee point on a run. `axis` is the direction the run travels here, so a new
 *  leg can be sent off perpendicular to it. */
interface BDot { x: number; y: number; childId: string; col: number; row: number; axis: 'h' | 'v'; }
interface ODot { x: number; y: number; parentId: string; branchId?: string; cell: Cell; }

type Fitting = SelKind | 'tool' | 'duct';
type MenuKind = Fitting | 'cap' | 'uncap' | 'delete' | 'configure';

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
  imports: [CommonModule, FormsModule, RouterLink, SelectorConfigComponent],
  styles: [`
    :host { display: flex; flex-direction: column; height: 100dvh; height: 100vh; overflow: hidden; }
    .bar { display: flex; align-items: center; gap: 12px; padding: 12px 16px; border-bottom: 1px solid var(--border); flex-shrink: 0; }
    .bar .title { font-size: 15px; font-weight: 600; flex: 1; }
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
    .node .body, .node .unit { fill: var(--surface); stroke: var(--border-strong, #444); stroke-width: 1.5; }
    .node.sel .body, .node.sel .unit { stroke: var(--accent); stroke-width: 2.5; }
    .node.live .body, .node.live .unit { stroke: var(--success); }
    .glabel { fill: var(--text); font-size: 12.5px; text-anchor: middle; font-weight: 500; }
    .gsub   { fill: var(--muted); font-size: 10.5px; text-anchor: middle; }
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
    .inspector .cfg { background: var(--bg); border: 1px solid var(--border); color: var(--text);
                      border-radius: 8px; padding: 7px 12px; font-size: 13px; }
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

          <path *ngFor="let d of ducts" class="duct" [class.live]="d.live" [attr.d]="ductD(d.childId)"/>
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
            <text *ngIf="n.glyph !== 'junction'" class="glabel" [attr.x]="labelX(n)" [attr.y]="labelY(n)">{{ n.name }}</text>
            <text *ngIf="n.glyph === 'tool'" class="gsub" y="42">{{ toolAuto(n.id) ? 'auto' : 'manual' }}</text>
            <!-- Setup state of a gate, so an unfinished shop reads at a glance rather
                 than only when the Live view refuses to run. -->
            <g *ngIf="n.setup" [attr.transform]="'translate(' + todoX(n) + ',' + todoY(n) + ')'">
              <title>{{ n.setup === 'done' ? 'Set up' : 'Needs setting up' }}</title>
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

    <!-- Element controls, anchored right above the selected piece (not a bottom bar).
         Never for run ends (they'd render an empty box — they use the menu), and
         hidden while a menu is open so the two don't stack on each other. -->
    <div class="inspector" *ngIf="inspectedPiece() as ins" [style.left]="inspectorPos().left" [style.top]="inspectorPos().top">
      <input [ngModel]="ins.name" (ngModelChange)="rename(ins.id, $event)" placeholder="Name"/>
      <span class="meta" *ngIf="ins.glyph === 'slidingGate'">
        <button class="step" (click)="changeOutlets(ins.id, -2)" [disabled]="!canRemoveOutlets(ins)">−</button>
        {{ ins.branchCount }} outlets
        <button class="step" (click)="changeOutlets(ins.id, 2)">+</button>
      </span>
      <span class="meta" *ngIf="ins.glyph === 'manifold'">2 outlets</span>
      <!-- Gates carry measurements the graph can't infer — servo angles, or a slider's
           swept rail. Everything else is fully described by where it sits. -->
      <button class="cfg" *ngIf="ins.setup" (click)="configure(ins.id)">
        {{ ins.setup === 'todo' ? 'Set up' : 'Configure' }}
      </button>
    </div>

    <!-- Configuring one gate — a sheet over the canvas, so the layout stays put. -->
    <div class="sheet-bg" *ngIf="configuring" (pointerdown)="configuring = null"></div>
    <div class="sheet" *ngIf="configuring as cfg">
      <app-selector-config [sel]="cfg" [topo]="topoDoc"
                           (saved)="onConfigured($event)" (cancelled)="configuring = null">
      </app-selector-config>
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
  private past: string[] = [];
  private future: string[] = [];
  private lastTag: string | null = null;
  airflowErrors: AirflowIssue[] = [];
  vbW = 400; vbH = 300;
  menu: { x: number; y: number; branch?: BDot; end?: string; convert?: string;
          addOutput?: { parentId: string; branchId?: string; cell: Cell } } | null = null;
  /** The one option list, resolved once when the menu opens (see openMenu). */
  menuOptions: MenuOption[] = [];
  menuTitle = '';
  readonly CELL = CELL; readonly UNIT_H = UNIT_H; readonly GATE_PAD = GATE_PAD;

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

  /** Ortho corner points for a duct — the base geometry BEFORE obstacle avoidance.
   *  Drop-jog-drop so ducts enter node TOPS and leave node BOTTOMS. */
  private baseDuctPoints(childId: string): { x: number; y: number }[] {
    const child = this.byId.get(childId); if (!child) return [];
    const o = this.outletOf.get(childId);
    if (o) {  // hangs off a specific unit outlet
      const unit = this.byId.get(o.unitId); if (!unit) return [];
      const ox = this.nx(unit) + o.index * CELL, oy = this.ny(unit) + UNIT_H / 2 + 12;
      const cxp = this.nx(child), cyTop = this.ny(child) - this.halfH(child);
      if (Math.abs(cxp - ox) < 1) return [{ x: ox, y: oy }, { x: ox, y: cyTop }];
      const jy = this.clearLaneY(ox, cxp, oy, cyTop, childId);
      return [{ x: ox, y: oy }, { x: ox, y: jy }, { x: cxp, y: jy }, { x: cxp, y: cyTop }];
    }
    const pid = this.parentOf.get(childId); const parent = pid ? this.byId.get(pid) : undefined;
    if (!parent) return [];
    if (child.isUnit) {  // a unit feeds from the TOP, in line with its FIRST outlet —
      // so a parent placed above outlet 0 drops straight in (no left-then-down jog).
      const ix = this.nx(child), iy = this.ny(child) - UNIT_H / 2;
      const px = this.nx(parent), pBot = this.ny(parent) + this.halfH(parent);
      if (Math.abs(px - ix) < 1) return [{ x: ix, y: pBot }, { x: ix, y: iy }];
      let jy = iy - 22;
      if (jy < pBot + 12) jy = (pBot + iy) / 2;
      return [{ x: px, y: pBot }, { x: px, y: jy }, { x: ix, y: jy }, { x: ix, y: iy }];
    }
    // A junction is a TEE on a run: the continuation drops straight through it, and a
    // branch taps off PERPENDICULAR — leaves the tee horizontally, then turns down to
    // its leg (never a parallel second vertical).
    if (parent.glyph === 'junction') {
      const jx = this.nx(parent), jy = this.ny(parent);
      const cxj = this.nx(child), cTopj = this.ny(child) - this.halfH(child);
      if (Math.abs(cxj - jx) < 1) return [{ x: jx, y: jy }, { x: jx, y: cTopj }];
      return [{ x: jx, y: jy }, { x: cxj, y: jy }, { x: cxj, y: cTopj }];
    }
    const cx = this.nx(child), px = this.nx(parent);
    const pBot = this.ny(parent) + this.halfH(parent), cTop = this.ny(child) - this.halfH(child);
    if (Math.abs(cx - px) < 1) return [{ x: px, y: pBot }, { x: px, y: cTop }];   // straight drop
    const jogY = this.clearLaneY(px, cx, pBot, cTop, childId);
    return [{ x: px, y: pBot }, { x: px, y: jogY }, { x: cx, y: jogY }, { x: cx, y: cTop }];
  }
  /** Lane height (px below the source) for a jog. A MONOTONIC, injective function of
   *  SIGNED column distance: every distinct target column gets its own lane, so two
   *  runs off the same source — even ones heading opposite ways — never share a
   *  horizontal (leftward runs jog shallow, rightward deep). Deterministic per duct. */
  private laneOffset(dx: number): number {
    const colDist = Math.max(-5, Math.min(5, Math.round(dx / CELL)));
    return 14 + (colDist + 5) * 7;   // -5→14 … +5→84, strictly increasing in colDist
  }

  /**
   * Pick the height for a duct's horizontal traverse: the first lane between the
   * source and the target that crosses NO device.
   *
   * The natural lane is just under the source (laneOffset), which staggers siblings
   * nicely and is nearly always clear. But when the target sits two or more rows
   * down, that lane runs straight through whatever occupies the row between — the
   * tools hanging off a manifold, typically — and the old code left it there for
   * avoidDevices to skirt, which produced a run that lassoed its way around a tool
   * box. So: if the near lane is blocked, traverse just ABOVE the target instead
   * (i.e. below the obstructions), then scan the gap. Still a pure function of this
   * duct's endpoints plus device boxes, so adding a sibling never reroutes it.
   */
  private clearLaneY(x0: number, x1: number, yFrom: number, yTo: number, childId: string): number {
    const near = yFrom + this.laneOffset(x1 - x0);
    const mid = (yFrom + yTo) / 2;
    const fallback = near > yTo - 10 ? mid : near;
    if (yTo - yFrom < 44) return fallback;                       // no room to be clever
    const boxes = this.deviceBoxes(childId);
    // The whole dogleg has to be clear, not just the horizontal: drop, traverse,
    // drop. Checking only the middle leg is what left the last drop cutting through
    // whatever sat directly above the target (a tool in the same column), which
    // avoidDevices then lassoed around.
    const clear = (y: number) => !boxes.some(b =>
      this.segBoxHit({ x: x0, y: yFrom }, { x: x0, y }, b) ||
      this.segBoxHit({ x: x0, y }, { x: x1, y }, b) ||
      this.segBoxHit({ x: x1, y }, { x: x1, y: yTo }, b));
    // Just above the target — i.e. BELOW anything in between — nudged by the same
    // signed-column stagger so two runs arriving in one corridor don't coincide.
    const low = yTo - 14 - (this.laneOffset(x1 - x0) - 14) / 5;
    const candidates = [fallback, low];
    for (let y = yTo - 24; y > yFrom + 20; y -= 16) candidates.push(y);   // search upward from the target
    return candidates.find(y => y > yFrom && y < yTo && clear(y)) ?? fallback;
  }

  /** The route the whole app uses: base geometry, then detoured AROUND any device it
   *  would run over (its own endpoints exempt). Everything — the drawn path, branch
   *  dots, the hit target, and the device-crossing block — reads this, so they stay
   *  consistent. Clear routes (the common case) pass straight through untouched. */
  private ductPoints(childId: string): { x: number; y: number }[] {
    return this.avoidDevices(this.baseDuctPoints(childId), childId);
  }

  /** Axis-aligned obstacle boxes for the devices a duct should avoid (inflated for
   *  clearance). The duct's own parent + child are exempt (they're its endpoints);
   *  junctions are tiny dots, not obstacles. */
  private deviceBoxes(childId: string): { x0: number; y0: number; x1: number; y1: number }[] {
    const parentId = this.parentOf.get(childId), M = 15;
    const out: { x0: number; y0: number; x1: number; y1: number }[] = [];
    for (const n of this.nodes) {
      if (n.id === childId || n.id === parentId || n.glyph === 'junction') continue;
      let x0: number, x1: number;
      if (n.isUnit) { x0 = this.nx(n) - GATE_PAD; x1 = this.nx(n) + (n.span - 1) * CELL + GATE_PAD; }
      else { const hw = this.halfW(n); x0 = this.nx(n) - hw; x1 = this.nx(n) + hw; }
      const hh = this.halfH(n);
      out.push({ x0: x0 - M, y0: this.ny(n) - hh - M, x1: x1 + M, y1: this.ny(n) + hh + M });
    }
    return out;
  }
  private halfW(n: NodeVM): number {
    if (n.isUnit) return (n.span - 1) * CELL / 2 + GATE_PAD;
    switch (n.glyph) { case 'collector': return 30; case 'ballvalve': return 22; case 'junction': return 8; default: return 38; }
  }

  /** Reroute an ortho polyline around device boxes: for each segment that runs through
   *  a box, jog out past the nearer edge and back. Iterates so a detour that meets a
   *  second device gets routed too; bails after a few passes if boxed in. */
  private avoidDevices(pts: { x: number; y: number }[], childId: string): { x: number; y: number }[] {
    if (pts.length < 2) return pts;
    const boxes = this.deviceBoxes(childId);
    if (!boxes.length) return pts;
    let cur = pts;
    for (let iter = 0; iter < 5; iter++) {
      let hit = false;
      const next: { x: number; y: number }[] = [cur[0]];
      for (let i = 0; i < cur.length - 1; i++) {
        const a = cur[i], b = cur[i + 1];
        const box = this.firstHitBox(a, b, boxes);
        if (!box) { next.push(b); continue; }
        for (const p of this.detourSeg(a, b, box, boxes).slice(1)) next.push(p);
        hit = true;
      }
      cur = this.simplifyPts(next);
      if (!hit) break;
    }
    return cur;
  }
  private segBoxHit(a: { x: number; y: number }, b: { x: number; y: number }, box: { x0: number; y0: number; x1: number; y1: number }): boolean {
    if (Math.abs(a.x - b.x) < 0.5) {
      const x = a.x, lo = Math.min(a.y, b.y), hi = Math.max(a.y, b.y);
      return x > box.x0 && x < box.x1 && hi > box.y0 && lo < box.y1;
    }
    const y = a.y, lo = Math.min(a.x, b.x), hi = Math.max(a.x, b.x);
    return y > box.y0 && y < box.y1 && hi > box.x0 && lo < box.x1;
  }
  private firstHitBox(a: { x: number; y: number }, b: { x: number; y: number }, boxes: { x0: number; y0: number; x1: number; y1: number }[]) {
    let best: { x0: number; y0: number; x1: number; y1: number } | null = null, bestD = Infinity;
    for (const box of boxes) {
      if (!this.segBoxHit(a, b, box)) continue;
      const cx = (box.x0 + box.x1) / 2, cy = (box.y0 + box.y1) / 2;
      const d = Math.hypot(cx - a.x, cy - a.y);
      if (d < bestD) { bestD = d; best = box; }
    }
    return best;
  }
  /** Waypoints [a, …, b] that skirt `box` on the side with the most room — preferring
   *  a side whose detour lane is clear of other devices and inside the canvas. */
  private detourSeg(a: { x: number; y: number }, b: { x: number; y: number }, box: { x0: number; y0: number; x1: number; y1: number }, boxes: { x0: number; y0: number; x1: number; y1: number }[]): { x: number; y: number }[] {
    if (Math.abs(a.x - b.x) < 0.5) {                       // vertical → jog left or right
      const x = a.x, down = b.y > a.y;
      const enter = down ? box.y0 : box.y1, exit = down ? box.y1 : box.y0;
      const yLo = Math.min(enter, exit), yHi = Math.max(enter, exit);
      const laneClear = (lx: number) => lx >= 4 && lx <= this.vbW - 4 &&
        !boxes.some(o => o !== box && lx > o.x0 && lx < o.x1 && yHi > o.y0 && yLo < o.y1);
      const leftX = box.x0 - 2, rightX = box.x1 + 2;
      const roomier = box.x0 >= this.vbW - box.x1 ? leftX : rightX;   // more canvas room
      const sideX = laneClear(roomier) ? roomier : laneClear(rightX) ? rightX : laneClear(leftX) ? leftX : roomier;
      return [a, { x, y: enter }, { x: sideX, y: enter }, { x: sideX, y: exit }, { x, y: exit }, b];
    }
    const y = a.y, right = b.x > a.x;                       // horizontal → jog up or down
    const enter = right ? box.x0 : box.x1, exit = right ? box.x1 : box.x0;
    const xLo = Math.min(enter, exit), xHi = Math.max(enter, exit);
    const laneClear = (ly: number) => ly >= 4 && ly <= this.vbH - 4 &&
      !boxes.some(o => o !== box && ly > o.y0 && ly < o.y1 && xHi > o.x0 && xLo < o.x1);
    const upY = box.y0 - 2, downY = box.y1 + 2;
    const roomier = box.y0 >= this.vbH - box.y1 ? upY : downY;
    const sideY = laneClear(roomier) ? roomier : laneClear(downY) ? downY : laneClear(upY) ? upY : roomier;
    return [a, { x: enter, y }, { x: enter, y: sideY }, { x: exit, y: sideY }, { x: exit, y }, b];
  }
  /** Drop duplicate + collinear points so detours don't bloat the polyline. */
  private simplifyPts(pts: { x: number; y: number }[]): { x: number; y: number }[] {
    const dedup: { x: number; y: number }[] = [];
    for (const p of pts) {
      const last = dedup[dedup.length - 1];
      if (last && Math.abs(last.x - p.x) < 0.5 && Math.abs(last.y - p.y) < 0.5) continue;
      dedup.push(p);
    }
    const res: { x: number; y: number }[] = [];
    for (let i = 0; i < dedup.length; i++) {
      const a = dedup[i - 1], b = dedup[i], c = dedup[i + 1];
      if (a && c && ((Math.abs(a.x - b.x) < 0.5 && Math.abs(b.x - c.x) < 0.5) || (Math.abs(a.y - b.y) < 0.5 && Math.abs(b.y - c.y) < 0.5))) continue;
      res.push(b);
    }
    return res;
  }

  /** Plain ortho path (no hops) — used for the fat invisible hit target. */
  ductPath(childId: string): string {
    const p = this.ductPoints(childId); if (!p.length) return '';
    return 'M ' + p.map(pt => `${pt.x} ${pt.y}`).join(' L ');
  }

  /** Visible path with ROUNDED corners + crossover hops. Corners are arced (radius
   *  CORNER_R) so two ducts whose corners land near the same point curve apart with a
   *  gap instead of overlapping into an X. Where a horizontal segment crosses another
   *  duct's vertical it bumps over it (electrical-diagram convention). */
  ductD(childId: string): string {
    const pts = this.ductPoints(childId); if (!pts.length) return '';
    if (pts.length === 1) return `M ${pts[0].x} ${pts[0].y}`;
    const CORNER_R = 12, HOP_R = 5;
    // vertical segments of OTHER ducts (for hops)
    const verts: { x: number; y1: number; y2: number }[] = [];
    for (const dd of this.ducts) {
      if (dd.childId === childId) continue;
      const op = this.ductPoints(dd.childId);
      for (let i = 0; i < op.length - 1; i++)
        if (Math.abs(op[i].x - op[i + 1].x) < 0.5)
          verts.push({ x: op[i].x, y1: Math.min(op[i].y, op[i + 1].y), y2: Math.max(op[i].y, op[i + 1].y) });
    }
    const dist = (p: { x: number; y: number }, q: { x: number; y: number }) => Math.hypot(q.x - p.x, q.y - p.y);
    const toward = (from: { x: number; y: number }, to: { x: number; y: number }, r: number) => {
      const L = dist(from, to) || 1; return { x: from.x + (to.x - from.x) / L * r, y: from.y + (to.y - from.y) / L * r };
    };
    // straight run a→b; a horizontal one bumps over any crossed verticals
    const run = (a: { x: number; y: number }, b: { x: number; y: number }): string => {
      if (Math.abs(a.y - b.y) >= 0.5) return ` L ${b.x} ${b.y}`;   // vertical → straight
      const y = a.y, dir = b.x > a.x ? 1 : -1; let s = '';
      const xs = verts
        .filter(v => v.x > Math.min(a.x, b.x) + HOP_R && v.x < Math.max(a.x, b.x) - HOP_R && y > v.y1 + 1 && y < v.y2 - 1)
        .map(v => v.x).sort((m, n) => dir * (m - n));
      for (const xc of xs) s += ` L ${xc - dir * HOP_R} ${y} A ${HOP_R} ${HOP_R} 0 0 ${dir > 0 ? 0 : 1} ${xc + dir * HOP_R} ${y}`;
      return s + ` L ${b.x} ${b.y}`;
    };
    const n = pts.length;
    let d = `M ${pts[0].x} ${pts[0].y}`;
    let start = pts[0];
    for (let i = 1; i < n; i++) {
      if (i === n - 1) { d += run(start, pts[i]); break; }
      const r = Math.min(CORNER_R, dist(pts[i - 1], pts[i]) / 2, dist(pts[i], pts[i + 1]) / 2);
      const before = toward(pts[i], pts[i - 1], r), after = toward(pts[i], pts[i + 1], r);
      d += run(start, before) + ` Q ${pts[i].x} ${pts[i].y} ${after.x} ${after.y}`;
      start = after;
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

  /** Approximate half-height of a glyph, for anchoring ducts to top/bottom edges. */
  private halfH(n: NodeVM): number {
    if (n.isUnit) return UNIT_H / 2;
    switch (n.glyph) {
      case 'collector': return 30;
      case 'ballvalve': return 22;
      case 'junction':  return 8;
      case 'tool':      return TOOL_HALF;
      default:          return TOOL_HALF;
    }
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
  labelY(n: NodeVM): number { return n.glyph === 'tool' ? 4 : (n.isUnit ? -UNIT_H / 2 - 9 : -34); }
  toolAuto(id: string): boolean { return !!(this.elem(id)?.['sensor'] as RawEl | undefined)?.['outlet']; }
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
  inspectedPiece(): NodeVM | null {
    const n = this.inspected();
    return n && n.glyph !== 'junction' && !this.menu ? n : null;
  }

  /** The doc, for the config sheet's bindings only — `configuring` is never set unless
   *  a topology is loaded, so the non-null assertion holds. */
  get topoDoc(): Topology { return this.topo!; }

  configure(id: string): void {
    const el = this.elems(this.topo!).find(e => e['id'] === id) as AnyElement | undefined;
    if (isServoSelector(el) || isLinearSelector(el)) this.configuring = el as unknown as ConfigurableSelector;
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
    pt.y = this.ny(n) - (n.isUnit ? UNIT_H / 2 : (n.glyph === 'tool' ? TOOL_HALF : 22));
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
    } else if (n && this.isGate(n)) {
      // A tap on a placed gate offers the other kinds, in the same menu style.
      this.openMenu(evt.clientX, evt.clientY, { convert: n.id });
    }
    this.dragId = null; this.detachDrag();
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
    const occ = this.occupiedExcept(new Set([n.id]));
    const cells: Cell[] = n.isUnit ? Array.from({ length: n.span }, (_, i) => ({ col: col + i, row })) : [{ col, row }];
    if (cells.some(c => occ.has(c.col + ',' + c.row))) return false;
    // No duct may cross a device. Test at the CANDIDATE position (so the moved node's
    // own ducts reroute), then require every device to be clear of every foreign duct —
    // this catches both "device lands on a duct" and "moved duct now runs through
    // another device". Restore position afterwards.
    const sc = n.col, sr = n.row, dx = n.dragX, dy = n.dragY;
    n.col = col; n.row = row; n.dragX = undefined; n.dragY = undefined;
    let ok = true;
    for (const m of this.nodes) {
      if (m.glyph === 'junction' || m.glyph === 'collector') continue;   // devices only
      if (this.deviceCrossed(m)) { ok = false; break; }
    }
    n.col = sc; n.row = sr; n.dragX = dx; n.dragY = dy;
    return ok;
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
      for (let i = 0; i < pts.length - 1; i++) if (this.segBoxHit(pts[i], pts[i + 1], box)) return true;
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
        if (this.ptSegDist(cx, cy, pts[i], pts[i + 1]) < thresh) return true;
    }
    return false;
  }
  private ptSegDist(px: number, py: number, a: { x: number; y: number }, b: { x: number; y: number }): number {
    const dx = b.x - a.x, dy = b.y - a.y, L2 = dx * dx + dy * dy;
    let t = L2 ? ((px - a.x) * dx + (py - a.y) * dy) / L2 : 0;
    t = Math.max(0, Math.min(1, t));
    return Math.hypot(px - (a.x + t * dx), py - (a.y + t * dy));
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
    this.menuTitle = ctx.convert ? 'Change this gate'
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
        if (!t || !t.cells.every(c => this.roomAt(c.col, c.row, t.span, t.selfId, t.occ))) { enabled = false; note = 'no room'; }
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

  /** The three gate kinds, for a tap on a placed gate. */
  private convertOptions(id: string): MenuOption[] {
    const n = this.byId.get(id); if (!n) return [];
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
        out.push({ x: this.nx(n) + this.halfH(n) + 16, y: this.ny(n), parentId: n.id, cell: { col: n.col + 1, row: n.row + 1 } });
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
  private roomAt(col: number, row: number, span: number, selfId: string, occ = this.cellOccupied()): boolean {
    if (col < 0 || row < 0) return false;
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
    if (parentEl && parentEl['type'] === 'junction') {
      junctionId = parentId;                          // already a tee — just add a leg
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
    const channel = this.elems(this.topo).filter(e => e['type'] === 'selector' && e['kind'] !== 'linear').length;
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
    const channel = this.elems(this.topo).filter(e => e['type'] === 'selector' && e['kind'] !== 'linear').length;
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
  private freeServoChannel(): number {
    const taken = new Set(
      this.elems(this.topo!)
        .filter(e => e['type'] === 'selector' && e['kind'] !== 'linear')
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
    const base: RawEl = { id: this.newId('sel'), type: 'selector', name: this.defaultName(kind), controllerId: 'primary', kind };
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
      ?? this.elems(this.topo).filter(e => e['type'] === 'selector' && e['kind'] !== 'linear').length;
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

  /**
   * The one contextual line in the guide bar. Priority: a failed save first, then
   * live airflow problems (with a fix + Cap action), then a save confirmation,
   * then onboarding (empty shop) → progress nudge.
   */
  get guide(): { text: string; kind: 'info' | 'warn' | 'ok'; cap?: boolean } {
    if (this.saveError) return { text: this.saveError, kind: 'warn' };
    if (this.wip) return { text: `Work in progress — saved here, but the controller won’t take it yet: ${this.wip}.`, kind: 'warn' };

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
      const channel = this.elems(this.topo).filter(e => e['type'] === 'selector' && e['kind'] !== 'linear').length;
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
    this.nodes = this.elems(this.topo).map(e => {
      const id = e['id'] as string, c = this.cells.get(id) ?? { col: 0, row: 0 };
      const branchCount = (e['branches'] as unknown[] | undefined)?.length ?? 0;
      const glyph = this.glyphFor(e);
      const isUnit = glyph === 'slidingGate' || glyph === 'manifold';
      const el = e as AnyElement;
      const configurable = isServoSelector(el) || isLinearSelector(el);
      const setup: NodeVM['setup'] = !configurable ? ''
        : isCalibrated(el as unknown as ConfigurableSelector) ? 'done' : 'todo';
      return { id, glyph, name: (e['name'] as string) || id, col: c.col, row: c.row, branchCount, isUnit, span: isUnit ? Math.max(1, branchCount) : 1, live: false, openIndex: 0, setup };
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
