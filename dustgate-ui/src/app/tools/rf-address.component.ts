import { Component, EventEmitter, Input, OnInit, Output } from '@angular/core';
import { CommonModule } from '@angular/common';

// ── Matching the collector's remote ──────────────────────────────────────────
//
// DustGate presses the dust collector's remote by transmitting its frame, which
// needs the remote's ADDRESS — eight bits set by a DIP switch inside the fob.
// There is no receiver in the product (see docs/mockups/rf-address.html for why
// "learn mode" was rejected), so the user reads the switch and copies it.
//
// THE WHOLE SCREEN IS ONE IDEA: don't ask a woodworker what an address is. Draw
// the part they are holding and let them match it. The number underneath is for
// a support call, not for them — which is also why the collector sheet's summary
// row shows the picture and not the number.
//
// THE BIT MAPPING IS THE ONE THING THAT CAN BE SILENTLY WRONG, so it is stated
// once, here, and asserted by rf-address.spec.ts:
//
//   nub UP   = at the ON legend = CLOSED = grounded = logic 0
//   nub DOWN = OPEN                                 = logic 1, worth (1 << i)
//
// Getting it backwards produces a valid address for the wrong remote, and the
// document stays perfectly valid — so nothing catches it until a tool starts and
// the blower doesn't.
//
// NO TEST BUTTON, and it is a gap rather than a decision. docs/mockups/
// rf-address.html specifies one ("test before Save, and test with a lamp") and
// it is the right design. It needs a device endpoint that transmits an address
// it has NOT been given in a layout — POST /api/collector only presses whatever
// the stored document says — so a Test here could do nothing but save first and
// press, which is not a test of the thing you are about to commit. Build the
// endpoint, then build the button.

/** The Rockler fob's own setting — rockers 1, 6 and 8 closed = 0b01011110 = 94.
 *  The firmware's `RfCollectorPresser::kRocklerAddress` default, so a sheet that
 *  has never been opened and this screen agree. */
export const ROCKLER_ADDRESS = 94;

@Component({
  selector: 'app-rf-address',
  standalone: true,
  imports: [CommonModule],
  styles: [`
    :host { display: block; }
    .card { background: var(--surface); border: 1px solid var(--border);
            border-radius: 16px; padding: 18px 16px; }
    .prompt { font-size: 13.5px; color: var(--muted); line-height: 1.55; margin: 0 0 15px; }
    .prompt b { color: var(--text); font-weight: 500; }

    /* Drawn as the physical part, not as eight checkboxes. Anything that does not
       LOOK like the switch makes the user translate, and translation is where it
       goes wrong. */
    .dip { background: var(--bg); border: 1px solid var(--border); border-radius: 12px;
           padding: 14px 12px 12px; }
    .dip-on { font-size: 9.5px; letter-spacing: .14em; color: #7a1e13; font-weight: 700;
              text-align: center; margin-bottom: 4px; text-transform: uppercase; }
    .dip-body { background: #d8452f; border-radius: 4px; padding: 9px 8px 7px;
                display: flex; gap: 5px; justify-content: center;
                box-shadow: inset 0 -2px 0 rgba(0,0,0,.25); }
    .rocker { width: 26px; flex: none; text-align: center; background: none;
              border: 0; padding: 0; font: inherit; cursor: pointer; }
    .slot { background: #2a2b2d; border-radius: 3px; height: 46px; position: relative;
            box-shadow: inset 0 1px 3px rgba(0,0,0,.6); }
    .nub { position: absolute; left: 2px; right: 2px; height: 20px; border-radius: 2px;
           background: #f2f2ef; box-shadow: 0 1px 2px rgba(0,0,0,.5); top: 24px; }
    .slot.up .nub { top: 2px; }
    .rocker .n { font-size: 10.5px; color: #7a1e13; font-weight: 700; margin-top: 3px; }
    .rocker:focus-visible .slot { outline: 2px solid var(--accent); outline-offset: 2px; }

    .readout { display: flex; align-items: baseline; justify-content: space-between;
               margin-top: 13px; padding-top: 12px; border-top: 1px solid var(--border); }
    .readout .lbl { font-size: 12.5px; color: var(--muted); }
    .readout .val { font-family: ui-monospace, Menlo, monospace; font-size: 19px;
                    font-weight: 600; color: var(--accent); }
    .readout .bin { font-family: ui-monospace, Menlo, monospace; font-size: 11.5px; color: var(--muted); }

    .where { background: var(--bg); border: 1px solid var(--border); border-radius: 11px;
             padding: 11px 12px; font-size: 12.5px; color: var(--muted);
             line-height: 1.55; margin-top: 13px; }
    .where b { color: var(--text); font-weight: 500; }

    .nav { display: flex; gap: 10px; margin-top: 16px; }
    .nav button { border-radius: var(--radius); padding: 12px 16px; font-size: 14px; }
    .nav .back { flex: 0 0 auto; background: var(--surface); border: 1px solid var(--border); color: var(--text); }
    .nav .save { flex: 1; background: var(--accent); border: none; color: #1a1200; font-weight: 600; }
    .nav button:disabled { opacity: .55; }
  `],
  template: `
    <div class="card">
      <p class="prompt">
        Open the battery cover on your collector's remote. Inside is a row of
        <b>eight small switches</b>. Set these to match.
      </p>

      <div class="dip">
        <div class="dip-on">on</div>
        <div class="dip-body">
          <button type="button" class="rocker" *ngFor="let open of bits; let i = index"
                  (click)="toggle(i)"
                  [attr.aria-pressed]="!open"
                  [attr.aria-label]="'Switch ' + (i + 1) + ', ' + (open ? 'down' : 'up')">
            <span class="slot" [class.up]="!open"><span class="nub"></span></span>
            <span class="n">{{ i + 1 }}</span>
          </button>
        </div>

        <div class="readout">
          <span class="lbl">Address</span>
          <span>
            <span class="bin">{{ binary }}</span>
            <span class="val" style="margin-left:9px">{{ address }}</span>
          </span>
        </div>
      </div>

      <p class="where">
        <b>Can't find it?</b> The same eight switches are inside the receiver box — the one
        plugged in at the collector. They have to match each other, so either will do.
      </p>

      <div class="nav">
        <button type="button" class="back" (click)="cancelled.emit()">Cancel</button>
        <button type="button" class="save" (click)="saved.emit(address)">Use this address</button>
      </div>
    </div>
  `,
})
export class RfAddressComponent implements OnInit {
  /** The address already stored, 0-255. */
  @Input() address = ROCKLER_ADDRESS;
  @Output() saved = new EventEmitter<number>();
  @Output() cancelled = new EventEmitter<void>();

  /** index i = rocker i+1 OPEN (nub down, logic 1). */
  bits: boolean[] = [];

  ngOnInit(): void {
    this.bits = Array.from({ length: 8 }, (_, i) => ((this.address >> i) & 1) === 1);
  }

  toggle(i: number): void {
    this.bits[i] = !this.bits[i];
    this.address = this.bits.reduce((a, open, n) => a + (open ? (1 << n) : 0), 0);
  }

  /** MSB-first, the way it reads on paper — rocker 8 on the left. */
  get binary(): string {
    return this.bits.map(o => (o ? '1' : '0')).reverse().join('');
  }
}
