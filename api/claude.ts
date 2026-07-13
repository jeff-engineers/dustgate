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

// ── Request validation ────────────────────────────────────────────────────────
//
// The UI only ever sends {model, max_tokens, system, tools, messages} (see
// claude.service.ts). Anything else hitting this endpoint is either a bug or
// someone calling it directly to run up the Anthropic bill — since this proxy
// holds the real API key, we can't just forward whatever the client claims.
// Reject out of bounds requests before spending a single token on Anthropic.

const ALLOWED_MODELS = new Set(['claude-sonnet-4-6']);
const MAX_TOKENS_CAP = 1024;       // matches what the UI itself requests
const MAX_MESSAGES = 60;           // generous for a setup conversation, not unbounded
const MAX_BODY_BYTES = 200_000;    // ~200KB; setup chat + tool results shouldn't exceed this
const ALLOWED_BODY_KEYS = new Set(['model', 'max_tokens', 'system', 'tools', 'messages']);

function validateAnthropicBody(body: Record<string, unknown>): string | null {
  for (const key of Object.keys(body)) {
    if (!ALLOWED_BODY_KEYS.has(key)) return `Unexpected field: ${key}`;
  }

  if (typeof body['model'] !== 'string' || !ALLOWED_MODELS.has(body['model'])) {
    return 'Invalid or unsupported model';
  }

  if (typeof body['max_tokens'] !== 'number' || body['max_tokens'] <= 0 || body['max_tokens'] > MAX_TOKENS_CAP) {
    return `max_tokens must be a number between 1 and ${MAX_TOKENS_CAP}`;
  }

  if (!Array.isArray(body['messages']) || body['messages'].length === 0) {
    return 'messages must be a non-empty array';
  }
  if (body['messages'].length > MAX_MESSAGES) {
    return `messages exceeds the ${MAX_MESSAGES} message limit`;
  }

  return null;
}

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

// ── Origin check ─────────────────────────────────────────────────────────────
//
// This endpoint sets no CORS headers, so a cross-site browser request can't
// read the response — but the request still fires and still spends tokens
// before the browser blocks the read. Rejecting a mismatched Origin stops
// that blind-cost case: some other page's JS silently POSTing to our
// endpoint from a visitor's browser to burn the shared daily quota.
//
// We compare against the request's own Host header rather than a hardcoded
// domain so this keeps working across Vercel preview deployments, which each
// get their own *.vercel.app URL. Requests with no Origin header (curl,
// server-to-server, some same-tab navigations) are let through — Origin
// spoofing there is trivial anyway, so this is a speed bump against casual
// cross-site abuse, not a substitute for the rate limits below.

function originIsAllowed(req: VercelRequest): boolean {
  const origin = req.headers.origin;
  if (!origin) return true;
  const host = req.headers.host;
  if (!host) return false;
  try {
    return new URL(origin).host === host;
  } catch {
    return false;
  }
}

// ── Handler ───────────────────────────────────────────────────────────────────

export default async function handler(req: VercelRequest, res: VercelResponse) {
  if (req.method !== 'POST') {
    return res.status(405).json({ error: 'Method not allowed' });
  }

  if (!originIsAllowed(req)) {
    console.warn(`[claude] rejected cross-origin request: ${req.headers.origin}`);
    return res.status(403).json({ error: 'Forbidden' });
  }

  const anthropicKey = process.env.ANTHROPIC_KEY;
  if (!anthropicKey) {
    console.error('[claude] ANTHROPIC_KEY env var not set');
    return res.status(500).json({ error: 'Server configuration error' });
  }

  // Pull the access code out of the body before forwarding — Anthropic
  // doesn't know about this field.
  const { accessCode, ...anthropicBody } = (req.body ?? {}) as Record<string, unknown> & { accessCode?: string };

  // Reject malformed/oversized/out-of-bounds requests before touching the
  // rate limiter or Anthropic — no tokens, no Redis calls spent on garbage.
  const bodyBytes = utf8.encode(JSON.stringify(anthropicBody)).length;
  if (bodyBytes > MAX_BODY_BYTES) {
    return res.status(413).json({ error: 'Request body too large' });
  }
  const validationError = validateAnthropicBody(anthropicBody);
  if (validationError) {
    console.warn(`[claude] rejected invalid request: ${validationError}`);
    return res.status(400).json({ error: validationError });
  }

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
  //
  // We always request streaming from Anthropic ourselves — the client's body
  // is validated above and never carries a `stream` field, so this is a
  // server-controlled override, not something a caller can turn on or off.
  // Streaming lets the UI render Claude's text as it's generated instead of
  // waiting for the whole turn (including any tool-call JSON) to finish.
  let anthropicRes: Response;
  try {
    anthropicRes = await fetch('https://api.anthropic.com/v1/messages', {
      method:  'POST',
      headers: {
        'Content-Type':      'application/json',
        'x-api-key':         anthropicKey,
        'anthropic-version': '2023-06-01',
      },
      body: JSON.stringify({ ...anthropicBody, stream: true }),
    });
  } catch (e) {
    console.error('[claude] fetch error:', e);
    return res.status(502).json({ error: 'Failed to reach Anthropic API' });
  }

  if (!anthropicRes.ok || !anthropicRes.body) {
    // Error responses from Anthropic come back as plain JSON, not a stream.
    const data = await anthropicRes.json().catch(() => ({ error: 'Anthropic request failed' }));
    return res.status(anthropicRes.status).json(data);
  }

  // Pass the SSE stream straight through byte-for-byte. We don't need to
  // understand Anthropic's event format here — that parsing lives in the
  // client (claude.service.ts), keeping this proxy a thin, low-maintenance
  // pass-through rather than a second place that has to track their wire format.
  res.writeHead(200, {
    'Content-Type':      'text/event-stream',
    'Cache-Control':     'no-cache, no-transform',
    'Connection':        'keep-alive',
    'X-Accel-Buffering': 'no', // disable proxy buffering (e.g. nginx) so chunks flush immediately
  });

  const reader = anthropicRes.body.getReader();
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      res.write(Buffer.from(value));
    }
  } catch (e) {
    console.error('[claude] stream error:', e);
  } finally {
    res.end();
  }
}
