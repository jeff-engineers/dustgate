#!/bin/bash
# Fail closed, the native shell's half (D-85): a board that loses its link. Two fake nodes hold one gate each; the layout's
# delay is the shortest allowed (10 s). Kill the node holding the running tool's gate and the brain must open the other
# node's gate (so the blower has a gate it can see), keep the blower running, believe the lost gate closed once the node
# will have closed it, and send it again when the node comes back. Each node is also sent its close plan in CONFIG.
# The rules themselves are host-tested (test_nodebus.cpp FC*, F*); this proves the native loop drives them.
cd "$(dirname "$0")/.." || exit 2
B=build/dustgate-brain; F=build/fakenode; P=18099; K=testkey; T=$(mktemp -d); fail=0
export DUSTGATE_STATE="$T/state"   # never the real ~/.dustgate
trap 'kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
api() { curl -s -m 5 -H "X-Api-Key: $K" "$@"; }
st() { api localhost:$P/api/status | python3 -c "import sys,json;d=json.load(sys.stdin);print(d['actuators'].get('$1'),d['collectorOn'])" 2>/dev/null; }
wait_for() { for i in $(seq 1 $2); do eval "$1" && return 0; sleep 0.5; done; return 1; }
python3 - "$T/layout.json" <<'PY'
import json,sys
d=json.load(open('../firmware/test/fixtures/twoGates.json'))
for e in d['systems'][0]['elements']:
    if e['id']=='gate1': e['controllerId']='fake1'
    if e['id']=='gate2': e['controllerId']='fake2'
d['failsafe']={'closeAfterMs':10000}
d['controllers'] += [{'id':'fake1','role':'secondary'},{'id':'fake2','role':'secondary'}]
for m in d['machines']: m.pop('sensor', None)   # tool power comes from --sim, not from plugs that do not exist
json.dump(d,open(sys.argv[1],'w'))
PY
$B --id fcbrain --key $K --port $P --ip 127.0.0.1 --broadcast 127.255.255.255 --plug-port 18100 --pair fake1,fake2 --sim > "$T/brain.log" 2>&1 &
sleep 0.5
$F 127.0.0.1 $P fake1 120 > "$T/f1.log" 2>&1 & F1=$!
$F 127.0.0.1 $P fake2 120 > "$T/f2.log" 2>&1 &
wait_for "[ \$(api localhost:$P/api/nodes | grep -o '\"online\":true' | wc -l) -ge 2 ]" 40
api -X PUT --data-binary @$T/layout.json localhost:$P/api/topology >/dev/null
echo "native fail closed"
check "each node is sent its close plan, with the layout's 10 s" 'wait_for "grep -q \"\\\"failsafe\\\":{\\\"closeAfterMs\\\":10000\" $T/f1.log && grep -q \"\\\"closeAfterMs\\\":10000\" $T/f2.log" 20'
api -X POST -d '{"toolId":"toolX","watts":100}' localhost:$P/api/sim/tool >/dev/null
check "toolX on: gate1 opens and the collector runs" 'wait_for "[ \"\$(st gate1)\" = \"open True\" ]" 20'
check "the status lights (D-86): fake1, with the open gate, is told it has the suction" 'wait_for "grep -q \"\\\"t\\\":\\\"ALERT\\\".*\\\"suction\\\":true\" $T/f1.log" 20'
check "...and fake2's shop light shows the collector running, green" 'wait_for "grep -q \"\\\"shop\\\":{\\\"rgb\\\":\[0,64,0\]\" $T/f2.log" 20'
kill $F1; wait $F1 2>/dev/null
check "fake1 lost: fake2 is told to open gate2" 'wait_for "grep -q \"\\\"selectorId\\\":\\\"gate2\\\".*\\\"stateId\\\":\\\"open\\\"\" $T/f2.log" 30'
check "...and the collector keeps running" '[ "$(st gate2 | cut -d" " -f2)" = True ]'
check "the lost gate is believed closed once its node will have closed it" 'wait_for "[ \"\$(st gate1)\" = \"closed True\" ]" 30'
$F 127.0.0.1 $P fake1 60 > "$T/f1b.log" 2>&1 &
check "fake1 back: it is sent gate1 open again" 'wait_for "grep -q \"\\\"selectorId\\\":\\\"gate1\\\".*\\\"stateId\\\":\\\"open\\\"\" $T/f1b.log" 40'
check "...and the collector never stopped" '[ "$(st gate1 | cut -d" " -f2)" = True ]'
[ $fail = 0 ] && echo "all native fail-closed checks passed" || { echo "--- brain log"; tail -40 "$T/brain.log"; }
exit $fail
