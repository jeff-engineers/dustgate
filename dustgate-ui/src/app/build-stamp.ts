/**
 * Build stamps, in the short form the OLED uses.
 *
 * MIRRORS formatBuild() in firmware/utils/StatusScreenModel.h — deliberately, so
 * the number you read on the little screen and the number you read in the footer
 * are the same number written the same way. Someone comparing the two is trying
 * to answer "is this board running what I just built", and two formats make that
 * a translation exercise at exactly the wrong moment.
 *
 * NOT a constants pair in CLAUDE.md's sense: nothing here is a value both sides
 * must agree on, it is a rendering both sides must agree on, and the C++ side is
 * covered by test_statusscreen.cpp while this side is covered by build-stamp.spec.ts.
 * Change the shape on one side and you want the other — the tests will tell you
 * what the strings looked like, which is the part that is easy to get wrong.
 *
 * PURE — no Angular, no browser.
 */

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun',
                'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

/**
 * `__DATE__ " " __TIME__` as the device reports it ("Sep  2 2026 07:25:00") into
 * "9/2/26 7:25:00".
 *
 * Anything it cannot parse comes back EMPTY rather than half-formatted, the same
 * rule the firmware follows and for the same reason: a wrong build date is worse
 * than no build date, because it sends you to the wrong binary.
 */
export function formatBuildStamp(built: string | null | undefined): string {
  if (!built) return '';
  // __DATE__ pads a single-digit day with a SPACE ("Sep  2 2026"), so split on
  // runs of whitespace rather than single spaces — the detail that breaks a
  // naive fixed-offset parse, and the one test_statusscreen.cpp calls out too.
  const parts = built.trim().split(/\s+/);
  if (parts.length < 4) return '';
  const [mon, dayStr, yearStr, time] = parts;

  const month = MONTHS.indexOf(mon) + 1;
  if (!month) return '';

  const day = Number(dayStr), year = Number(yearStr);
  if (!Number.isInteger(day) || day < 1 || day > 31) return '';
  if (!Number.isInteger(year) || year < 2000) return '';

  const t = /^(\d{1,2}):(\d{2}):(\d{2})$/.exec(time);
  if (!t) return '';

  const yy = String(year % 100).padStart(2, '0');
  return `${month}/${day}/${yy} ${Number(t[1])}:${t[2]}:${t[3]}`;
}

/**
 * The same, for a millisecond epoch — what the UI's own build stamp is baked as.
 *
 * Rendered in LOCAL time on purpose. Both stamps in the footer are read side by
 * side against a wall clock ("I flashed that ten minutes ago"), and a UTC stamp
 * beside the device's local one would look like a drift that isn't there.
 */
export function formatEpochStamp(ms: number | null | undefined): string {
  if (typeof ms !== 'number' || !Number.isFinite(ms) || ms <= 0) return '';
  const d = new Date(ms);
  const p = (n: number) => String(n).padStart(2, '0');
  return `${d.getMonth() + 1}/${d.getDate()}/${p(d.getFullYear() % 100)} `
       + `${d.getHours()}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}
