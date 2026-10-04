// utils/Diag.cpp — see Diag.h.
#include "Diag.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <vector>

namespace diag {

static void heapFor(JsonObject o, uint32_t caps) {
    o["total"]   = (uint32_t)heap_caps_get_total_size(caps);
    o["free"]    = (uint32_t)heap_caps_get_free_size(caps);
    o["min"]     = (uint32_t)heap_caps_get_minimum_free_size(caps);
    o["largest"] = (uint32_t)heap_caps_get_largest_free_block(caps);
}

void writeHeap(JsonObject out) {
    heapFor(out.createNestedObject("internal"), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heapFor(out.createNestedObject("psram"),    MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT);
}

namespace {
struct Row { String name; char state; uint32_t prio; int core; uint32_t stackFreeMin; float cpuPct; };

std::vector<Row> snapshot() {
    std::vector<Row> rows;
    const UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t* st = (TaskStatus_t*)heap_caps_malloc(n * sizeof(TaskStatus_t),
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!st) return rows;
    uint32_t total = 0;
    const UBaseType_t got = uxTaskGetSystemState(st, n, &total);
    for (UBaseType_t i = 0; i < got; i++) {
        char s = '?';
        switch (st[i].eCurrentState) {
            case eRunning: s = 'X'; break; case eReady: s = 'R'; break;
            case eBlocked: s = 'B'; break; case eSuspended: s = 'S'; break;
            case eDeleted: s = 'D'; break; default: break;
        }
        Row r;
        r.name = st[i].pcTaskName;
        r.state = s;
        r.prio = st[i].uxCurrentPriority;
        r.core = (st[i].xCoreID > 8) ? -1 : (int)st[i].xCoreID;   // tskNO_AFFINITY reads as -1
        r.stackFreeMin = st[i].usStackHighWaterMark;
        r.cpuPct = total ? 100.0f * st[i].ulRunTimeCounter / total : 0.0f;
        rows.push_back(r);
    }
    free(st);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.stackFreeMin < b.stackFreeMin; });
    return rows;
}
} // namespace

void writeTasks(JsonArray out) {
    for (const Row& r : snapshot()) {
        JsonObject o = out.createNestedObject();
        o["name"]         = r.name;                 // copied (ArduinoJson copies a String)
        o["state"]        = String(r.state);
        o["prio"]         = r.prio;
        o["core"]         = r.core;
        o["stackFreeMin"] = r.stackFreeMin;
        o["cpuPct"]       = (float)((int)(r.cpuPct * 10 + 0.5f)) / 10.0f;
    }
}

void printHeap(Print& p) {
    auto line = [&](const char* name, uint32_t caps) {
        p.printf("  %-9s total %7u  free %7u  min %7u  largest %7u\n", name,
                 (unsigned)heap_caps_get_total_size(caps), (unsigned)heap_caps_get_free_size(caps),
                 (unsigned)heap_caps_get_minimum_free_size(caps), (unsigned)heap_caps_get_largest_free_block(caps));
    };
    p.println(F("[HEAP] bytes"));
    line("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    line("psram",    MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT);
}

void printTasks(Print& p) {
    p.println(F("[TASKS] tightest stack first; stackFreeMin in bytes, cpu since boot"));
    p.println(F("  name              st prio core  stackFreeMin   cpu%"));
    for (const Row& r : snapshot())
        p.printf("  %-17s %c  %4u %4d  %12u  %5.1f\n", r.name.c_str(), r.state,
                 (unsigned)r.prio, r.core, (unsigned)r.stackFreeMin, r.cpuPct);
}

} // namespace diag
