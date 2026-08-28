import { Component } from '@angular/core';
import { RouterOutlet } from '@angular/router';

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

const pad = (n: number) => String(n).padStart(2, '0');

@Component({
  selector: 'app-root',
  standalone: true,
  imports: [RouterOutlet],
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
      /* Dimmer than --muted on purpose: --muted is for text meant to be read. */
      color: #3a3a38;
      padding: 18px 8px 12px;
      user-select: text;
    }
    /* Highlighting it is how you read it, so make the selection legible. */
    .build-stamp::selection { background: var(--accent); color: #000; }
  `],
  template: `
    <router-outlet />
    <div class="build-stamp">build {{ build }} · loaded {{ loaded }}</div>
  `
})
export class AppComponent {
  readonly build = buildToken();
  /** When this page was loaded — the other half of "am I looking at current
   *  code": a fresh token with an old load time means a reload is all that's
   *  missing. */
  readonly loaded = (() => {
    const d = new Date();
    return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
  })();
}
