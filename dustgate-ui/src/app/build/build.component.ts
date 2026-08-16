import { AfterViewInit, Component, ElementRef, HostListener, NgZone, OnDestroy, OnInit, ViewChild } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { ActivatedRoute, Router as NgRouter, RouterLink } from '@angular/router';
import { DomSanitizer, SafeHtml } from '@angular/platform-browser';
import { ApiService, DiscoveredOutlet, NodeLinkState, Topology, TopologyStatus } from '../services/api.service';
import { airflowIssues, redundantSelectors, type AirflowIssue } from '@topology';
import { validateShop, SHOP_SCHEMA_VERSION } from '@shop';
import { SelectorConfigComponent } from '../gates/selector-config.component';
import { ElementOutletConfigComponent } from '../tools/element-outlet-config.component';
import { matchAll } from '../tools/outlet-match';
import {
  AnyElement, ConfigurableSelector, elementsOf,
  controllersOf, firstFreeChannel, isCalibrated, isLinearSelector, isServoSelector,
  selectorsOnController,
} from '../gates/selector-types';
import {
  type ShopDoc, type ShopSystem,
  addMachineWithPort, addSupplementalPort, addSystem, isPortSupplemental, machineOfPort,
  machinesOf, outletOf, portsOf, primaryPortOf, removeMachine, removePort, supplementalCount,
  systemById, systemsOf, systemViews, toShop,
} from '../services/shop-doc';
import {
  type Glyph, type Pt, type SceneNode,
  CELL, GATE_PAD, PAD, TOOL_HALF, UNIT_H,
  deviceBox, halfH as glyphHalfH, halfW as glyphHalfW, ptSegDist, segBoxHit,
} from './routing/geometry';
import { type RoutedDuct, type Scene, Router, sceneBounds } from './routing/router';
import { CanvasViewport } from './canvas-viewport';
import {
  BOARD_H, BOARD_SLOT, BOARD_W, FIND_W, PORT_H, RAIL_H, SERVO_PORTS, TAB_H, TAB_W,
  cablePath, cableRun, crossing,
  portPos, portWidth, portExit, railSlot, rankByTravel, segmentsOf, slotAt,
} from './wiring/wire-geometry';

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

/** Empty cell rows between one system's last band and the next system's first.
 *  ONE — the grey ground under each system is what says where it stops, so the gap
 *  only has to be big enough that the two grounds don't touch. Two rows read as a
 *  missing band; a labelled gutter reads as two documents. */
const SYSTEM_GAP = 1;
/** How far a system's grey ground stops short of its outermost row. Enough that two
 *  bands dragged flush against each other still read as two. */
const GROUND_INSET = 10;
/** Depth of the fade below a pinned rail. The board passes UNDER the rail rather than
 *  being cut off by it: a plain edge slices a gate bar mid-glyph and the drawing reads
 *  as two panes. Also the depth of the free lane — see cableCost(). */
const SCRIM_H = 60;
/** Where the first pickup hood sits on a machine's top edge, and the pitch between
 *  them. Right of centre, because the trunk's own inlet lands at the middle and every
 *  drawing that already exists has to keep landing there. */
const PICKUP_DX = 20;
const PICKUP_STEP = 20;
/** Extra pickups one machine may have. Mirrors MAX_SUPPLEMENTAL_PORTS in shop.js —
 *  change one, change both. */
const MAX_PICKUPS = 2;

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
  /** Offset from the node's own cell, in board px. Only a `pickup` uses it: a
   *  supplemental port has NO cell — it rides on the top edge of the machine's box,
   *  which is what keeps one machine drawn as one machine. */
  anchor?: { dx: number; dy: number };
  dragX?: number; dragY?: number;
}
interface DuctVM { childId: string; live: boolean; open: boolean; }
/** A tee point on a run. `axis` is the direction the run travels here, so a new
 *  leg can be sent off perpendicular to it. `elbow` marks a corner rather than a
 *  point on a straight; corners carry `legs` — the cells a new leg could take,
 *  already in preference order, because a corner's free directions depend on which
 *  way it turns and can't be derived from `axis` alone. */
interface BDot { x: number; y: number; childId: string; col: number; row: number; axis: 'h' | 'v'; elbow?: boolean; legs?: Cell[]; }
interface ODot { x: number; y: number; parentId: string; branchId?: string; cell: Cell; }

// ── Wiring layer ──────────────────────────────────────────────────────────────
// A second view of the same canvas: where the boards are and what cable runs to
// what. Nothing here is new information — a gate has carried `controllerId` and
// `servo.channel` since the schema existed, which is exactly "which board, which
// port". It has just never had a picture, and a board has never had a place.
//
// Board placements ride in the doc under `ui.wiring.boards`, deliberately NOT in
// `ui.layout`: board ids are minted from mDNS hostnames (see /boards) and could
// collide with an element id in one flat map.

/** A placed board, ready to draw. `used` maps servo channel → the gate on it. */
interface BoardVM {
  id: string; name: string; primary: boolean;
  col: number; row: number; x: number; y: number;
  used: Map<number, string>;
  servoCount: number;
  dragX?: number; dragY?: number;
}
/** One cable run, port → the gate's servo tab. */
interface CableVM { id: string; gateId: string; boardId: string; channel: number; d: string; shade: string; }

/** One shade per board, so a wire can be traced back to the brain it leaves without
 *  following it.
 *
 *  All four stay firmly in the blue family, and deliberately so: azure means WIRE on
 *  this canvas the way amber means setup, green means live and red means fault. A
 *  board tinted toward green would claim a state it isn't in. What varies is hue
 *  within the band and not value, because a board drawn brighter than its neighbour
 *  reads as the important one — and none of them is.
 *
 *  Both ends take it: the cable, and the gate's servo tab. The tab is the half you
 *  can see when the rail is scrolled away, which makes it the half that has to
 *  answer "which brain is this on". */
const CABLE_SHADES = ['#38b6f0', '#45cfd8', '#6f9df2', '#2f9fd0'];


type Fitting = SelKind | 'tool' | 'duct';
type MenuKind = Fitting | 'cap' | 'uncap' | 'delete' | 'board' | 'travel' | 'outlet' | 'pickup';

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

/** How many legs a fitting can feed. A ball valve is a two-port device: one in, one
 *  out. Only the units fan out. */
const outletsFor = (kind: MenuKind): number => kind === 'linear' ? 4 : kind === 'servoManifold' ? 2 : 1;

@Component({
  selector: 'app-build',
  standalone: true,
  imports: [CommonModule, FormsModule, RouterLink, SelectorConfigComponent, ElementOutletConfigComponent],
  styleUrls: ['./build.component.css'],
  templateUrl: './build.component.html',
})
export class BuildComponent implements OnInit, AfterViewInit, OnDestroy {
  @ViewChild('svg') svgRef?: ElementRef<SVGSVGElement>;
  @ViewChild('wrap') wrapRef?: ElementRef<HTMLDivElement>;
  @ViewChild('menuEl') menuRef?: ElementRef<HTMLDivElement>;

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
  /** Which half of that sheet the menu asked for. */
  configPane: 'board' | 'travel' = 'board';
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
  readonly TOOL_HALF = TOOL_HALF;
  readonly BOARD_W = BOARD_W; readonly BOARD_H = BOARD_H; readonly RAIL_H = RAIL_H;
  readonly SCRIM_H = SCRIM_H;
  readonly PORT_H = PORT_H; readonly TAB_W = TAB_W; readonly TAB_H = TAB_H;
  readonly SERVO_PORTS = SERVO_PORTS; readonly FIND_W = FIND_W;

  // ── wiring layer ────────────────────────────────────────────────────────────
  /** Which view of the canvas is on. The pieces and their cells are identical in
   *  both — only what's drawn over them changes. */
  /** controllerId → slot index along the rail. Boards are one-dimensional: the rail
   *  sits above the whole grid, which is what makes "a cable always leaves the
   *  underside of its port and goes down" an invariant instead of a hope. */
  private boardSlots = new Map<string, number>();
  /** Overflow menu in the toolbar — everything that isn't Save or the layer switch. */
  moreOpen = false;
  /** The WiFi network the boards are on, once the device tells us. Empty until then,
   *  and empty forever on a device too old to report it — the rail just shows
   *  BOARDS, which is honest, rather than a guess at the shop's network. */
  netName = '';
  /** Mid-drag rubber cable. `mode` says which END came loose: from a tab you're
   *  hunting a port, from a port you're hunting a tab. */
  wireDrag: {
    mode: 'toPort' | 'toGate';
    gateId?: string;                                   // anchored end, mode toPort
    port?: { boardId: string; channel: number };       // anchored end, mode toGate
    from: Pt; to: Pt;
    over: { boardId: string; channel: number } | null; // hovered port
    overGate: string | null;                           // hovered tab
  } | null = null;
  /** A tray chip being dragged onto the canvas, or a placed board being moved. */
  private bodrag: { id: string; dx: number; dy: number; fromTray: boolean; moved: boolean } | null = null;
  /** Why the cable being dragged can't land where the pointer is, or ''. */
  wireBlocked = '';
  /** What a legal drop will DO, when that isn't just "connect" — currently a swap. */
  wireNote = '';
  private wMove = (e: PointerEvent) => this.onWireMove(e);
  private wUp = () => this.onWireUp();
  private boMove = (e: PointerEvent) => this.onBoardMove(e);
  private boUp = () => this.onBoardUp();

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

  constructor(private api: ApiService, private route: ActivatedRoute,
              private nav: NgRouter, private zone: NgZone, sanitizer: DomSanitizer) {
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
      // A hood: the tapered inlet a second pickup draws on the machine's edge.
      pickup:        svg('<path d="M6 17 L12 7 L18 17 Z"/><path d="M4 20h16"/>'),
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
    // Migrate on read, once, at the boundary. The device still serves whichever
    // shape it was last given (its own reader handles both), so this is the only
    // place a v1 layout becomes a shop — a lazier conversion would write back a
    // half-migrated document the first time someone hit Save.
    const shop = toShop(loaded);
    this.topo = shop && elementsOf(shop as unknown as Topology).some(e => e.type === 'collector')
      ? (shop as unknown as Topology)
      : this.blankTopology();
    this.activeSystemId = (this.topo as unknown as ShopDoc).systems?.[0]?.id ?? null;
    this.buildGraph(this.topo);
    const saved = this.savedLayout(this.topo);
    if (saved) for (const [id, c] of Object.entries(saved)) this.cells.set(id, c);
    else this.autoLayoutInto(this.cells);
    this.loadBoardCells(this.topo);
    this.syncNodes();
    await this.mergePairedBoards();
    // /boards used to hand you back with ?layer=wiring so you'd land on the view
    // the boards belonged to. There is only one view now, so the parameter is
    // ignored — harmlessly, for any link still carrying it.
    try { this.applyLive(await this.api.getStatus()); } catch { /* not running */ }
    // KEEP it live. This used to be a single fetch at load, so the duct highlighting
    // was a snapshot from whenever the page opened — and the plug row, which shows a
    // wattage, would have been worse: a number that looks live and isn't.
    // Sweep for plugs ONCE, and only when there's a job for them: a machine on the
    // canvas with nothing paired to it. A shop that's fully wired opens without an
    // mDNS sweep and without a tray.
    if (this.unpairedMachines().length) void this.scanOutlets();
    this.livePoll = setInterval(() => {
      void this.api.getStatus().then(st => this.applyLive(st)).catch(() => { /* offline */ });
    }, 2000);
    // The rail names the network the boards share. Best-effort: an unreachable or
    // older device just leaves the label off.
    try { this.netName = (await this.api.getMotionStatus()).ssid ?? ''; } catch { /* no device */ }
    // The load is async, so it can settle either side of ngAfterViewInit. Both paths
    // ask to fit and maybeFit() runs whichever gets there second, once with a real
    // layout and a measurable wrap.
    setTimeout(() => { this.recomputeExtent(); this.vp.maybeFit(); });
  }
  /** The wrap's size isn't known until the view exists — size the board to it then
   *  (deferred a tick so we're not writing bindings mid-check). */
  ngAfterViewInit(): void {
    setTimeout(() => { this.recomputeExtent(); this.vp.maybeFit(); });
    this.vp.attach();
  }

  // ── viewport ──────────────────────────────────────────────────────────────────
  /** Framing and movement: zoom, pan, pinch, edge auto-scroll — see canvas-viewport.ts.
   *  Public because the template binds its scale and drives its buttons. */
  readonly vp = new CanvasViewport({
    wrapEl: () => this.wrapRef?.nativeElement,
    inkExtent: () => this.inkExtent(),
    hasDrawing: () => this.nodes.length > 0,
    recomputeExtent: () => this.recomputeExtent(),
    runInZone: (fn) => this.zone.run(fn),
  });
  /** Natural size of the drawing in board units, set by recomputeExtent(). */
  private contentW = 0; private contentH = 0;

  /** The bounding box of the drawing itself — glyphs and the board rail — with a
   *  little air, in board units. */
  inkExtent(): { w: number; h: number } {
    if (!this.nodes.length) return { w: this.contentW, h: this.contentH };
    let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
    for (const n of this.nodes) {
      // A UNIT's origin is its FIRST OUTLET, not its centre: the body runs right
      // from there and overhangs by GATE_PAD at each end. Treating its half-width
      // as symmetric invented ~300 units of empty canvas to the LEFT of a 4-outlet
      // sliding gate, and the fit shrank the shop to 36% to make room for nothing.
      const left  = n.isUnit ? GATE_PAD : this.halfW(n);
      const right = n.isUnit ? (n.span - 1) * CELL + GATE_PAD : this.halfW(n);
      const hh = this.halfH(n);
      x0 = Math.min(x0, this.nx(n) - left); x1 = Math.max(x1, this.nx(n) + right);
      // Tools carry a name above and the plug row inside, both already in halfH;
      // gates carry their name ABOVE the body, which halfH doesn't know about.
      y0 = Math.min(y0, this.ny(n) - hh - 16); y1 = Math.max(y1, this.ny(n) + hh);
    }
    const AIR = 24;
    return {
      w: Math.max(1, x1 - x0 + AIR * 2),
      // The rail sits above the drawing and has to be on screen too.
      h: Math.max(1, y1 - y0 + AIR * 2 + RAIL_H),
    };
  }

  private livePoll: ReturnType<typeof setInterval> | null = null;

  ngOnDestroy(): void {
    if (this.livePoll) clearInterval(this.livePoll);
    this.detachDrag();
    this.vp.destroy();
    window.removeEventListener('pointermove', this.bMove);
    window.removeEventListener('pointerup', this.bUp);
    window.removeEventListener('pointermove', this.oMove);
    window.removeEventListener('pointerup', this.oUp);
    window.removeEventListener('pointermove', this.wMove);
    window.removeEventListener('pointerup', this.wUp);
    window.removeEventListener('pointermove', this.boMove);
    window.removeEventListener('pointerup', this.boUp);
  }

  iconFor(kind: string): SafeHtml { return this.icons[kind]; }

  // ── geometry ────────────────────────────────────────────────────────────────
  nx(n: NodeVM): number { return (n.dragX ?? (PAD + n.col * CELL)) + (n.anchor?.dx ?? 0); }
  ny(n: NodeVM): number { return (n.dragY ?? (PAD + n.row * CELL)) + (n.anchor?.dy ?? 0); }
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

  /** The setup badge takes the TOP-LEFT corner, opposite the servo tab. A unit's group
   *  origin is its FIRST outlet, not its centre, so the left edge is a bare pad while
   *  the right one has to count the span. */
  todoX(n: NodeVM): number { return n.isUnit ? -GATE_PAD : -this.halfW(n); }
  todoY(n: NodeVM): number { return -this.halfH(n); }

  labelX(n: NodeVM): number { return n.isUnit ? (n.span - 1) * CELL / 2 : 0; }
  /** Below the glyph AND below its add-dot, which sits at halfH + 18 — the two used
   *  to land on top of each other on a ball valve. */
  /** Directly under the name, in the slot the name itself normally occupies. */
  redundantY(n: NodeVM): number { return this.nameBaseY(n); }
  labelY(n: NodeVM): number {
    // A redundant gate carries a second line, so its name moves up to make room and
    // the two read as one stacked caption instead of something adrift under the
    // glyph. namePos() follows this, so the edit field stays on the name.
    return this.nameBaseY(n) - (n.redundant ? 13 : 0);
  }
  private nameBaseY(n: NodeVM): number { return n.glyph === 'tool' ? -8 : (n.isUnit ? -UNIT_H / 2 - 9 : -34); }
  toolAuto(id: string): boolean { return !!outletOf(this.topo as unknown as ShopDoc, this.elem(id)); }

  // ── the plug row ─────────────────────────────────────────────────────────────
  // Live wattage per MACHINE, from /api/status. Keyed by machine because that is
  // what draws power — a two-port saw is one reading, not two.
  private machineWatts = new Map<string, number>();
  /** A machine waiting for the next tap in the plug tray. */
  armedTool: string | null = null;
  /** The tool whose plug row the current press started on, or null. Set on
   *  pointerdown, consumed by onUp — see {@link onDockDown}. */
  private dockTap: string | null = null;

  /** What this tool's plug row is showing. Recomputed per change-detection pass,
   *  like toolAuto() before it — the lookups are two map hits and a property walk,
   *  and caching it would need invalidating on every mutation AND every poll. */
  plugOf(n: NodeVM): { state: 'none' | 'idle' | 'standby' | 'live'; text: string; hint: string } {
    const el = this.elem(n.id);
    const doc = this.topo as unknown as ShopDoc;
    const outlet = outletOf(doc, el);
    if (!outlet) {
      return { state: 'none', text: 'no plug',
               hint: `No smart plug on ${n.name} — you switch it on yourself. Tap to pair one.` };
    }
    // The topology stores gen/ip/host/thresholdW — no friendly name, because the
    // name lives on the Shelly itself. So prefer what the last scan saw it call
    // itself ("Table Saw"), and fall back to the hostname only when we haven't
    // scanned. Otherwise the row reads "G4-295BD19…", which identifies nothing.
    const ip = outlet['ip'] as string | undefined;
    const seen = ip ? this.outlets.find(o => o.ip === ip) : undefined;
    const name = seen?.name || (outlet['host'] as string) || ip || 'plug';
    const machine = machineOfPort(doc, el);
    const watts = (machine && this.machineWatts.get(machine.id as string)) ?? 0;
    const trip = (outlet['thresholdW'] as number) ?? 0;
    const detail = `${name} · ${(outlet['ip'] as string) ?? ''} · ${Math.round(watts)} W`;
    if (watts >= trip && trip > 0) return { state: 'live', text: this.fmtW(watts), hint: detail };
    if (watts >= 1) return { state: 'standby', text: this.fmtW(watts), hint: detail };
    // Idle: show WHICH plug, since there's no number worth reading yet.
    return { state: 'idle', text: this.shortPlug(name), hint: detail };
  }

  private fmtW(w: number): string { return w >= 1000 ? (w / 1000).toFixed(1) + ' kW' : Math.round(w) + ' W'; }
  /** The row is 62 units wide; a Shelly's own hostname is longer than that. */
  private shortPlug(name: string): string {
    const s = name.replace(/^shelly(plug(us)?|plus)?-?/i, '');
    return s.length > 11 ? s.slice(0, 10) + '…' : (s || name);
  }

  // ── the plug tray ────────────────────────────────────────────────────────────
  /** Every plug the last scan found. Not filtered here — freeOutlets() decides
   *  what's still unclaimed, so a plug that gets unpaired reappears without a
   *  rescan. */
  outlets: DiscoveredOutlet[] = [];
  scanning = false;
  scanned = false;
  chipDrag: { outlet: DiscoveredOutlet; x: number; y: number; moved: boolean; over: string | null } | null = null;

  /** Hidden until there is something to say — an empty strip under a fresh canvas
   *  is furniture. Appears while scanning, and afterwards for as long as there is
   *  either a free plug to place or a machine still missing one. */
  get showTray(): boolean {
    if (this.scanning) return true;
    if (!this.scanned) return false;
    return this.freeOutlets().length > 0 || this.unpairedMachines().length > 0;
  }

  async scanOutlets(): Promise<void> {
    if (this.scanning) return;
    this.scanning = true;
    try { this.outlets = await this.api.discoverOutlets(); }
    catch { /* leave the last known list rather than blanking the tray */ }
    finally { this.scanning = false; this.scanned = true; }
  }

  /** IPs already spoken for, anywhere in the shop — a machine's sensor or the
   *  collector's switch. One physical plug driving two things would make the brain
   *  believe two machines started at once. */
  private claimedIps(): Set<string> {
    const out = new Set<string>();
    if (!this.topo) return out;
    const doc = this.topo as unknown as ShopDoc;
    for (const m of machinesOf(doc)) {
      const ip = ((m.sensor as RawEl | undefined)?.['outlet'] as RawEl | undefined)?.['ip'] as string | undefined;
      if (ip) out.add(ip);
    }
    for (const e of this.allElems()) {
      if ((e as RawEl)['type'] !== 'collector') continue;
      const ip = (((e as RawEl)['control'] as RawEl | undefined)?.['outlet'] as RawEl | undefined)?.['ip'] as string | undefined;
      if (ip) out.add(ip);
    }
    return out;
  }

  freeOutlets(): DiscoveredOutlet[] {
    const claimed = this.claimedIps();
    return this.outlets.filter(o => !claimed.has(o.ip));
  }

  chipLevel(o: DiscoveredOutlet): 'idle' | 'standby' | 'live' {
    if (!o.reachable || o.powerW < 1) return 'idle';
    return o.powerW >= 100 ? 'live' : 'standby';
  }
  chipWatts(o: DiscoveredOutlet): string { return this.fmtW(o.powerW); }

  /** Machines with no plug yet — what Match by name has to work with. */
  private unpairedMachines(): { id: string; name: string }[] {
    const doc = this.topo as unknown as ShopDoc;
    return machinesOf(doc)
      .filter(m => !((m.sensor as RawEl | undefined)?.['outlet']))
      .map(m => ({ id: m.id as string, name: (m.name as string) || (m.id as string) }));
  }

  canMatch(): boolean { return this.freeOutlets().length > 0 && this.unpairedMachines().length > 0; }

  trayNote(): string {
    if (this.armedTool) {
      const n = this.byId.get(this.armedTool);
      return `tap the plug powering the ${n?.name ?? 'tool'}`;
    }
    if (this.matchNote) return this.matchNote;
    const left = this.unpairedMachines().length;
    return `${this.freeOutlets().length} free · ${left} to place`;
  }
  private matchNote = '';

  /** Assigns outright — there is no per-row suggestion to accept. Strongest pairs
   *  first, globally: greedy in list order lets a weak match consume the machine a
   *  perfect one wanted. See outlet-match.ts. */
  matchByName(): void {
    const free = this.freeOutlets().map(o => ({ id: o.ip, name: o.name || o.hostname }));
    const pairs = matchAll(free, this.unpairedMachines());
    for (const p of pairs) {
      const o = this.outlets.find(x => x.ip === p.outletId);
      if (o) this.pairPlug(o, p.machineId);
    }
    const left = this.unpairedMachines().length;
    this.matchNote = pairs.length
      ? `paired ${pairs.length}${left ? ` · ${left} to drag` : ' · all done'}`
      : 'no clear name match — drag them across';
    if (pairs.length) { this.afterMutation(null); }
  }

  /** Write a plug onto a MACHINE. Threshold seeded from what it's drawing right
   *  now, ~10% under so it clears standby but still trips — the same guess the
   *  pairing sheet makes. */
  private pairPlug(o: DiscoveredOutlet, machineId: string): void {
    const doc = this.topo as unknown as ShopDoc;
    const m = machinesOf(doc).find(x => x.id === machineId);
    if (!m) return;
    const outlet: RawEl = { gen: o.generation || 2, ip: o.ip };
    if (o.hostname) outlet['host'] = o.hostname;
    outlet['thresholdW'] = o.powerW >= 5 ? Math.max(10, Math.round(o.powerW * 0.9 / 10) * 10) : 50;
    (m as unknown as RawEl)['sensor'] = { outlet };
  }

  // ── dragging a chip onto a tool ──────────────────────────────────────────────
  onChipDown(evt: PointerEvent, o: DiscoveredOutlet): void {
    evt.preventDefault();
    this.chipDrag = { outlet: o, x: evt.clientX, y: evt.clientY, moved: false, over: null };
    window.addEventListener('pointermove', this.chipMove);
    window.addEventListener('pointerup', this.chipUp);
  }
  private chipMove = (evt: PointerEvent): void => {
    if (!this.chipDrag) return;
    this.chipDrag.moved = true;
    this.chipDrag.x = evt.clientX; this.chipDrag.y = evt.clientY;
    // Hit-test against the real DOM rather than doing the inverse transform by
    // hand: the canvas pans, zooms and scrolls, and elementFromPoint already knows.
    const el = document.elementFromPoint(evt.clientX, evt.clientY);
    const dock = el?.closest?.('.dock') as SVGGElement | null;
    this.chipDrag.over = dock ? (dock.getAttribute('data-tool') ?? null) : null;
    this.zone.run(() => { /* repaint the ghost */ });
  };
  private chipUp = (): void => {
    window.removeEventListener('pointermove', this.chipMove);
    window.removeEventListener('pointerup', this.chipUp);
    const d = this.chipDrag; this.chipDrag = null;
    if (!d) return;
    const target = d.over ?? (d.moved ? null : this.armedTool);
    if (!target) return;
    const machine = machineOfPort(this.topo as unknown as ShopDoc, this.elem(target));
    if (!machine) return;
    this.pairPlug(d.outlet, machine.id as string);
    this.armedTool = null; this.matchNote = '';
    this.afterMutation(null);
  };

  /** The plug row's own tap target.
   *
   *  It used to swallow the event outright, which made the bottom half of every tool
   *  a dead zone for dragging — a 62×22 no-go strip inside a 76×68 box, right where a
   *  thumb lands. Grab a tool there and it simply didn't move, which reads as the
   *  piece having locked up rather than as "you missed". So the event bubbles on to
   *  startDrag now and the row only claims the gesture if it turns out to be a tap;
   *  {@link onUp} decides which it was. */
  onDockDown(_evt: PointerEvent, n: NodeVM): void {
    this.dockTap = n.id;
  }
  /** What a tap — not a drag — on the plug row does. */
  private dockAction(n: NodeVM): void {
    this.focus(n.id);
    const el = this.elem(n.id);
    if (outletOf(this.topo as unknown as ShopDoc, el)) { this.configureOutlet(n.id); return; }
    // No plug yet: arm the row and let the tray be the picker. Same gesture as the
    // drag, minus the drag, which is what a phone wants.
    this.armedTool = this.armedTool === n.id ? null : n.id;
  }
  /** Does this piece have a plug paired — sensed for a tool, switched for the
   *  collector? Drives the one button's label for both. */
  /** Plug paired? Sensed for a tool, switched for the collector — one question,
   *  which is why the badge can be one badge. */
  private hasPlugEl(el: RawEl): boolean {
    return !!outletOf(this.topo as unknown as ShopDoc, el);
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
  /** Delete a specific element — the run-end menu's Delete, which reaches a junction
   *  that was never selected, so it can't go through deleteSelected(). */
  private removeAt(id: string): void {
    if (!this.topo) return;
    this.focus(id);
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

  /** Names we hand out to a piece that has just been placed. Nobody wants to keep
   *  one, so they get out of the way as soon as you click in. */
  private static readonly PLACEHOLDER_NAMES = new Set(['New tool', 'New gate', 'Open end', 'Wye', 'Cap']);

  /** Clear a just-placed piece's default name on focus, so typing starts on an empty
   *  field instead of after "New tool".
   *
   *  Done to the DOM value only — the model keeps the default until something is
   *  actually typed, so clicking in and straight back out changes nothing and the
   *  undo history stays clean. */
  onNameFocus(evt: Event, n: NodeVM): void {
    if (!BuildComponent.PLACEHOLDER_NAMES.has(n.name)) return;
    (evt.target as HTMLInputElement).value = '';
  }

  /** Put the default back if they left without typing anything. */
  onNameBlur(evt: Event, n: NodeVM): void {
    const el = evt.target as HTMLInputElement;
    if (!el.value.trim()) el.value = n.name;
  }

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
    // + RAIL_H because the field is a DOM element measured from the top of the SVG,
    // and the SVG's viewBox starts at -RAIL_H (the board rail). Without it the field
    // lands a rail's height above the label it replaces — the name appeared to jump
    // upward the moment you clicked a piece.
    // × zoom: left/top are wrap pixels, and the SVG next to it is drawn scaled.
    return {
      left: (this.nx(n) + this.labelX(n)) * this.vp.zoom,
      top: (this.ny(n) + this.labelY(n) - 4 + RAIL_H) * this.vp.zoom,
    };
  }

  /** Enter commits by leaving the field; the value is already saved per keystroke. */
  blurName(e: Event): void { (e.target as HTMLElement | null)?.blur(); }

  /** The doc, for the config sheet's bindings only — `configuring` is never set unless
   *  a topology is loaded, so the non-null assertion holds. */
  get topoDoc(): Topology { return this.topo!; }

  /** Right-click a piece: the same menu its badge opens.
   *
   *  The badge is the only way in on a phone and stays the primary one, but on a
   *  desktop a right-click is where everyone's hand already goes — and on a passive
   *  manifold, which carries no badge at all, it saves hunting for the one gesture
   *  that works. Same menu, same options, so there is nothing extra to learn. */
  onNodeContext(evt: MouseEvent, n: NodeVM): void {
    evt.preventDefault(); evt.stopPropagation();
    this.focus(n.id);
    this.selectedId = n.id;
    this.openMenu(evt.clientX, evt.clientY, { convert: n.id });
  }

  /** Open the gate config straight from its dot, without selecting-then-tapping.
   *  Must swallow the event: the node group under it starts a drag on pointerdown,
   *  so without this a tap on the dot moves the gate instead of configuring it. */
  onSetupDot(evt: PointerEvent, n: NodeVM): void {
    evt.preventDefault(); evt.stopPropagation();
    this.focus(n.id);
    this.selectedId = n.id;
    // The badge is now the handle for the whole piece, not just its setup: it opens
    // the same menu the body used to, which already carried setup alongside the
    // conversions. That leaves the body free to mean "rename me".
    this.openMenu(evt.clientX, evt.clientY, { convert: n.id });
  }

  configure(id: string, pane: 'board' | 'travel' = 'board'): void {
    const el = this.elems(this.topo!).find(e => e['id'] === id) as AnyElement | undefined;
    if (!isServoSelector(el) && !isLinearSelector(el)) return;
    this.configPane = pane;
    this.configuring = el as unknown as ConfigurableSelector;
  }

  /** Open the smart-plug sheet for one tool. Edits a COPY, so cancelling leaves the
   *  doc untouched — same contract as the gate sheet. */
  configureOutlet(id: string): void {
    const el = this.elems(this.topo!).find(e => e['id'] === id) as RawEl | undefined;
    if (!el || (el['type'] !== 'tool' && el['type'] !== 'collector')) return;
    // A tool's plug is a sensor (we watch its draw); the collector's is a switch
    // (we command it). Same picker, different field — see the sheet's header.
    this.outletMode = el['type'] === 'collector' ? 'switch' : 'sensor';
    // The sheet edits "the thing that owns the plug", and under a shop that is the
    // MACHINE for a tool — one plug behind however many ports (RFC §6.3) — and
    // still the element for a collector, which belongs to one system and has no
    // machine to lift it onto. Handing it the machine directly means the sheet
    // itself needed no changes: a machine already has the `name` and `sensor` it
    // reads.
    const target = el['type'] === 'collector'
      ? el
      : (machineOfPort(this.topo as unknown as ShopDoc, el) as unknown as RawEl | null);
    if (!target) return;
    this.outletTool = JSON.parse(JSON.stringify(target)) as RawEl;
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
    for (const e of this.allElems()) {
      const el = e as RawEl;
      if (el['type'] === 'collector') {
        // …unless the collector IS what's being configured: its own plug has to
        // stay pickable, or re-opening the sheet greys out the current choice.
        if (el['id'] === me) continue;
        const dc = ((el['control'] as RawEl | undefined)?.['outlet'] as RawEl | undefined)?.['ip'] as string | undefined;
        if (dc) { ips.push(dc); reason[dc] = 'reserved — dust collector'; }
        continue;
      }
    }
    // Tool plugs live on MACHINES now, and the exclusion is per machine too: a
    // machine's ports share one plug by design, so two ports of the same saw are
    // not a clash — two different machines are.
    const doc = this.topo as unknown as ShopDoc;
    const myMachine = machineOfPort(doc, this.elem(me))?.id ?? me;
    for (const m of machinesOf(doc)) {
      if (m.id === myMachine) continue;
      const ip = (m.sensor?.outlet as RawEl | undefined)?.['ip'] as string | undefined;
      if (ip) { ips.push(ip); reason[ip] = `already paired with ${m.name || 'another tool'}`; }
    }
    return { ips, reason };
  }

  /** Splice the paired tool back in. Shares onConfigured's path deliberately: a
   *  sensor change is a topology edit like any other and gets the same validation,
   *  history entry and save. */
  onOutletConfigured(updated: RawEl): void {
    if (!this.topo) return;
    const doc = this.topo as unknown as ShopDoc;
    const id = updated['id'] as string;
    // Splices back wherever it came from — machines[] for a tool's sensor,
    // elements[] for the collector's switch. See configureOutlet.
    const mi = doc.machines.findIndex(m => m.id === id);
    if (mi >= 0) {
      this.pushHistory(id);
      doc.machines[mi] = updated as unknown as (typeof doc.machines)[number];
    } else {
      const els = this.ownerElems(id);
      const i = els.findIndex(e => e['id'] === id);
      if (i < 0) return;
      this.pushHistory(id);
      els[i] = updated as unknown as (typeof els)[number];
    }
    this.outletTool = null;
    this.dirty = true;
    this.buildGraph(this.topo); this.syncNodes(); this.refreshHandles();
    void this.save();
  }

  /** Fold the configured gate back into the doc and persist through the normal save,
   *  so it goes through the same validation and work-in-progress handling. */
  onConfigured(updated: ConfigurableSelector): void {
    if (!this.topo) return;
    const els = this.ownerElems(updated.id);
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
    // A pickup has no cell to drag it to — it is part of a machine, and the machine
    // is the thing that moves. A press on one opens its menu instead.
    if (n.glyph === 'pickup') {
      this.focus(n.id); this.selectedId = n.id;
      this.openMenu(evt.clientX, evt.clientY, { convert: n.id });
      return;
    }
    this.focus(n.id);
    this.selectedId = n.id; this.menu = null;
    this.dragId = n.id;
    const pt = this.toSvg(evt);
    this.grab = { dx: pt.x - this.nx(n), dy: pt.y - this.ny(n) };
    window.addEventListener('pointermove', this.moveH);
    window.addEventListener('pointerup', this.upH);
    this.vp.beginEdgeScroll(this.moveH, evt);
  }
  private onMove(evt: PointerEvent): void {
    this.vp.trackEdge(evt);
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
    // Claimed on pointerdown by the plug row, settled here: a press that turned into
    // a drag was a drag, and only a press that went nowhere was a tap on the plug.
    const dockTap = this.dockTap; this.dockTap = null;
    const dragged = n?.dragX != null && n?.dragY != null;
    let moved = false;
    if (n && dragged) {
      const col = Math.max(0, Math.round((n.dragX! - PAD) / CELL)), row = Math.max(0, Math.round((n.dragY! - PAD) / CELL));
      moved = col !== n.col || row !== n.row;
      if (this.canPlace(n, col, row) && moved) {
        this.pushHistory(null);
        n.col = col; n.row = row; this.cells.set(n.id, { col, row });
        this.dirty = true;
      }
      n.dragX = undefined; n.dragY = undefined;
      this.recomputeExtent();
    }
    if (n && dockTap === n.id && !moved) {
      this.dockAction(n);
    } else if (n && !dragged && n.glyph === 'junction') {
      // A tap (no drag) on a run end — open or capped — opens its menu: what goes
      // here, plus cap/reopen/delete. Ends carry no inspector of their own.
      this.openMenu(evt.clientX, evt.clientY, { end: n.id });
    } else if (n && !dragged && !n.setup) {
      // A passive manifold has no setup state, so it gets no badge — and the badge is
      // what opens a piece's menu. That left it with nothing to tap at all, which only
      // stopped mattering because the red (−) was there to delete it. It isn't now, so
      // a tap on a badge-less piece opens the menu itself. Selection already happened
      // on pointerdown, so dismissing the menu still leaves the name editable.
      this.openMenu(evt.clientX, evt.clientY, { convert: n.id });
    }
    // A tap on the body of a piece now does one thing: select it, which puts the
    // editable name on its label. What the piece IS — its kind, its setup, its plug —
    // moved to the badge, because tapping the name to rename it and tapping it to
    // open a menu were the same gesture, and the menu won.
    this.dragId = null; this.hoverCell = null; this.dropBlocked = '';
    this.detachDrag();
  }
  private detachDrag(): void {
    this.vp.endEdgeScroll();
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
    // A system owns a contiguous stripe of rows, and that's what makes the grey
    // ground drawable at all: the two never interleave, so the boundary is one row
    // rather than a shape. Refusing the drop keeps that true without the drag having
    // to reflow the other system out of the way mid-gesture.
    const band = this.bandBlockedBy(n.id, row);
    if (band) return `That row belongs to ${band}. A piece stays in its own system.`;
    const occ = new Map<string, NodeVM>();
    for (const m of this.nodes) {
      if (m.id === n.id || m.glyph === 'pickup') continue;   // rides its machine's cell
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

  /** Why `id` can't sit on `row`, in terms of the system next door — null when it can.
   *
   *  Systems keep their rows in order, and a piece stays between its neighbours: the
   *  row below the system above it, the row above the system below it. Checking
   *  "is this row inside someone else's band" is not enough, because a drag can jump
   *  clean OVER a band and interleave the two just as thoroughly from the far side.
   *
   *  Growing downward into the empty row between two systems is allowed — that just
   *  moves the boundary, which is the layout doing its job. */
  private bandBlockedBy(id: string, row: number): string | null {
    const bands = this.systemRowBands();
    if (bands.length < 2) return null;
    const i = bands.findIndex(b => b.id === this.systemOf.get(id));
    if (i < 0) return null;
    const above = bands[i - 1], below = bands[i + 1];
    if (above && row <= above.hi) return above.name ?? 'another collector';
    if (below && row >= below.lo) return below.name ?? 'another collector';
    return null;
  }

  /** Each system's row band, topmost first. A system with nothing placed yet has no
   *  band and drops out — there is no stripe to defend until something is on it.
   *
   *  The one place rows-per-system is worked out: the drag check above, the placement
   *  defaults below, and the grey ground that draws these bands all read it. Two of
   *  them disagreeing is how you get a ground drawn where a piece may not stand, or a
   *  default that puts a piece where a later drag refuses to move it.
   *
   *  `name` stays raw — the collector's own, or undefined when it hasn't been named
   *  yet. Callers supply their own fallback because they are writing different
   *  sentences: a label on the ground, versus who is in your way. */
  private systemRowBands(): Array<{ id: string; lo: number; hi: number; name?: string }> {
    if (!this.topo) return [];
    return systemsOf(this.topo as unknown as ShopDoc).map(s => {
      let lo = Infinity, hi = -Infinity;
      for (const e of s.elements) {
        const c = this.cells.get(e['id'] as string); if (!c) continue;
        lo = Math.min(lo, c.row); hi = Math.max(hi, c.row);
      }
      const dc = s.elements.find(e => e['type'] === 'collector');
      return { id: s.id, lo, hi, name: dc?.['name'] as string | undefined };
    }).filter(b => isFinite(b.lo)).sort((a, b) => a.lo - b.lo);
  }

  /** The first row a piece in `id`'s system must not reach: the top of the next system
   *  down, or Infinity when nothing is below it.
   *
   *  Deliberately the neighbour's `lo` and not this system's `hi` — growing downward
   *  into the empty gap between two systems just moves the boundary, which is the
   *  layout doing its job. Landing IN the neighbour's rows is what interleaves the two
   *  and makes the grey ground undrawable. Same predicate `bandBlockedBy()` refuses on
   *  a drag. */
  private bandFloor(id: string): number {
    const bands = this.systemRowBands();
    const i = bands.findIndex(b => b.id === this.systemOf.get(id));
    return i >= 0 && bands[i + 1] ? bands[i + 1].lo : Infinity;
  }

  /** The mirror of bandFloor(): the last row above `id`'s system that belongs to the
   *  neighbour, or -Infinity with nothing above. A default that reaches up — a hood's
   *  open end — has to clear it. */
  private bandCeiling(id: string): number {
    const bands = this.systemRowBands();
    const i = bands.findIndex(b => b.id === this.systemOf.get(id));
    return i > 0 ? bands[i - 1].hi : -Infinity;
  }

  /** Where a piece displaced from `cell` goes: the nearest free cell still inside its
   *  own system's band. Straight down first — which is where it went unconditionally
   *  before, and still does whenever the band has room — then a column over, because
   *  columns are free to grow rightward and rows are not.
   *
   *  Without the floor the downward walk was bounded only by the shop: a column filled
   *  contiguously below would march the piece past the gap and into the next system,
   *  interleaving the bands `bandBlockedBy()` exists to keep apart. */
  private freeCellBelow(id: string, cell: Cell): Cell {
    const taken = new Set([...this.cells].filter(([k]) => k !== id).map(([, c]) => c.col + ',' + c.row));
    const floor = this.bandFloor(id);
    for (let dc = 0; dc < 256; dc++) {
      const col = cell.col + dc;
      for (let row = cell.row + 1; row < floor; row++) {
        if (!taken.has(col + ',' + row)) return { col, row };
      }
      // Sitting on the last row before the neighbour: sideways is the only way out,
      // so the piece keeps its row and steps over instead.
      if (dc > 0 && !taken.has(col + ',' + cell.row)) return { col, row: cell.row };
    }
    return cell;
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
    // Whatever the menu is about decides which system the choice will be written
    // into. Every context names a piece, so this is the one place all four routes
    // through — a menu opened over system 2 must not add to system 1.
    this.focus(ctx.convert ?? ctx.end ?? ctx.branch?.childId ?? ctx.addOutput?.parentId);
    // A gate keeps its selection so the name/outlet controls are still there once
    // the menu is dismissed; every other context shows the menu alone.
    if (!ctx.convert) this.selectedId = null;
    this.menu = { x, y, ...ctx };
    this.menuTitle = ctx.convert ? this.convertTitle(ctx.convert)
                   // A tee reaches this menu by being a junction, but it is a fork,
                   // not an end — and saying "the end of this run" over a list that
                   // refuses to cap or delete it explains nothing.
                   : ctx.end ? (this.childrenOf(ctx.end).length > 1 ? 'Where this run splits'
                              : this.isCap(ctx.end) ? 'This capped end' : 'At the end of this run')
                   : ctx.branch ? (ctx.branch.elbow ? 'Add at this corner' : 'Add on this run')
                   : 'Add here';
    this.menuOptions = this.resolveOptions();
    this.clampMenu();
  }

  /** Pull the menu back inside the window once it has a size.
   *
   *  It opens at the point you tapped, which near the bottom or right edge puts most
   *  of it off screen — and on a phone that's most of the canvas. Measured after
   *  render rather than guessed from the option count, because the options differ
   *  per context and one of them wraps. */
  private clampMenu(): void {
    setTimeout(() => {
      const el = this.menuRef?.nativeElement, m = this.menu;
      if (!el || !m) return;
      const r = el.getBoundingClientRect();
      const gap = 8;
      const maxX = window.innerWidth - r.width - gap;
      const maxY = window.innerHeight - r.height - gap;
      const x = Math.max(gap, Math.min(m.x, maxX));
      const y = Math.max(gap, Math.min(m.y, maxY));
      if (x !== m.x || y !== m.y) { m.x = x; m.y = y; }
    });
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
      // A tee directly below is going to BE this gate (see absorbTee), so the cell
      // it stands on is room, not an obstacle — otherwise a 2-cell manifold could
      // never be dropped onto the branch it is replacing.
      const below = this.elem(m.branch.childId);
      const free = below?.['type'] === 'junction' && this.childrenOf(m.branch.childId).length > 1
        && outletsFor(kind) >= this.childrenOf(m.branch.childId).length
        ? this.cellOccupied(m.branch.childId) : occ;
      return { cells: [{ col: m.branch.col, row: m.branch.row }], span: spanFor(kind), selfId: m.branch.childId, occ: free };
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
    // A junction with legs is a TEE, not an end, however you arrived at its menu:
    // from a dot on the run above it, or by tapping the fork itself. Both have to
    // answer the same question — can this fitting carry every leg out of here.
    const legsHere = m.branch ? this.legsAtCell(m.branch.col, m.branch.row)
                   : m.end   ? this.childrenOf(m.end).length
                   : 0;
    const tee = legsHere > 1;
    const opts: MenuOption[] = FITTINGS.map(f => {
      let enabled = true, note: string | undefined;
      if (f.kind === 'duct' && m.end)    { enabled = false; note = 'drag the end'; }
      else if (f.kind === 'tool' && mid) { enabled = false; note = 'ends a run'; }
      // A tool terminates a run, so it can't stand on a fork either.
      else if (f.kind === 'tool' && tee) { enabled = false; note = 'ends a run'; }
      // A fitting that stands where the run SPLITS has to carry every leg out of it.
      // A ball valve has one outlet, so putting one on a tee leaves a leg with
      // nowhere to come from — only a manifold or a sliding gate can stand there.
      // A duct is exempt: it hangs another leg off the tee rather than replacing it.
      else if (tee && f.kind !== 'duct' && outletsFor(f.kind) < legsHere) {
        enabled = false; note = `a tee needs ${legsHere} outlets`;
      }
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
      // None of it applies to a TEE, which reaches this menu by being a junction but
      // is a fork, not an end: you can't cap a fork, and binning it would leave its
      // legs fed by nothing. Thin it back to one leg first.
      opts.push(this.isCap(m.end)
        ? { kind: 'uncap', label: 'Reopen this end', enabled: !tee }
        : { kind: 'cap', label: 'Cap this end', enabled: !tee, note: tee ? 'not an end' : undefined });
      opts.push({ kind: 'delete', label: 'Delete', enabled: !tee,
                  note: tee ? 'remove its legs first' : undefined });
    } else {
      opts.push({ kind: 'cap', label: 'Cap this outlet', enabled: true });
    }
    return opts;
  }

  /** How many ways the run divides at this cell. A junction standing here with more
   *  than one child is a TEE, and whatever replaces it has to feed all of them. */
  private legsAtCell(col: number, row: number): number {
    for (const [id, c] of this.cells) {
      if (c.col !== col || c.row !== row) continue;
      if (this.elem(id)?.['type'] !== 'junction') continue;
      return this.childrenOf(id).length;
    }
    return 0;
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
   *  isn't already said by where they sit. Everything but the collector also ends
   *  with Delete — this menu became the ONLY way to bin a placed piece when the red
   *  (−) badge came off on 2026-08-15, and a phone has no Delete key. */
  private convertOptions(id: string): MenuOption[] {
    const n = this.byId.get(id); if (!n) return [];
    // A pickup is a port, not a piece: it has no plug, no kind to convert to and no
    // cell to move it to. All it can do is go.
    if (n.glyph === 'pickup') return [this.deleteOption(n)];
    if (n.glyph === 'tool' || n.glyph === 'collector') {
      const paired = n.setup === 'done';
      const opts: MenuOption[] = [{
        kind: 'outlet',
        label: paired ? 'Smart outlet' : 'Set up smart outlet',
        enabled: true,
        note: paired ? undefined : (n.glyph === 'collector' ? 'started by hand' : 'switched by hand'),
      }];
      // A machine can grow a second place to collect from — an overarm guard, a hood.
      // Offered on the tool itself because that is where the question comes up, and
      // capped where the model caps it: a list long enough to scroll is a modelling
      // mistake, not a shop.
      if (n.glyph === 'tool') {
        const doc = this.topo as unknown as ShopDoc;
        const machine = machineOfPort(doc, this.elem(n.id));
        const used = machine ? supplementalCount(doc, machine.id as string) : 0;
        opts.push({
          kind: 'pickup', label: 'Add second pickup',
          enabled: !!machine && used < MAX_PICKUPS,
          note: used >= MAX_PICKUPS ? `${MAX_PICKUPS} is the limit` : 'overarm guard, hood',
        });
      }
      // The collector is the one piece with nothing above it, so there's no run to
      // heal — it stays, and offering a dead Delete would only invite the tap.
      if (n.glyph !== 'collector') opts.push(this.deleteOption(n));
      return opts;
    }
    // Tapping a gate is how you get at it, so its setup lives here alongside the
    // conversions — the floating inspector is only reachable right after placing one.
    // Two entries, not one "Gate setup": picking the board and measuring the travel
    // are different jobs on different days — the board is chosen once when the gate
    // is wired, the limits are re-measured whenever the valve is disturbed. Only the
    // second needs the board awake, and only the second is what "not done yet" is
    // about, so the badge sits on that row rather than over both.
    const opts: MenuOption[] = n.setup
      ? [{ kind: 'board', label: 'Select a board', enabled: true },
          { kind: 'travel', label: 'Adjust travel limits', enabled: true,
            note: n.setup === 'todo' ? 'not done yet' : undefined }]
      : [];
    return opts.concat(this.gateTypes(n).map(t => ({
      kind: t.kind, label: t.current ? `${t.label} (current)` : t.label,
      enabled: t.enabled, note: t.note,
    })), [this.deleteOption(n)]);
  }

  /** Delete, with the reason it's greyed out when a fork can't go yet. Last row of
   *  the tap menu, so it's never the neighbour of the row you meant to press. */
  private deleteOption(n: NodeVM): MenuOption {
    const ok = this.canDelete(n);
    return { kind: 'delete', label: 'Delete', enabled: ok,
             note: ok ? undefined : 'remove its other legs first' };
  }

  /** Route a chosen option to the right primitive for the context it was opened in. */
  choose(kind: MenuKind): void {
    const m = this.menu; if (!m || !this.topo) return;
    if (!this.menuOptions.find(o => o.kind === kind)?.enabled) return;
    this.pushHistory(null);
    if (m.convert) {
      const id = m.convert; this.closeMenu();
      if (kind === 'board' || kind === 'travel') this.configure(id, kind);
      else if (kind === 'outlet') this.configureOutlet(id);
      else if (kind === 'pickup') this.addPickup(id);
      else if (kind === 'delete') { this.selectedId = id; this.deleteSelected(); }
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
      else if (!this.absorbTee(bd, kind as SelKind))
        this.insertInline(bd.childId, kind as SelKind, { col: bd.col, row: bd.row });
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
    // A TEE is not an end. Tapping one opens this same menu (it's a junction), but
    // "put a gate here" means the gate REPLACES the fork and carries its legs — the
    // old path hung the gate off the tee as one more leg, in the tee's own cell, so
    // a ball valve ended up drawn on top of a three-way wye with the run carrying on
    // past it. What can't carry the legs is refused by resolveOptions before we get
    // here; this is the conversion itself.
    if (kind !== 'tool' && this.childrenOf(endId).length > 1) {
      this.absorbJunction(endId, kind, at);
      return;
    }
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
    // Another system's rows are not room, even when the cell is empty. Every default
    // placement comes through here, so this is where "new pieces land in the system
    // you're working in" is one rule rather than a habit each call site has to keep.
    // An id with no system yet answers null and stays permissive, as before.
    if (this.bandBlockedBy(selfId, row)) return false;
    for (let i = 0; i < Math.max(1, span); i++) {
      if (occ.has((col + i) + ',' + row)) return false;
      if (this.cellOnDuct(col + i, row, selfId)) return false;
    }
    return true;
  }

  /** Where a tee's new leg goes. On a straight: PERPENDICULAR to the run it taps
   *  off — a horizontal run drops down (or up), a vertical one goes right (or left).
   *  On a corner: whichever of the two directions the run doesn't already use, in
   *  the order branchDots worked out (down first, then the open side, then up).
   *  Never onto another duct. Null → no room, and the option greys out. */
  private legCellFor(bd: BDot, selfId: string): Cell | null {
    const occ = this.cellOccupied();
    const tries: Cell[] = bd.legs ?? (bd.axis === 'h'
      ? [{ col: bd.col, row: bd.row + 1 }, { col: bd.col, row: bd.row - 1 }]
      : [{ col: bd.col + 1, row: bd.row }, { col: bd.col - 1, row: bd.row }]);
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

  /**
   * Put a gate on the run that FEEDS a tee: the tee becomes the gate. Its legs move
   * onto the gate's outlets and the wye itself goes away — which is what "make this
   * branch a manifold" means. The plain inline insert would have left a two-way wye
   * hanging under a two-way manifold, one outlet doing all the work.
   *
   * The ordering is the other half. Legs live in the doc in the order they were
   * drawn, which has nothing to do with where they ended up on the canvas, so binding
   * them to outlets by index sent the left-hand duct out of the right-hand port and
   * drew the manifold crossed over itself. They bind left-to-right instead, so the
   * picture matches the plumbing you'd actually build.
   *
   * Returns false when the piece below isn't a tee, leaving the plain inline insert.
   */
  private absorbTee(bd: BDot, kind: SelKind): boolean {
    const j = this.elem(bd.childId);
    if (!j || j['type'] !== 'junction') return false;
    return !!this.absorbJunction(bd.childId, kind, { col: bd.col, row: bd.row });
  }

  /**
   * Replace a tee with a gate: the junction goes away, its legs move onto the gate's
   * outlets left-to-right, and the gate takes over the tee's feed. Returns the new
   * gate's id, or null when this junction can't be replaced by this kind.
   */
  private absorbJunction(jid: string, kind: SelKind, cell: Cell): string | null {
    if (!this.topo) return null;
    const j = this.elem(jid); if (!j || j['type'] !== 'junction') return null;
    const inDuct = this.ductsRaw().find(d => d['child'] === jid); if (!inDuct) return null;
    const legs = this.legsLeftToRight(jid);
    if (legs.length < 2) return null;                   // not a fork
    const sel = this.makeSelector(kind);
    const have = (sel['branches'] as Branch[]).length;
    if (kind === 'linear' && have < legs.length) this.appendLinearOutlets(sel, legs.length - have);
    const branches = sel['branches'] as Branch[];
    if (branches.length < legs.length) return null;     // menu should have refused it
    this.elems(this.topo).push(sel);
    // The gate inherits the tee's feed, including a parent selector's outlet.
    const up: RawEl = { child: sel['id'], parent: inDuct['parent'] };
    if (inDuct['parentBranch']) {
      up['parentBranch'] = inDuct['parentBranch'];
      const pb = (this.elem(inDuct['parent'] as string)?.['branches'] as Branch[] | undefined)
        ?.find(x => x.id === inDuct['parentBranch']);
      if (pb) pb.role = 'feed';
    }
    this.ductsRaw().push(up);
    legs.forEach((d, i) => {
      d['parent'] = sel['id']; d['parentBranch'] = branches[i].id;
      branches[i].role = this.elem(d['child'] as string)?.['type'] === 'tool' ? 'tool' : 'feed';
    });
    this.setElems(this.elems(this.topo).filter(e => e !== j));
    this.setDucts(this.ductsRaw().filter(d => d !== inDuct));
    this.cells.delete(jid);
    this.placeAt(sel['id'] as string, cell);
    this.afterMutation(sel['id'] as string);
    return sel['id'] as string;
  }

  /** The legs under a fitting, in the order they are DRAWN — leftmost first. Doc
   *  order is insertion order, so anything binding legs to outlets by index has to
   *  come through here or the run crosses over itself. */
  private legsLeftToRight(id: string): RawEl[] {
    return [...this.childDucts(id)].sort((a, b) => {
      const ca = this.cells.get(a['child'] as string) ?? { col: 0, row: 0 };
      const cb = this.cells.get(b['child'] as string) ?? { col: 0, row: 0 };
      return ca.col - cb.col || ca.row - cb.row;
    });
  }

  /** Splice a gate INTO the run at the clicked point: the downstream reconnects to
   *  the new gate's FIRST outlet (so a manifold becomes a real 2-way — one leg used,
   *  one free; a ball valve is a plain inline on/off). Remaining outlets stay
   *  capped-but-available, never dead. */
  private insertInline(childId: string, kind: SelKind, cell?: Cell): string | null {
    if (!this.topo) return null;
    const duct = this.ductsRaw().find(d => d['child'] === childId); if (!duct) return null;
    const parentId = duct['parent'] as string;
    const sel = this.makeSelector(kind);
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
   *  vertical and horizontal segments, plus one at each unambiguous CORNER. Clicking
   *  one branches the run right there. */
  branchDots(): BDot[] {
    const out: BDot[] = [];
    if (this.dragId || this.bdrag || this.odrag) return out;   // hide while dragging
    const seen = new Set<string>();
    const push = (x: number, y: number, childId: string, axis: 'h' | 'v', elbow?: boolean, legs?: Cell[]) => {
      const col = Math.round((x - PAD) / CELL), row = Math.round((y - PAD) / CELL);
      const key = col + ',' + row;
      if (seen.has(key)) return;                      // one dot per cell even where ducts overlap
      seen.add(key); out.push({ x, y, childId, col, row, axis, elbow, legs });
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
    // Corners. A gate at an elbow is a perfectly ordinary thing to want — it's often
    // the only place on a short run that ISN'T a straight. The catch is that a corner
    // doesn't sit on the grid: it lands wherever the router put it, up to half a cell
    // off. So the dot is drawn ON the corner (that's the thing you're pointing at) but
    // the fitting lands in the cell that corner rounds into, and the run re-solves
    // through it. Only offered where exactly ONE corner rounds into a given cell:
    // two corners half a cell apart round to the same square, and then there's no
    // saying which one a click meant.
    type Corner = { x: number; y: number; childId: string; axis: 'h' | 'v'; legs: Cell[] };
    const perCell = new Map<string, Corner | null>();
    for (const d of this.ducts) {
      const pts = this.ductPoints(d.childId);
      for (let i = 1; i < pts.length - 1; i++) {      // interior vertices only — the ends are ports
        const p = pts[i], prev = pts[i - 1], next = pts[i + 1];
        const col = Math.round((p.x - PAD) / CELL), row = Math.round((p.y - PAD) / CELL);
        const key = col + ',' + row;
        // Second corner in this cell → ambiguous, and neither gets a dot.
        if (perCell.has(key)) { perCell.set(key, null); continue; }
        // The run already occupies two of the four directions here — the one it
        // arrives from and the one it leaves by. A leg takes one of the OTHER two.
        // Because a corner turns, those two are always one vertical and one
        // horizontal, so the order below ("down, then the open side, then up") is a
        // real preference and not a coin toss.
        const step = (a: Pt, b: Pt) => ({ dx: Math.sign(Math.round(a.x - b.x)), dy: Math.sign(Math.round(a.y - b.y)) });
        const used = [step(prev, p), step(next, p)];
        const legs = [{ dx: 0, dy: 1 }, { dx: 1, dy: 0 }, { dx: -1, dy: 0 }, { dx: 0, dy: -1 }]
          .filter(v => !used.some(u => u.dx === v.dx && u.dy === v.dy))
          .map(v => ({ col: col + v.dx, row: row + v.dy }));
        perCell.set(key, { x: p.x, y: p.y, childId: d.childId, legs,
                           axis: Math.abs(prev.x - p.x) < 0.5 ? 'v' : 'h' });
      }
    }
    const occ = this.cellOccupied();
    for (const [key, e] of perCell) {
      // A corner that rounds onto a piece has nowhere to put the fitting; the dot
      // would just sit on top of the glyph.
      if (!e || occ.has(key)) continue;
      push(e.x, e.y, e.childId, e.axis, true, e.legs);
    }
    return out;
  }
  private addJunctionChild(junctionId: string, kind: SelKind | 'tool' | 'duct'): string | null {
    if (!this.topo) return null;
    if (kind === 'tool') {
      const tool = this.newPort('New tool');
      if (!tool) return null;
      this.ductsRaw().push({ child: tool['id'], parent: junctionId });
      return tool['id'] as string;
    }
    if (kind === 'duct') {
      const j: RawEl = { id: this.newId('j'), type: 'junction', name: 'Open end' };
      this.elems(this.topo).push(j);
      this.ductsRaw().push({ child: j['id'], parent: junctionId });
      return j['id'] as string;
    }
    const sel = this.makeSelector(kind);
    this.elems(this.topo).push(sel);
    this.ductsRaw().push({ child: sel['id'], parent: junctionId });
    return sel['id'] as string;
  }
  private cellOccupied(except?: string): Set<string> {
    const occ = new Set<string>();
    for (const [id, c] of this.cells) {
      if (id === except) continue;
      const el = this.elem(id);
      const span = el && isUnitKind(el['kind']) ? Math.max(1, (el['branches'] as unknown[] | undefined)?.length ?? 1) : 1;
      for (let i = 0; i < span; i++) occ.add((c.col + i) + ',' + c.row);
    }
    return occ;
  }
  // ── wiring layer ──────────────────────────────────────────────────────────────
  // Everything below draws or edits the SAME two fields a gate has always had —
  // controllerId and servo.channel — and adds one genuinely new thing: where each
  // board physically sits.

  private controllersRaw(): RawEl[] { return ((this.topo as { controllers?: RawEl[] } | null)?.controllers) ?? []; }

  /** Draw every board that is actually PAIRED, not just the ones the layout has
   *  heard of.
   *
   *  Pairing lives on the device (NVS) and is deliberately independent of the
   *  topology — a board can be paired before any shop is drawn. The Boards screen
   *  is what writes a paired board into `controllers[]`, so until someone opens
   *  it the canvas has no idea the board exists: it pairs, it shows up in the
   *  scan, and the rail stays empty. That reads as "the board didn't work".
   *
   *  In memory only. Persisting from here would make merely LOOKING at the canvas
   *  a write, and the Boards screen already saves this the moment it is opened. */
  private async mergePairedBoards(): Promise<void> {
    if (!this.topo) return;
    let links: NodeLinkState[] = [];
    try { links = await this.api.getNodes(); } catch { return; }   // older/offline device
    const controllers = this.controllersRaw();
    let added = false;
    for (const l of links) {
      if (!l.id || controllers.some(c => c['id'] === l.id)) continue;
      controllers.push({
        id: l.id, role: 'secondary', name: l.name || l.host || l.id, board: l.board,
        link: { transport: 'wifi-ws', host: l.host },
      } as unknown as RawEl);
      added = true;
    }
    if (added) this.ensureSlots();
  }



  /** Boards that have been placed on the canvas, with their port rosters. */
  /** Every paired board gets a slot, in controller order, keeping whatever order was
   *  saved. Stale entries (a board that has since been unpaired) drop out. */
  private ensureSlots(): void {
    const ids = this.controllersRaw().map(c => c['id'] as string);
    for (const id of [...this.boardSlots.keys()]) if (!ids.includes(id)) this.boardSlots.delete(id);
    const used = new Set(this.boardSlots.values());
    let next = 0;
    for (const id of ids) {
      if (this.boardSlots.has(id)) continue;
      while (used.has(next)) next++;
      this.boardSlots.set(id, next); used.add(next);
    }
  }
  boards(): BoardVM[] {
    if (!this.topo) return [];
    this.ensureSlots();
    const out: BoardVM[] = [];
    const fallback = this.defaultControllerId();
    for (const c of this.controllersRaw()) {
      const id = c['id'] as string;
      const slot = this.boardSlots.get(id); if (slot === undefined) continue;
      const used = new Map<number, string>();
      let servoCount = 0;
      // A board is shop-level and may drive selectors in any system, so its port
      // budget is counted across all of them — scoping this to the drawn-on system
      // would hand the same channel out twice.
      for (const e of this.allElems()) {
        if (e['type'] !== 'selector') continue;
        if (((e['controllerId'] as string) ?? fallback) !== id) continue;
        if (e['kind'] === 'linear') { used.set(SERVO_PORTS, e['id'] as string); continue; }
        const ch = ((e['servo'] as RawEl | undefined)?.['channel'] as number) ?? 0;
        used.set(ch, e['id'] as string); servoCount++;
      }
      const p = railSlot(slot);
      const drag = this.bodrag?.id === id ? this.bodrag : null;
      out.push({
        id, name: (c['name'] as string) || id, primary: c['role'] === 'primary',
        col: slot, row: 0, x: p.x, y: p.y, used, servoCount,
        dragX: drag ? this.boardDragPt?.x : undefined,
        dragY: drag ? this.boardDragPt?.y : undefined,
      });
    }
    return out.sort((a, b) => a.col - b.col);
  }
  private boardDragPt: Pt | null = null;
  /** x of the "+ Find boards" chip. Pinned to the FAR RIGHT of the rail rather than
   *  trailing the last board: it's the one control up here, it shouldn't wander every
   *  time a board is paired, and the space between it and the boards is what makes
   *  the free slots visible. */
  findX(): number { return this.vbW - FIND_W - 14; }
  /** Last slot a board can occupy without sliding under the Find boards chip. */
  private maxSlot(): number {
    const limit = this.findX() - 14 - BOARD_W / 2;
    let s = 0;
    while (railSlot(s + 1).x <= limit) s++;
    return s;
  }
  goBoards(): void { void this.nav.navigate(['/boards']); }
  bx(b: BoardVM): number { return b.dragX ?? b.x; }
  /** The rail's pin shift rides HERE, in the one funnel every part of a board goes
   *  through — its glyph, its ports, the cable that leaves them, and the hit-testing
   *  for a drop. Add it once and the whole rail pins coherently; add it in the
   *  template and the cable would stay behind while the board moved. */
  by(b: BoardVM): number { return (b.dragY ?? b.y) + this.railShift(); }
  /** How far down the rail has been pushed to stay on screen. 0 at rest. */
  railShift(): number { return this.vp.pinShift(); }
  railPinned(): boolean { return this.vp.pinned(); }
  /** Board y of the scrim's lower edge — the band a cable may cross for free. */
  scrimBottom(): number { return this.railShift() + SCRIM_H; }
  /** Anything being dragged right now, which is when the scrim backs off. */
  dragActive(): boolean { return !!this.dragId || !!this.bodrag; }

  private rosterFor(boardId: string): Map<number, string> {
    const used = new Map<number, string>();
    if (!this.topo) return used;
    const fallback = this.defaultControllerId();
    for (const e of this.allElems()) {
      if (e['type'] !== 'selector') continue;
      if (((e['controllerId'] as string) ?? fallback) !== boardId) continue;
      if (e['kind'] === 'linear') { used.set(SERVO_PORTS, e['id'] as string); continue; }
      used.set(((e['servo'] as RawEl | undefined)?.['channel'] as number) ?? 0, e['id'] as string);
    }
    return used;
  }

  // ── ports and tabs ──────────────────────────────────────────────────────────
  portsOf(b: BoardVM): number[] { return Array.from({ length: SERVO_PORTS + 1 }, (_, i) => i); }
  portX(b: BoardVM, ch: number): number { return portPos({ x: this.bx(b), y: this.by(b) }, ch).x - portWidth(ch) / 2; }
  portY(b: BoardVM, ch: number): number { return portPos({ x: this.bx(b), y: this.by(b) }, ch).y - PORT_H / 2; }
  portW(ch: number): number { return portWidth(ch); }
  portTaken(b: BoardVM, ch: number): boolean { return b.used.has(ch); }
  portLabel(ch: number): string { return ch >= SERVO_PORTS ? 'ST' : String(ch); }
  /** Amber, not red: a full board isn't broken, it's out of room. */
  boardFull(b: BoardVM): boolean { return b.servoCount >= SERVO_PORTS; }
  portLit(b: BoardVM, ch: number): boolean {
    const d = this.wireDrag; if (!d) return false;
    if (!!d.over && d.over.boardId === b.id && d.over.channel === ch) return true;
    return d.mode === 'toGate' && d.port!.boardId === b.id && d.port!.channel === ch;
  }
  tabLit(gateId: string): boolean {
    const d = this.wireDrag; if (!d) return false;
    return d.mode === 'toGate' ? d.overGate === gateId : d.gateId === gateId;
  }

  /** Gates get a tab on their TOP-RIGHT corner: the servo, which is the actual
   *  electrical end of a blast gate.
   *
   *  It sat level with the gate's centre until 2026-08-15, which put it exactly
   *  where a horizontal duct leaves the same edge — tab and duct overlapping,
   *  reading as one object. Either corner clears the outgoing run; the top is the
   *  side the cables come down from, so the tab now meets its own cable instead of
   *  the cable reaching around the gate to find it. It shared the corner with the
   *  red (−) badge for a day; the badge went, so the tab has it outright.
   *
   *  It STRADDLES that corner rather than floating outside it. Held off the edge
   *  it read as a separate thing parked in the gap, and on a manifold — whose body
   *  ends well left of its own outlets — the tab landed nearer the next tool than
   *  its own gate: the cable looked like it went to the jointer. Half-in, half-out
   *  of the body, there is nothing to misread. */
  gateTabs(): Array<{ id: string; x: number; y: number; channel: string; wired: boolean; shade: string }> {
    return this.nodes.filter(n => this.isGate(n)).map(n => {
      const el = this.elem(n.id);
      const linear = el?.['kind'] === 'linear';
      const ch = linear ? SERVO_PORTS : (((el?.['servo'] as RawEl | undefined)?.['channel'] as number) ?? 0);
      const board = this.boardOf(n.id);
      return {
        id: n.id, x: this.tabX(n), y: this.tabY(n),
        channel: linear ? 'ST' : String(ch),
        wired: this.boardSlots.has(board),
        shade: this.boardShade(board),
      };
    });
  }
  private tabX(n: NodeVM): number {
    return this.nx(n) + (n.isUnit ? (n.span - 1) * CELL + GATE_PAD : this.halfW(n));
  }
  /** Level with the gate's top edge, not its centre — see gateTabs(). */
  private tabY(n: NodeVM): number { return this.ny(n) - this.halfH(n); }
  private boardOf(gateId: string): string {
    return ((this.elem(gateId)?.['controllerId'] as string) ?? this.defaultControllerId());
  }
  private channelOf(gateId: string): number {
    const el = this.elem(gateId);
    if (el?.['kind'] === 'linear') return SERVO_PORTS;
    return ((el?.['servo'] as RawEl | undefined)?.['channel'] as number) ?? 0;
  }

  // ── cable runs ──────────────────────────────────────────────────────────────
  /**
   * One path per wired gate whose board is on the canvas.
   *
   * Ranked PER BOARD by how far the cable travels, LONGEST first, so the long runs
   * take the high lanes and shorter ones nest beneath them instead of weaving
   * through each other (see cableRun). Hops go on
   * whichever cable is drawn later, and only ever where cable crosses CABLE — a
   * cable crossing a duct implies nothing electrical, and bumping every one of
   * those turns an ordinary run into a washboard.
   */
  /** What a cable pays to cross things, for the drop-column choice in cableRun().
   *
   *  A device body is expensive: a wire drawn over a gate or through a tool reads as
   *  going INTO it, and the piece it actually serves is somewhere else entirely. A
   *  duct is cheap but not free — cable crosses ductwork all day in a real shop, and
   *  a crossing is drawn plainly rather than hopped (see cablePath), so it costs
   *  enough to break a tie and not enough to send a wire round the shop to avoid one.
   *
   *  The gate being wired is excluded: its tab sits ON its own top edge, so it would
   *  otherwise price every approach to itself out of reach. */
  private cableCost(gateId: string): (a: Pt, b: Pt) => number {
    const boxes = this.nodes
      .filter(n => n.id !== gateId)
      .map(n => deviceBox(this.sceneNode(n), 6));
    const ducts = this.ducts.map(d => this.ductPoints(d.childId)).filter(p => p.length > 1);
    // The band under a pinned rail is a FREE LANE. Everything down there is faded on
    // purpose, so a wire crossing it says nothing misleading — and making cables detour
    // around glyphs the user can barely see would produce contortions with no visible
    // cause. Only while pinned: at rest that band is ordinary board.
    const free = this.railPinned() ? this.scrimBottom() : -Infinity;
    return (a, b) => {
      if (Math.max(a.y, b.y) <= free) return 0;
      let c = 0;
      for (const box of boxes) if (segBoxHit(a, b, box)) c += 100;
      for (const pts of ducts) {
        for (let i = 0; i < pts.length - 1; i++) {
          if (crossing([a, b] as const, [pts[i], pts[i + 1]] as const)) { c += 3; break; }
        }
      }
      return c;
    };
  }

  /** The shade this board's wiring wears, by its slot on the rail. Stable under a
   *  reorder is not the goal — being DISTINCT from its neighbour is. */
  boardShade(boardId: string): string {
    const slot = this.boardSlots.get(boardId) ?? 0;
    return CABLE_SHADES[slot % CABLE_SHADES.length];
  }

  cables(): CableVM[] {
    if (!this.topo) return [];
    const out: CableVM[] = [];
    const drawn: ReturnType<typeof segmentsOf> = [];
    const all = this.boards();
    all.forEach((b, bi) => {
      const legs: Array<{ gateId: string; channel: number; from: Pt; to: Pt }> = [];
      for (const [ch, gateId] of b.used) {
        const n = this.byId.get(gateId); if (!n) continue;
        const to = { x: this.tabX(n), y: this.tabY(n) };
        legs.push({
          gateId, channel: ch,
          from: portExit({ x: this.bx(b), y: this.by(b) }, ch),
          to,
        });
      }
      const rank = rankByTravel(legs, l => Math.abs(l.to.x - l.from.x));
      // Half a lane step per board, so two boards never hand out the same lane row.
      const bias = bi * (7);
      for (const l of legs) {
        // No body clearance to reserve any more: the tab is on the gate's TOP edge,
        // so a lane one gap above it is already clear of the box. It was halfH back
        // when the cable landed underneath and had to get past the gate to reach it.
        const pts = cableRun(l.from, l.to, rank.get(l) ?? 0, bias, 0, this.cableCost(l.gateId));
        out.push({ id: b.id + ':' + l.channel, gateId: l.gateId, boardId: b.id, channel: l.channel,
                   d: cablePath(pts, drawn), shade: this.boardShade(b.id) });
        drawn.push(...segmentsOf(pts));
      }
    });
    return out;
  }

  // ── dragging a board ────────────────────────────────────────────────────────
  onBoardDown(evt: PointerEvent, b: BoardVM): void {
    evt.preventDefault(); evt.stopPropagation();
    const p = this.toSvg(evt);
    this.bodrag = { id: b.id, dx: p.x - b.x, dy: 0, fromTray: false, moved: false };
    this.boardDragPt = { x: b.x, y: b.y };
    window.addEventListener('pointermove', this.boMove);
    window.addEventListener('pointerup', this.boUp);
    this.vp.beginEdgeScroll(this.boMove, evt);
  }
  // Boards slide ALONG the rail and nowhere else, so a drag is a reorder rather than
  // a placement — there is no invalid drop and nothing to refuse.
  private onBoardMove(evt: PointerEvent): void {
    this.vp.trackEdge(evt);
    if (!this.bodrag) return;
    this.bodrag.moved = true;
    const p = this.toSvg(evt);
    this.boardDragPt = { x: p.x - this.bodrag.dx, y: -RAIL_H / 2 };
  }
  private onBoardUp(): void {
    this.vp.endEdgeScroll();
    window.removeEventListener('pointermove', this.boMove);
    window.removeEventListener('pointerup', this.boUp);
    const d = this.bodrag; const pt = this.boardDragPt;
    this.bodrag = null; this.boardDragPt = null;
    if (!d || !pt || !d.moved) return;
    // A board goes wherever there's room, not into a packed left-justified queue —
    // the rail is a shelf, and where you put a board on it is information (the one
    // by the far wall belongs over there). So: drop into the slot under the pointer,
    // and if something is already in it, the two trade places.
    const from = this.boardSlots.get(d.id);
    const to = Math.max(0, Math.min(this.maxSlot(), slotAt(pt.x)));
    if (from === undefined || from === to) return;
    const occupant = [...this.boardSlots.entries()].find(([id, s]) => s === to && id !== d.id);
    this.pushHistory(null);
    this.boardSlots.set(d.id, to);
    if (occupant) this.boardSlots.set(occupant[0], from);
    this.dirty = true; this.saveError = ''; this.saveNote = '';
    this.recomputeExtent();
  }

  // ── dragging a cable ────────────────────────────────────────────────────────
  // Either end. Grab the servo tab and the BOARD end comes loose — drop it on a
  // port. Grab a port and the GATE end comes loose — drop it on a tab. Symmetric,
  // and it means you can re-home a gate from whichever end you happen to be
  // looking at.

  /** From a gate's tab: the loose end is the board end, so we're hunting a port. */
  onTabDown(evt: PointerEvent, t: { id: string; x: number; y: number }): void {
    evt.preventDefault(); evt.stopPropagation();
    const p = { x: t.x, y: t.y };
    this.wireDrag = { mode: 'toPort', gateId: t.id, from: p, to: p, over: null, overGate: null };
    this.wireBlocked = ''; this.wireNote = '';
    window.addEventListener('pointermove', this.wMove);
    window.addEventListener('pointerup', this.wUp);
    this.vp.beginEdgeScroll(this.wMove, evt);
  }
  /** From a board port: the loose end is the gate end, so we're hunting a tab. An
   *  occupied port picks up the cable that's already there. */
  onPortDown(evt: PointerEvent, b: BoardVM, ch: number): void {
    evt.preventDefault(); evt.stopPropagation();
    const p = portExit({ x: this.bx(b), y: this.by(b) }, ch);
    this.wireDrag = { mode: 'toGate', port: { boardId: b.id, channel: ch }, from: p, to: p, over: null, overGate: null };
    this.wireBlocked = ''; this.wireNote = '';
    window.addEventListener('pointermove', this.wMove);
    window.addEventListener('pointerup', this.wUp);
    this.vp.beginEdgeScroll(this.wMove, evt);
  }
  private onWireMove(evt: PointerEvent): void {
    this.vp.trackEdge(evt);
    const d = this.wireDrag; if (!d) return;
    const p = this.toSvg(evt);
    d.to = p;
    if (d.mode === 'toPort') {
      d.over = this.portUnder(p); d.overGate = null;
      const v = d.over ? this.bindCheck(d.gateId!, d.over.boardId, d.over.channel) : null;
      this.wireBlocked = v?.blocked ?? ''; this.wireNote = v?.note ?? '';
    } else {
      // From a port you can land on a gate (wire this channel to that gate) OR on
      // another port (move the cable's board end — the other board, or a different
      // channel on this one). Ports win the hit test: they're the smaller target.
      const port = this.portUnder(p);
      d.over = port && !this.samePort(port, d.port!) ? port : null;
      d.overGate = d.over ? null : this.tabUnder(p);
      if (d.over) {
        const moving = this.rosterFor(d.port!.boardId).get(d.port!.channel);
        const v = moving ? this.bindCheck(moving, d.over.boardId, d.over.channel)
                         : { blocked: 'Nothing is wired to that port yet.', note: '' };
        this.wireBlocked = v.blocked; this.wireNote = v.note;
      } else if (d.overGate) {
        const v = this.bindCheck(d.overGate, d.port!.boardId, d.port!.channel);
        this.wireBlocked = v.blocked; this.wireNote = v.note;
      } else { this.wireBlocked = ''; this.wireNote = ''; }
    }
  }
  private onWireUp(): void {
    this.vp.endEdgeScroll();
    window.removeEventListener('pointermove', this.wMove);
    window.removeEventListener('pointerup', this.wUp);
    const d = this.wireDrag; this.wireDrag = null;
    const blocked = this.wireBlocked; this.wireBlocked = ''; this.wireNote = '';
    if (!d || blocked) return;
    // Port → port: the cable's BOARD end moves, and the gate on the far end comes
    // along unchanged. That's how a gate gets re-homed to another board.
    if (d.mode === 'toGate' && d.over) {
      const moving = this.rosterFor(d.port!.boardId).get(d.port!.channel);
      if (!moving) return;
      this.pushHistory(null);
      this.bindGate(moving, d.over.boardId, d.over.channel);
      return;
    }
    const gateId = d.mode === 'toPort' ? d.gateId! : d.overGate;
    const target = d.mode === 'toPort' ? d.over : d.port!;
    if (!gateId || !target) return;
    this.pushHistory(null);
    this.bindGate(gateId, target.boardId, target.channel);
  }
  private portUnder(p: Pt): { boardId: string; channel: number } | null {
    for (const b of this.boards()) {
      for (const ch of this.portsOf(b)) {
        const c = portPos({ x: this.bx(b), y: this.by(b) }, ch);
        if (Math.abs(p.x - c.x) <= 11 && Math.abs(p.y - c.y) <= 12) return { boardId: b.id, channel: ch };
      }
    }
    return null;
  }
  private samePort(a: { boardId: string; channel: number }, b: { boardId: string; channel: number }): boolean {
    return a.boardId === b.boardId && a.channel === b.channel;
  }
  private tabUnder(p: Pt): string | null {
    for (const t of this.gateTabs())
      if (Math.abs(p.x - t.x) <= 14 && Math.abs(p.y - t.y) <= 14) return t.id;
    return null;
  }

  /**
   * Whether this gate may take that port, and what will happen if it does.
   *
   * An occupied port is a SWAP, not a refusal. Every gate is bound to some board
   * and channel already (the schema requires a controllerId), so trading two
   * bindings always lands both somewhere valid and leaves every board's channel
   * count exactly as it was. Refusing instead would mean shuffling gates by hand
   * to make room for a move the app could have made itself.
   */
  private bindCheck(gateId: string, boardId: string, ch: number): { blocked: string; note: string } {
    const el = this.elem(gateId);
    if (!el) return { blocked: 'That gate is gone.', note: '' };
    const linear = el['kind'] === 'linear';
    // The one thing that can't be traded: a stepper port and a servo channel are
    // different hardware.
    if (linear && ch < SERVO_PORTS) return { blocked: 'A sliding gate needs the stepper port, not a servo channel.', note: '' };
    if (!linear && ch >= SERVO_PORTS) return { blocked: 'The stepper port drives a sliding gate, not a valve.', note: '' };
    const holder = this.rosterFor(boardId).get(ch);
    if (!holder || holder === gateId) return { blocked: '', note: '' };
    const other = this.elem(holder);
    if ((other?.['kind'] === 'linear') !== linear)
      return { blocked: 'Those two gates need different kinds of port.', note: '' };
    return { blocked: '', note: `Swap with “${(other?.['name'] as string) ?? holder}”` };
  }

  /** Put a gate on a board+channel, trading with whatever held it. */
  private bindGate(gateId: string, boardId: string, ch: number): void {
    const el = this.elem(gateId); if (!el) return;
    const holder = this.rosterFor(boardId).get(ch);
    if (holder && holder !== gateId) {
      const was = { board: this.boardOf(gateId), ch: this.channelOf(gateId) };
      this.writeBinding(holder, was.board, was.ch);
    }
    this.writeBinding(gateId, boardId, ch);
    this.dirty = true; this.saveError = ''; this.saveNote = ''; this.wip = '';
    this.syncNodes();
  }
  private writeBinding(gateId: string, boardId: string, ch: number): void {
    const el = this.elem(gateId); if (!el) return;
    el['controllerId'] = boardId;
    // One stepper driver per board, so a sliding gate's channel is always 0 — the
    // port index it was dropped on is the strip position, not the driver number.
    if (el['kind'] === 'linear') el['linear'] = { ...(el['linear'] as RawEl ?? {}), channel: 0 };
    else el['servo'] = { ...(el['servo'] as RawEl ?? {}), channel: ch };
  }

  /** The rubber cable while a drag is in flight. Dashed, and it follows the pointer
   *  rather than snapping — the routed run only appears once it lands. */
  dragCableD(): string {
    const d = this.wireDrag; if (!d) return '';
    const mx = (d.from.x + d.to.x) / 2;
    return `M ${d.from.x} ${d.from.y} C ${mx} ${d.from.y}, ${mx} ${d.to.y}, ${d.to.x} ${d.to.y}`;
  }

  // ── undo / redo ───────────────────────────────────────────────────────────────
  // Whole-state snapshots (topology + cell positions). The doc is small and every
  // mutation already rebuilds the graph, so restoring a snapshot is the same work
  // as any other edit — no need for per-action inverse operations.

  /** Snapshot now, and remember what produced it so keystroke-by-keystroke edits
   *  (renaming) collapse into one undo step instead of dozens. */
  private snapshot(): string {
    return JSON.stringify({ topo: this.topo, cells: [...this.cells], boards: [...this.boardSlots] });
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
    const state = JSON.parse(snap) as { topo: Topology; cells: [string, Cell][]; boards?: [string, number][] };
    this.topo = state.topo;
    this.cells = new Map(state.cells);
    this.boardSlots = new Map(state.boards ?? []);
    this.lastTag = null;                       // never coalesce across an undo
    this.selectedId = null; this.closeMenu();
    this.dirty = true; this.saveError = ''; this.wip = ''; this.saveNote = '';
    this.airflowErrors = [];
    this.buildGraph(this.topo); this.syncNodes(); this.refreshHandles();
  }

  private afterMutation(selectId: string | null): void {
    if (!this.topo) return;
    this.dirty = true; this.saveError = ''; this.airflowErrors = []; this.saveNote = ''; this.wip = '';
    this.normalizeCells();
    this.collapsePassThroughJunctions();
    this.buildGraph(this.topo); this.syncNodes();
    this.selectedId = selectId; this.refreshHandles();
  }

  private childDucts(id: string): RawEl[] { return this.allDucts().filter(d => d['parent'] === id); }

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
      this.setElems(this.elems(this.topo).filter(e => e['id'] !== jid));
      this.setDucts(this.ductsRaw().filter(d => d !== inDuct));
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

  /** Which board a newly drawn gate is driven by, and on what channel.
   *
   *  This used to be "the primary, always", with the channel search falling back to
   *  0 when all four were taken. In a one-board shop that is right and invisible. In
   *  a shop with a second board sitting idle it meant the FIFTH gate you drew
   *  collided on channel 0 and the layout wouldn't save until you opened "Set up this
   *  gate" and reassigned it by hand — the picker was there, the default never used
   *  it.
   *
   *  Order of preference: a board with room, then one already driving something in
   *  the system you're drawing (a board is mounted where the cable run is convenient,
   *  so the one already reaching into this system is the one whose wire is short),
   *  then the primary. Ties keep document order, which is the order /boards paired
   *  them in.
   *
   *  Room means different things per kind: four servo channels per board, but only
   *  ONE linear selector — a sliding gate uses the board's single stepper driver.
   *
   *  With nothing free anywhere it still answers the primary at channel 0, exactly as
   *  before. That is a real over-budget layout and validation says so on save; the
   *  alternative is refusing to draw the gate, which hides the problem behind a
   *  no-op. */
  private pickBoard(kind: SelKind): { controllerId: string; channel: number } {
    const t = this.topo;
    const primary = this.defaultControllerId();
    if (!t) return { controllerId: primary, channel: 0 };
    const ids = controllersOf(t).map(c => c.id);
    if (!ids.length) return { controllerId: primary, channel: 0 };

    // Boards already driving a gate in the system being drawn — elems() is the active
    // system, which is what "the system you're working in" means everywhere else.
    const here = new Set(
      this.elems(t).filter(e => e['type'] === 'selector')
        .map(e => (e['controllerId'] as string | undefined) ?? primary),
    );
    const hasRoom = (id: string) => kind === 'linear'
      ? !selectorsOnController(t, id).some(s => s.kind === 'linear')
      : firstFreeChannel(t, id) !== null;
    const rank = (id: string) =>
      (hasRoom(id) ? 0 : 4) + (here.has(id) ? 0 : 2) + (id === primary ? 0 : 1);

    const best = [...ids].sort((a, b) => rank(a) - rank(b))[0];
    return { controllerId: best, channel: kind === 'linear' ? 0 : firstFreeChannel(t, best) ?? 0 };
  }

  private addSelector(parentId: string, kind: SelKind): string | null {
    if (!this.topo) return null;
    const els = this.elems(this.topo);
    const sel = this.makeSelector(kind); els.push(sel);
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
  /** A new port, and the machine that owns it.
   *
   *  "Add a tool" has always meant one machine with one pickup, so that is what
   *  this makes: a port and a machine sharing an id. Sharing it is deliberate —
   *  anything already holding that id (`ui.layout`, a status blob, a bug report)
   *  still resolves, which is the same choice migrateToShop makes (RFC §12).
   *  A SECOND port on an existing machine is a different action and doesn't come
   *  through here. */
  private newPort(name: string): RawEl | null {
    const s = this.sys();
    if (!this.topo || !s) return null;
    return addMachineWithPort(this.topo as unknown as ShopDoc, s, this.newId('tool'), name);
  }

  private addTool(parentId: string): string | null {
    if (!this.topo) return null;
    const p = this.elem(parentId);
    let branch: Branch | undefined;
    if (p && p['type'] === 'selector') {
      // Checked BEFORE the port is created: the old code pushed the element and
      // then popped it on failure, which under a shop would also have to unwind
      // the machine. Not creating it is simpler and can't leave a stray behind.
      branch = (p['branches'] as Branch[]).find(x => x.role === 'blocked');
      if (!branch) return null;
    }
    const tool = this.newPort('New tool');
    if (!tool) return null;
    const duct: RawEl = { child: tool['id'], parent: parentId };
    if (branch) { branch.role = 'tool'; duct['parentBranch'] = branch.id; }
    this.ductsRaw().push(duct);
    return tool['id'] as string;
  }
  /** `at` is only passed when a gate is being REBUILT and has to keep the board it
   *  was already wired to (see convertKind). Every fresh gate picks its own. */
  private makeSelector(kind: SelKind, at: { controllerId: string; channel: number } = this.pickBoard(kind)): RawEl {
    const { controllerId, channel } = at;
    const base: RawEl = { id: this.newId('sel'), type: 'selector', name: this.defaultName(kind), controllerId, kind };
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
    const legs = this.legsLeftToRight(id);   // left leg → left outlet, not doc order
    // Swapping the mechanism must not re-home the gate: it keeps the board it was
    // already wired to, and its channel where it had one. Rebuilding from scratch
    // would hand a gate on a secondary back to the primary — silently unwiring it
    // from the board someone chose in "Set up this gate".
    const controllerId = (el['controllerId'] as string | undefined) ?? this.defaultControllerId();
    const channel = ((el['servo'] as RawEl | undefined)?.['channel'] as number | undefined)
      ?? firstFreeChannel(this.topo, controllerId, id) ?? 0;
    const fresh = this.makeSelector(kind, { controllerId, channel });
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
    const els = this.ownerElems(id);
    els[els.findIndex(e => e['id'] === id)] = fresh;
    this.afterMutation(id);
  }

  rename(id: string, name: string): void {
    const el = this.elem(id); if (!el) return;
    this.focus(id);
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
  deleteSelected(): void {
    const n = this.inspected(); if (!n || !this.canDelete(n) || !this.topo) return;
    this.focus(n.id);
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
      this.setDucts(this.ductsRaw().filter(d => d !== inDuct));
    } else {
      if (pBranch) pBranch.role = 'blocked';          // nothing below → the outlet frees up
      this.setDucts(this.ductsRaw().filter(d => d['child'] !== id && d['parent'] !== id));
    }
    // Deleting a tool node deletes the MACHINE. A primary port is not deletable
    // on its own (RFC §6.3) — it is the machine's one required connection — so
    // the machine and its ports go together. Dropping a single supplemental
    // pickup is `removePort`, which the multi-port UI will call instead.
    const doomed = this.elem(id);
    const machineId = doomed?.['machineId'] as string | undefined;
    if (doomed?.['type'] === 'tool' && machineId) {
      const doc = this.topo as unknown as ShopDoc;
      if (!removePort(doc, id)) removeMachine(doc, machineId);
    } else {
      this.setElems(this.elems(this.topo).filter(e => e['id'] !== id));
    }
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
    // Board ORDER along the rail, under its own key — board ids come from mDNS
    // hostnames while element ids are minted here, and one flat map would let a
    // board called "sel1" quietly take a gate's square.
    const boards: Record<string, number> = {};
    for (const [id, slot] of this.boardSlots) boards[id] = slot;
    const ui: Record<string, unknown> = { layout };
    if (Object.keys(boards).length) ui['wiring'] = { boards };
    return { ...(this.topo as Record<string, unknown>), ui };
  }

  /** Live always-open leaks (tools with no gate on their path to the collector). */
  private liveLeaks(): AirflowIssue[] {
    if (!this.topo) return [];
    // Per system: a leak is "this tool can be selected without pulling air
    // somewhere else too", which is a question about one blower's ducts. Handed
    // the whole shop, airflowIssues would read no `elements` at the root and
    // cheerfully report nothing wrong.
    try { return systemViews(this.topo as unknown as ShopDoc).flatMap(v => airflowIssues(v)); }
    catch { return []; }
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
    // as an offer rather than a demand — tap it and Delete is in its menu.
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
    const v = validateShop(doc);
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
    this.pushHistory(null);
    const n = this.capAlwaysOn();
    this.buildGraph(this.topo); this.syncNodes(); this.refreshHandles();
    this.airflowErrors = [];
    await this.save();
    if (!this.saveError && !this.airflowErrors.length) {
      this.saveNote = `Put a closed gate on ${n} leaking outlet${n === 1 ? '' : 's'} — wire a servo to each, or delete the tool.`;
    }
  }
  /**
   * Put a gate on every leaking outlet — asking again after each one.
   *
   * It used to plan the whole set against the UNTOUCHED document: collect every
   * issue, expand each co-open pair to all its partners, then fit a gate to each.
   * But gating one tool can resolve another's issue on its own, and the plan never
   * noticed — so it fitted gates for leaks that no longer existed, and those gates
   * isolate nothing. They came out flagged (redundant), which is the shop telling
   * you the fix was wrong. Re-asking after every insertion stops as soon as the
   * shop is tight, which makes the result minimal by construction.
   */
  private capAlwaysOn(): number {
    if (!this.topo) return 0;
    const done = new Set<string>();
    const targets: string[] = [];
    for (let guard = 0; guard < 64; guard++) {
      const leaks = airflowIssues(this.topo);
      if (!leaks.length) break;
      // Take the first issue and gate something that isn't gated yet — the tool it
      // names, or failing that one of the partners it shares an outlet with. Two
      // tools on one outlet still report a leak after the first gate goes on (run
      // that one and its neighbour is still open), and the named tool is the one
      // already fixed, so the partner is what's left to do.
      const iss = leaks[0];
      const next = [iss.id, ...(iss.with ?? []).map(w => w.id)].find(id => !done.has(id));
      if (!next) break;                        // nothing left we haven't tried
      done.add(next);
      if (this.capOne(next)) targets.push(next);
    }
    this.dirty = true;
    return targets.length;
  }
  /** One gate, spliced above one tool. False when the tool has no feed to splice. */
  private capOne(id: string): boolean {
    if (!this.topo) return false;
    {
      const duct = this.ductsRaw().find(d => d['child'] === id); if (!duct) return false;
      const parentId = duct['parent'] as string;
      const parentBranch = duct['parentBranch'] as string | undefined;
      const gate = this.makeSelector('servoGate');
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
      // The gate takes the tool's cell and the tool moves to the first free cell in
      // its own system — capping a shared outlet inserts several gates at once, so
      // "one row down" alone would stack tools on top of each other.
      const tcell = this.cells.get(id) ?? { col: 0, row: 0 };
      this.cells.set(gate['id'] as string, { col: tcell.col, row: tcell.row });
      this.cells.set(id, this.freeCellBelow(id, tcell));
    }
    this.buildGraph(this.topo);
    return true;
  }

  /** A second dust collector, with its own duct tree.
   *
   *  In the overflow rather than on the toolbar because a shop grows one roughly
   *  once — and it is not a mode: the new system's collector simply appears below
   *  everything already drawn, on the top-left cell of its own ground, with an open
   *  end to drag pipe out of. Which system you are editing then follows whatever you
   *  touch (see focus()), so there is no switch to remember.
   *
   *  The collector goes unnamed on purpose. Pairing its smart outlet names it, the
   *  same way the first one got its name, so adding a system asks nothing. */
  addSystem(): void {
    if (!this.topo) return;
    this.pushHistory(null);
    const doc = this.topo as unknown as ShopDoc;
    const sys = addSystem(doc, {
      system: this.newSystemId(),
      collector: this.newId('dc'),
      end: this.newId('j'),
    });
    this.activeSystemId = sys.id;
    this.buildGraph(this.topo);
    // Place the new pair below everything drawn rather than re-running the whole
    // auto-layout: anything already arranged by hand has to stay where it was put.
    let bottom = -1;
    for (const c of this.cells.values()) bottom = Math.max(bottom, c.row);
    const top = bottom + SYSTEM_GAP + 1;
    this.cells.set(sys.elements[0]['id'] as string, { col: 0, row: top });
    this.cells.set(sys.elements[1]['id'] as string, { col: 0, row: top + 1 });
    // Numbered from the start, so two collectors are never both "Dust collector"
    // even if the name field is dismissed without typing.
    sys.elements[0]['name'] = `Dust collector ${systemsOf(doc).length}`;
    // SELECTED, not deselected: selecting a piece is what puts the editable name on
    // it, so the new collector arrives with its name field live and the caret in it.
    // That name IS the system's name on the seam above it — leaving the user to
    // discover renaming later is how a shop ends up unable to tell them apart.
    this.selectedId = sys.elements[0]['id'] as string;
    this.dirty = true; this.saveError = '';
    this.syncNodes(); this.refreshHandles();
    // The drawing just got a band taller, so the extent has to be recomputed before
    // anything measures it — then scroll to what was just made. A new collector that
    // lands below the fold looks like the menu item did nothing.
    this.recomputeExtent();
    this.focusName();
    this.vp.revealBoard(PAD + top * CELL - CELL, PAD + (top + 1) * CELL + CELL / 2);
  }
  /** Put the caret in the selected piece's name field, once Angular has drawn it.
   *  Two frames: the field only exists after the render that selection triggers. */
  private focusName(): void {
    setTimeout(() => {
      const el = document.querySelector('input.nameedit') as HTMLInputElement | null;
      el?.focus(); el?.select();
    });
  }

  /** System ids only have to be unique among systems, but they share the counter so
   *  a doc never grows two things called `s3`. */
  private newSystemId(): string {
    const taken = new Set(systemsOf(this.topo as unknown as ShopDoc).map(s => s.id));
    let id: string, n = taken.size;
    do { id = `s${++n}`; } while (taken.has(id));
    return id;
  }

  /** A second place this machine collects from — an overarm guard, a hood.
   *
   *  A PORT, not a machine: the name, the plug and the trip point already exist and
   *  stay where they are. It lands in the system the machine already lives in and
   *  arrives as an open end you drag out and gate like any other run — including,
   *  if you drag it there, into the other collector's system, which is the case the
   *  shop container exists for.
   *
   *  It never gets a cell. The hood rides on the machine's own box (pickupSeat), so
   *  the drawing gains an inlet rather than a second saw. */
  private addPickup(toolId: string): void {
    if (!this.topo) return;
    const doc = this.topo as unknown as ShopDoc;
    const machine = machineOfPort(doc, this.elem(toolId)); if (!machine) return;
    const used = supplementalCount(doc, machine.id as string);
    if (used >= MAX_PICKUPS) return;
    const sys = this.sys(); if (!sys) return;
    this.pushHistory(null);
    const port = addSupplementalPort(
      doc, sys, machine.id as string, this.newId('p'),
      used === 0 ? 'overarm' : 'hood',
    );
    // An open end above it, so there is something to drag pipe out of — the same
    // shape every other run starts as.
    const end: RawEl = { id: this.newId('j'), type: 'junction', name: 'Open end' };
    sys.elements.push(end);
    sys.ducts.push({ child: port['id'], parent: end['id'] });
    this.buildGraph(this.topo);
    // The open end DOES get a cell: it is a piece you place. One row above the
    // machine and one column right, which is where its hood points — but not up out
    // of its own system. A machine on its band's top row keeps the end level with it
    // rather than reaching into the collector above.
    const c = this.cells.get(toolId);
    const id = end['id'] as string;
    if (c) this.cells.set(id, { col: c.col + 1, row: Math.max(0, this.bandCeiling(id) + 1, c.row - 1) });
    this.selectedId = null;
    this.dirty = true; this.saveError = '';
    this.syncNodes(); this.refreshHandles(); this.recomputeExtent();
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

    // Import opens WORK IN PROGRESS, so the bar here is "can the editor open it",
    // not "is it a finished shop". A half-built layout — two gates sharing a servo
    // channel, an outlet with nothing on it — is exactly the sort of thing you save
    // to a file and come back to, and refusing it meant you couldn't.
    const broken = this.importBlocker(doc);
    if (broken) { this.saveError = 'That file doesn’t look like a shop layout: ' + broken; return; }

    this.saving = true; this.saveError = ''; this.saveNote = ''; this.wip = '';
    try {
      this.pushHistory(null);                 // an import is undoable like any other edit
      this.topo = JSON.parse(JSON.stringify(doc)) as Topology;
      this.cells.clear();
      this.buildGraph(this.topo);
      const saved = this.savedLayout(this.topo);
      if (saved) for (const [id, c] of Object.entries(saved)) this.cells.set(id, c);
      else this.autoLayoutInto(this.cells);
      this.loadBoardCells(this.topo);
      this.selectedId = null;
      this.syncNodes();

      // Only a whole layout goes to the controller — it validates server-side and
      // would reject a draft anyway. An unfinished one stays here, dirty, with the
      // reason on the guide bar, and Save picks it up once you've sorted it out.
      const v = validateShop(this.docWithLayout());
      if (!v.ok) {
        this.dirty = true;
        this.wip = v.errors[0]?.message ?? 'incomplete layout';
        return;
      }
      await this.api.putTopology(this.docWithLayout() as Topology);
      this.dirty = false;
      this.airflowErrors = this.liveLeaks();
      try { this.applyLive(await this.api.getStatus()); } catch { /* not running */ }
    } catch {
      this.dirty = true;
      this.saveError = 'Imported here, but couldn’t reach the controller to store it.';
    } finally { this.saving = false; }
  }

  /** Why this file can't be opened at all, or '' if it can.
   *
   *  Deliberately narrow: it checks the shape the editor indexes on — ids it can key
   *  by, ducts that point at elements that exist — and nothing about whether the
   *  shop makes sense. Anything past this loads as a draft. */
  private importBlocker(doc: unknown): string {
    if (!doc || typeof doc !== 'object' || Array.isArray(doc)) return 'it isn’t a layout object.';
    const d = doc as { elements?: unknown; ducts?: unknown; controllers?: unknown; systems?: unknown };
    if (d.controllers !== undefined && !Array.isArray(d.controllers)) return 'its controller list is malformed.';

    // Both shapes land here: a schemaVersion-2 shop keeps its elements and ducts
    // inside `systems[]`, a v1 file at the root. Checked FLATTENED, because the
    // questions this asks — are ids unique, do ducts point at something real —
    // are shop-wide either way, and element ids are unique shop-wide by contract.
    // (Which system a duct is in is validateShop's business, not this gate's.)
    let elements: unknown[] = [];
    let ductList: unknown[] = [];
    if (d.systems !== undefined) {
      if (!Array.isArray(d.systems)) return 'its system list is malformed.';
      for (const raw of d.systems) {
        if (!raw || typeof raw !== 'object') return 'one of its systems isn’t readable.';
        const s = raw as { elements?: unknown; ducts?: unknown };
        if (s.elements !== undefined && !Array.isArray(s.elements)) return 'its element list is malformed.';
        if (s.ducts !== undefined && !Array.isArray(s.ducts)) return 'its duct list is malformed.';
        elements = elements.concat((s.elements ?? []) as unknown[]);
        ductList = ductList.concat((s.ducts ?? []) as unknown[]);
      }
    } else {
      if (d.elements !== undefined && !Array.isArray(d.elements)) return 'its element list is malformed.';
      if (d.ducts !== undefined && !Array.isArray(d.ducts)) return 'its duct list is malformed.';
      elements = (d.elements ?? []) as unknown[];
      ductList = (d.ducts ?? []) as unknown[];
    }

    const ids = new Set<string>();
    for (const raw of elements) {
      if (!raw || typeof raw !== 'object') return 'one of its pieces isn’t readable.';
      const el = raw as { id?: unknown; type?: unknown };
      if (typeof el.id !== 'string' || !el.id) return 'a piece is missing its id.';
      if (ids.has(el.id)) return `two pieces share the id “${el.id}”.`;
      if (typeof el.type !== 'string' || !el.type) return `piece “${el.id}” is missing its type.`;
      ids.add(el.id);
    }

    // A duct pointing at something that isn't there would draw from nowhere — that's
    // a damaged file rather than an unfinished one.
    for (const raw of ductList) {
      if (!raw || typeof raw !== 'object') return 'one of its ducts isn’t readable.';
      const duct = raw as { child?: unknown; parent?: unknown };
      if (typeof duct.child !== 'string' || typeof duct.parent !== 'string') return 'a duct is missing an end.';
      if (!ids.has(duct.child)) return `a duct points at a missing piece “${duct.child}”.`;
      if (!ids.has(duct.parent)) return `a duct points at a missing piece “${duct.parent}”.`;
    }
    return '';
  }

  // ── graph helpers ─────────────────────────────────────────────────────────────
  //
  // THE CANVAS EDITS ONE SYSTEM AT A TIME. Everything below resolves against
  // `activeSystemId`, not the document root, because a system is exactly one duct
  // tree and this tool draws a duct tree. Readers that want the whole shop —
  // "every gate needing calibration", "is this id taken" — use elementsOf() /
  // ductsOf() from gates/selector-types.ts, which flatten across systems.
  //
  // Keeping both seams honest is what makes the container cheap: the drawing code
  // below never learned that a second system exists, because from in here there
  // still is only one.
  private activeSystemId: string | null = null;

  /** The system being drawn. Never null once a document is loaded — a shop always
   *  has at least one system, and systemById falls back to the first. */
  private sys(t: Topology | null = this.topo): ShopSystem | null {
    return systemById(t as unknown as ShopDoc | null, this.activeSystemId);
  }
  private elems(t: Topology): RawEl[] { return this.sys(t)?.elements ?? []; }
  private ductsRaw(): RawEl[] { return this.sys()?.ducts ?? []; }

  /** Which system each element belongs to. Rebuilt with the graph, and the thing
   *  that lets `activeSystemId` follow whatever you touched instead of being a mode
   *  you have to remember to switch. */
  private systemOf = new Map<string, string>();

  /** Point the mutation seam at the system this piece lives in.
   *
   *  Everything under "graph helpers" writes to the ACTIVE system, which used to be
   *  a safe assumption because there was only one. With several on screen at once
   *  the assumption has to be re-established at every entry point — a tap, a drag, a
   *  menu — rather than assumed. Cheap enough to call on the way past. */
  private focus(id: string | null | undefined): void {
    const s = id ? this.systemOf.get(id) : null;
    if (s) this.activeSystemId = s;
  }

  /** Every element in the shop, not just the drawn-on system.
   *
   *  For facts that are shop-wide however many systems there are: which plug IPs are
   *  spoken for, which servo channels a BOARD has handed out (a board is mounted
   *  where the cable is convenient and may drive selectors in any system — shop.js
   *  §controllers), and looking an element up by an id that is unique shop-wide. */
  private allElems(t: Topology | null = this.topo): RawEl[] {
    return systemsOf(t as unknown as ShopDoc | null).flatMap(s => s.elements);
  }
  private allDucts(t: Topology | null = this.topo): RawEl[] {
    return systemsOf(t as unknown as ShopDoc | null).flatMap(s => s.ducts);
  }
  /** The elements array that actually holds this id — the one a write has to land
   *  in. Falls back to the active system for an id that isn't placed yet. */
  private ownerElems(id: string): RawEl[] {
    const sid = this.systemOf.get(id);
    const s = sid ? systemsOf(this.topo as unknown as ShopDoc).find(x => x.id === sid) : null;
    return s ? s.elements : this.elems(this.topo!);
  }
  /** Replace the active system's arrays. The old code assigned `topo.elements`
   *  directly; a shop keeps them one level down, so the write needs the same
   *  indirection the reads have. */
  private setElems(next: RawEl[]): void { const s = this.sys(); if (s) s.elements = next; }
  private setDucts(next: RawEl[]): void { const s = this.sys(); if (s) s.ducts = next; }

  /** A fresh shop: a primary controller, one system holding a collector, and one
   *  bare open run off it — an immediate anchor to draw/tap from (duct-first:
   *  there's always a run end to pull pipe from or drop the first fitting onto). */
  private blankTopology(): Topology {
    return {
      schemaVersion: SHOP_SCHEMA_VERSION,
      name: 'My Shop',
      controllers: [{ id: 'primary', role: 'primary', name: 'Shop Brain', board: 'devkitc' }],
      systems: [{
        id: 'system-1',
        name: 'Dust collection',
        elements: [
          { id: 'dc', type: 'collector', name: 'Dust collector' },
          { id: 'end0', type: 'junction', name: 'Open end' },
        ],
        ducts: [{ child: 'end0', parent: 'dc' }],
      }],
      machines: [],
      devices: [],
    } as unknown as Topology;
  }
  /** Shop-wide: element ids are unique across systems (validateShop enforces it),
   *  and half the callers here are asking about a piece on a system that isn't the
   *  one being written to. */
  private elem(id: string): RawEl | undefined { return this.topo ? this.allElems().find(e => e['id'] === id) : undefined; }
  /** A fresh id, unique across the WHOLE shop — not just the system being drawn.
   *  Element ids address hardware and appear in logs and in `ui.layout`, so
   *  validateShop requires shop-wide uniqueness; checking only the active system
   *  would mint a collision the moment a second system exists. Machine ids share
   *  the namespace for the same reason. */
  private newId(prefix: string): string {
    const taken = new Set<string>();
    if (this.topo) {
      for (const e of elementsOf(this.topo)) taken.add(e.id);
      for (const m of machinesOf(this.topo as unknown as ShopDoc)) taken.add(m.id);
    }
    let id: string;
    do { id = `${prefix}${++this.counter}`; } while (taken.has(id));
    return id;
  }

  private buildGraph(t: Topology): void {
    this.parentOf.clear(); this.outletOf.clear(); this.systemOf.clear();
    // Every system, not just the drawn-on one: the canvas shows the whole shop now,
    // and the id → system index below is what keeps the writes pointed at the right
    // one. Systems share no duct and no element id, so flattening them is lossless.
    for (const s of systemsOf(t as unknown as ShopDoc)) {
      for (const e of s.elements) this.systemOf.set(e['id'] as string, s.id);
    }
    const ducts = this.allDucts(t);
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
  /** Where a supplemental port sits: on the top edge of its machine's primary box,
   *  right of the trunk's own inlet, one hood per pickup.
   *
   *  It borrows the primary's CELL rather than owning one. Everything about placement
   *  on this canvas is written in cells — the layout, the drop checks, the band rule —
   *  and a pickup is not a thing you place; it is part of a machine that has already
   *  been placed. Giving it a cell would let you drag half a saw into another system. */
  private pickupSeat(e: RawEl): { cell: Cell; anchor: { dx: number; dy: number } } | null {
    const doc = this.topo as unknown as ShopDoc;
    const machine = machineOfPort(doc, e); if (!machine) return null;
    const primary = primaryPortOf(doc, machine.id as string); if (!primary) return null;
    const cell = this.cells.get(primary['id'] as string); if (!cell) return null;
    // Which pickup this is, in document order, so two hoods never land on each other.
    const mine = portsOf(doc, machine.id as string)
      .filter(({ port }) => isPortSupplemental(port))
      .map(({ port }) => port['id'] as string);
    const i = Math.max(0, mine.indexOf(e['id'] as string));
    return { cell, anchor: { dx: PICKUP_DX + i * PICKUP_STEP, dy: -TOOL_HALF } };
  }

  /** What a pickup is FOR, in the woodworker's word for it — the only thing that
   *  distinguishes two ports on one saw. */
  pickupRole(n: NodeVM): string { return (this.elem(n.id)?.['role'] as string) || 'pickup'; }

  private glyphFor(e: RawEl): Glyph {
    if (e['type'] === 'collector') return 'collector';
    if (e['type'] === 'tool') return isPortSupplemental(e) ? 'pickup' : 'tool';
    if (e['type'] === 'junction') return 'junction';
    switch (e['kind']) { case 'servoGate': return 'ballvalve'; case 'servoManifold': return 'manifold'; default: return 'slidingGate'; }
  }
  private syncNodes(): void {
    if (!this.topo) return;
    // Derived, never stored: a gate that stops being redundant stops being flagged
    // on the very next mutation, with nothing to keep in sync.
    let redundant = new Set<string>();
    // Per system, for the same reason as liveLeaks() — a selector is redundant
    // relative to the run it sits on, and runs don't cross systems.
    try {
      redundant = new Set(systemViews(this.topo as unknown as ShopDoc)
        .flatMap(v => redundantSelectors(v)).map(r => r.id));
    } catch { /* mid-edit doc */ }
    this.nodes = this.allElems().map(e => {
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
      const seat = glyph === 'pickup' ? this.pickupSeat(e) : null;
      return { id, glyph, name: (e['name'] as string) || id,
               col: seat?.cell.col ?? c.col, row: seat?.cell.row ?? c.row,
               branchCount, isUnit, span: isUnit ? Math.max(1, branchCount) : 1,
               live: false, openIndex: 0, setup, redundant: redundant.has(id),
               anchor: seat?.anchor };
    });
    this.byId = new Map(this.nodes.map(n => [n.id, n]));
    this.recomputeExtent();
  }
  private occupiedExcept(exclude: Set<string>): Set<string> {
    const occ = new Set<string>();
    for (const n of this.nodes) {
      if (exclude.has(n.id) || n.glyph === 'pickup') continue;   // rides its machine's cell
      if (n.isUnit) for (let i = 0; i < n.span; i++) occ.add((n.col + i) + ',' + n.row);
      else occ.add(n.col + ',' + n.row);
    }
    return occ;
  }
  /** The board is at least the whole visible area and grows past it as the layout
   *  does — so there's always empty grid to build onto, and it scrolls only once
   *  the shop genuinely outgrows the screen. */
  private recomputeExtent(): void {
    // Boards count toward the extent too, or one parked past the last tool would
    // sit outside the scrollable area with no way to reach it.
    const maxCol = Math.max(1, ...this.nodes.map(n => n.col + n.span - 1));
    const maxRow = Math.max(1, ...this.nodes.map(n => n.row));
    const wrap = this.wrapRef?.nativeElement;
    // The rail has to fit too: out to the FURTHEST OCCUPIED slot (boards can sit
    // anywhere along it now, so a count isn't the width) plus the chip on the end.
    const last = Math.max(0, ...this.boardSlots.values());
    const railW = railSlot(last).x + BOARD_W / 2 + 14 + FIND_W + 14;
    // What the drawing actually occupies, BEFORE the viewport floor below. fitZoom()
    // needs this: vbW/vbH are grown to fill the screen, so they always "fit" by
    // construction and are useless for deciding a scale.
    this.contentW = Math.max(PAD * 2 + maxCol * CELL + CELL, railW);
    this.contentH = PAD * 2 + maxRow * CELL + CELL + RAIL_H;
    // The wrap's size is in SCREEN px and the extent is in BOARD units, so the
    // viewport floor has to be divided back through the zoom — otherwise zooming out
    // grows the board to fill the screen again and there is nothing left to zoom out to.
    this.vbW = Math.max(this.contentW, (wrap?.clientWidth ?? 0) / this.vp.zoom);
    // − RAIL_H because the SVG renders vbH PLUS the rail above it. Flooring vbH at the
    // full viewport height made the drawing exactly one rail taller than the screen at
    // every zoom, so the board never quite fit vertically and always kept a sliver of
    // scroll.
    this.vbH = Math.max(PAD * 2 + maxRow * CELL + CELL, (wrap?.clientHeight ?? 0) / this.vp.zoom - RAIL_H);
  }
  /** The grey ground under each system: the rows its pieces occupy, full width.
   *
   *  Derived from where things actually ARE rather than from the layout pass, so it
   *  still tells the truth after a piece has been dragged. Full width on purpose —
   *  a ground that hugged its contents would change shape with every drag, and the
   *  thing being said is "these rows are that collector's", which is about rows. */
  systemGrounds(): Array<{ id: string; name: string; y: number; h: number }> {
    return this.systemBands().map(b => ({ id: b.id, name: b.name, y: b.y0, h: b.y1 - b.y0 }));
  }

  /** The line between two systems, with the name of the one it introduces.
   *
   *  The empty row alone was doing this job, and it stopped the moment anything was
   *  dragged into it: two grounds that meet are one ground, and the drawing quietly
   *  claimed the shop had a single collector. A rule can't be dragged shut.
   *
   *  It carries the lower system's name because that is the question a boundary
   *  raises — not "what did I just leave" but "what am I now looking at" — and the
   *  name is the collector's own, so there is nothing extra to name or keep in sync. */
  systemSeparators(): Array<{ y: number; name: string }> {
    const bands = this.systemBands();
    const out: Array<{ y: number; name: string }> = [];
    for (let i = 1; i < bands.length; i++) {
      out.push({ y: (bands[i - 1].y1 + bands[i].y0) / 2, name: bands[i].name });
    }
    return out;
  }

  /** Each system's rows, in board px, top to bottom. The drawn form of
   *  systemRowBands() — same rows, so the ground can't disagree with the rule that
   *  decides what may stand on it. */
  private systemBands(): Array<{ id: string; name: string; y0: number; y1: number }> {
    return this.systemRowBands().map(b => ({
      id: b.id,
      name: b.name ?? 'Dust collector',
      // Half a cell of air past the outermost row, less GROUND_INSET so two bands
      // that end up adjacent still show a seam rather than fusing into one.
      y0: PAD + b.lo * CELL - CELL / 2 + GROUND_INSET,
      y1: PAD + b.hi * CELL + CELL / 2 - GROUND_INSET,
    }));
  }
  private childrenOf(id: string): string[] { const out: string[] = []; for (const [c, p] of this.parentOf) if (p === id) out.push(c); return out; }
  private canAddChild(id: string): boolean {
    const el = this.elem(id); if (!el) return false;
    if (el['type'] === 'collector') return true;
    if (el['type'] === 'junction') return true;   // a tee/open end always takes another leg
    if (el['type'] === 'selector') return (el['branches'] as Branch[]).some(b => b.role === 'blocked');
    return false;
  }
  /** Auto-layout, "left rail": the trunk owns column 0 and every branch feeds off it
   *  sideways, stacked down the page.
   *
   *  This used to fan out horizontally — the collector centred over its children, each
   *  fork widening the drawing again. It read beautifully on a laptop and was unusable
   *  on the phone the shop actually gets built on: a six-tool shop came out ten columns
   *  wide, which is a 40% zoom, which is a drawing you cannot tap. Three vertical
   *  arrangements were mocked up (`docs/mockups/vertical-layout.html`) and this is the
   *  one that won: it costs a column against the tightest option and buys a drawing
   *  where the main line is unmistakable and no duct crosses another.
   *
   *  So: the collector sits at the head of column 0, that column stays empty beneath it
   *  for the trunk to run down, and each of its legs gets its own BAND of rows starting
   *  at column 1. Bands share columns — that's what keeps the thing narrow — and are
   *  separated by one blank row so two branches never look joined. Inside a band the
   *  old left-to-right packer still runs, because a manifold and its tools genuinely are
   *  a horizontal object.
   *
   *  Only ever runs on a shop with no saved `ui.layout`; anything arranged by hand is
   *  left alone. */
  private autoLayoutInto(target: Map<string, Cell>): void {
    if (!this.topo) return;
    target.clear();
    let top = 0;
    for (const s of systemsOf(this.topo as unknown as ShopDoc)) {
      // Each system gets its own stack of bands, starting one empty cell row below
      // the last one's deepest. That one row IS the boundary — the grounds are drawn
      // to the rows either side of it, so nothing else has to mark it.
      top = this.layoutSystem(s, target, top) + SYSTEM_GAP + 1;
    }
  }

  /** One system's bands, from row `top` down. Returns its deepest row. */
  private layoutSystem(s: ShopSystem, target: Map<string, Cell>, top: number): number {
    const root = s.elements.find(e => e['type'] === 'collector');
    if (!root) return top;
    const TRUNK_COL = 0, BRANCH_COL = 1;
    let cursor = BRANCH_COL, deepest = top;
    const widthOf = (id: string): number => {
      const el = this.elem(id);
      if (isUnitKind(el?.['kind'])) return Math.max(1, (el!['branches'] as unknown[]).length);
      const kids = this.childrenOf(id).filter(k => !this.isUnitChild(k));
      if (!kids.length) return 1;
      return Math.max(1, kids.reduce((s, k) => s + widthOf(k), 0));
    };
    /** Place a subtree. `forceCol` pins the piece to a column its parent chose for it —
     *  what a unit does to whatever hangs on outlet N — instead of taking the next free
     *  one. The cursor is borrowed and put back around a pinned descent so a deep leg
     *  can reuse the columns above it without stealing them from its own siblings. */
    const place = (id: string, row: number, forceCol?: number): void => {
      deepest = Math.max(deepest, row);
      const el = this.elem(id);
      // A pickup is skipped entirely: it has no cell, it sits on its machine's box.
      const kids = this.childrenOf(id).filter(k => this.glyphFor(this.elem(k) ?? {}) !== 'pickup');
      const onOutlet = kids.filter(k => this.isUnitChild(k));
      const runKids = kids.filter(k => !this.isUnitChild(k));
      if (isUnitKind(el?.['kind'])) {
        // A unit's origin is its FIRST outlet, and everything it feeds hangs one row
        // down at the column of the outlet it came off. Recursive, so a manifold on a
        // manifold's last outlet lands under that outlet rather than being skipped.
        const col = forceCol ?? cursor;
        target.set(id, { col, row });
        if (forceCol === undefined) cursor += widthOf(id);
        for (const k of onOutlet) place(k, row + 1, col + (this.outletOf.get(k)?.index ?? 0));
        return;
      }
      if (!runKids.length) {
        target.set(id, { col: forceCol ?? cursor, row });
        if (forceCol === undefined) cursor += 1;
        return;
      }
      if (forceCol !== undefined) {
        const keep = cursor; cursor = forceCol;
        for (const k of runKids) place(k, row + 1);
        cursor = keep;
        target.set(id, { col: forceCol, row });
        return;
      }
      const start = cursor;
      for (const k of runKids) place(k, row + 1);
      // A lone unit child: sit directly above its FIRST outlet (col of the unit) so the
      // run drops straight in. Otherwise centre over the children's span.
      if (runKids.length === 1 && isUnitKind(this.elem(runKids[0])?.['kind'])) {
        target.set(id, { col: target.get(runKids[0])?.col ?? start, row });
      } else {
        target.set(id, { col: Math.floor((start + cursor - 1) / 2), row });
      }
    };
    target.set(root['id'] as string, { col: TRUNK_COL, row: top });
    let band = top + 1;
    let bottom = top;
    for (const leg of this.childrenOf(root['id'] as string)) {
      cursor = BRANCH_COL; deepest = band;
      place(leg, band);
      bottom = deepest;
      band = deepest + 2;   // one blank row between bands
    }
    return bottom;
  }
  private savedLayout(t: Topology): Record<string, Cell> | null { return (t as { ui?: { layout?: Record<string, Cell> } }).ui?.layout ?? null; }
  private savedBoards(t: Topology): Record<string, number | Cell> | null {
    return (t as { ui?: { wiring?: { boards?: Record<string, number | Cell> } } }).ui?.wiring?.boards ?? null;
  }
  /** Restore board placements, dropping any whose board is no longer paired — an
   *  unpaired board's square would otherwise sit there blocking cells for a device
   *  that isn't in the shop any more. */
  private loadBoardCells(t: Topology): void {
    this.boardSlots.clear();
    const saved = this.savedBoards(t);
    // Docs written before the rail stored a {col,row} cell. Their LEFT-TO-RIGHT
    // order is the part that still means something, so read it as the slot order
    // rather than discarding the layout someone arranged.
    if (saved) {
      const known = new Set(this.controllersRaw().map(c => c['id'] as string));
      const rows = Object.entries(saved).filter(([id]) => known.has(id))
        .map(([id, v]) => [id, typeof v === 'number' ? v : v.col] as const)
        .sort((a, b) => a[1] - b[1]);
      rows.forEach(([id], i) => this.boardSlots.set(id, i));
    }
    this.ensureSlots();
  }
  private applyLive(status: TopologyStatus): void {
    // Per-machine draw, for the plug row. Keyed by machine in the device's own
    // status view too — what runs is a machine, not a port.
    this.machineWatts.clear();
    for (const [id, t] of Object.entries(status.tools ?? {})) {
      this.machineWatts.set(id, (t as { watts?: number }).watts ?? 0);
    }
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
