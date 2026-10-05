#!/bin/bash
# End to end on one machine: the native brain, two fake nodes, a layout whose gates live on them.
# A tool drawing power must move ITS gate on ITS node (the routing core, the node bus and the session
# all in the loop). No hardware, no network beyond loopback.
cd "$(dirname "$0")/.." || exit 2
B=build/dustgate-brain; F=build/fakenode; P=18080; T=$(mktemp -d); fail=0
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

$B --id testbrain --port $P --ip 127.0.0.1 --broadcast 127.255.255.255 --pair fake1,fake2 > "$T/brain.log" 2>&1 &
sleep 0.5
$F 127.0.0.1 $P fake1 25 > "$T/f1.log" 2>&1 &
$F 127.0.0.1 $P fake2 25 > "$T/f2.log" 2>&1 &
python3 - "$T/layout.json" <<'PY'
import json,sys
d=json.load(open('../firmware/test/fixtures/twoGates.json'))
for e in d['systems'][0]['elements']:
    if e['id']=='gate1': e['controllerId']='fake1'
    if e['id']=='gate2': e['controllerId']='fake2'
json.dump(d,open(sys.argv[1],'w'))
PY
online() { curl -s -m 2 localhost:$P/api/nodes | python3 -c "import sys,json;print(sum(1 for n in json.load(sys.stdin)['nodes'] if n['online']))" 2>/dev/null; }
for i in $(seq 1 30); do [ "$(online)" = 2 ] && break; sleep 0.3; done
echo "native end to end"
check "both fake nodes link" '[ "$(online)" = 2 ]'
check "a layout is adopted" '[ "$(curl -s -m 3 -X PUT --data-binary @$T/layout.json localhost:$P/api/topology)" = "{\"ok\":true}" ]'
curl -s -m 3 -X POST -d '{"machineId":"toolX","watts":100}' localhost:$P/api/dev/power >/dev/null
for i in $(seq 1 30); do grep -q '"t":"SET"' "$T/f1.log" && break; sleep 0.3; done
check "toolX drawing power sends a SET to the node that owns its gate" 'grep -q "\"selectorId\":\"gate1\".*\"stateId\":\"open\"" "$T/f1.log"'
check "...and the OTHER node is only ever told to close its gate" '! grep -q "\"stateId\":\"open\"" "$T/f2.log"'
sleep 1
check "the arrival is in the status" '[ "$(curl -s -m 3 localhost:$P/api/status | python3 -c "import sys,json;print(json.load(sys.stdin)[\"actuators\"][\"gate1\"])")" = open ]'
curl -s -m 3 -X POST -d '{"machineId":"toolY","watts":100}' localhost:$P/api/dev/power >/dev/null
for i in $(seq 1 40); do grep -q '"stateId":"open"' "$T/f2.log" && break; sleep 0.3; done
check "toolY (most recent) opens ITS gate on the other node" 'grep -q "\"selectorId\":\"gate2\".*\"stateId\":\"open\"" "$T/f2.log"'
[ $fail = 0 ] && echo "all native end-to-end checks passed" || { echo "--- brain log"; cat "$T/brain.log"; echo "--- f1"; cat "$T/f1.log"; echo "--- f2"; cat "$T/f2.log"; }
exit $fail
