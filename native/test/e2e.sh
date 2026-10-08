#!/bin/bash
# End to end on one machine: the native brain, two fake nodes, a layout whose gates live on them.
# A tool drawing power must move ITS gate on ITS node (the routing core, the node bus and the session
# all in the loop). No hardware, no network beyond loopback.
cd "$(dirname "$0")/.." || exit 2
B=build/dustgate-brain; F=build/fakenode; P=18080; T=$(mktemp -d); fail=0
export DUSTGATE_STATE="$T/state"   # never the real ~/.dustgate: a test brain must not inherit a layout or pairings
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

$B --id testbrain --key testkey --port $P --ip 127.0.0.1 --broadcast 127.255.255.255 --pair fake1,fake2 > "$T/brain.log" 2>&1 &
sleep 0.5
$F 127.0.0.1 $P fake1 25 > "$T/f1.log" 2>&1 &
$F 127.0.0.1 $P fake2 25 > "$T/f2.log" 2>&1 &
python3 - "$T/layout.json" <<'PY'
import json,sys
d=json.load(open('../firmware/test/fixtures/twoGates.json'))
for e in d['systems'][0]['elements']:
    if e['id']=='gate1': e['controllerId']='fake1'
    if e['id']=='gate2': e['controllerId']='fake2'
    if e['id']=='dc': e['control']={'rf':{'controllerId':'fake1'}}
json.dump(d,open(sys.argv[1],'w'))
PY
online() { curl -s -m 2 -H "X-Api-Key: testkey" localhost:$P/api/nodes | python3 -c "import sys,json;print(sum(1 for n in json.load(sys.stdin)['nodes'] if n['online']))" 2>/dev/null; }
for i in $(seq 1 30); do [ "$(online)" = 2 ] && break; sleep 0.3; done
echo "native end to end"
check "both fake nodes link" '[ "$(online)" = 2 ]'
check "a layout is adopted" '[ "$(curl -s -m 3 -H "X-Api-Key: testkey" -X PUT --data-binary @$T/layout.json localhost:$P/api/topology)" = "{\"ok\":true}" ]'
curl -s -m 3 -H "X-Api-Key: testkey" -X POST -d '{"machineId":"toolX","watts":100}' localhost:$P/api/dev/power >/dev/null
for i in $(seq 1 30); do grep -q '"t":"SET"' "$T/f1.log" && break; sleep 0.3; done
check "toolX drawing power sends a SET to the node that owns its gate" 'grep -q "\"selectorId\":\"gate1\".*\"stateId\":\"open\"" "$T/f1.log"'
check "...and the OTHER node is only ever told to close its gate" '! grep -q "\"stateId\":\"open\"" "$T/f2.log"'
sleep 1
for i in $(seq 1 30); do grep -q '"t":"PRESS"' "$T/f1.log" && break; sleep 0.3; done
sleep 1
check "the collector is pressed ONCE, by the node that has its transmitter" '[ "$(grep -c "\"t\":\"PRESS\"" "$T/f1.log")" = 1 ] && ! grep -q "\"t\":\"PRESS\"" "$T/f2.log"'
check "the arrival is in the status" '[ "$(curl -s -m 3 -H "X-Api-Key: testkey" localhost:$P/api/status | python3 -c "import sys,json;print(json.load(sys.stdin)[\"actuators\"][\"gate1\"])")" = open ]'
curl -s -m 3 -H "X-Api-Key: testkey" -X POST -d '{"machineId":"toolY","watts":100}' localhost:$P/api/dev/power >/dev/null
for i in $(seq 1 40); do grep -q '"stateId":"open"' "$T/f2.log" && break; sleep 0.3; done
check "toolY (most recent) opens ITS gate on the other node" 'grep -q "\"selectorId\":\"gate2\".*\"stateId\":\"open\"" "$T/f2.log"'
nodeids() { curl -s -m 2 -H "X-Api-Key: testkey" localhost:$P/api/nodes | python3 -c "import sys,json;print(','.join(sorted(n['id'] for n in json.load(sys.stdin)['nodes'])))"; }
curl -s -m 3 -H "X-Api-Key: testkey" -X POST -d '{"host":"fake3","name":"Third"}' localhost:$P/api/nodes/pair >/dev/null
check "pairing adds a node live" '[ "$(nodeids)" = fake1,fake2,fake3 ]'
curl -s -m 3 -H "X-Api-Key: testkey" -X POST -d '{"host":"fake3","remove":true}' localhost:$P/api/nodes/pair >/dev/null
check "unpairing removes it" '[ "$(nodeids)" = fake1,fake2 ]'
check "a request without the key is refused" '[ "$(curl -s -m 2 -o /dev/null -w %{http_code} localhost:$P/api/nodes)" = 401 ]'
# A SAVE IS NOT A REBOOT (TopologyRuntime::adopt). Saving the layout while tools run used to reset the brain: the blower it
# had started read as a stranger's, and this open-loop collector (no plug watches it) was pressed AGAIN — a toggle, so OFF,
# mid-cut. Now nothing is pressed by a save, and the blower is still the brain's to stop once the tools do.
presses() { grep -c '"t":"PRESS"' "$T/f1.log"; }
before=$(presses)
check "the layout is saved again while tools run" '[ "$(curl -s -m 3 -H "X-Api-Key: testkey" -X PUT --data-binary @$T/layout.json localhost:$P/api/topology)" = "{\"ok\":true}" ]'
sleep 2
check "...and the save presses nothing" '[ "$(presses)" = "$before" ]'
curl -s -m 3 -H "X-Api-Key: testkey" -X POST -d '{"machineId":"toolX","watts":0}' localhost:$P/api/dev/power >/dev/null
curl -s -m 3 -H "X-Api-Key: testkey" -X POST -d '{"machineId":"toolY","watts":0}' localhost:$P/api/dev/power >/dev/null
for i in $(seq 1 40); do [ "$(presses)" -gt "$before" ] && break; sleep 0.3; done
check "when the tools stop after the save, the brain presses its blower OFF" '[ "$(presses)" = $((before + 1)) ]'
curl -s -m 3 -H "X-Api-Key: testkey" -X POST -d '{"paused":true}' localhost:$P/api/nodes/pause >/dev/null
for i in $(seq 1 30); do [ "$(online)" = 0 ] && break; sleep 0.3; done
check "pausing closes every link" '[ "$(online)" = 0 ]'
[ $fail = 0 ] && echo "all native end-to-end checks passed" || { echo "--- brain log"; cat "$T/brain.log"; echo "--- f1"; cat "$T/f1.log"; echo "--- f2"; cat "$T/f2.log"; }
exit $fail
