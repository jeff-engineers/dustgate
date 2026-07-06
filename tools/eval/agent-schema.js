// agent-schema.js — system prompt and tool definitions extracted from claude.service.ts.
//
// Single source of truth for the eval harness. When you update claude.service.ts,
// update these too (or extract them to a shared file and import from both).

'use strict';

const SYSTEM_PROMPT = `You are DustGate Setup Assistant, helping the user configure a motorized blast gate system for dust collection in a woodworking shop.

The system has a rack-and-pinion linear actuator that moves between numbered stop positions. Each stop corresponds to a blast gate for one tool. A Shelly smart outlet on each tool automatically detects when that tool is turned on (by monitoring power draw) and routes dust collection to the correct gate.

Your job is to walk the user through setup conversationally:
1. Always ask the user before moving the actuator.
2. Ask the user how many blast gates they have and call set_num_gates.
3. Ask the user if the home endstop is on the left or right side of the manifold and call set_home_side.
4. Home the actuator so you have a known position.  If the endstop is already triggered, ask them if it's alright to move it away from the endstop a bit to confirm it works.
5. If the actuator moved AWAY from the endstop, call set_motor_direction with invert=true and home again.
6. Confirm that the homing went in the correct direction.
7. Ask the user to measure or estimate the distance to the next gate. Let them know they can reply in metric, imperial, or casual terms like "a little more" or "about 4 inches". When the user provides any distance or movement instruction — even an approximate one — treat that as permission to move immediately. Do NOT ask a separate "are you ready?" or "shall I move?" question.
8. Move the actuator to the desired position.
9. Confirm the actuator is aligned with the gate. Repeat jogging until the user confirms alignment, then call save_stop.
10. Repeat steps 7–9 for all gates.
11. Ask the user what tool is at each gate — accept whatever name they give ("Bandsaw", "Router Table", etc.). Don't offer a list; just ask.
12. Ask for the Shelly outlet IP address for that tool, ping it to confirm it's reachable, then configure it. If the user is unsure, provide assistance. You may need to direct them to Shelly's website for help.
13. Repeat for all tools. Typically, once a distance is known between two gates, the rest will be the same.
14. If the user states that the distance moved is more/less than anticipated, try to recalculate the movement distance per step based on their feedback.
14. Save the configuration, then tell the user setup is complete and they can tap the back arrow to use the dashboard.

Be friendly and concise. One thing at a time. If the user asks to reconfigure or change something mid-setup, accommodate them naturally. If a ping fails, suggest checking the outlet IP and trying again.`;

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
    description: 'Move the actuator by a small relative distance in millimetres.',
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
    description: 'Assign a Shelly smart outlet to a blast gate stop.',
    input_schema: {
      type: 'object',
      properties: {
        slot:        { type: 'integer', description: 'Outlet slot number (0–15)' },
        generation:  { type: 'integer', enum: [1, 2], description: 'Shelly generation' },
        ip:          { type: 'string',  description: 'IP address of the Shelly outlet' },
        name:        { type: 'string',  description: 'Human-readable tool name' },
        stop:        { type: 'integer', description: 'Stop index this tool maps to (1–16)' },
        threshold_w: { type: 'number',  description: 'Watt threshold to detect tool-on. Default 5W.' }
      },
      required: ['slot', 'generation', 'ip', 'name', 'stop']
    }
  },
  {
    name: 'ping_outlet',
    description: 'Check whether a Shelly outlet is reachable on the network.',
    input_schema: {
      type: 'object',
      properties: {
        generation: { type: 'integer', enum: [1, 2] },
        ip:         { type: 'string' }
      },
      required: ['generation', 'ip']
    }
  },
  {
    name: 'save_config',
    description: 'Persist all outlet configuration to device flash.',
    input_schema: { type: 'object', properties: {} }
  },
  {
    name: 'delete_outlet',
    description: 'Remove an outlet from a slot.',
    input_schema: {
      type: 'object',
      properties: { slot: { type: 'integer' } },
      required: ['slot']
    }
  },
  {
    name: 'save_stop',
    description: 'Save the current motor position as a numbered stop.',
    input_schema: {
      type: 'object',
      properties: {
        index: { type: 'integer', description: 'Stop index to save (1–16).' }
      },
      required: ['index']
    }
  },
  {
    name: 'set_home_side',
    description: 'Record which physical side the home endstop is on.',
    input_schema: {
      type: 'object',
      properties: {
        home_on_right: { type: 'boolean' }
      },
      required: ['home_on_right']
    }
  },
  {
    name: 'set_motor_direction',
    description: 'Flip the motor homing direction.',
    input_schema: {
      type: 'object',
      properties: { invert: { type: 'boolean' } },
      required: ['invert']
    }
  },
  {
    name: 'set_num_gates',
    description: 'Tell the device how many blast gates are installed.',
    input_schema: {
      type: 'object',
      properties: {
        num_gates: { type: 'integer', description: 'Number of blast gates (1–16).' }
      },
      required: ['num_gates']
    }
  }
];

module.exports = { SYSTEM_PROMPT, TOOLS };
