import { Component, EventEmitter, Input, OnChanges, Output, inject } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { ApiService, DiscoveredOutlet } from '../services/api.service';
import { takeoverWarning } from '@plug-claim';

// ── The paired plug, once you have one ───────────────────────────────────────
// One row, two screens: the pairing sheet on the build canvas and the tool-setup
// walk. They asked the same question and answered it differently — setup left the
// whole scan list open forever with a tick beside the choice, the sheet collapsed
// to a name and a "Change" link — which is why neither had anywhere to put a name
// or an unpair. Sharing the component is what stops that happening again.
//
// TWO WRITES, DIFFERENT DESTINATIONS, and keeping them apart is the whole design:
//
//   the NAME goes to the plug. It is the Shelly's own app-visible name, written
//     here on blur, and it is not part of the topology transaction — a device on
//     the far side of the LAN cannot be rolled back by a Cancel. The parent's
//     Save still only ever writes the layout.
//   the UNPAIRING goes to both. Releasing the plug (our suffix off its name, its
//     push target handed back) is best-effort and reported; deleting
//     `sensor.outlet` is the parent's job and happens either way. A plug you have
//     physically unplugged is exactly when you want to detach it.
//
// Ownership (RFC §8) gates the first of those, but it GATES rather than forbids.
// Nothing AUTOMATIC ever writes a plug belonging to Home Assistant or another
// brain — that is the invariant, and it is what makes silent theft impossible.
// A person who has been told what breaks is a different matter, and there are two
// distinct answers they can give, kept apart because their blast radius is not
// remotely the same:
//
//   RENAME ANYWAY writes the label and nothing else. The plug keeps reporting to
//     whoever owns it; no automation over there stops working. It carries no owner
//     suffix, because a plug does not become ours by being relabelled.
//   TAKE THIS OUTLET repoints its push target at us. This is the one that breaks
//     something on a machine the user is not looking at, so it names who goes
//     quiet — plug-claim's takeoverWarning(), shared with the device so the
//     sentence cannot drift — and goes through its own endpoint.
//
// Refusing both outright would only move the same repoint into the Shelly app,
// where there is no record of it at all. That is worse, not safer.

@Component({
  selector: 'app-paired-outlet-row',
  standalone: true,
  imports: [CommonModule, FormsModule],
  styles: [`
    :host { display: block; }
    .paired { background: var(--bg); border: 1px solid var(--border); border-radius: 12px;
              padding: 12px 13px; display: flex; flex-direction: column; gap: 9px; }

    /* A visible field label, not a placeholder. Two name fields sit on the setup
       screen — the tool's and the plug's — and they are the same control at the
       same size; without labels there is nothing to tell you which is which. */
    .fld { font-size: 11px; letter-spacing: 0.07em; text-transform: uppercase;
           color: var(--muted); }

    .namerow { display: flex; align-items: center; gap: 8px; }
    .namerow input { flex: 1; min-width: 0; font-size: 15px; font-weight: 500;
                     background: var(--surface); border: 1px solid var(--border);
                     color: var(--text); border-radius: 8px; padding: 7px 9px;
                     font-family: inherit; }
    .namerow input:disabled { color: var(--muted); background: #191919; }
    .suffix { font-size: 11.5px; color: var(--muted); white-space: nowrap; }

    .meta { font-size: 11.5px; color: var(--muted); font-variant-numeric: tabular-nums; }
    .why  { font-size: 11.5px; line-height: 1.5; color: var(--cable, #38b6f0); }
    .why.warn { color: var(--accent); }
    .said { font-size: 11.5px; line-height: 1.5; color: var(--muted); }
    .said.bad { color: var(--accent); }
    .said.good { color: var(--success); }

    .btns { display: flex; gap: 8px; }
    .btns button { flex: 1; background: var(--surface); border: 1px solid var(--border);
                   color: var(--text); border-radius: 9px; padding: 8px 10px; font-size: 12.5px; }
    .btns button.danger { color: var(--danger, #d94444); border-color: rgba(217,68,68,0.4); }
    .btns button:disabled { opacity: 0.4; }

    .confirm { background: rgba(217,68,68,0.07); border: 1px solid rgba(217,68,68,0.35);
               border-radius: 12px; padding: 13px; display: flex; flex-direction: column; gap: 10px; }
    .confirm h4 { margin: 0; font-size: 14px; font-weight: 600; color: var(--danger, #d94444); }
    .confirm p { font-size: 12.5px; color: var(--muted); line-height: 1.55; margin: 0; }
    .confirm p b { color: var(--text); font-weight: 500; }
    .confirm .acts { display: flex; gap: 8px; }
    .confirm .acts button { flex: 1; border-radius: 9px; padding: 9px; font-size: 13px;
                            border: 1px solid var(--border); background: var(--surface); color: var(--text); }
    .confirm .acts button.go { background: var(--danger, #d94444); border-color: var(--danger, #d94444);
                               color: #fff; font-weight: 600; }
    /* A takeover is not a deletion — amber, the colour this UI already uses for
       "this needs your attention", rather than red for "this destroys something". */
    .confirm.take { background: rgba(240,165,0,0.07); border-color: rgba(240,165,0,0.4); }
    .confirm.take h4 { color: var(--accent); }
    .confirm.take .acts button.go { background: var(--accent); border-color: var(--accent);
                                    color: #241a00; }

    /* Inline, in the sentence that explains the refusal — the way past a rule
       belongs next to the rule, not in a menu somewhere else. */
    .link { background: none; border: none; padding: 0; margin-left: 6px; font: inherit;
            color: var(--accent); text-decoration: underline; cursor: pointer; }
    .link:disabled { opacity: 0.4; }

    .take { background: var(--surface); border: 1px solid rgba(240,165,0,0.4);
            color: var(--accent); border-radius: 9px; padding: 8px 10px; font-size: 12.5px; width: 100%; }
    .take:disabled { opacity: 0.4; }
  `],
  template: `
    <div class="confirm" *ngIf="confirming" role="alertdialog"
         [attr.aria-label]="'Confirm removing the smart outlet from ' + (toolName || 'this tool')">
      <h4>Take this outlet off {{ toolName || 'this tool' }}?</h4>
      <p [innerHTML]="removeConsequence()"></p>
      <div class="acts">
        <button (click)="confirming = false"
                title="Leave the outlet paired — nothing changes">Keep it</button>
        <button class="go" (click)="doRemove()" [disabled]="busy"
                [title]="'Remove this outlet from ' + (toolName || 'this tool') + ' and from the system'">
          {{ busy ? 'Removing…' : 'Remove outlet' }}
        </button>
      </div>
    </div>

    <div class="paired" *ngIf="!confirming && !takeAsking">
      <label class="fld" [attr.for]="fieldId">Outlet name</label>
      <div class="namerow">
        <input [id]="fieldId" [(ngModel)]="draft" (blur)="commit()"
               [disabled]="!canRename() || busy"
               placeholder="Not named yet"
               [title]="canRename()
                  ? 'The name this plug shows in the Shelly app. Saved to the plug itself when you tap away.'
                  : renameBlockedWhy()"/>
        <span class="suffix" *ngIf="showSuffix()"
              title="DustGate adds its own name so the plug says who is using it in the Shelly app. You don't type this part.">·&nbsp;{{ owner }}</span>
      </div>

      <div class="meta">{{ metaLine() }}</div>

      <div class="why" [class.warn]="!reachable" *ngIf="!canRename()">
        {{ renameBlockedWhy() }}
        <!-- A refusal with no way past it is where this row was stuck. Both of
             these are visible text, not hidden behind the tooltip. -->
        <button class="link" *ngIf="ownedByOther() && reachable" (click)="overrideName()"
                [disabled]="busy"
                title="Write the name to this outlet anyway. Only the name — it keeps reporting to its current owner, so nothing over there stops working.">
          Rename it anyway
        </button>
      </div>
      <div class="said" [class.bad]="saidBad" [class.good]="!saidBad" *ngIf="said">{{ said }}</div>

      <div class="btns">
        <button (click)="change.emit()" [disabled]="busy"
                [title]="'Pick a different smart outlet for ' + (toolName || 'this tool') + '. The current one is freed for something else.'">
          Change outlet
        </button>
        <button class="danger" (click)="confirming = true; said = ''" [disabled]="busy"
                [title]="'Remove this outlet from ' + (toolName || 'this tool') + ' and from the system'">
          Remove
        </button>
      </div>

      <!-- The loud one, and its own row so it never sits shoulder to shoulder
           with the ordinary verbs. Only when someone else actually has it. -->
      <button class="take" *ngIf="canTakeover()" (click)="takeAsking = true; said = ''"
              [disabled]="busy"
              [title]="'Point this outlet at DustGate instead of ' + (seen?.holder || 'its current owner') + '. Breaks what they have built on it.'">
          Take this outlet from {{ seen?.holder || 'its owner' }}
        </button>
    </div>

    <!-- Takeover confirm. Names WHO goes quiet, from the shared claim model —
         never "are you sure?", which asks nothing. -->
    <div class="confirm take" *ngIf="takeAsking" role="alertdialog"
         aria-label="Confirm taking this outlet from its current owner">
      <h4>Take this outlet from {{ seen?.holder || 'its current owner' }}?</h4>
      <p>{{ takeoverText() }}</p>
      <p><b>You can hand it back.</b> Removing the outlet later restores the target it
         reports to now, so this is reversible from here.</p>
      <div class="acts">
        <button (click)="takeAsking = false" title="Leave it where it is — nothing changes">Leave it</button>
        <button class="go" (click)="doTakeover()" [disabled]="busy"
                title="Repoint this outlet at DustGate now">
          {{ busy ? 'Taking…' : 'Take it' }}
        </button>
      </div>
    </div>

  `,
})
export class PairedOutletRowComponent implements OnChanges {
  /** The machine (or the collector) this plug is on — used in every sentence. */
  @Input() toolName = '';
  @Input({ required: true }) ip = '';
  @Input() host = '';
  /** `sensor.outlet.name` — the plug's name as cached in the layout. A DISPLAY
   *  FALLBACK, not the source of truth: the plug itself holds the real name and
   *  the live scan overrides this, so renaming in the Shelly app still wins. It
   *  exists so a plug that is switched off, or paired before the last sweep,
   *  still reads as a name instead of an IP. */
  @Input() label = '';
  /** The plug as the last scan saw it, if it was in one. Absent means the scan
   *  hasn't run or the plug didn't answer — which is not the same as "offline",
   *  and the row shouldn't claim it is. */
  @Input() seen: DiscoveredOutlet | null = null;
  /** Our mDNS name, from GET /api/info — the owner suffix we stamp on our plugs. */
  @Input() owner = '';
  /** The collector's plug is commanded, never sensed. Only changes the wording. */
  @Input() isSwitch = false;
  /** Distinguishes the two name fields on the setup screen. */
  @Input() fieldId = 'outlet-name';

  /** The label as it now stands, after a successful write to the plug. The parent
   *  caches it on `sensor.outlet.label` when it next writes the layout. */
  @Output() renamed = new EventEmitter<string>();
  /** Re-open the picker. */
  @Output() change = new EventEmitter<void>();
  /** A takeover landed, so the parent's cached scan is stale. */
  @Output() rescan = new EventEmitter<void>();
  /** Unpaired — the parent deletes `sensor.outlet`. Fired whether or not the
   *  device half succeeded, carrying what actually happened to the plug. The
   *  parent has to surface that: this row is usually destroyed by the unpair, so
   *  a message shown here would vanish before it could be read. Empty string
   *  means a clean release with nothing to report. */
  @Output() removed = new EventEmitter<string>();

  private readonly api = inject(ApiService);

  draft = '';
  confirming = false;
  /** The takeover confirm is open. Separate from `confirming` so the two can
   *  never both be up, and so neither can be mistaken for the other. */
  takeAsking = false;
  busy = false;
  said = '';
  saidBad = false;
  /** The user said "rename it anyway" for a plug someone else owns. Per-row and
   *  not persisted — it expires when the sheet closes, which is the right
   *  lifetime for an override of a safety rule. */
  overridden = false;

  ngOnChanges(): void {
    // Only reseed while the user isn't mid-edit, so a background scan landing
    // can't yank the text out from under a half-typed name.
    if (!this.busy && document.activeElement?.id !== this.fieldId) {
      this.draft = this.displayName();
    }
  }

  /** Live name beats the cached one — a rename done in the Shelly app should win. */
  displayName(): string {
    return this.seen?.name || this.label || '';
  }

  get reachable(): boolean { return this.seen ? this.seen.reachable : true; }

  /** Ownership decides this, not reachability alone: we never write a plug that
   *  belongs to someone else, and a claim we couldn't read is also a no. */
  canRename(): boolean {
    if (!this.reachable) return false;   // no override for a plug that isn't there
    if (this.overridden) return true;    // a human answered the question below
    const c = this.seen?.claim;
    if (!c) return true;            // no scan data — let the device refuse if it must
    return c === 'ours' || c === 'unclaimed';
  }

  /** Only stamp the suffix on a plug that is actually ours. An unclaimed plug
   *  renamed before pairing gets the bare label — claiming it would be a lie. */
  showSuffix(): boolean {
    return !!this.owner && this.canRename() && this.seen?.claim === 'ours';
  }

  /** Somebody else has it — Home Assistant, another brain. Distinct from "not
   *  answering", which blocks a write for a completely different reason and has
   *  no override worth offering. */
  ownedByOther(): boolean {
    return this.seen?.claim === 'foreign' || this.seen?.claim === 'dustgate';
  }

  /** Offer the repoint only when the device says it is takeable. A plug we could
   *  not read the ownership of is NOT takeable: we cannot show the user whose it
   *  is, and confirming a question nobody could pose is not consent. */
  canTakeover(): boolean {
    return !!this.seen?.takeable && this.reachable;
  }

  /** The sentence naming who goes quiet, straight from the shared claim model so
   *  the UI and the device cannot end up saying different things about the same
   *  act. See plug-claim.js — never "are you sure?", which asks nothing. */
  takeoverText(): string {
    const s = this.seen;
    const w = s && takeoverWarning({
      state: (s.claim ?? 'foreign') as 'foreign' | 'dustgate' | 'ours' | 'unclaimed',
      owner: null, holder: s.holder ?? null, takeable: !!s.takeable,
    });
    return w ?? 'Its current owner will stop receiving updates from this outlet.';
  }

  /** "Rename it anyway" — the NAME override for a plug someone else owns. Runs
   *  the ordinary rename path with the override flag, so there is one code path
   *  and one place the result is reported. */
  async overrideName(): Promise<void> {
    // Nothing typed yet: unblock the field and let them type, rather than writing
    // whatever happened to be cached. The override is a permission, not an action.
    if (this.draft.trim() === this.displayName()) {
      this.overridden = true;
      this.said = `Naming unblocked. This writes the name only — ${this.seen?.holder || 'its owner'} keeps receiving its updates.`;
      this.saidBad = false;
      return;
    }
    this.overridden = true;
    await this.commit();
  }

  async doTakeover(): Promise<void> {
    this.busy = true;
    try {
      const r = await this.api.takeoverOutlet(this.ip);
      if (r.ok) {
        // The device arms a one-shot approval and repoints on its next
        // provisioning pass, so what the last scan says is now out of date.
        // Reflect it locally and tell the caller to rescan.
        if (this.seen) {
          this.seen.claim = 'ours';
          this.seen.takeable = false;
          this.seen.holder = undefined;
        }
        this.said = 'Taken. This outlet reports to DustGate now — you can rename it.';
        this.saidBad = false;
        this.rescan.emit();
      } else {
        this.said = r.error ? `Couldn't take it: ${r.error}.` : 'Couldn\'t take it.';
        this.saidBad = true;
      }
    } finally {
      this.busy = false;
      this.takeAsking = false;
    }
  }

  renameBlockedWhy(): string {
    if (!this.reachable) return 'This outlet isn\'t answering, so its name can\'t be changed until it\'s back.';
    const holder = this.seen?.holder;
    if (this.seen?.claim === 'foreign') {
      return `${holder || 'Something else on the network'} owns this outlet. DustGate reads it and never writes to it.`;
    }
    if (this.seen?.claim === 'dustgate') {
      return `${holder || 'Another DustGate'} owns this outlet. DustGate reads it and never writes to it.`;
    }
    return 'This outlet can\'t be renamed from here.';
  }

  metaLine(): string {
    const bits = [this.host || this.seen?.hostname, this.ip].filter(Boolean);
    if (!this.seen) return bits.join(' · ');
    if (!this.seen.reachable) return `${bits.join(' · ')} · not responding`;
    const w = Math.round(this.seen.powerW);
    return `${bits.join(' · ')} · ${w} W`;
  }

  /** What unpairing will actually do, which differs by who owns the plug — the
   *  reason this is an inline confirm and not a browser one. */
  removeConsequence(): string {
    const who = this.toolName || 'This tool';
    const manual = this.isSwitch
      ? `You'll start ${who} yourself. Gates still route to whatever tool is running.`
      : `${who} goes manual — you'll switch it on yourself, and gates still route to whatever is running.`;
    if (this.seen?.claim === 'foreign' || this.seen?.claim === 'dustgate') {
      return `${manual} This outlet belongs to ${this.seen.holder || 'something else on the network'}, ` +
             'so nothing is written to it and nothing there changes.';
    }
    if (!this.reachable) {
      return `${manual} The outlet isn't answering, so it <b>keeps DustGate's name and keeps reporting to us</b> ` +
             'until it\'s back on the network — removing it here just stops DustGate watching it.';
    }
    if (!this.seen) {
      // No scan data. We don't know whether the plug will answer, so promise the
      // ATTEMPT rather than the outcome — and report the outcome afterwards.
      return `${manual} DustGate will try to take its name off the outlet and stop it reporting here. ` +
             'If the outlet doesn\'t answer, it keeps both until it\'s back — you\'ll be told either way.';
    }
    return `${manual} DustGate will also take its name off the outlet and stop it reporting here, ` +
           'so it\'s free for anything else on the network.';
  }

  /** Rename, on blur. The plug is a separate device from the layout and can't be
   *  part of its transaction, so this lands when you tap away and says so. */
  async commit(): Promise<void> {
    const v = this.draft.trim();
    if (!this.canRename() || v === this.displayName()) return;
    this.busy = true; this.said = ''; this.saidBad = false;
    try {
      const r = await this.api.renameOutlet(this.ip, v, this.overridden && this.ownedByOther());
      if (r.ok) {
        this.said = 'Saved to the outlet.';
        this.saidBad = false;
        // The LABEL, not r.name. r.name is what physically landed on the plug,
        // suffix and all; the scan reports names with the suffix stripped, so
        // caching the full one here would show a suffix that vanishes on the next
        // sweep — the display disagreeing with itself between refreshes.
        if (this.seen) this.seen.name = v;
        this.renamed.emit(v);
      } else {
        this.said = r.error ? `Couldn't rename it: ${r.error}.` : 'Couldn\'t rename it.';
        this.saidBad = true;
        this.draft = this.displayName();   // put back what is actually true
      }
    } finally {
      this.busy = false;
    }
  }

  /** Release the plug, then hand the layout half to the parent — which happens
   *  whether or not the release worked. */
  async doRemove(): Promise<void> {
    this.busy = true;
    let note = '';
    try {
      const r = await this.api.releaseOutlet(this.ip);
      if (!r.ok && r.error) {
        note = `Removed from ${this.toolName || 'this tool'}, but the outlet kept DustGate's ` +
               `name and is still reporting here (${r.error}). It'll be freed when it's back.`;
      } else if (r.released && r.restored) {
        note = `Removed from ${this.toolName || 'this tool'}. Its previous controller has it back.`;
      }
    } finally {
      this.busy = false;
      this.confirming = false;
    }
    this.removed.emit(note);
  }
}
