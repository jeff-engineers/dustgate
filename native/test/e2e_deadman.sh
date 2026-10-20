#!/bin/bash
# The collector's switching plug carries a dead-man (D-85, piece 4): every ON says "and off again after outletDeadmanSec()"
# — 20 s for the default 30 s gate delay, so the blower stops before any gate closes — and the brain renews it three times a
# period while it wants the blower on, so a brain that dies leaves a plug that switches the blower off by itself. A fake Shelly logs every Switch.Set it is sent. Whether a REAL
# Shelly restarts its countdown on a renewal is not tested here (TODO.md) — only that the brain sends what it should.
cd "$(dirname "$0")/.." || exit 2
B=build/dustgate-brain; F=build/fakenode; PL=build/fakeplug; P=18103; PP=18104; K=testkey; T=$(mktemp -d); fail=0
export DUSTGATE_STATE="$T/state"   # never the real ~/.dustgate
trap 'kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
api() { curl -s -m 5 -H "X-Api-Key: $K" "$@"; }
wait_for() { for i in $(seq 1 $2); do eval "$1" && return 0; sleep 0.5; done; return 1; }
ons() { grep -c "on=true&toggle_after=20" "$T/power.swlog" 2>/dev/null || echo 0; }
python3 - "$T/layout.json" <<'PY'
import json,sys
d=json.load(open('../firmware/test/fixtures/twoGates.json'))
for e in d['systems'][0]['elements']:
    if e['id'] in ('gate1','gate2'): e['controllerId']='fake1'
    if e['type']=='collector': e['control']={'outlet':{'ip':'127.0.0.1','gen':2,'name':'Collector'}}
d['controllers'].append({'id':'fake1','role':'secondary'})
for m in d['machines']: m.pop('sensor', None)
json.dump(d,open(sys.argv[1],'w'))
PY
echo 0 > "$T/power"
$PL $PP "$T/power" "" "Collector" > "$T/plug.log" 2>&1 &
$B --id dmbrain --key $K --port $P --ip 127.0.0.1 --broadcast 127.255.255.255 --plug-port $PP --pair fake1 > "$T/brain.log" 2>&1 &
sleep 0.5
$F 127.0.0.1 $P fake1 120 > "$T/f1.log" 2>&1 &
wait_for "api localhost:$P/api/nodes | grep -q '\"online\":true'" 40
api -X PUT --data-binary @$T/layout.json localhost:$P/api/topology >/dev/null
echo "native collector dead-man"
api -X POST -d '{"on":true}' localhost:$P/api/collector >/dev/null
check "switching the blower on sends ON with a 20 s dead-man (the gates' 30 s, less 10)" 'wait_for "[ \$(ons) -ge 1 ]" 30'
check "...and renews it about every 7 s while it stays on" 'wait_for "[ \$(ons) -ge 2 ]" 30'
api -X POST -d '{"on":false}' localhost:$P/api/collector >/dev/null
check "switching it off sends a plain OFF" 'wait_for "tail -1 $T/power.swlog | grep -q \"on=false\"" 20'
N=$(ons); sleep 12
check "...and nothing is renewed once it is off" '[ "$(ons)" = "$N" ]'
[ $fail = 0 ] && echo "all native dead-man checks passed" || { echo "--- switch log"; cat "$T/power.swlog"; echo "--- brain log"; tail -30 "$T/brain.log"; }
exit $fail
