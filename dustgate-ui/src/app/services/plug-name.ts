/**
 * What to call a smart plug wherever a person reads it (review §1.3, 2026-11-25). Pure, so the spec drives it.
 *
 * A plug nobody has named answers with its factory hostname, which ends in its MAC: "ShellyPlugUSG4-84E6D7047170",
 * "tasmota-0F0E99-3737". That is a serial number, not a name, and it was all the tray, the picker and the Tools list
 * had to show. So: the name someone gave it; else the tool it is on ("Table Saw plug"); else "Unnamed plug". The
 * address, the hostname and the kind are a developer's details (developer mode), never the name.
 */

/** A name that is really the factory hostname: a MAC-like run of 6–12 hex digits at the end, after a dash or the model. */
export function isFactoryName(name: string | null | undefined): boolean {
  const n = (name ?? '').trim();
  if (!n) return true;
  return /[-_]?[0-9a-f]{6,12}(-\d{1,5})?$/i.test(n) && /[0-9]/.test(n.slice(-12));
}

export interface PlugLike { name?: string | null; hostname?: string | null; host?: string | null }

/** The plug's name for a person: its own, else the tool it is on, else "Unnamed plug". */
export function plugName(p: PlugLike | null | undefined, onTool?: string | null): string {
  const own = (p?.name ?? '').trim();
  if (own && !isFactoryName(own)) return own;
  const host = (p?.hostname ?? p?.host ?? '').trim();
  if (host && !isFactoryName(host)) return host;      // a hostname someone chose ("shellyplugus-tablesaw") is a name
  if (onTool) return `${onTool} plug`;
  return 'Unnamed plug';
}
