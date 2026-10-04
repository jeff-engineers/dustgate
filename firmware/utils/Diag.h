// =============================================================================
// utils/Diag.h — where the memory and the CPU are going, on a running board.
//
// WHY (2026-10-04). Internal RAM is the limiting resource on the brain and every
// decision about moving things to PSRAM, trimming stacks or shrinking buffers was
// being made from one number (free heap) and from reasoning. This gives the other
// half: the whole heap picture by capability, and every FreeRTOS task with its
// stack headroom and share of the CPU. Read it on a fully loaded shop, not on a
// bench with one node, and decide from that.
//
//   GET /api/info   gains the heap breakdown (writeHeap)
//   GET /api/tasks  every task (writeTasks)
//   serial: `heap`, `tasks`
//
// `stackFreeMin` is the LOWEST the task's unused stack has ever been, in BYTES (ESP-IDF's
// FreeRTOS reports bytes, not words). A task that has never run its deep paths looks
// healthier than it is, so read it after the system has done everything once.
// `cpuPct` is the task's share of CPU time SINCE BOOT, not a recent rate.
// =============================================================================
#pragma once
#include <ArduinoJson.h>
#include <Print.h>

namespace diag {

// Internal RAM and PSRAM: total, free, lowest-ever free, largest free block.
void writeHeap(JsonObject out);

// One object per task: name, state (R/B/S/D/X), priority, core, stackFreeMin, cpuPct.
// Sorted by stackFreeMin ascending, so the tightest stack is first.
void writeTasks(JsonArray out);

// The same, as text for a serial console.
void printHeap(Print& p);
void printTasks(Print& p);

} // namespace diag
