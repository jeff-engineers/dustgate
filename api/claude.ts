import type { VercelRequest, VercelResponse } from '@vercel/node';
import { Ratelimit } from '@upstash/ratelimit';
import { Redis } from '@upstash/redis';

const kv = new Redis({
  url:   process.env.UPSTASH_REDIS_REST_URL!,
  token: process.env.UPSTASH_REDIS_REST_TOKEN!,
});

// ── Rate limiters ─────────────────────────────────────────────────────────────
//
// Two independent layers:
//   global — shared bucket across all visitors; protects total daily spend
//   ip     — per-IP bucket; prevents one person from burning the global quota
//
// Both use sliding window so a burst at 23:59 doesn't reset at midnight.

const globalLimit = new Ratelimit({
  redis:     kv,
  limiter:   Ratelimit.slidingWindow(50, '1 d'),
  prefix:    'dg:global',
  analytics: false,
});

const ipLimit = new Ratelimit({
  redis:     kv,
  limiter:   Ratelimit.slidingWindow(3, '1 d'),
  prefix:    'dg:ip',
  analytics: false,
});

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

  // Extract IP — Vercel sets x-forwarded-for; fall back to a safe sentinel
  const forwarded = req.headers['x-forwarded-for'];
  const ip = (Array.isArray(forwarded) ? forwarded[0] : forwarded)
    ?.split(',')[0]
    ?.trim() ?? '0.0.0.0';

  // ── Global rate check ─────────────────────────────────────────────────────
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
      body: JSON.stringify(req.body),
    });
  } catch (e) {
    console.error('[claude] fetch error:', e);
    return res.status(502).json({ error: 'Failed to reach Anthropic API' });
  }

  const data = await anthropicRes.json();
  return res.status(anthropicRes.status).json(data);
}
