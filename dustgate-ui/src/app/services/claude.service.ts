import { Injectable } from '@angular/core';
import { ApiService } from './api.service';

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
        stop: { type: 'integer', description: 'Stop index: 0 = home, 1–7 = blast gate positions' }
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
    description: 'Assign a Shelly smart outlet to a blast gate stop. Call after ping_outlet confirms the device is reachable.',
    input_schema: {
      type: 'object',
      properties: {
        slot:        { type: 'integer', description: 'Outlet slot number (0–6)' },
        generation:  { type: 'integer', enum: [1, 2], description: 'Shelly generation: 1 for Gen1, 2 for Gen2/Plus' },
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
    description: 'Check whether a Shelly outlet is reachable on the network and read its current power draw.',
    input_schema: {
      type: 'object',
      properties: {
        generation: { type: 'integer', enum: [1, 2], description: 'Shelly generation' },
        ip:         { type: 'string',  description: 'IP address to ping' }
      },
      required: ['generation', 'ip']
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
  }
];

const SYSTEM_PROMPT = `You are DustGate Setup Assistant, helping the user configure a motorized blast gate system for dust collection in a woodworking shop.

The system has a rack-and-pinion linear actuator that moves between numbered stop positions. Each stop corresponds to a blast gate for one tool. A Shelly smart outlet on each tool automatically detects when that tool is turned on (by monitoring power draw) and routes dust collection to the correct gate.

Your job is to walk the user through setup conversationally:
1. Home the actuator so you have a known position.
2. For each blast gate, jog the actuator to align it with that gate, then confirm the stop number.
3. Ask the user what tool is at each gate — accept whatever name they give ("Bandsaw", "Router Table", etc.). Don't offer a list; just ask.
4. Ask for the Shelly outlet IP address for that tool, ping it to confirm it's reachable, then configure it.
5. Repeat for all tools.
6. Save the configuration, then tell the user setup is complete and they can tap the back arrow to use the dashboard.

Be friendly and concise. One thing at a time. If the user asks to reconfigure or change something mid-setup, accommodate them naturally. If a ping fails, suggest checking the outlet IP and trying again.`;

// ── Service ────────────────────────────────────────────────────────────────────

@Injectable({ providedIn: 'root' })
export class ClaudeService {

  constructor(private api: ApiService) {}

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
        const msg = e instanceof Error ? e.message : String(e);
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
        return this.api.pingOutlet(
          input['generation'] as number,
          input['ip']         as string
        );

      case 'save_config':
        return this.api.saveOutletConfig();

      case 'delete_outlet':
        return this.api.deleteOutlet(input['slot'] as number);

      default:
        throw new Error(`Unknown tool: ${name}`);
    }
  }
}
