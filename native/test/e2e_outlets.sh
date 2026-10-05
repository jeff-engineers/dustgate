#!/bin/bash
# The plug screens: probe an address, rename a plug, hand it back, and sweep the subnet for plugs — against a fake Shelly.
cd "$(dirname "$0")/.." || exit 2
B=build/dustgate-brain; PL=build/fakeplug; P=18084; PP=18085; T=$(mktemp -d); fail=0; K=testkey
export DUSTGATE_STATE="$T/state"   # never the real ~/.dustgate: a test brain must not inherit a layout or pairings
trap 'kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
api() { curl -s -m 20 -H "X-Api-Key: $K" "$@"; }
echo 240 > "$T/power"
$PL $PP "$T/power" > /dev/null 2>&1 &
$B --id plugscreen --key $K --port $P --ip 127.0.0.1 --broadcast 127.255.255.255 --plug-port $PP > "$T/brain.log" 2>&1 &
sleep 0.6
echo "native plug screens"
echo '{"ip":"127.0.0.1"}' > "$T/ping.json"
api -X POST --data @$T/ping.json localhost:$P/api/outlets/ping > "$T/row.json"
check "pinging an address answers the picker's row: reachable, a Shelly, its draw" 'python3 -c "
import json; r=json.load(open(\"$T/row.json\")); assert r[\"reachable\"] and r[\"kind\"]==\"shelly\" and r[\"powerW\"]>200, r"'
check "...and says nobody owns it, so it can be picked" 'python3 -c "
import json; r=json.load(open(\"$T/row.json\")); assert r[\"claim\"]==\"unclaimed\" and r[\"pickable\"], r"'
echo '{"ip":"127.0.0.1","label":"Router saw"}' > "$T/name.json"
check "renaming an unclaimed plug writes the bare label" '[ "$(api -X POST --data @$T/name.json localhost:$P/api/outlets/name | python3 -c "import sys,json;d=json.load(sys.stdin);print(d[\"ok\"],d[\"name\"])")" = "True Router saw" ]'
check "...and the plug now carries it" '[ "$(cat $T/power.name)" = "Router saw" ]'
check "releasing a plug we never claimed reports it honestly" 'api -X POST --data @$T/ping.json localhost:$P/api/outlets/release | grep -q "\"ok\":true"'
echo '{"ip":"127.0.0.2"}' > "$T/ping2.json"
check "an address with no plug is reported unreachable, not an error" 'api -X POST --data @$T/ping2.json localhost:$P/api/outlets/ping | grep -q "\"reachable\":false"'
check "a takeover is accepted (the write itself is in e2e_push.sh)" 'api -X POST --data @$T/ping.json localhost:$P/api/outlets/takeover | grep -q "\"ok\":true"'
# the sweep: 127.0.0.1-254 knocked, the one plug found
api -X POST localhost:$P/api/outlets/sweep >/dev/null
for i in $(seq 1 60); do api localhost:$P/api/outlets/sweep | grep -q '"running":false' && break; sleep 0.5; done
check "a sweep finishes having scanned the subnet" 'api localhost:$P/api/outlets/sweep | python3 -c "import sys,json;d=json.load(sys.stdin);assert d[\"everRan\"] and not d[\"running\"] and d[\"scanned\"]>=254, d"'
check "...and found the plug at 127.0.0.1 and nothing else" 'api localhost:$P/api/outlets/sweep | python3 -c "import sys,json;d=json.load(sys.stdin);assert [f[\"ip\"] for f in d[\"found\"]]==[\"127.0.0.1\"], d[\"found\"]"'
check "discover lists what the sweep found" 'api localhost:$P/api/outlets/discover | grep -q "127.0.0.1"'
[ $fail = 0 ] && echo "all native plug-screen checks passed" || { echo "--- brain log"; tail -30 "$T/brain.log"; }
exit $fail
