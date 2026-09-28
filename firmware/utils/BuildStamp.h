// =============================================================================
// utils/BuildStamp.h — what this board is running: commit + build time.
//
// WHY (2026-09-28): "what did I flash last?" had no answer. Every board said
// version "1.0.0" and always had; `built` came from __DATE__/__TIME__, which is
// the time ONE translation unit last compiled — a stale date whenever that file
// was not the one that changed. Now extra_script.py stamps every build, into
// BuildStamp.cpp ALONE (a per-file define), so the stamp is always this build's
// and only one tiny file recompiles for it.
//
//   commit() "3944308" or "3944308+" — `+` = built with uncommitted firmware changes
//   date()   "Sep 28 2026"  — __DATE__'s exact format (space-padded day), because
//   time()   "17:40:12"       the OLED and the app's footer already parse that
//   fw()     "3944308+ 0928-1740" — compact, ≤ 23 chars: the WELCOME `fw` field,
//                               which the Boards page shows per node
//
// A build without git or without the pre-script (a bench env, a bare compile)
// reads "nogit" and the compile time — never empty, never a lie that looks real.
// =============================================================================
#pragma once

namespace buildstamp {
const char* commit();
const char* date();
const char* time();
const char* fw();
} // namespace buildstamp
