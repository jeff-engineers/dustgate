// Types for plug-claim.js — who owns a smart plug, and what we may do to it.
// See docs/shop-schema-rfc.md §8. Mirrored in firmware/outlets/PlugClaim.h.

/** ours | unclaimed | dustgate | foreign. "unknown" is a WIRE-only state the
 *  device reports when it could not read the plug's push config — callers must
 *  treat it as "don't touch", never as "nobody owns it". */
export type ClaimState = 'ours' | 'unclaimed' | 'dustgate' | 'foreign';

export interface Claim {
  state: ClaimState;
  /** Owner parsed out of the name suffix, null if none. */
  owner: string | null;
  /** The name with any owner suffix stripped — what to SHOW. */
  label: string;
  pickable: boolean;
  /** push (we may rewrite Ws) | poll (we must not). */
  mode: 'push' | 'poll';
  /** Someone else has it, but a human who has been told what breaks may take it. */
  takeable: boolean;
  /** Who has it now, for the confirmation text. */
  holder: string | null;
  /** Ours, at an address we no longer have. */
  stale?: boolean;
  /** Why it isn't pickable, or how it will be paired — UI text. */
  reason: string | null;
}

export const OWNER_SEP: string;
export const DUSTGATE_WS_PATH: string;

export function formatPlugName(label: string, owner: string | null): string;
export function parsePlugName(name: string): { label: string; owner: string | null };
export function wsHost(url: string): string;
export function wsPath(url: string): string;

export function claimOf(p: {
  pushUrl: string; pushEnabled?: boolean; ourHost: string;
  ourName?: string; name?: string;
}): Claim;

export function mayRepoint(claim: Claim, confirmed?: boolean): boolean;

/**
 * The sentence to put in front of the user before a takeover, naming what
 * breaks. Null when the claim isn't takeable. Never "are you sure?".
 */
export function takeoverWarning(claim: Pick<Claim, 'state' | 'owner' | 'holder' | 'takeable'>): string | null;
