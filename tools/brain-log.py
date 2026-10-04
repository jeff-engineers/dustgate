#!/usr/bin/env python3
"""Follow the brain's serial output over WiFi — `bash dev.sh log` runs this.

Polls GET /api/serial (utils/SerialLog.h on the board) once a second with the
cursor the last reply handed back, prints what is new, and saves it to
.monitor-logs/brainlog-<time>.log as it goes — the same text a USB monitor shows,
with no cable, so a test can be read while it runs.

Usage: brain-log.py <host> <api-key> <logfile> [--once]
  --once   print what the board still holds and exit, instead of following.

Two things it says out loud rather than hide: a GAP (the board's 32 KB ring
wrapped before we caught up) and a RESTART (X-Serial-Boot changed).
"""
import sys
import time
import urllib.request
import urllib.error
from datetime import datetime


def fetch(host, key, cursor):
    req = urllib.request.Request(f"http://{host}/api/serial?from={cursor}",
                                 headers={"X-Api-Key": key})
    with urllib.request.urlopen(req, timeout=5) as r:
        h = r.headers
        return (r.read().decode("utf-8", errors="replace"),
                int(h.get("X-Serial-Start", "0")), int(h.get("X-Serial-Next", "0")),
                h.get("X-Serial-Boot", ""))


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    host, key, path = sys.argv[1], sys.argv[2], sys.argv[3]
    once = "--once" in sys.argv[4:]
    cursor, boot, down_since = 0, None, None
    pending = ""   # a line the board hasn't finished yet

    with open(path, "a", encoding="utf-8") as log:
        def out(text):
            sys.stdout.write(text)
            sys.stdout.flush()
            log.write(text)
            log.flush()

        def note(msg):
            out(f"\n── {datetime.now():%H:%M:%S} {msg}\n")

        while True:
            try:
                text, start, nxt, b = fetch(host, key, cursor)
            except (urllib.error.URLError, OSError, ValueError) as e:
                if once:
                    print(f"✗ {host}: {e}", file=sys.stderr)
                    return 1
                if down_since is None:
                    down_since = time.time()
                    note(f"can't reach {host} ({getattr(e, 'reason', e)}) — retrying")
                time.sleep(3)
                continue
            if down_since is not None:
                note(f"{host} answering again after {int(time.time() - down_since)}s")
                down_since = None
            if boot is not None and b != boot:
                note("BRAIN RESTARTED — what follows is from the new boot")
                cursor, pending = 0, ""
                boot = b
                continue
            if boot is not None and start > cursor:
                note(f"{start - cursor} bytes missed — the board's buffer wrapped first")
            boot = b
            cursor = nxt
            if text:
                # Stamp each COMPLETE line with when it reached us; hold a partial
                # one until the rest arrives, so a stamp never lands mid-line.
                pending += text
                *lines, pending = pending.split("\n")
                for ln in lines:
                    out(f"{datetime.now():%H:%M:%S} {ln.rstrip(chr(13))}\n")
            # --once means "everything the board still holds", and the board hands out
            # 8 KB at a time: stop only when a fetch comes back short. It used to stop
            # after the first chunk, so `--once` showed the OLDEST 8 KB and never the
            # recent lines it was run to read (2026-10-04).
            if once and len(text) >= 8000:
                continue
            if once:
                if pending:
                    out(f"{datetime.now():%H:%M:%S} {pending}\n")
                return 0
            # Drain a backlog at full speed; idle at one poll a second.
            time.sleep(0.05 if len(text) >= 8000 else 1.0)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
