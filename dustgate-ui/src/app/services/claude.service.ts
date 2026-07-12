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

/** Emitted during a turn so the UI can show progress. */
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
    description: 'Assign a Shelly smart outlet to a blast gate stop. Call after ping_outlet confirms the device is reachable, using the generation it returned — there is no need to ask the user which generation their outlet is.',
    input_schema: {
      type: 'object',
      properties: {
        slot:        { type: 'integer', description: 'Outlet slot number (0–15)' },
        generation:  { type: 'integer', enum: [1, 2], description: 'Shelly generation, as returned by ping_outlet for this IP — do not guess or ask the user.' },
        ip:          { type: 'string',  description: 'IP address of the Shelly outlet (e.g. 192.168.1.101)' },
        name:        { type: 'string',  description: 'Human-readable tool name chosen by the user, e.g. "Bandsaw"' },
        stop:        { type: 'integer', description: 'Stop index this tool maps to (1–7)' },
        threshold_w: { type: 'number',  description: 'Watt threshold to detect tool-on. Default 5W; increase for tools with a high standby draw.' }
      },
      required: ['slot', 'generation', 'ip', 'name', 'stop']
    }
  },
  {
    name: 'ping_outlet',
    description: 'Check whether a Shelly outlet is reachable on the network and read its current power draw. The device auto-detects the Shelly API generation (tries Gen 1, falls back to Gen 2) — the response includes which one it found, so there is no need to ask the user their outlet\'s generation.',
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
    description: 'Assign a Shelly smart outlet to the dust collector itself (separate from any gate), so DustGate can switch the collector on and off remotely. There is no wattage threshold to configure here — call ping_outlet first to confirm it is reachable, then call this using the generation ping_outlet returned.',
    input_schema: {
      type: 'object',
      properties: {
        generation: { type: 'integer', enum: [1, 2], description: 'Shelly generation, as returned by ping_outlet for this IP — do not guess or ask the user.' },
        ip:         { type: 'string',  description: 'IP address of the Shelly outlet controlling the dust collector' }
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
];

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
11. Before moving on to the next gate, finish configuring THIS gate: ask what tool lives here — accept whatever name they give ("Bandsaw", "Router Table", etc.), don't offer a list, just ask. Then ask for the Shelly outlet IP address for that tool, and call ping_outlet to confirm it's reachable — it auto-detects whether the outlet is Gen 1 or Gen 2, so don't ask the user which generation they have. If the user is unsure of the IP, provide assistance; you may need to direct them to Shelly's website for help. Once reachable, help them pick a detection threshold: ask them to turn the tool on at its lowest setting with no load (e.g. no material feeding, blade/bit spinning free), then ping again to capture that running wattage. Suggest a threshold about 10% below that reading, rounded to a clean number (nearest 10W normally, nearest 50W for readings above a couple hundred watts) — this gives margin below the running draw while safely clearing standby power. Confirm the suggestion with the user (they can override it) before calling configure_outlet with the generation ping_outlet returned. If the tool has no smart plug, skip the outlet sub-steps and just note the gate's name for now — configure_outlet is only needed when there's a plug to assign.
12. Only once this gate is fully positioned AND named/configured (outlet or not) should you move on: repeat steps 8–11 for the next gate, one gate at a time, until every gate is done. Typically, once a distance is known between two gates, the rest will be the same — but still confirm alignment and finish naming/outlet setup for each gate before starting the next one.
13. If the user states that the distance moved is more/less than anticipated, try to recalculate the movement distance per step based on their feedback.
16. Once all gates are configured, ask if their dust collector is also on a Shelly smart plug, separate from any tool's gate — DustGate can switch it on and off automatically alongside the gates. This step is optional; skip it if they say no or don't have one. There's no wattage threshold to pick here since it's just a remote switch. If they want it:
    a. Ask for its IP address, then call ping_outlet to confirm it's reachable — it auto-detects whether the outlet is Gen 1 or Gen 2, so don't ask the user which generation they have.
    b. Once reachable, call configure_dust_collector with the generation ping_outlet returned.
    c. Ask the user to turn the collector on using its own physical switch so it's ready to draw power, and ask if it's OK for DustGate to switch it off and back on remotely to verify it can detect the collector running. If they'd rather skip this test, move on without it.
    d. If they agree, call switch_dust_collector with on=true, then call ping_outlet again on the same IP to read the live wattage. Look for at least roughly 100W — if you see that much, tell the user detection looks good; if not, suggest checking that the collector's own switch is on and it's plugged into the right outlet, and offer to check again.
    e. Once confirmed (or skipped), ask if it's OK to switch the collector back off, then call switch_dust_collector with on=false.
17. Save the configuration and tell the user setup is complete — a link back to the dashboard will appear automatically, so don't tell them to use the back arrow.

Be friendly and concise. One thing at a time. If the user asks to reconfigure or change something mid-setup, accommodate them naturally. If a ping fails, suggest checking the outlet IP and trying again.`;

// ── Service ────────────────────────────────────────────────────────────────────

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

    // Run the agentic loop
    let continueLoop = true;
    while (continueLoop) {
      const body = {
        model:      'claude-sonnet-4-6',
        max_tokens: 1024,
        system:     SYSTEM_PROMPT,
        tools:      TOOLS,
        messages:   history
      };

      let response: Record<string, unknown>;
      try {
        response = await this.api.agentChat(body) as Record<string, unknown>;
      } catch (e: unknown) {
        // Angular HttpClient throws HttpErrorResponse, not a standard Error.
        // Extract the most useful message available.
        let msg: string;
        if (e && typeof e === 'object' && 'error' in e) {
          const httpErr = e as { status?: number; error?: unknown };
          const body = httpErr.error;
          const detail = body && typeof body === 'object' && 'error' in body
            ? (body as { error: string }).error
            : JSON.stringify(body);
          msg = `HTTP ${httpErr.status ?? '?'}: ${detail}`;
        } else {
          msg = e instanceof Error ? e.message : String(e);
        }
        onEvent({ type: 'error', text: `API error: ${msg}` });
        return;
      }

      const stopReason = response['stop_reason'] as string;
      const content    = response['content']    as ContentBlock[];

      // Append assistant message to history
      history.push({ role: 'assistant', content });

      // Process content blocks
      const toolUseBlocks = content.filter(b => b['type'] === 'tool_use');
      const textBlocks    = content.filter(b => b['type'] === 'text');

      // Emit any text first
      for (const block of textBlocks) {
        onEvent({ type: 'text', text: block['text'] as string });
      }

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

  // ── Tool executor ─────────────────────────────────────────────────────────────

  private async executeTool(name: string, input: Record<string, unknown>): Promise<unknown> {
    switch (name) {

      case 'get_status':
        return this.api.getStatus();

      case 'home':
        return this.api.home();

      case 'move_to_stop':
        return this.api.moveToStop(input['stop'] as number);

      case 'jog':
        return this.api.jog(input['mm'] as number);

      case 'configure_outlet':
        return this.api.configureOutlet({
          slot:        input['slot']        as number,
          generation:  input['generation']  as number,
          ip:          input['ip']          as string,
          name:        input['name']        as string,
          stop:        input['stop']        as number,
          threshold_w: input['threshold_w'] as number | undefined
        });

      case 'ping_outlet':
        return this.api.pingOutlet(input['ip'] as string);

      case 'save_config':
        return this.api.saveOutletConfig();

      case 'delete_outlet':
        return this.api.deleteOutlet(input['slot'] as number);

      case 'save_stop':
        return this.api.saveStop(input['index'] as number);

      case 'set_home_side':
        return this.api.setOrientation(input['home_on_right'] as boolean);

      case 'set_motor_direction':
        return this.api.setMotorDirection(input['invert'] as boolean);

      case 'set_num_gates':
        return this.api.setNumGates(input['num_gates'] as number);

      case 'configure_dust_collector':
        return this.api.configureDustCollector(
          input['generation'] as number,
          input['ip']         as string
        );

      case 'switch_dust_collector':
        return this.api.setDustCollector(input['on'] as boolean);

      case 'set_port_size':
        this.hardwareProfile.set(input['size'] as PortSize);
        return { ok: true };

      default:
        throw new Error(`Unknown tool: ${name}`);
    }
  }
}
