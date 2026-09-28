# extra_script.py
# Workaround for PlatformIO parallel-build race: SCons queues Mkdir and
# compile actions concurrently; GCC fires before the output directory exists,
# then fails writing its -MF dep file ("No such file or directory").
#
# Three-layer fix:
#   1. Serialise the build (-j1) so Mkdir always completes before compile.
#   2. Clear PlatformIO's dep-flag variable so GCC never tries to write .d files.
#   3. Pre-create the build root so SCons subdirectory Mkdir calls have a parent.

Import("env")
import os, re

# ── 1. Serialise the build ──────────────────────────────────────────────────
# NOTE: env.SetOption("num_jobs", 1) does NOT reliably take effect here on
# this PlatformIO/SCons version — verified empirically: the race still
# happened with it set. The actual serialization has to come from `-j 1` on
# the `pio run` command line itself (see dev.sh/deploy.sh), which does work.
# Left commented rather than removed so it's not silently re-added as a fix
# that looks right but isn't.
# env.SetOption("num_jobs", 1)

# ── Clear PlatformIO's dependency-file flag variable ────────────────────────
# Without -MMD / -MF, GCC doesn't try to write .d files at all.
# Incremental header-change detection is disabled; full recompiles on clean.
for dep_var in ("PIODEPFLAGS", "CCDEPFLAGS", "DEPFLAGS"):
    if dep_var in env:
        env[dep_var] = ""

# Also strip dep references from CCCOM / CXXCOM in case they're baked in.
for cmd_var in ("CCCOM", "CXXCOM"):
    if cmd_var not in env:
        continue
    cmd = env[cmd_var]
    if not isinstance(cmd, str):
        continue
    cmd = re.sub(r'\$[{(]?(?:PIODEPFLAGS|CCDEPFLAGS|DEPFLAGS)[)}]?', '', cmd)
    env[cmd_var] = cmd

# ── 3. Pre-create the build root ────────────────────────────────────────────
build_dir = env.subst("$BUILD_DIR")
if build_dir:
    os.makedirs(build_dir, exist_ok=True)


# ── Build stamp (2026-09-28) ─────────────────────────────────────────────────
# Every build says what it is: the git commit (+ when firmware/ or platformio.ini
# has uncommitted changes) and the build time. Added as defines to
# firmware/utils/BuildStamp.cpp ONLY, via a build middleware, so a fresh stamp
# recompiles that one file instead of every translation unit — a global define
# that changes every build would turn every flash into a full rebuild, which is
# the exact slowness this was written alongside fixing. See utils/BuildStamp.h.
import subprocess, datetime

def _dustgate_stamp():
    root = env.subst("$PROJECT_DIR")
    try:
        sha = subprocess.check_output(["git", "rev-parse", "--short=7", "HEAD"],
                                      cwd=root, stderr=subprocess.DEVNULL).decode().strip()
        dirty = subprocess.call(["git", "diff", "--quiet", "HEAD", "--", "firmware", "platformio.ini"],
                                cwd=root, stderr=subprocess.DEVNULL) != 0
        commit = sha + ("+" if dirty else "")
    except Exception:
        commit = "nogit"
    now = datetime.datetime.now()
    # __DATE__'s exact shape — "Sep  2 2026", day padded with a SPACE — because the
    # OLED (StatusScreenModel.h formatBuild) and the app's footer parse it.
    date = now.strftime("%b ") + ("%2d" % now.day) + now.strftime(" %Y")
    return commit, date, now.strftime("%H:%M:%S"), "%s %s" % (commit, now.strftime("%m%d-%H%M"))

_DG_COMMIT, _DG_DATE, _DG_TIME, _DG_FW = _dustgate_stamp()
print("DustGate build stamp: %s  (%s %s)" % (_DG_FW, _DG_DATE, _DG_TIME))

def _dustgate_stamp_middleware(env, node):
    # list(): SCons keeps CPPDEFINES as a deque here, which does not support +.
    return env.Object(node, CPPDEFINES=list(env.get("CPPDEFINES", [])) + [
        ("DUSTGATE_BUILD_COMMIT", env.StringifyMacro(_DG_COMMIT)),
        ("DUSTGATE_BUILD_DATE",   env.StringifyMacro(_DG_DATE)),
        ("DUSTGATE_BUILD_TIME",   env.StringifyMacro(_DG_TIME)),
        ("DUSTGATE_BUILD_FW",     env.StringifyMacro(_DG_FW)),
    ])

env.AddBuildMiddleware(_dustgate_stamp_middleware, "*BuildStamp.cpp")
