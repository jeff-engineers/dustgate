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
