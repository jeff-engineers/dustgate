import { Component } from '@angular/core';
import { NgIf } from '@angular/common';
import { RouterOutlet } from '@angular/router';
import { BUILD_TIME_MS } from '../build-info';
import { ApiService } from './services/api.service';
import { formatBuildStamp, formatEpochStamp } from './build-stamp';

/**
 * A short token identifying the BUNDLE that is running, for the footer stamp.
 *
 * Taken from this module's own URL, because that is the one string that changes
 * when the code does and cannot go stale the way a hand-edited constant would:
 * a production build emits hashed filenames (main-A1B2C3D4.js), and the dev
 * server appends a cache-busting query that moves on every rebuild.
 *
 * Falls back to 'dev' rather than throwing — a diagnostic that can break the app
 * it is meant to diagnose is worse than no diagnostic.
 */
function buildToken(): string {
  try {
    const url = import.meta.url;
    // Angular's content hash is uppercase base-36 (main-QLBMHST3.js), not hex —
    // a hex-only pattern silently never matched and every build read 'dev'.
    const hashed = /[-.]([A-Za-z0-9]{6,})\.[cm]?js/.exec(url);
    if (hashed) return hashed[1].slice(0, 8);
    const query = /[?&](?:v|t)=([^&]+)/i.exec(url);
    if (query) return decodeURIComponent(query[1]).slice(0, 8);
  } catch { /* import.meta unavailable — fall through */ }
  return 'dev';
}

@Component({
  selector: 'app-root',
  standalone: true,
  imports: [RouterOutlet, NgIf],
  styles: [`
    :host {
      display: block;
      height: 100%;
      /* 1440, up from 960 on 2026-08-16. This is the ceiling for the BUILD canvas
         and nothing else: every other screen sets a 360–460px cap of its own, so
         they stay phone-shaped and centred however wide this gets. The canvas is
         the one screen whose usefulness scales with the window — a shop grows
         down-and-right and the boards now sit at its top-right corner. */
      max-width: 1440px;
      margin: 0 auto;
      /* On wide viewports the app column no longer reaches the screen edge,
         so give it its own borders to stay visually separated from the bg. */
      border-left: 1px solid var(--border);
      border-right: 1px solid var(--border);
    }

    /* ── Build stamp ──────────────────────────────────────────────────────
       Which bundle is actually running, on every screen.

       This exists because a whole afternoon went into a bug that looked like
       broken code and was partly a stale page: with no way to tell a current
       build from an old one, "still not working" and "you are not running it
       yet" are the same sentence (2026-08-24).

       Deliberately almost invisible — it is for the two minutes a year someone
       needs it, not for daily use. Low contrast, small, and out of the way at
       the bottom of the column; selectable, so it can be highlighted to read or
       copied into a bug report. Not a link, not a control, never in the way. */
    .build-stamp {
      font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
      font-size: 10px;
      letter-spacing: 0.04em;
      text-align: center;
      /* Three segments that each have to stay whole. At 375px the line no longer
         fits — it wraps, and left to itself it broke INSIDE a timestamp, which
         reads as a rendering fault rather than a wrap. Flex with nowrap segments
         puts the break at a separator instead, where it looks intentional. */
      display: flex;
      flex-wrap: wrap;
      justify-content: center;
      gap: 0 4px;
      /* Dimmer than --muted on purpose: --muted is for text meant to be read. */
      color: #3a3a38;
      padding: 18px 8px 12px;
      user-select: text;
    }
    .build-stamp span { white-space: nowrap; }
    /* Highlighting it is how you read it, so make the selection legible. */
    .build-stamp::selection { background: var(--accent); color: #000; }
    .build-stamp span::selection { background: var(--accent); color: #000; }
  `],
  template: `
    <router-outlet />
    <div class="build-stamp">
      <span>build {{ build }}</span>
      <span *ngIf="app">· app {{ app }}</span>
      <span *ngIf="device">· device {{ device }}</span>
    </div>
  `
})
export class AppComponent {
  readonly build = buildToken();

  /** When THIS BUNDLE was compiled — the app's own __DATE__/__TIME__, baked in by
   *  gen-build-info.js. Answers "is the ng build I just made the one I'm looking
   *  at", which the hash token identifies but cannot date. */
  readonly app = formatEpochStamp(BUILD_TIME_MS);

  /** When the DEVICE's firmware was compiled, exactly as the OLED shows it.
   *
   *  The two together are the diagnostic. Deployed onto the device they move as a
   *  pair, so a mismatch means a half-finished deploy; under `ng serve` against a
   *  real board they are meant to differ, and seeing how far apart they are is the
   *  fastest way to know which half is stale.
   *
   *  Absent until /api/info answers, and absent forever with no device — the stamp
   *  drops the segment rather than showing a placeholder, because an empty slot
   *  reads as "not known" and a placeholder reads as a value. */
  device = '';

  constructor(private readonly api: ApiService) {
    // A snapshot, not a subscription: /api/info is fetched once at startup and a
    // compile stamp cannot change without the board rebooting, which reloads this
    // page anyway. ready$ is a BehaviorSubject, so a late subscriber still gets it.
    this.api.ready$.subscribe(() => {
      this.device = formatBuildStamp(this.api.deviceInfo?.built);
    });
  }
}
