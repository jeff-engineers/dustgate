// Types for plug-identity.js — is the plug at a saved address still the plug the layout means?
export function normMac(m: string | null | undefined): string;
export function normKind(k: string | null | undefined): 'shelly' | 'tasmota';
export function samePlug(saved: { kind?: string; mac?: string } | null | undefined,
                         seen: { reachable?: boolean; kind?: string; mac?: string } | null | undefined): 'yes' | 'no' | 'unknown';
