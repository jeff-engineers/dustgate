import { Component, EventEmitter, Input, OnInit, Output, inject } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import type { Topology } from '@topology';
import { ApiService, NodeLinkState } from '../services/api.service';
import { ServoCalibrationComponent } from './servo-calibration.component';
import { LinearCalibrationComponent } from './linear-calibration.component';
import { systemsOf } from '@shop';
import type { Shop, System } from '@shop';
import {
  AnyElement, ConfigurableSelector, Controller, LinearSelector, ServoSelector,
  configurableSelectorsOf, controllersOf, isCalibrated, isConfigurableSelector,
  isServoKind, kindLabel, servoSelectorsOf,
} from './selector-types';
import {
  type Drives, applyDrivesCache, canHost, drivesFromCaps, drivesFromHasLinear, resolveDrives,
} from '../boards/board-drives';

// ── Configuring one ball valve or manifold ───────────────────────────────────
// Two facts the graph can't infer — what it's called, and which board + PWM channel
// drives it — then the calibration widget. Nothing else: motion settings are either
// unread by firmware (moveMs) or build-wide (holdAtRest), and outlet roles are already
// implied by whatever the canvas connected downstream.
//
// Shared by both entry points: the build canvas inspector and the /gates setup pass.
//
// TWO PANES, one component. They were one screen, and the wiring fields had no way
// out: `saved` was emitted only by the calibration widget, so changing a gate's board
// and closing dropped the change on the floor — the edit reached `working`, and
// nothing ever emitted it. Splitting them gives the wiring its own Save.
//
// Still one component because both panes need the same paired-board merge and the
// same link state; two would either duplicate that or need a third to hold it.

/** Mirrors SERVO_COUNT in config.h / MAX_SERVOS_PER_HOST in topology.js.
 *
 *  A PWM board's ports, and ONLY a PWM board's. A slider board has one port and
 *  no channels at all — the serial bus and PWM channel 1 are the same pads, so a
 *  board is flashed as one or the other and never offers both. Reading this as
 *  every board's capacity is what had a one-port slider node advertising four
 *  free servo channels (2026-09-08). See boards/board-drives.ts. */
const SERVO_CHANNELS = [0, 1, 2, 3];

@Component({
  selector: 'app-selector-config',
  standalone: true,
  imports: [CommonModule, FormsModule, ServoCalibrationComponent, LinearCalibrationComponent],
  styles: [`
    :host { display: block; }
    .card { background: var(--surface); border: 1px solid var(--border); border-radius: 16px;
            padding: 18px 16px; margin-bottom: 12px; }
    .head { display: flex; align-items: center; justify-content: space-between; margin-bottom: 15px; }
    .head .kind { font-size: 12.5px; color: var(--muted); }
    .badge { font-size: 11.5px; padding: 3px 10px; border-radius: 20px; }
    .badge.ok { color: var(--success); background: rgba(60,190,110,0.12); }
    .badge.todo { color: var(--accent); background: rgba(240,165,0,0.12); }

    label { display: block; font-size: 12.5px; color: var(--muted); margin-bottom: 7px; }
    input, select { background: var(--bg); border: 1px solid var(--border); color: var(--text);
                    border-radius: 8px; padding: 9px 10px; font-size: 14px; width: 100%; }
    .field { margin-bottom: 15px; }
    .field:last-child { margin-bottom: 0; }
    .two { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
    .why { font-size: 11.5px; color: var(--muted); margin-top: 6px; line-height: 1.5; }
    .why.offline { color: var(--danger); }

    .actions { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 18px; }
    button { border-radius: 8px; padding: 10px 12px; font-size: 14px; border: 1px solid var(--border);
             background: var(--bg); color: var(--text); }
    button.primary { background: var(--accent); border-color: var(--accent); color: #241a00; font-weight: 600; }
    button[disabled] { opacity: 0.45; }
  `],
  template: `
    <div class="card" *ngIf="pane === 'board'">
      <div class="head">
        <span class="kind">{{ kindName }}</span>
        <span class="badge" [class.ok]="calibrated" [class.todo]="!calibrated">
          {{ calibrated ? 'Configured' : 'Needs setup' }}
        </span>
      </div>

      <div class="field">
        <label for="gate-name">Name</label>
        <input id="gate-name" [(ngModel)]="name" (ngModelChange)="touch()"/>
      </div>

      <div class="field">
        <label>Driven by</label>
        <!-- Two columns only when there IS a second control. A slider board has no
             channel to pick, and the grid left the board picker in half a row with
             dead space beside it. -->
        <div [class.two]="isServo && selectedIsServoBoard">
          <!-- A board this gate cannot run on is SHOWN and disabled, never dropped:
               the same rule the gates list uses for a sleeping board. Hiding it
               makes a board you paired look like it never arrived, and the fix for
               "gone" is nothing like the fix for "wrong kind of board". -->
          <select [(ngModel)]="controllerId" (ngModelChange)="touch()">
            <option *ngFor="let c of controllers" [value]="c.id" [disabled]="!canDrive(c)">
              {{ boardLabel(c) }}
            </option>
          </select>
          <!-- Only a PWM board has channels to choose between. On a slider board the
               one port IS the answer, so there is nothing left to pick. -->
          <select *ngIf="isServo && selectedIsServoBoard" [(ngModel)]="channel" (ngModelChange)="touch()">
            <option *ngFor="let ch of channels" [ngValue]="ch" [disabled]="!!takenBy(ch)">
              {{ channelLabel(ch) }}
            </option>
          </select>
        </div>
        <p class="why" *ngIf="!boardOffline && !wrongKindOfBoard && !noBoardCanDrive">
          {{ isServo
             ? 'Each PWM board drives up to four servo gates, one per channel. A slider board drives none.'
             : 'A sliding gate needs a board flashed for the serial bus, and that board drives one rack and nothing else.' }}
        </p>
        <!-- The state the picker used to allow silently, and the one in the report
             that started this: a manifold sitting on a slider node. The device
             refuses that document, so say it here rather than at save time. -->
        <p class="why offline" *ngIf="wrongKindOfBoard">
          {{ boardName }} is {{ isServo ? 'a sliding-gate board — it drives one rack over a serial bus, not servos'
                                        : 'a servo board — it drives the PWM bank, not a rack' }}.
          Pick another: this gate can't run on it.
        </p>
        <p class="why offline" *ngIf="noBoardCanDrive">
          No board in this shop can drive {{ isServo ? 'a servo gate' : 'a sliding gate' }}.
          Pair {{ isServo ? 'a PWM board' : 'a board flashed for the slider' }} from Boards and come back.
        </p>
        <p class="why offline" *ngIf="boardOffline">
          {{ boardName }} isn't answering right now. You can still pick it — travel
          limits are the part that needs the board awake.
        </p>
        <p class="why" *ngIf="noFreeChannel">
          All four servo channels on {{ boardName }} are taken — pick another board,
          or free a channel by moving one of its gates.
        </p>
      </div>

      <!-- Its own Save. This pane used to have none: the only emitter was the
           calibration widget, so a board change made here left with the sheet. -->
      <div class="actions">
        <button type="button" (click)="cancelled.emit()">Cancel</button>
        <button type="button" class="primary" (click)="saveBoard()">Save</button>
      </div>
    </div>

    <ng-container *ngIf="pane === 'travel'">
      <app-servo-calibration *ngIf="isServo" [sel]="asServo(working)" [topo]="topo"
                             (saved)="onCalibrated($event)" (cancelled)="cancelled.emit()">
      </app-servo-calibration>
      <app-linear-calibration *ngIf="!isServo" [sel]="asLinear(working)" [topo]="topo"
                              (saved)="onCalibrated($event)" (cancelled)="cancelled.emit()">
      </app-linear-calibration>
    </ng-container>
  `,
})
export class SelectorConfigComponent implements OnInit {
  @Input({ required: true }) sel!: ConfigurableSelector;
  @Input({ required: true }) topo!: Topology;
  /** Which half to show: the wiring, or the travel limits. */
  @Input() pane: 'board' | 'travel' = 'board';
  /** Emits the edited selector. From either pane — the one you didn't open is
   *  carried through untouched, since `working` starts as a full copy. */
  @Output() saved = new EventEmitter<ConfigurableSelector>();
  @Output() cancelled = new EventEmitter<void>();

  private readonly api = inject(ApiService);

  name = '';
  controllerId = '';
  channel = 0;
  controllers: Controller[] = [];
  readonly channels = SERVO_CHANNELS;

  /** Live link state, so the picker can say a board isn't answering rather than
   *  letting the user calibrate against a jog control that silently does nothing. */
  private links: NodeLinkState[] = [];

  /** The selector the calibration widget edits. ONE stable object, mutated in place by
   *  the fields above — handing the child a fresh copy each change-detection pass would
   *  churn its @Input for no reason. */
  working!: ConfigurableSelector;

  ngOnInit(): void {
    this.name = this.sel.name ?? '';
    this.controllerId = this.sel.controllerId;
    this.channel = isServoKind(this.sel) ? (this.sel.servo?.channel ?? 0) : 0;
    this.controllers = this.orderBoards(controllersOf(this.topo));
    this.working = isServoKind(this.sel)
      ? { ...this.sel, servo: { ...this.sel.servo } }
      : { ...this.sel, linear: { ...(this.sel as LinearSelector).linear } };

    // Paired boards are the SOURCE OF TRUTH for what this picker offers, not the
    // layout's controllers[]. Pairing lives on the device and can happen before a
    // layout exists at all, so a board paired from /boards would otherwise be
    // invisible here until something happened to write it into the document —
    // which was exactly the symptom: visible under Boards, missing from "Driven by".
    //
    // Best-effort: an older firmware without /api/nodes falls back to whatever
    // the layout already names, and simply can't warn about reachability.
    void this.api.getNodes()
      .then((n) => { this.links = n; this.mergePairedBoards(); })
      .catch(() => { /* no link info */ });
  }

  get calibrated(): boolean { return isCalibrated(this.sel); }
  get isServo(): boolean { return isServoKind(this.sel); }
  get kindName(): string { return kindLabel(this.sel); }

  // The template needs the concrete type for each child's @Input; `isServo` already
  // decided which one renders.
  asServo(s: ConfigurableSelector): ServoSelector { return s as ServoSelector; }
  asLinear(s: ConfigurableSelector): LinearSelector { return s as LinearSelector; }

  /**
   * Fold every paired board into the options, adding a controllers[] entry for any
   * the layout doesn't know about yet.
   *
   * Written into the topology rather than held as a display-only extra: the schema
   * requires a controllers entry behind each gate's controllerId, so a selection
   * offered but not backed would fail validation at save time — a mysterious
   * rejection in place of a missing option. The parent persists this same document,
   * so the entry lands with the gate that needed it.
   */
  private mergePairedBoards(): void {
    const controllers = controllersOf(this.topo);
    for (const l of this.links) {
      if (controllers.some((c) => c.id === l.id)) continue;
      const entry: Controller = {
        id: l.id,                       // the node's mDNS host IS its controllerId
        role: 'secondary',
        name: l.name || l.host || l.id,
        board: l.board,
        link: { transport: 'wifi-ws', host: l.host },
      };
      // ...INCLUDING what it drives. This merge wrote no `drives` at all until
      // 2026-09-08, which is B4 in board-drives.spec.ts happening a second time:
      // the canvas's own merge was fixed, this one was missed. The canvas still
      // DREW a paired slider correctly, because it reads the live report — but the
      // document this sheet saved said nothing, and the validator's
      // `c.drives || 'servo'` default turned a slider node into a servo board the
      // device then refused.
      applyDrivesCache(entry as unknown as Record<string, unknown>, drivesFromCaps(l.caps));
      controllers.push(entry);
    }
    this.controllers = this.orderBoards(controllersOf(this.topo));
  }

  /** Name, role, and — the part that was missing — what is still FREE on it.
   *
   *  The channel picker beside this one already said "free" per channel, but only
   *  once you had committed to a board; choosing between boards meant selecting each
   *  in turn to find out which had room.
   *
   *  ASKS THE BOARD, not the gate. This used to branch on the gate's kind, so a
   *  servo gate was told every board had "n of 4 free" — including a one-port
   *  slider node, which advertised four channels it does not have (2026-09-08). A
   *  board's capacity is a fact about the board; whether this gate can use it is
   *  the separate question canDrive() answers, and the label says which when the
   *  answer is no rather than quoting a count nobody can spend. */
  boardLabel(c: Controller): string {
    const name = `${c.name || c.id}${c.role === 'primary' ? ' (primary)' : ''}`;
    if (!this.canDrive(c)) {
      return `${name} — ${this.drivesOf(c) === 'linear' ? 'sliding gates only' : 'servo gates only'}`;
    }
    if (this.drivesOf(c) === 'linear') {
      return `${name} — ${this.sliderTaken(c) ? 'its one gate is taken' : 'its one gate is free'}`;
    }
    const free = this.freeChannels(c);
    return `${name} — ${free ? `${free} of ${this.channels.length} free` : 'no free channel'}`;
  }

  /** What a board is FLASHED to drive: live report first, saved cache second,
   *  'servo' last. Same order and same reason as the canvas and the Boards list —
   *  see boards/board-drives.ts, which exists because those two once disagreed. */
  private drivesOf(c: Controller): Drives {
    return resolveDrives(this.reportedDrives(c), c.drives);
  }

  /** What a board SAYS it drives, or null if it has not said. The primary reports
   *  `hasLinear` in its status; a node reports `caps.linear` in its WELCOME. */
  private reportedDrives(c: Controller): Drives | null {
    if (c.role === 'primary') return drivesFromHasLinear(this.api.status$.value?.hasLinear);
    return drivesFromCaps(this.links.find((l) => l.id === c.id)?.caps);
  }

  /** Can this board run THIS gate at all? An equality test, not a capacity one:
   *  the PWM bank and the serial bus are the same pads, so neither substitutes for
   *  the other and a board is flashed as one or the other. */
  canDrive(c: Controller): boolean { return canHost(this.drivesOf(c), this.sel.kind); }

  /** Free servo channels on a board, NOT counting the gate being edited: the question
   *  is "if I move it here, is there room", and a gate never competes with itself.
   *  Without that, the board a gate is already on always reads one short. */
  private freeChannels(c: Controller): number {
    const taken = new Set(
      servoSelectorsOf(this.topo)
        .filter((s) => s.controllerId === c.id && s.id !== this.sel.id)
        .map((s) => s.servo?.channel)
        .filter((ch): ch is number => typeof ch === 'number'),
    );
    return this.channels.filter((ch) => !taken.has(ch)).length;
  }

  /** Is this board's ONE slider port already spoken for by another sliding gate?
   *
   *  Called the stepper until 2026-09-08, which it has not been since the TMC2209
   *  went to the attic in 2026-08-28 — the port drives an ST3215 over a serial bus
   *  now, and the canvas's own port label was corrected at the time. */
  private sliderTaken(c: Controller): boolean {
    return configurableSelectorsOf(this.topo)
      .some((s) => s.controllerId === c.id && s.id !== this.sel.id && !isServoKind(s));
  }

  /** Boards already driving something in THIS system first, the rest after.
   *
   *  A board is mounted where the cable reaches, so the one already wired into this
   *  system's other gates is nearly always the answer — and in a two-collector shop
   *  the list is otherwise shop-wide with no hint which half you are in. Stable
   *  within each group, so the primary keeps its place among its peers. */
  private orderBoards(list: Controller[]): Controller[] {
    const here = this.boardsInThisSystem();
    return [...list].sort((a, b) => Number(here.has(b.id)) - Number(here.has(a.id)));
  }

  /** Controllers driving the other gates in the system this gate belongs to.
   *
   *  Goes through the shop's systems[] rather than the flattening readers in
   *  selector-types: this is the one question here that is about WHICH system, which
   *  is exactly the seam that file says to leave alone. A v1 document has no
   *  systems[] and answers with an empty set, which just leaves the order as it was. */
  private boardsInThisSystem(): Set<string> {
    const shop = this.topo as unknown as Shop;
    const mine = systemsOf(shop).find(
      (sys: System) => (sys.elements as AnyElement[] ?? []).some((e) => e.id === this.sel.id),
    );
    const out = new Set<string>();
    for (const e of (mine?.elements as AnyElement[]) ?? []) {
      if (isConfigurableSelector(e) && e.id !== this.sel.id && e.controllerId) out.add(e.controllerId);
    }
    return out;
  }

  /** Which OTHER gate already holds this channel on the selected board — the schema
   *  rejects a collision, so show it here rather than at save time. */
  takenBy(ch: number): string {
    const clash = servoSelectorsOf(this.topo).find(
      (s) => s.id !== this.sel.id && s.controllerId === this.controllerId && s.servo?.channel === ch,
    );
    return clash ? (clash.name || clash.id) : '';
  }

  channelLabel(ch: number): string {
    const taken = this.takenBy(ch);
    return `Servo ${ch + 1} — ${taken || 'free'}`;
  }

  get boardName(): string {
    const c = this.controllers.find((x) => x.id === this.controllerId);
    return c?.name || this.controllerId || 'that board';
  }

  /** Is the selected board a secondary that isn't currently answering? The
   *  primary is always "reachable" — it's the board serving this page. */
  get boardOffline(): boolean {
    const link = this.links.find((l) => l.id === this.controllerId);
    return !!link && !link.online;
  }

  /** Every channel on the selected board is spoken for by another gate. Asked only
   *  of a board that HAS channels — on a slider board the answer is meaningless. */
  get noFreeChannel(): boolean {
    return this.isServo && this.selectedIsServoBoard && this.channels.every((ch) => !!this.takenBy(ch));
  }

  /** The selected board, or undefined while the list is still loading. */
  private get selected(): Controller | undefined {
    return this.controllers.find((c) => c.id === this.controllerId);
  }

  /** Does the selected board have a PWM bank to pick a channel out of? */
  get selectedIsServoBoard(): boolean {
    const c = this.selected;
    return !c || this.drivesOf(c) === 'servo';
  }

  /** The gate is sitting on a board that cannot drive it.
   *
   *  Not reachable by picking any more — those options are disabled — but very
   *  reachable by ARRIVING: a layout saved before this check, or a gate whose board
   *  was later re-flashed the other way. The device refuses that document, so the
   *  sheet says so while there is still something to change. */
  get wrongKindOfBoard(): boolean {
    const c = this.selected;
    return !!c && !this.canDrive(c);
  }

  /** Nothing in the shop can drive this kind of gate — every option is disabled and
   *  the picker has no answer to offer. Says to pair a board rather than leaving a
   *  dropdown that refuses everything with no reason given. */
  get noBoardCanDrive(): boolean {
    return this.controllers.length > 0 && !this.controllers.some((c) => this.canDrive(c));
  }

  /** Name/wiring edits ride along with the calibration result, so push them into the
   *  object the widget is holding — a channel change has to reach the jog calls. */
  touch(): void {
    // Moving a gate to a different board can land it on a channel that board has
    // already given away. Slide to a free one rather than leaving a selection the
    // schema will reject at save time (and that the greyed-out <option> only
    // documents for channels the user hasn't picked yet).
    if (this.isServo && this.takenBy(this.channel)) {
      const free = this.channels.find((ch) => !this.takenBy(ch));
      if (free !== undefined) this.channel = free;
    }
    this.working.name = this.name;
    this.working.controllerId = this.controllerId;
    if (isServoKind(this.working)) this.working.servo.channel = this.channel;
  }

  /** Save the wiring on its own.
   *
   *  Emits `working`, not a fresh object: it is a full copy of the gate, so whatever
   *  calibration the gate already had rides through untouched. Building the payload
   *  from the three fields on this pane would silently decalibrate a configured gate
   *  every time someone moved it to another board. */
  saveBoard(): void {
    this.touch();
    this.saved.emit({ ...this.working });
  }

  /** The calibration widget hands back its own copy, so re-apply the fields edited up
   *  here — otherwise a rename made mid-calibration would be lost. */
  onCalibrated(calibrated: ConfigurableSelector): void {
    this.saved.emit({ ...calibrated, name: this.name, controllerId: this.controllerId });
  }
}
