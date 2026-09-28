// See BuildStamp.h. The DUSTGATE_BUILD_* defines are added to THIS FILE ONLY by
// extra_script.py (a PlatformIO build middleware), so a new stamp each build
// recompiles this one file rather than the whole firmware.
#include "BuildStamp.h"

#ifndef DUSTGATE_BUILD_COMMIT
#define DUSTGATE_BUILD_COMMIT "nogit"
#endif
#ifndef DUSTGATE_BUILD_DATE
#define DUSTGATE_BUILD_DATE __DATE__
#endif
#ifndef DUSTGATE_BUILD_TIME
#define DUSTGATE_BUILD_TIME __TIME__
#endif
#ifndef DUSTGATE_BUILD_FW
#define DUSTGATE_BUILD_FW DUSTGATE_BUILD_COMMIT
#endif

namespace buildstamp {
const char* commit() { return DUSTGATE_BUILD_COMMIT; }
const char* date()   { return DUSTGATE_BUILD_DATE; }
const char* time()   { return DUSTGATE_BUILD_TIME; }
const char* fw()     { return DUSTGATE_BUILD_FW; }
} // namespace buildstamp
