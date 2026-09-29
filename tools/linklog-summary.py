#!/usr/bin/env python3
"""Summarise a primary's link log — `bash dev.sh linklog` runs this.

The log (firmware/utils/LinkLog.h) is one JSON object per line. This answers the
questions a shop soak exists to answer, per node:

  - how many times was it cut off, and for how long (outages)
  - when it came back, had the NODE rebooted meanwhile, and why (power vs crash)
  - how many WiFi rejoins the primary did, and did each one bring the node back
  - did the primary itself reboot, and why

Nothing here is clever: the firmware already measured the durations (link_up
carries downMs), so this only counts and sorts. Lines it does not understand are
skipped, so an older or newer firmware's log still summarises.
"""
import json
import sys
from collections import defaultdict
from datetime import datetime, timezone


def when(e):
    """Wall time if the board had NTP, else boot#/uptime — the log's own fallback."""
    ts = e.get("ts") or 0
    if ts:
        return datetime.fromtimestamp(ts).strftime("%a %d %b %H:%M:%S")
    return f"boot {e.get('boot', '?')} +{int(e.get('up', 0)) // 1000}s"


def fmt_ms(ms):
    s = int(ms) // 1000
    if s < 120:
        return f"{s}s"
    if s < 7200:
        return f"{s // 60}m{s % 60:02d}s"
    return f"{s // 3600}h{(s % 3600) // 60:02d}m"


# A reset reason that means the NODE's power went away, which on a CT node is the
# tool being switched off at the wall — normal. Anything else is worth a look.
POWER = {"poweron", "brownout", "pwr_glitch"}


def main(paths):
    events = []
    for p in paths:
        try:
            with open(p, encoding="utf-8", errors="replace") as f:
                for line in f:
                    line = line.strip()
                    if not line.startswith("{"):
                        continue
                    try:
                        events.append(json.loads(line))
                    except json.JSONDecodeError:
                        continue
        except FileNotFoundError:
            continue
    if not events:
        print("  (the log is empty)")
        return 0

    # Order by boot, then uptime — correct with or without a wall clock.
    events.sort(key=lambda e: (e.get("boot", 0), e.get("up", 0)))
    first, last = events[0], events[-1]
    print(f"  {len(events)} events, {when(first)}  →  {when(last)}")

    boots = [e for e in events if e.get("ev") == "boot"]
    print(f"\n  PRIMARY: {len(boots)} boot(s)")
    for e in boots:
        rst = e.get("rst", "?")
        flag = "" if rst in ("usb", "poweron", "sw", "ext") else "   ← worth a look"
        print(f"    {when(e):<28} reset: {rst}{flag}")

    wdrops = [e for e in events if e.get("ev") == "wifi_down"]
    wups = [e for e in events if e.get("ev") == "wifi_up"]
    aps = sorted({e.get("bssid", "?") for e in wups})
    print(f"\n  WIFI: {len(wdrops)} drop(s), {len(wups)} join(s); access points seen: {', '.join(aps) or '-'}")
    for e in wdrops[:20]:
        print(f"    {when(e):<28} dropped, reason {e.get('reason', '?')}")
    if len(wdrops) > 20:
        print(f"    … {len(wdrops) - 20} more")

    nodes = sorted({e["node"] for e in events if e.get("node")})
    for n in nodes:
        mine = [e for e in events if e.get("node") == n]
        ups = [e for e in mine if e.get("ev") == "link_up" and not e.get("first")]
        downs = [e for e in mine if e.get("ev") == "link_down"]
        rejoins = [e for e in mine if e.get("ev") == "rejoin"]
        refused = [e for e in mine if e.get("ev") == "refused"]
        print(f"\n  NODE {n}: {len(downs)} drop(s), {len(ups)} recover(ies), "
              f"{len(rejoins)} rejoin(s){f', {len(refused)} REFUSED' if refused else ''}")
        if ups:
            durs = sorted(e.get("downMs", 0) for e in ups)
            print(f"    outage: typical {fmt_ms(durs[(len(durs) - 1) // 2])}, longest {fmt_ms(durs[-1])}")
        # DID THE NODE REBOOT? Compare its uptime with how long ago the link was
        # last GOOD (the previous link_up, same primary boot) — not with downMs.
        # The primary only notices a dead link after the heartbeat times out, so
        # downMs (~2 s) badly under-states how long a node has really been gone:
        # a node that rebooted 10 s ago reads "down 2 s", and comparing against
        # that labelled every brownout "the network dropped it" (2026-09-29).
        prev_ok = {}
        for e in sorted(mine, key=lambda x: (x.get("boot", 0), x.get("up", 0))):
            if e.get("ev") == "link_up":
                b = e.get("boot")
                e["_since_ok_s"] = (e.get("up", 0) - prev_ok[b]) / 1000 if b in prev_ok else None
                prev_ok[b] = e.get("up", 0)
        for e in ups:
            down_s = e.get("downMs", 0) / 1000
            since_ok = e.get("_since_ok_s")
            gone_s = since_ok if since_ok is not None else down_s
            node_up = e.get("nodeUpS", -1)
            rst = e.get("nodeRst", "")
            if node_up is not None and node_up >= 0 and node_up < gone_s:
                why = "node REBOOTED" + (f" ({rst})" if rst else "")
                if rst == "unknown":
                    # Node firmware before 2026-09-29 reported the C5's power-glitch
                    # reset as "unknown" (utils/ResetReason.h) — so on those nodes
                    # this is most likely POWER, not a crash.
                    why += "   ← cause not named (older node firmware: likely a power glitch)"
                elif rst and rst not in POWER:
                    why += "   ← a crash, not a power cut"
            elif node_up is not None and node_up >= 0:
                why = "node stayed up — the NETWORK dropped it"
            else:
                why = "node too old to say"
            hollow = e.get("hollow", 0)
            print(f"    {when(e):<28} back after {fmt_ms(e.get('downMs', 0)):>8}  — {why}"
                  f"{f', {hollow} hollow' if hollow else ''}")
        # A rejoin "worked" when the node is back within three minutes of it.
        for r in rejoins:
            back = next((u for u in ups if u.get("boot") == r.get("boot")
                         and 0 <= u.get("up", 0) - r.get("up", 0) <= 180000), None)
            verdict = (f"worked — back {fmt_ms(back['up'] - r['up'])} later" if back
                       else "DID NOT bring it back within 3 min")
            print(f"    rejoin #{r.get('n', '?')} at {when(r)}: {verdict}")
        if downs and len(ups) < len(downs):
            print(f"    ⚠ {len(downs) - len(ups)} drop(s) never recovered in this log")

    hourly = [e for e in events if e.get("ev") == "hourly"]
    if hourly:
        e = hourly[-1]
        print(f"\n  LAST HOURLY ({when(e)}): {e.get('nodes', '')}  rssi {e.get('rssi', '?')}  "
              f"ap {e.get('bssid', '?')}  heap {e.get('heap', '?')}")
    dropped = sum(e.get("lines", 0) for e in events if e.get("ev") == "dropped")
    if dropped:
        print(f"\n  ⚠ {dropped} line(s) were dropped by a full queue on the board")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
