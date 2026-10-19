#!/bin/bash
# Is the plug at a saved address still the layout's plug? control/OutletRelocate.h, run by the native shell (Relocator.h).
# A fake Shelly answers Shelly.GetDeviceInfo with the MAC in a file, so a test puts a DIFFERENT plug at the address by
# writing that file: it must not be polled, claimed or read as the tool, and the brain must say so. The matching itself
# (relocation by MAC, elimination, swapped leases) is host-tested in firmware/test/test_relocate.cpp; on macOS a fake plug
# can only own 127.0.0.1, so a plug moving to another address cannot be staged here.
cd "$(dirname "$0")/.." || exit 2
B=build/dustgate-brain; F=build/fakenode; PL=build/fakeplug; P=18093; PP=18094; K=testkey; fail=0
T=$(mktemp -d)
export DUSTGATE_STATE="$T/state"   # never the real ~/.dustgate
trap 'kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
api() { curl -s -m 10 -H "X-Api-Key: $K" "$@"; }
watts() { api localhost:$P/api/status | python3 -c "import sys,json;print(int(json.load(sys.stdin)['tools']['toolX']['watts']))" 2>/dev/null; }
problem() { api localhost:$P/api/status | python3 -c "import sys,json;print(' '.join(p['code'] for p in json.load(sys.stdin).get('problems',[])))" 2>/dev/null; }
savedMac() { api localhost:$P/api/topology | python3 -c "import sys,json;print(json.load(sys.stdin)['machines'][0]['sensor']['outlet'].get('mac',''))" 2>/dev/null; }
wait_for() { for i in $(seq 1 $2); do eval "$1" && return 0; sleep 0.5; done; return 1; }
layout() {   # layout <mac or ''>
  python3 - "$T/layout.json" "$1" <<'PY'
import json,sys
d=json.load(open('../firmware/test/fixtures/twoGates.json'))
for e in d['systems'][0]['elements']:
    if e['id'] in ('gate1','gate2'): e['controllerId']='fake1'
o=d['machines'][0]['sensor']['outlet']; o['ip']='127.0.0.1'
if sys.argv[2]: o['mac']=sys.argv[2]
else: o.pop('mac',None)
del d['machines'][1]['sensor']
json.dump(d,open(sys.argv[1],'w'))
PY
}
start() {   # start <mac the plug reports>
  kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$T/state"; rm -f "$T"/power*; echo 300 > "$T/power"; echo "$1" > "$T/power.mac"
  $PL $PP "$T/power" "" "Router saw" > "$T/plug.log" 2>&1 &
  $B --id idbrain --key $K --port $P --ip 127.0.0.1 --broadcast 127.255.255.255 --plug-port $PP --pair fake1 > "$T/brain.log" 2>&1 &
  sleep 0.5
  $F 127.0.0.1 $P fake1 20 > "$T/f1.log" 2>&1 &
  wait_for "api localhost:$P/api/nodes | grep -q '\"online\":true'" 40
  api -X PUT --data-binary @$T/layout.json localhost:$P/api/topology >/dev/null
}

echo "native plug identity"
echo "a plug paired with no MAC"
layout ""; start "AABBCCDDEE01"
check "has its MAC recorded in the layout" 'wait_for "[ \"\$(savedMac)\" = AA:BB:CC:DD:EE:01 ]" 30'
check "...and is still read" 'wait_for "[ \"\$(watts)\" -ge 250 ]" 20'

echo "the layout's plug, by MAC"
layout "AA:BB:CC:DD:EE:01"; start "AABBCCDDEE01"
check "is checked, then claimed" 'wait_for "grep -q \"^1 ws://127.0.0.1:$P/shelly-rpc\" $T/power.ws 2>/dev/null" 40'
check "...and read" 'wait_for "[ \"\$(watts)\" -ge 250 ]" 20'
check "...with no problem raised" '! problem | grep -q plug-replaced'

echo "a different plug at the saved address"
layout "AA:BB:CC:DD:EE:01"; start "AABBCCDDEE02"
check "is reported as a different plug" 'wait_for "problem | grep -q plug-replaced" 30'
check "...is never claimed" 'sleep 3; ! grep -q "idbrain" $T/power.ws 2>/dev/null && ! grep -q "idbrain" $T/power.name 2>/dev/null'
check "...and its 300 W are not read as the tool" '[ "$(watts)" = 0 ]'
check "...and the brain log says why" 'grep -q "a different plug answers at 127.0.0.1" $T/brain.log'
check "...and goes looking for the real one" 'grep -q "sweeping the subnet for it" $T/brain.log'

[ $fail = 0 ] && echo "all native plug-identity checks passed" || { echo "--- brain log"; tail -40 "$T/brain.log"; }
exit $fail
