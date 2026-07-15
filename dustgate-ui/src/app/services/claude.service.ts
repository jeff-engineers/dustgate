import { Injectable } from '@angular/core';
import { ApiService } from './api.service';
import { HardwareProfileService, PortSize } from './hardware-profile.service';

// ── Types ──────────────────────────────────────────────────────────────────────

export interface ChatMessage {
  role: 'user' | 'assistant';
  content: string | ContentBlock[];
}

interface ContentBlock {
  type: string;
  [key: string]: unknown;
}

export interface ToolCall {
  name: string;
  input: Record<string, unknown>;
  result?: unknown;
  error?: string;
}

/**
 * Emitted during a turn so the UI can show progress.
 *
 * 'text' now carries an incremental DELTA, not a whole block — with
 * streaming, a single sentence arrives as many small 'text' events rather
 * than one big one. Consumers should append, not replace.
 */
export interface TurnEvent {
  type: 'text' | 'tool_start' | 'tool_done' | 'error';
  text?: string;
  tool?: ToolCall;
}

// ── Tool schemas sent to Claude ────────────────────────────────────────────────

const TOOLS = [
  {
    name: 'get_status',
    description: 'Get the current system state, motor position, homing status, and all configured outlet mappings.',
    input_schema: { type: 'object', properties: {} }
  },
  {
    name: 'home',
    description: 'Drive the actuator to the home position (stop 0) and zero the position counter.',
    input_schema: { type: 'object', properties: {} }
  },
  {
    name: 'move_to_stop',
    description: 'Move the actuator to a numbered stop position.',
    input_schema: {
      type: 'object',
      properties: {
        stop: { type: 'integer', description: 'Stop index: 0 = home, 1–16 = blast gate positions' }
      },
      required: ['stop']
    }
  },
  {
    name: 'jog',
    description: 'Move the actuator by a small relative distance in millimetres (useful for fine-positioning a gate).',
    input_schema: {
      type: 'object',
      properties: {
        mm: { type: 'number', description: 'Distance in mm. Positive = away from home, negative = toward home.' }
      },
      required: ['mm']
    }
  },
  {
    name: 'configure_outlet',
    description: 'Assign a Shelly smart outlet to a blast gate stop. Call after ping_outlet or discover_outlets confirms the device is reachable, using the generation it returned — there is no need to ask the user which generation their outlet is.',
    input_schema: {
      type: 'object',
      properties: {
        slot:        { type: 'integer', description: 'Outlet slot number (0–15)' },
        generation:  { type: 'integer', enum: [1, 2], description: 'Shelly generation, as returned by ping_outlet/discover_outlets for this IP — do not guess or ask the user.' },
        ip:          { type: 'string',  description: 'IP address of the Shelly outlet (e.g. 192.168.1.101)' },
        host:        { type: 'string',  description: 'mDNS hostname of the outlet, from discover_outlets, if it was found via a scan. Omit for a manually-provided IP — this lets the device re-resolve the outlet after a DHCP IP change instead of losing it.' },
        name:        { type: 'string',  description: 'Human-readable tool name, e.g. "Bandsaw". Always ask the user what to call this gate/tool and use their answer — do NOT default to the outlet\'s Shelly-app device name (it is often unset or a stale cloud label). Identify the outlet to them by its hostname and IP instead.' },
        stop:        { type: 'integer', description: 'Stop index this tool maps to (1–7)' },
        threshold_w: { type: 'number',  description: 'Watt threshold to detect tool-on. Default 5W; increase for tools with a high standby draw.' }
      },
      required: ['slot', 'generation', 'ip', 'name', 'stop']
    }
  },
  {
    name: 'discover_outlets',
    description: 'Scan the local network via mDNS for Shelly smart outlets — no IP needed. Returns each outlet found with its IP, mDNS hostname, the name the user gave it in the Shelly app (if any), reachability, API generation, and current power reading. Call this FIRST when locating an outlet for a gate or the dust collector, before asking the user for an IP address — it saves them from having to look it up. Only fall back to asking for a manual IP (then call ping_outlet) if the scan finds nothing, or the outlet the user wants isn\'t in the results.',
    input_schema: { type: 'object', properties: {} }
  },
  {
    name: 'ping_outlet',
    description: 'Check whether a Shelly outlet at a specific IP is reachable on the network and read its current power draw and Shelly-app device name. The device auto-detects the Shelly API generation (tries Gen 1, falls back to Gen 2) — the response includes which one it found, so there is no need to ask the user their outlet\'s generation. Prefer discover_outlets first; use this when the user gives you a specific IP instead (e.g. discover_outlets didn\'t find it).',
    input_schema: {
      type: 'object',
      properties: {
        ip: { type: 'string', description: 'IP address to ping' }
      },
      required: ['ip']
    }
  },
  {
    name: 'save_config',
    description: 'Persist all outlet configuration to device flash. Call this after all outlets are configured and tested.',
    input_schema: { type: 'object', properties: {} }
  },
  {
    name: 'delete_outlet',
    description: 'Remove an outlet from a slot (clears its configuration).',
    input_schema: {
      type: 'object',
      properties: {
        slot: { type: 'integer', description: 'Slot index to clear (0–6)' }
      },
      required: ['slot']
    }
  },
  {
    name: 'save_stop',
    description: 'Save the current motor position as a numbered stop. Call this after the user confirms the actuator is aligned with a gate. The position is persisted to device flash.',
    input_schema: {
      type: 'object',
      properties: {
        index: { type: 'integer', description: 'Stop index to save (1–16). Stop 0 is always home and cannot be overwritten.' }
      },
      required: ['index']
    }
  },
  {
    name: 'set_home_side',
    description: 'Record which physical side the home endstop is on. This controls how the visualizer renders the gate layout. Call this once at the start of setup after asking the user.',
    input_schema: {
      type: 'object',
      properties: {
        home_on_right: { type: 'boolean', description: 'true if the home endstop / stop-0 position is on the RIGHT side of the manifold when viewed from the front; false if it is on the LEFT.' }
      },
      required: ['home_on_right']
    }
  },
  {
    name: 'set_motor_direction',
    description: 'Flip the motor homing direction. Call this if the actuator moves AWAY from the endstop when homing instead of TOWARD it — that means the direction is backwards and needs to be inverted.',
    input_schema: {
      type: 'object',
      properties: {
        invert: { type: 'boolean', description: 'true to invert the homing direction, false to restore normal direction.' }
      },
      required: ['invert']
    }
  },
  {
    name: 'set_num_gates',
    description: 'Tell the device how many blast gates are installed. Call this early in setup so the visualizer and move validation use the correct count.',
    input_schema: {
      type: 'object',
      properties: {
        num_gates: { type: 'integer', description: 'Number of blast gates (1–16), not counting the home/parked position.' }
      },
      required: ['num_gates']
    }
  },
  {
    name: 'configure_dust_collector',
    description: 'Assign a Shelly smart outlet to the dust collector itself (separate from any gate), so DustGate can switch the collector on and off remotely. There is no wattage threshold to configure here — call discover_outlets or ping_outlet first to confirm it is reachable, then call this using the generation that returned.',
    input_schema: {
      type: 'object',
      properties: {
        generation: { type: 'integer', enum: [1, 2], description: 'Shelly generation, as returned by ping_outlet/discover_outlets for this IP — do not guess or ask the user.' },
        ip:         { type: 'string',  description: 'IP address of the Shelly outlet controlling the dust collector' },
        host:       { type: 'string',  description: 'mDNS hostname of the outlet, from discover_outlets, if it was found via a scan. Omit for a manually-provided IP.' }
      },
      required: ['generation', 'ip']
    }
  },
  {
    name: 'switch_dust_collector',
    description: 'Remotely switch the dust collector on or off. Only call this after the user has confirmed it is OK to control the collector remotely, and only after configure_dust_collector has been called.',
    input_schema: {
      type: 'object',
      properties: {
        on: { type: 'boolean', description: 'true to switch the collector on, false to switch it off' }
      },
      required: ['on']
    }
  },
  {
    name: 'set_port_size',
    description: 'Record which DustGate hardware size the user has. This only seeds a starting guess for how far to jog toward the next gate before any real position is known — call it once at the very start of setup after asking the user.',
    input_schema: {
      type: 'object',
      properties: {
        size: { type: 'string', enum: ['2.5in', '4in'], description: '2.5in is the standard/reference size; 4in is the larger hose variant.' }
      },
      required: ['size']
    }
  }
] as Array<Record<string, unknown>>;

// A cache_control breakpoint on the LAST tool caches this entire tools array
// (Anthropic caches everything up to and including the marked block). TOOLS
// never changes at runtime, so every turn after the first reuses the cached
// copy instead of re-processing ~1.5KB of schema — cheaper and faster per turn.
TOOLS[TOOLS.length - 1] = { ...TOOLS[TOOLS.length - 1], cache_control: { type: 'ephemeral' } };

const SYSTEM_PROMPT = `You are DustGate Setup Assistant, helping the user configure a motorized blast gate system for dust collection in a woodworking shop.

The system has a rack-and-pinion linear actuator that moves between numbered stop positions. Each stop corresponds to a blast gate for one tool. A Shelly smart outlet on each tool automatically detects when that tool is turned on (by monitoring power draw) and routes dust collection to the correct gate.

Your job is to walk the user through setup conversationally:
1. Always ask the user before moving the actuator.
2. Ask which DustGate hardware size they have — 2.5" (standard) or 4" — and call set_port_size. On the reference hardware (both sizes, until 4" is measured separately), the endstop sits about 2mm from the first gate, and adjacent gates beyond that are spaced about 89mm apart; treat these as rough starting expectations only, not facts to enforce — the actual jogged position always wins.
3. Ask the user how many blast gates they have and call set_num_gates.
4. Ask the user if the home endstop is on the left or right side of the manifold and call set_home_side.
5. Home the actuator so you have a known position.  If the endstop is already triggered, ask them if it's alright to move it away from the endstop a bit to confirm it works.
6. If the actuator moved AWAY from the endstop, call set_motor_direction with invert=true and home again.
7. Confirm that the homing went in the correct direction.
8. Ask the user to measure or estimate the distance to the next gate, offering your own starting estimate so they have something to react to rather than guessing cold: about 2mm for the very first gate (endstop to gate 1), and about 89mm for gate-to-gate distances after that. Let them know they can reply in metric, imperial, casual terms like "a little more" or "about 4 inches", or just confirm your estimate. When the user provides any distance or movement instruction — even an approximate one, or a simple "yes" to your estimate — treat that as permission to move immediately. Do NOT ask a separate "are you ready?" or "shall I move?" question.
9. Move the actuator to the desired position.
10. Confirm the actuator is aligned with the gate. Repeat jogging until the user confirms alignment, then call save_stop.
11. Before moving on to the next gate, finish configuring THIS gate. Resolve the outlet question first so that, when you ask the user to name the gate, you can point to the specific outlet (by hostname/IP) they're naming:
    a. Ask whether this gate's tool has a Shelly smart plug. If not, skip to (e) and just ask for the gate/tool name directly.
    b. If yes, call discover_outlets first (don't ask for an IP yet) and check the results against what the user describes (e.g. "the one near the bandsaw"). Present any matches by their hostname and IP (mention the Shelly-app name too only if one happens to be set, as extra context) and confirm with the user which one is theirs, rather than assuming. Never suggest or accept an IP already configured for a different gate earlier in this session (track which IPs you've already called configure_outlet with) — if the user picks one anyway, point out it's already assigned to that other gate and ask them to choose a different outlet or confirm they want to move it off the other gate first.
    c. If discover_outlets finds nothing, or none of the results match, ask the user for the outlet's IP address directly and call ping_outlet to confirm it's reachable. You may need to direct them to Shelly's website for help finding the IP. Either way (scan or manual ping), the response tells you the generation and, if the outlet was named in the Shelly app, that name too — don't ask the user which generation they have.
    d. Once the outlet is confirmed reachable, ask the user what they want to call this tool/gate, referring to the outlet by its hostname/IP so they know which one they're naming. Always let them choose the name themselves — do NOT propose the Shelly-app device name as the name (it's often unset or stale). Accept whatever name they give without offering a list. Then help them pick a detection threshold: ask them to turn the tool on at its lowest setting with no load (e.g. no material feeding, blade/bit spinning free), then ping again to capture that running wattage. Suggest a threshold about 10% below that reading, rounded to a clean number (nearest 10W normally, nearest 50W for readings above a couple hundred watts) — this gives margin below the running draw while safely clearing standby power. Confirm the suggestion with the user (they can override it) before calling configure_outlet with the generation and host (if the outlet came from discover_outlets) it returned.
    e. If the tool has no smart plug, just ask for and note the gate's name — configure_outlet is only needed when there's a plug to assign.
12. Only once this gate is fully positioned AND named/configured (outlet or not) should you move on: repeat steps 8–11 for the next gate, one gate at a time, until every gate is done. Typically, once a distance is known between two gates, the rest will be the same — but still confirm alignment and finish naming/outlet setup for each gate before starting the next one.
13. If the user states that the distance moved is more/less than anticipated, try to recalculate the movement distance per step based on their feedback.
16. Once all gates are configured, ask if their dust collector is also on a Shelly smart plug, separate from any tool's gate — DustGate can switch it on and off automatically alongside the gates. This step is optional; skip it if they say no or don't have one. There's no wattage threshold to pick here since it's just a remote switch. If they want it:
    a. Call discover_outlets first and check the results against what the user describes, rather than asking for an IP up front. If nothing matches (or nothing is found), ask for the IP address directly and call ping_outlet to confirm it's reachable — either way, the result tells you the generation, so don't ask the user which one they have.
    b. Once reachable, call configure_dust_collector with the generation (and host, if it came from discover_outlets) that was returned.
    c. Ask the user to turn the collector on using its own physical switch so it's ready to draw power, and ask if it's OK for DustGate to switch it off and back on remotely to verify it can detect the collector running. If they'd rather skip this test, move on without it.
    d. If they agree, call switch_dust_collector with on=true, then call ping_outlet again on the same IP to read the live wattage. Look for at least roughly 100W — if you see that much, tell the user detection looks good; if not, suggest checking that the collector's own switch is on and it's plugged into the right outlet, and offer to check again.
    e. Once confirmed (or skipped), ask if it's OK to switch the collector back off, then call switch_dust_collector with on=false.
17. Save the configuration and tell the user setup is complete — a link back to the dashboard will appear automatically, so don't tell them to use the back arrow.

Be friendly and concise. One thing at a time. If the user asks to reconfigure or change something mid-setup, accommodate them naturally. If a ping fails, suggest checking the outlet IP and trying again.`;

// Same idea as the TOOLS cache_control above: this prompt is ~1.5KB and
// identical on every turn of a setup session, so mark it cacheable rather
// than paying full input-token price for it on every single API call.
const SYSTEM_BLOCKS = [
  { type: 'text', text: SYSTEM_PROMPT, cache_control: { type: 'ephemeral' } }
];

// ── Service ────────────────────────────────────────────────────────────────────

// Kept comfortably below api/claude.ts's server-side MAX_MESSAGES (60) so we
// trim proactively and the conversation degrades gracefully, instead of the
// server hard-rejecting the request once a long setup session crosses 60.
const MAX_HISTORY_MESSAGES = 40;

@Injectable({ providedIn: 'root' })
export class ClaudeService {

  constructor(private api: ApiService, private hardwareProfile: HardwareProfileService) {}

  /**
   * Run one conversational turn.
   *
   * @param history  Full message history so far (mutated in-place — caller keeps the ref)
   * @param userText User's new message
   * @param onEvent  Called for each event during the turn (text chunks, tool calls)
   */
  async sendMessage(
    history: ChatMessage[],
    userText: string,
    onEvent: (e: TurnEvent) => void
  ): Promise<void> {

    // Append user message
    history.push({ role: 'user', content: userText });

    this.trimHistory(history);

    // Run the agentic loop
    let continueLoop = true;
    while (continueLoop) {
      const body = {
        // Sonnet, not Haiku or Opus: this loop drives real hardware through
        // multi-step tool calls (home, jog, configure_outlet, ...), so
        // tool-call reliability matters more than shaving cost with Haiku,
        // while Opus's extra cost isn't justified for this task's reasoning
        // demands. Must match ALLOWED_MODELS in api/claude.ts, which rejects
        // any other model server-side.
        model:      'claude-sonnet-4-6',
        max_tokens: 1024,
        system:     SYSTEM_BLOCKS,
        tools:      TOOLS,
        messages:   this.withConversationCache(history)
      };

      let response: Response;
      try {
        response = await this.api.agentChat(body);
      } catch (e: unknown) {
        onEvent({ type: 'error', text: `API error: ${e instanceof Error ? e.message : String(e)}` });
        return;
      }

      if (!response.ok) {
        let detail = `HTTP ${response.status}`;
        try {
          const errBody = await response.json() as Record<string, unknown>;
          if (typeof errBody['error'] === 'string') detail += `: ${errBody['error']}`;
        } catch {
          // body wasn't JSON — stick with the bare status
        }
        onEvent({ type: 'error', text: `API error: ${detail}` });
        return;
      }

      // The demo deployment (api/claude.ts) always streams SSE; the real
      // ESP32 proxy and local mock server return one buffered JSON object.
      // Content-Type tells us which we got, so one call site handles both.
      const isStreaming = (response.headers.get('content-type') ?? '').includes('text/event-stream');
      const { content, stopReason } = isStreaming
        ? await this.readStreamedResponse(response, onEvent)
        : await this.readBufferedResponse(response, onEvent);

      // Append assistant message to history
      history.push({ role: 'assistant', content });

      const toolUseBlocks = content.filter(b => b['type'] === 'tool_use');

      if (stopReason === 'tool_use' && toolUseBlocks.length > 0) {
        // Execute each tool call and collect results
        const toolResults: ContentBlock[] = [];

        for (const block of toolUseBlocks) {
          const toolCall: ToolCall = {
            name:  block['name'] as string,
            input: block['input'] as Record<string, unknown>
          };
          onEvent({ type: 'tool_start', tool: toolCall });

          let result: unknown;
          let isError = false;
          try {
            result = await this.executeTool(toolCall.name, toolCall.input);
            toolCall.result = result;
          } catch (e: unknown) {
            isError = true;
            toolCall.error = e instanceof Error ? e.message : String(e);
            result = { error: toolCall.error };
          }

          onEvent({ type: 'tool_done', tool: toolCall });

          toolResults.push({
            type:        'tool_result',
            tool_use_id: block['id'] as string,
            content:     JSON.stringify(result),
            is_error:    isError
          });
        }

        // Append tool results and loop
        history.push({ role: 'user', content: toolResults });
        continueLoop = true;
      } else {
        // end_turn or no tool calls — we're done
        continueLoop = false;
      }
    }
  }

  // ── Prompt caching ────────────────────────────────────────────────────────────

  /**
   * Returns a shallow copy of `history` with an `ephemeral` cache_control
   * breakpoint on the last content block of the last message, so Anthropic
   * caches the whole conversation prefix (system + tools + all prior messages).
   *
   * Why this matters: one user turn triggers an agentic loop of several API
   * calls (tool_use → tool_result → next call), and each call re-sends the
   * entire growing message history. Without a breakpoint here, only the
   * static system+tools prefix is cached and every message is re-billed at
   * full input price on every call. Marking the last message caches the prefix
   * up to it, so the *next* call reads all of that from cache (~10% of input
   * price) and only pays full price for the newest turn. Anthropic matches the
   * longest cached prefix, so moving the single breakpoint forward each call
   * extends the cache incrementally rather than invalidating it.
   *
   * The breakpoint is applied to a COPY (the stored `history` stays plain),
   * so exactly one message-level breakpoint exists per request — system(1) +
   * tools(1) + message(1) = 3, comfortably under Anthropic's 4-breakpoint cap.
   */
  private withConversationCache(history: ChatMessage[]): ChatMessage[] {
    if (history.length === 0) return history;
    const out = history.slice();
    const last = out[out.length - 1];
    // Normalise string content to a text block so we have somewhere to hang
    // cache_control; clone blocks so we never mutate the stored history.
    const blocks: ContentBlock[] = typeof last.content === 'string'
      ? [{ type: 'text', text: last.content }]
      : last.content.map(b => ({ ...b }));
    if (blocks.length === 0) return history;
    blocks[blocks.length - 1] = {
      ...blocks[blocks.length - 1],
      cache_control: { type: 'ephemeral' },
    };
    out[out.length - 1] = { ...last, content: blocks };
    return out;
  }

  // ── History trimming ──────────────────────────────────────────────────────────

  /**
   * Drops the oldest complete "rounds" once history grows past
   * MAX_HISTORY_MESSAGES, so a long setup session keeps costing roughly the
   * same per turn instead of growing forever.
   *
   * A round is one human-typed message plus everything that followed it
   * (assistant text, tool_use, tool_result) up to the next human message.
   * Anthropic requires each tool_use to be immediately followed by its
   * tool_result, so we can only ever cut on round boundaries — never in the
   * middle of one — or the next request would be malformed.
   *
   * Trimmed-away facts (gate count, port size, etc.) aren't lost from the
   * device itself — they're persisted via tool calls — so the model can
   * re-discover them with get_status if it needs to.
   */
  private trimHistory(history: ChatMessage[]): void {
    const isRoundStart = (m: ChatMessage) => m.role === 'user' && typeof m.content === 'string';

    while (history.length > MAX_HISTORY_MESSAGES) {
      // Find the start of the second round — everything before it is the
      // oldest round in full, safe to drop as one unit.
      const secondRoundStart = history.findIndex((m, i) => i > 0 && isRoundStart(m));
      if (secondRoundStart <= 0) break; // only one round left; nothing safe to cut
      history.splice(0, secondRoundStart);
    }
  }

  // ── Response readers ──────────────────────────────────────────────────────────

  /** Real ESP32 proxy / local mock: one buffered JSON /v1/messages response. */
  private async readBufferedResponse(
    response: Response,
    onEvent: (e: TurnEvent) => void
  ): Promise<{ content: ContentBlock[]; stopReason: string }> {
    const data = await response.json() as Record<string, unknown>;
    const content = (data['content'] as ContentBlock[]) ?? [];
    // No true streaming here, but emitting through the same 'text' event
    // keeps the UI code path identical whether or not the deployment streams.
    for (const block of content) {
      if (block['type'] === 'text') onEvent({ type: 'text', text: block['text'] as string });
    }
    return { content, stopReason: data['stop_reason'] as string };
  }

  /**
   * Demo deployment (api/claude.ts): parses Anthropic's SSE stream directly,
   * emitting 'text' as each delta arrives and reassembling tool_use blocks
   * from their incremental input_json_delta chunks. Returns the same
   * {content, stopReason} shape as the buffered path so the caller doesn't
   * need to know which one ran.
   */
  private async readStreamedResponse(
    response: Response,
    onEvent: (e: TurnEvent) => void
  ): Promise<{ content: ContentBlock[]; stopReason: string }> {
    const blocks: ContentBlock[] = [];
    const partialJson: Record<number, string> = {};
    let stopReason = 'end_turn';

    for await (const evt of this.parseSSE(response)) {
      const type = evt['type'] as string;

      switch (type) {
        case 'content_block_start': {
          const index = evt['index'] as number;
          const cb = evt['content_block'] as ContentBlock;
          blocks[index] = cb['type'] === 'tool_use'
            ? { type: 'tool_use', id: cb['id'], name: cb['name'], input: {} }
            : { type: 'text', text: '' };
          if (cb['type'] === 'tool_use') partialJson[index] = '';
          break;
        }

        case 'content_block_delta': {
          const index = evt['index'] as number;
          const delta = evt['delta'] as Record<string, unknown>;
          if (delta['type'] === 'text_delta') {
            const chunk = delta['text'] as string;
            blocks[index]['text'] = (blocks[index]['text'] as string) + chunk;
            onEvent({ type: 'text', text: chunk });
          } else if (delta['type'] === 'input_json_delta') {
            partialJson[index] = (partialJson[index] ?? '') + (delta['partial_json'] as string);
          }
          break;
        }

        case 'content_block_stop': {
          const index = evt['index'] as number;
          if (blocks[index]?.['type'] === 'tool_use') {
            try {
              blocks[index]['input'] = partialJson[index] ? JSON.parse(partialJson[index]) : {};
            } catch {
              blocks[index]['input'] = {};
            }
          }
          break;
        }

        case 'message_delta': {
          const delta = evt['delta'] as Record<string, unknown> | undefined;
          if (delta && typeof delta['stop_reason'] === 'string') stopReason = delta['stop_reason'];
          break;
        }

        case 'error': {
          const err = evt['error'] as Record<string, unknown> | undefined;
          throw new Error(typeof err?.['message'] === 'string' ? err['message'] : 'Stream error');
        }

        // message_start, ping, message_stop — nothing to reconstruct
        default:
          break;
      }
    }

    return { content: blocks, stopReason };
  }

  /** Decodes an SSE byte stream into parsed `data:` JSON payloads, one per event. */
  private async *parseSSE(response: Response): AsyncGenerator<Record<string, unknown>> {
    const reader = response.body?.getReader();
    if (!reader) return;
    const decoder = new TextDecoder();
    let buffer = '';

    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });

      let boundary: number;
      while ((boundary = buffer.indexOf('\n\n')) !== -1) {
        const rawEvent = buffer.slice(0, boundary);
        buffer = buffer.slice(boundary + 2);

        const dataStr = rawEvent
          .split('\n')
          .filter(line => line.startsWith('data:'))
          .map(line => line.slice(5).trim())
          .join('');
        if (!dataStr) continue;

        try {
          yield JSON.parse(dataStr);
        } catch {
          // Ignore a malformed/partial event rather than aborting the turn.
        }
      }
    }
  }

  // ── Tool input validation ────────────────────────────────────────────────────
  //
  // Claude's tool arguments are model output, not trusted input — a
  // hallucinated or manipulated value here goes straight to real hardware
  // (a motor, a mains-voltage smart outlet) with no human in the loop for
  // most calls. Every case below validates before calling the API so a bad
  // value throws (which becomes a tool_result error Claude can see and
  // correct) instead of reaching the device.

  /**
   * Rejects anything that isn't a syntactically valid, private-range IPv4
   * address. Outlet IPs come from the model and are used by the ESP32 to
   * make outbound HTTP requests on the local network — without this check,
   * a manipulated or hallucinated IP could point the device at a public host
   * or a sensitive local address (e.g. a cloud metadata endpoint) instead of
   * a Shelly outlet on the user's own LAN.
   */
  private assertPrivateIp(value: unknown): string {
    if (typeof value !== 'string') throw new Error('ip must be a string');
    const octets = value.split('.');
    if (octets.length !== 4 || !octets.every(o => /^\d{1,3}$/.test(o) && Number(o) <= 255)) {
      throw new Error(`Invalid IPv4 address: ${value}`);
    }
    const [a, b] = octets.map(Number);
    const isPrivate =
      a === 10 ||                          // 10.0.0.0/8
      (a === 172 && b >= 16 && b <= 31) || // 172.16.0.0/12
      (a === 192 && b === 168) ||          // 192.168.0.0/16
      (a === 169 && b === 254);            // 169.254.0.0/16 (link-local)
    if (!isPrivate) throw new Error(`IP must be on the local network: ${value}`);
    return value;
  }

  private assertIntInRange(value: unknown, min: number, max: number, label: string): number {
    if (typeof value !== 'number' || !Number.isInteger(value) || value < min || value > max) {
      throw new Error(`${label} must be an integer between ${min} and ${max}`);
    }
    return value;
  }

  private assertNumberInRange(value: unknown, min: number, max: number, label: string): number {
    if (typeof value !== 'number' || !Number.isFinite(value) || value < min || value > max) {
      throw new Error(`${label} must be a number between ${min} and ${max}`);
    }
    return value;
  }

  private assertNonEmptyString(value: unknown, label: string): string {
    if (typeof value !== 'string' || value.trim().length === 0) {
      throw new Error(`${label} must be a non-empty string`);
    }
    return value;
  }

  // ── Tool executor ─────────────────────────────────────────────────────────────

  private async executeTool(name: string, input: Record<string, unknown>): Promise<unknown> {
    switch (name) {

      case 'get_status':
        return this.api.getStatus();

      case 'home':
        return this.api.home();

      case 'move_to_stop':
        return this.api.moveToStop(this.assertIntInRange(input['stop'], 0, 16, 'stop'));

      case 'jog':
        // Generous but bounded: a single jog shouldn't be able to run the
        // actuator the length of a room from one tool call.
        return this.api.jog(this.assertNumberInRange(input['mm'], -300, 300, 'mm'));

      case 'configure_outlet':
        return this.api.configureOutlet({
          slot:        this.assertIntInRange(input['slot'], 0, 15, 'slot'),
          generation:  this.assertIntInRange(input['generation'], 1, 2, 'generation'),
          ip:          this.assertPrivateIp(input['ip']),
          host:        typeof input['host'] === 'string' ? input['host'] : undefined,
          name:        this.assertNonEmptyString(input['name'], 'name'),
          stop:        this.assertIntInRange(input['stop'], 0, 16, 'stop'),
          threshold_w: input['threshold_w'] === undefined
            ? undefined
            : this.assertNumberInRange(input['threshold_w'], 0, 5000, 'threshold_w')
        });

      case 'discover_outlets':
        return this.api.discoverOutlets();

      case 'ping_outlet':
        return this.api.pingOutlet(this.assertPrivateIp(input['ip']));

      case 'save_config':
        return this.api.saveOutletConfig();

      case 'delete_outlet':
        return this.api.deleteOutlet(this.assertIntInRange(input['slot'], 0, 15, 'slot'));

      case 'save_stop':
        return this.api.saveStop(this.assertIntInRange(input['index'], 1, 16, 'index'));

      case 'set_home_side':
        if (typeof input['home_on_right'] !== 'boolean') throw new Error('home_on_right must be a boolean');
        return this.api.setOrientation(input['home_on_right']);

      case 'set_motor_direction':
        if (typeof input['invert'] !== 'boolean') throw new Error('invert must be a boolean');
        return this.api.setMotorDirection(input['invert']);

      case 'set_num_gates':
        return this.api.setNumGates(this.assertIntInRange(input['num_gates'], 1, 16, 'num_gates'));

      case 'configure_dust_collector':
        return this.api.configureDustCollector(
          this.assertIntInRange(input['generation'], 1, 2, 'generation'),
          this.assertPrivateIp(input['ip']),
          typeof input['host'] === 'string' ? input['host'] : ''
        );

      case 'switch_dust_collector':
        if (typeof input['on'] !== 'boolean') throw new Error('on must be a boolean');
        return this.api.setDustCollector(input['on']);

      case 'set_port_size':
        if (input['size'] !== '2.5in' && input['size'] !== '4in') {
          throw new Error('size must be "2.5in" or "4in"');
        }
        this.hardwareProfile.set(input['size'] as PortSize);
        return { ok: true };

      default:
        throw new Error(`Unknown tool: ${name}`);
    }
  }
}
