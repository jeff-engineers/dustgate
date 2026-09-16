import type { Topology } from '@topology';

// The shop demo mode starts with — a REAL saved layout, exported from the bench
// device on 2026-09-16 (`jeff-s-shop-7.json`) rather than composed by hand.
//
// IT IS NOW JEFF'S ACTUAL SHOP, boards and all, laid out by him in the tool
// itself and exported straight back. That is a stronger version of the point
// below: bugs found here are bugs he will hit, and the board list is the one in
// his garage rather than seven invented hostnames.
//
// The brain is AT THE COLLECTOR, and that is load-bearing rather than a taste:
// `control.rf` carries no controllerId and firmware.ino builds the RF presser on
// whichever board is the primary, so the board keying the Rockler remote MUST be
// the primary. Same for the bin sensor: both live in firmware.ino and neither was
// ever moved into the node program, so a SECONDARY has neither however it is
// flashed. (That used to be recorded against a `xiao_c5_collector_node` env; the
// env is gone since 2026-09-16 — one pin map for every PWM board — but the gap it
// warned about is not.) A one-collector shop is a whole shop, so this is the
// supported shape.
//
// `dustgate-planer-sensor` DRIVES NOTHING. The 240 V planer has no plug to
// meter, so its clamp sits on a board of its own beside the machine
// (tool-sensing RFC §5.6), and a controller with no selectors is valid
// precisely so this can exist.
//
// That is the point of it. The hand-built seed it replaced was a tidy teaching
// example: one blower, one of each gate kind, everything named and plugged in.
// A real shop is lumpier, and the lumps are what the UI has to survive — which
// is why the two systems, the cross-system port and the half-paired tools below
// are worth more than a neater drawing would be:
//
//   TWO SYSTEMS. A cyclone with the machines on it, and a shop vacuum with a
//     six-outlet sliding gate. The live view's per-system blocks, the canvas's
//     row bands and the grey ground all have something to draw.
//   NO PORT CROSSES THE SEAM ANY MORE (2026-09-16). `p47`, the table saw's
//   overarm collector on the shop-vac system, was removed at jeff's request —
//   the two systems are now independent, which is what his shop actually is.
//   The cross-system run is still a supported shape; it simply is not seeded
//   here. The original note is kept below because the RULES it describes still
//   hold for anyone who draws one.
//
//   A PORT THAT CROSSES THE SEAM. `p47` was the table saw's overarm collector: it
//     stands in the shop-vac system while its machine lives on the cyclone. That
//     run is the only thing allowed to cross, and it is the case the grey dashed
//     line exists for — a seed without one left it untested by eye.
//   SIX PLUGS, FIVE MACHINES WITHOUT. Both halves of the plug row, the tray with
//     something in it, and tools that are honestly switched on by hand.
//   THREE BOARDS, one of them paired and driving nothing.
//
// Machine names were HEALED on the way in: this shop was drawn entirely on the
// canvas, which until 2026-08-22 wrote a rename to the port and not the machine,
// so every machine in the export is still called "New tool". healMachineNames()
// in shop-doc.ts does the same repair for anyone else's saved shop on read; here
// it is baked in, so the fixture is a fixture and not a bug preserved in amber.
//
// Verified against validateShop (shared/device-model/shop.js), and shopReadiness
// says READY — so `/` lands on the live view, which is what demo mode is for.
// Keep both true if you edit: an invalid seed leaves demo mode with no shop at
// all, and an unready one drops every visitor into the layout tool instead.
export const DEMO_TOPOLOGY: Topology = {
  schemaVersion: 2,
  name: "Jeff's Shop",
  controllers: [
    {
      id: "primary",
      role: "primary",
      name: "Cyclone board",
      board: "xiao_c5",
      link: {
        transport: "wifi-ws",
        host: "dustgate.local"
      }
    },
    {
      id: "dustgate-planer",
      role: "secondary",
      name: "Planer gate",
      board: "xiao_c5",
      link: {
        transport: "wifi-ws",
        host: "dustgate-planer"
      }
    },
    {
      id: "dustgate-tablesaw",
      role: "secondary",
      name: "Table saw gate",
      board: "xiao_c5",
      link: {
        transport: "wifi-ws",
        host: "dustgate-tablesaw"
      }
    },
    {
      id: "dustgate-planer-sensor",
      role: "secondary",
      name: "Planer sensor",
      board: "xiao_c5",
      link: {
        transport: "wifi-ws",
        host: "dustgate-planer-sensor"
      }
    },
    {
      id: "dustgate-drum-sander",
      role: "secondary",
      name: "Drum sander gate",
      board: "xiao_c5",
      link: {
        transport: "wifi-ws",
        host: "dustgate-drum-sander"
      }
    },
    {
      id: "dustgate-miter-saw",
      role: "secondary",
      name: "Miter Saw Gate",
      board: "xiao_c5",
      link: {
        transport: "wifi-ws",
        host: "dustgate-miter-saw"
      }
    },
    {
      id: "dustgate-routertable-jointer",
      role: "secondary",
      name: "Router table & jointer",
      board: "xiao_c5",
      link: {
        transport: "wifi-ws",
        host: "dustgate-routertable-jointer"
      }
    },
    {
      id: "dustgate-slider-1",
      role: "secondary",
      name: "ShopVac node",
      board: "xiao_c5",
      drives: "linear",
      link: {
        transport: "wifi-ws",
        host: "dustgate-slider-1"
      }
    }
  ],
  systems: [
    {
      id: "system-1",
      name: "Dust collection",
      elements: [
        {
          id: "dc",
          type: "collector",
          name: "Cyclone",
          control: {
            rf: {
              address: 94
            },
            offDelayMs: 8000
          },
          sensor: {
            ct: {
              channel: 0
            }
          },
          bin: {
            sensor: {
              kind: "threshold"
            }
          }
        },
        {
          id: "wye2",
          type: "junction",
          name: "Wye"
        },
        {
          id: "sel4",
          type: "selector",
          name: "Ball valve",
          controllerId: "dustgate-planer",
          kind: "servoGate",
          states: [
            {
              id: "open",
              isClosed: false,
              offsetDeg: 0
            },
            {
              id: "closed",
              isClosed: true,
              offsetDeg: 90
            }
          ],
          branches: [
            {
              id: "b1",
              opensState: "open",
              role: "tool"
            }
          ],
          servo: {
            channel: 0,
            detented: true,
            referenceAngle: 0
          }
        },
        {
          id: "tool6",
          type: "tool",
          name: "Planer",
          machineId: "tool6"
        },
        {
          id: "wye7",
          type: "junction",
          name: "Wye"
        },
        {
          id: "sel9",
          type: "selector",
          name: "Ball valve",
          controllerId: "dustgate-tablesaw",
          kind: "servoGate",
          states: [
            {
              id: "open",
              isClosed: false,
              offsetDeg: 0
            },
            {
              id: "closed",
              isClosed: true,
              offsetDeg: 90
            }
          ],
          branches: [
            {
              id: "b1",
              opensState: "open",
              role: "tool"
            }
          ],
          servo: {
            channel: 0,
            detented: true,
            referenceAngle: 0
          }
        },
        {
          id: "tool11",
          type: "tool",
          name: "Table Saw",
          machineId: "tool11"
        },
        {
          id: "wye13",
          type: "junction",
          name: "Wye"
        },
        {
          id: "sel15",
          type: "selector",
          name: "Ball valve",
          controllerId: "dustgate-drum-sander",
          kind: "servoGate",
          states: [
            {
              id: "open",
              isClosed: false,
              offsetDeg: 0
            },
            {
              id: "closed",
              isClosed: true,
              offsetDeg: 90
            }
          ],
          branches: [
            {
              id: "b1",
              opensState: "open",
              role: "tool"
            }
          ],
          servo: {
            channel: 0,
            detented: true,
            referenceAngle: 0
          }
        },
        {
          id: "tool17",
          type: "tool",
          name: "Drum Sander",
          machineId: "tool17"
        },
        {
          id: "tool18",
          type: "tool",
          name: "Miter Saw",
          machineId: "tool18"
        },
        {
          id: "sel19",
          type: "selector",
          name: "Ball valve",
          controllerId: "dustgate-miter-saw",
          kind: "servoGate",
          states: [
            {
              id: "open",
              isClosed: false,
              offsetDeg: 0
            },
            {
              id: "closed",
              isClosed: true,
              offsetDeg: 90
            }
          ],
          branches: [
            {
              id: "b1",
              opensState: "open",
              role: "tool"
            }
          ],
          servo: {
            channel: 0,
            detented: true,
            referenceAngle: 0
          }
        },
        {
          id: "wye20",
          type: "junction",
          name: "Wye"
        },
        {
          id: "sel27",
          type: "selector",
          name: "Manifold",
          controllerId: "dustgate-routertable-jointer",
          kind: "servoManifold",
          states: [
            {
              id: "left",
              isClosed: false,
              offsetDeg: 0
            },
            {
              id: "closed",
              isClosed: true,
              offsetDeg: 80
            },
            {
              id: "right",
              isClosed: false,
              offsetDeg: 161
            }
          ],
          branches: [
            {
              id: "mL",
              opensState: "left",
              role: "tool"
            },
            {
              id: "mR",
              opensState: "right",
              role: "tool"
            }
          ],
          servo: {
            channel: 0,
            detented: true,
            referenceAngle: 0
          }
        },
        {
          id: "tool31",
          type: "tool",
          name: "Jointer",
          machineId: "tool31"
        },
        {
          id: "tool32",
          type: "tool",
          name: "Router Table",
          machineId: "tool32"
        }
      ],
      ducts: [
        {
          child: "wye2",
          parent: "dc"
        },
        {
          child: "sel4",
          parent: "wye2"
        },
        {
          child: "tool6",
          parent: "sel4",
          parentBranch: "b1"
        },
        {
          child: "wye7",
          parent: "wye2"
        },
        {
          child: "sel9",
          parent: "wye7"
        },
        {
          child: "tool11",
          parent: "sel9",
          parentBranch: "b1"
        },
        {
          child: "wye13",
          parent: "wye7"
        },
        {
          child: "sel15",
          parent: "wye13"
        },
        {
          child: "tool17",
          parent: "sel15",
          parentBranch: "b1"
        },
        {
          child: "tool18",
          parent: "sel19",
          parentBranch: "b1"
        },
        {
          child: "sel19",
          parent: "wye20"
        },
        {
          child: "wye20",
          parent: "wye13"
        },
        {
          child: "sel27",
          parent: "wye20"
        },
        {
          child: "tool31",
          parent: "sel27",
          parentBranch: "mL"
        },
        {
          child: "tool32",
          parent: "sel27",
          parentBranch: "mR"
        }
      ]
    },
    {
      id: "s2",
      elements: [
        {
          id: "dc1",
          type: "collector",
          name: "Shop Vacuum"
        },
        {
          id: "sel36",
          type: "selector",
          name: "Sliding gate",
          controllerId: "dustgate-slider-1",
          kind: "linear",
          states: [
            {
              id: "home",
              isClosed: true,
              positionMm: 0
            },
            {
              id: "s1",
              isClosed: false,
              positionMm: 1
            },
            {
              id: "s2",
              isClosed: false,
              positionMm: 83.9
            },
            {
              id: "s3",
              isClosed: false,
              positionMm: 166.8
            },
            {
              id: "s4",
              isClosed: false,
              positionMm: 249.7
            },
            {
              id: "s5",
              isClosed: false,
              positionMm: 332.6
            },
            {
              id: "s6",
              isClosed: false,
              positionMm: 415.5
            }
          ],
          branches: [
            {
              id: "b1",
              opensState: "s1",
              role: "tool"
            },
            {
              id: "b2",
              opensState: "s2",
              role: "tool"
            },
            {
              id: "b3",
              opensState: "s3",
              role: "tool"
            },
            {
              id: "b4",
              opensState: "s4",
              role: "tool"
            },
            {
              id: "b5",
              opensState: "s5",
              role: "tool"
            },
            {
              id: "b6",
              opensState: "s6",
              role: "unassigned"
            }
          ],
          linear: {
            channel: 0,
            calibration: {
              stepsPerMm: 40.44657863145258,
              measuredSpanSteps: 16846,
              homeIsMaxEndstop: false,
              manifoldModel: "rockler-2.5"
            }
          }
        },
        {
          id: "tool38",
          type: "tool",
          name: "Bandsaw",
          machineId: "tool38"
        },
        {
          id: "tool40",
          type: "tool",
          name: "Drill Press",
          machineId: "tool40"
        },
        {
          id: "tool42",
          type: "tool",
          name: "Small Saw",
          machineId: "tool42"
        },
        {
          id: "tool44",
          type: "tool",
          name: "V. Belt Sand",
          machineId: "tool44"
        },
        {
          id: "tool46",
          type: "tool",
          name: "H. Belt Sand",
          machineId: "tool46"
        }
      ],
      ducts: [
        {
          child: "sel36",
          parent: "dc1"
        },
        {
          child: "tool38",
          parent: "sel36",
          parentBranch: "b1"
        },
        {
          child: "tool40",
          parent: "sel36",
          parentBranch: "b2"
        },
        {
          child: "tool42",
          parent: "sel36",
          parentBranch: "b3"
        },
        {
          child: "tool44",
          parent: "sel36",
          parentBranch: "b4"
        },
        {
          child: "tool46",
          parent: "sel36",
          parentBranch: "b5"
        }
      ]
    }
  ],
  machines: [
    {
      id: "tool6",
      name: "Planer",
      sensor: {
        ct: {
          controllerId: "dustgate-planer-sensor",
          channel: 0
        }
      }
    },
    {
      id: "tool11",
      name: "Table Saw",
      sensor: {
        outlet: {
          gen: 2,
          ip: "192.168.87.30",
          host: "shellyplugus-tablesaw",
          thresholdW: 50
        }
      }
    },
    {
      id: "tool17",
      name: "Drum Sander"
    },
    {
      id: "tool18",
      name: "Miter Saw",
      sensor: {
        outlet: {
          gen: 2,
          ip: "192.168.87.27",
          thresholdW: 50,
          host: "shellyplugus-bandsaw"
        }
      }
    },
    {
      id: "tool31",
      name: "Jointer",
      sensor: {
        outlet: {
          gen: 2,
          ip: "192.168.87.53",
          thresholdW: 50,
          host: "ShellyPlugUSG4-0F0E99229E66"
        }
      }
    },
    {
      id: "tool32",
      name: "Router Table",
      sensor: {
        outlet: {
          gen: 2,
          ip: "192.168.87.75",
          host: "ShellyPlugUSG4-998E49CDEBAD",
          thresholdW: 50
        }
      }
    },
    {
      id: "tool38",
      name: "Bandsaw"
    },
    {
      id: "tool40",
      name: "Drill Press"
    },
    {
      id: "tool42",
      name: "Small Saw"
    },
    {
      id: "tool44",
      name: "V. Belt Sand"
    },
    {
      id: "tool46",
      name: "H. Belt Sand"
    }
  ],
  devices: [],
  ui: {
    layout: {
      dc: {
        col: 0,
        row: 0
      },
      dc1: {
        col: 0,
        row: 7
      },
      wye2: {
        col: 1,
        row: 0
      },
      sel4: {
        col: 1,
        row: 1
      },
      tool6: {
        col: 1,
        row: 2
      },
      wye7: {
        col: 3,
        row: 0
      },
      sel9: {
        col: 3,
        row: 1
      },
      tool11: {
        col: 3,
        row: 2
      },
      wye13: {
        col: 5,
        row: 0
      },
      sel15: {
        col: 5,
        row: 1
      },
      tool17: {
        col: 5,
        row: 2
      },
      tool18: {
        col: 6,
        row: 5
      },
      sel19: {
        col: 7,
        row: 4
      },
      wye20: {
        col: 7,
        row: 3
      },
      sel27: {
        col: 3,
        row: 4
      },
      tool31: {
        col: 2,
        row: 5
      },
      tool32: {
        col: 4,
        row: 5
      },
      sel36: {
        col: 1,
        row: 7
      },
      tool38: {
        col: 1,
        row: 8
      },
      tool40: {
        col: 2,
        row: 8
      },
      tool42: {
        col: 3,
        row: 8
      },
      tool44: {
        col: 4,
        row: 8
      },
      tool46: {
        col: 5,
        row: 8
      }
    },
    wiring: {
      boards: {
        primary: {
          col: 0,
          row: 1
        },
        "dustgate-slider-1": {
          col: 7,
          row: 7
        },
        "dustgate-planer": {
          col: 2,
          row: 1
        },
        "dustgate-tablesaw": {
          col: 4,
          row: 1
        },
        "dustgate-planer-sensor": {
          col: 2,
          row: 2
        },
        "dustgate-drum-sander": {
          col: 6,
          row: 1
        },
        "dustgate-miter-saw": {
          col: 8,
          row: 4
        },
        "dustgate-routertable-jointer": {
          col: 5,
          row: 4
        }
      }
    }
  }
};
