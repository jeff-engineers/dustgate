// =============================================================================
// utils/JsonAlloc.h — JSON documents that live in PSRAM when there is some.
//
// WHY. ArduinoJson allocates its whole pool up front. TopologyStore::save() built a
// 24 KB document to validate a layout that is ~3 KB, on every save, from the
// board's INTERNAL heap — the one that has ~30 KB free with four nodes linked.
// Each save in the app dipped the heap by 10–20 KB (the `[HEAP] LOW` lines in the
// log of 2026-10-03 sit directly after `[API] topology saved`), and the runtime then
// held a second copy while the first was freed. PSRAM has 8 MB doing nothing.
//
// Use BigJsonDocument for anything larger than a few hundred bytes that is built
// and thrown away, or kept (the adopted layout). Small, short-lived documents stay
// on the stack as StaticJsonDocument. Falls back to internal RAM if there is no
// PSRAM, so a board without it still works. On the host it IS DynamicJsonDocument.
// =============================================================================
#pragma once
#include <ArduinoJson.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
struct BigJsonAlloc {
    void* allocate(size_t n) {
        void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return p ? p : heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    void deallocate(void* p) { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) {
        void* q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return q ? q : heap_caps_realloc(p, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
};
using BigJsonDocument = BasicJsonDocument<BigJsonAlloc>;
#else
using BigJsonDocument = DynamicJsonDocument;
#endif
