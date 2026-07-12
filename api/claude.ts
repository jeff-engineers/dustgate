import type { VercelRequest, VercelResponse } from '@vercel/node';
import { timingSafeEqual } from 'crypto';
import { Ratelimit } from '@upstash/ratelimit';
import { Redis } from '@upstash/redis';

const kv = Redis.fromEnv();

// ── Rate limiters ─────────────────────────────────────────────────────────────
//
// Two independent layers:
//   global — shared bucket across all visitors; protects total daily spend
//   ip     — per-IP bucket; prevents one person from burning the global quota
//
// A third, separate bucket exists for the guest access code (see below) so
// interview traffic doesn't compete with — or get throttled by — public demo
// visitors.
//
// All use sliding window so a burst at 23:59 doesn't reset at midnight.

const globalLimit = new Ratelimit({
  redis:     kv,
  limiter:   Ratelimit.slidingWindow(200, '1 d'),
  prefix:    'dg:global',
  analytics: false,
});

const ipLimit = new Ratelimit({
  redis:     kv,
  limiter:   Ratelimit.slidingWindow(25, '1 d'),
  prefix:    'dg:ip',
  analytics: false,
});

const guestLimit = new Ratelimit({
  redis:     kv,
  limiter:   Ratelimit.slidingWindow(150, '1 d'),
  prefix:    'dg:guest',
  analytics: false,
});

// ── Access codes ──────────────────────────────────────────────────────────────
//
// Optional `accessCode` field in the request body:
//   ADMIN_ACCESS_CODE — bypasses all rate limits entirely.
//   GUEST_ACCESS_CODE — routed through its own generous, separate bucket
//                       (dg:guest) instead of the public global/IP buckets.
//                       Hand this one to interviewers; it does not affect,
//                       or get affected by, other demo visitors.
//   GUEST_ACCESS_CODE_EXPIRES — optional ISO 8601 timestamp; once passed,
//                       the guest code silently stops working (falls back
//                       to normal public limits) without needing manual
//                       rotation in Vercel.
//
// Comparisons use timingSafeEqual to avoid leaking code length/prefix via
// response timing. Unset env vars mean that code path is simply disabled.

const utf8 = new TextEncoder();

function safeCodeMatch(provided: string | undefined, expected: string | undefined): boolean {
  if (!provided || !expected) return false;
  const a = utf8.encode(provided);
  const b = utf8.encode(expected);
  if (a.length !== b.length) return false;
  return timingSafeEqual(a, b);
}

function guestCodeStillValid(): boolean {
  const expires = process.env.GUEST_ACCESS_CODE_EXPIRES;
  if (!expires) return true;
  const expiryMs = Date.parse(expires);
  if (Number.isNaN(expiryMs)) return true; // malformed value — fail open rather than lock everyone out
  return Date.now() < expiryMs;
}

// ── Handler ───────────────────────────────────────────────────────────────────

export default async function handler(req: VercelRequest, res: VercelResponse) {
  if (req.method !== 'POST') {
    return res.status(405).json({ error: 'Method not allowed' });
  }

  const anthropicKey = process.env.ANTHROPIC_KEY;
  if (!anthropicKey) {
    console.error('[claude] ANTHROPIC_KEY env var not set');
    return res.status(500).json({ error: 'Server configuration error' });
  }

  // Pull the access code out of the body before forwarding — Anthropic
  // doesn't know about this field.
  const { accessCode, ...anthropicBody } = (req.body ?? {}) as Record<string, unknown> & { accessCode?: string };

  const isAdmin = safeCodeMatch(accessCode, process.env.ADMIN_ACCESS_CODE);
  const isGuest = !isAdmin
    && safeCodeMatch(accessCode, process.env.GUEST_ACCESS_CODE)
    && guestCodeStillValid();

  // Extract IP — Vercel sets x-forwarded-for; fall back to a safe sentinel
  const forwarded = req.headers['x-forwarded-for'];
  const ip = (Array.isArray(forwarded) ? forwarded[0] : forwarded)
    ?.split(',')[0]
    ?.trim() ?? '0.0.0.0';

  if (isAdmin) {
    console.log('[claude] admin access code — bypassing all rate limits');
  } else if (isGuest) {
    // ── Guest rate check (separate pool, doesn't touch global/IP buckets) ────
    const { success: guestOk } = await guestLimit.limit('guest');
    if (!guestOk) {
      console.warn('[claude] guest rate limit hit');
      return res.status(429).json({
        error: "The interview quota has been used up for today.",
        limit: 'guest',
      });
    }
    console.log(`[claude] guest access code — forwarding  ip=${ip}`);
  } else {
    // ── Global rate check ────────────────────────────────────────────────────
    const { success: globalOk, remaining: globalRem } = await globalLimit.limit('global');
    if (!globalOk) {
      console.warn('[claude] global rate limit hit');
      return res.status(429).json({
        error: "The demo has hit its daily request limit. Check back tomorrow!",
        limit: 'global',
      });
    }

    // ── Per-IP rate check ─────────────────────────────────────────────────────
    const { success: ipOk } = await ipLimit.limit(ip);
    if (!ipOk) {
      console.warn(`[claude] per-IP rate limit hit for ${ip}`);
      return res.status(429).json({
        error: "You've used your demo quota for today. Come back tomorrow!",
        limit: 'ip',
      });
    }

    console.log(`[claude] forwarding to Anthropic  ip=${ip}  global_remaining=${globalRem}`);
  }

  // ── Proxy to Anthropic ────────────────────────────────────────────────────
  let anthropicRes: Response;
  try {
    anthropicRes = await fetch('https://api.anthropic.com/v1/messages', {
      method:  'POST',
      headers: {
        'Content-Type':      'application/json',
        'x-api-key':         anthropicKey,
        'anthropic-version': '2023-06-01',
      },
      body: JSON.stringify(anthropicBody),
    });
  } catch (e) {
    console.error('[claude] fetch error:', e);
    return res.status(502).json({ error: 'Failed to reach Anthropic API' });
  }

  const data = await anthropicRes.json();
  return res.status(anthropicRes.status).json(data);
}
