#!/bin/bash
# The brain polls a tool's plug itself (the ESP32's own Shelly driver, over a native HTTP client) and routes on it.
# One fake node owns the gate; a fake Shelly on loopback stands in for the tool's plug. Writing watts to a file runs the tool.
cd "$(dirname "$0")/.." || exit 2
B=build/dustgate-brain; F=build/fakenode; PL=build/fakeplug; P=18081; PP=18082; T=$(mktemp -d); fail=0
export DUSTGATE_STATE="$T/state"   # never the real ~/.dustgate: a test brain must not inherit a layout or pairings
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
K=testkey
echo 0 > "$T/power"
$PL $PP "$T/power" > "$T/plug.log" 2>&1 &
$B --id plugbrain --key $K --port $P --ip 127.0.0.1 --broadcast 127.255.255.255 --plug-port $PP --pair fake1 > "$T/brain.log" 2>&1 &
sleep 0.5
$F 127.0.0.1 $P fake1 20 > "$T/f1.log" 2>&1 &
python3 - "$T/layout.json" <<'PY'
import json,sys
d=json.load(open('../firmware/test/fixtures/twoGates.json'))
for e in d['systems'][0]['elements']:
    if e['id']=='gate1': e['controllerId']='fake1'
    if e['id']=='gate2': e['controllerId']='fake1'
d['machines'][0]['sensor']['outlet']['ip']='127.0.0.1'   # toolX's plug is the fake Shelly
del d['machines'][1]['sensor']                           # toolY has none: nothing else to time out
json.dump(d,open(sys.argv[1],'w'))
PY
echo "native plug polling"
for i in $(seq 1 30); do curl -s -m 2 -H "X-Api-Key: $K" localhost:$P/api/nodes | grep -q '"online":true' && break; sleep 0.3; done
curl -s -m 3 -H "X-Api-Key: $K" -X PUT --data-binary @$T/layout.json localhost:$P/api/topology >/dev/null
sleep 2
# A layout loaded from nothing SETTLES (TopologyRuntime::settleAtBoot): the path to the first machine opens, the rest close.
check "a first layout settles: gate1 opened, gate2 commanded closed" 'grep -q "\"selectorId\":\"gate1\".*\"stateId\":\"open\"" "$T/f1.log" && grep -q "\"selectorId\":\"gate2\".*\"stateId\":\"closed\"" "$T/f1.log"'
sets() { grep -c '"t":"SET"' "$T/f1.log"; }
settled=$(sets)
sleep 2
check "an idle plug moves nothing more" '[ "$(sets)" = "$settled" ]'
echo 300 > "$T/power"
for i in $(seq 1 40); do [ "$(sets)" -gt "$settled" ] && break; sleep 0.3; done
check "a tool whose plug reads 300 W commands its gate" 'tail -n +1 "$T/f1.log" | grep "\"t\":\"SET\"" | tail -n +$((settled + 1)) | grep -q "\"selectorId\":\"gate1\".*\"stateId\":\"open\""'
watts() { curl -s -m 3 -H "X-Api-Key: $K" localhost:$P/api/status | python3 -c "import sys,json;print(int(json.load(sys.stdin)['tools']['toolX']['watts']))" 2>/dev/null || echo 0; }
for i in $(seq 1 20); do [ "$(watts)" -ge 250 ] && break; sleep 0.3; done
check "...and the reading is in the status" '[ "$(watts)" -ge 250 ]'
[ $fail = 0 ] && echo "all native plug checks passed" || { echo "--- brain log"; cat "$T/brain.log"; echo "--- f1"; cat "$T/f1.log"; }
exit $fail
