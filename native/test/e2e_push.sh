#!/bin/bash
# A plug is CLAIMED, not just polled: the brain points an unclaimed Shelly's Outbound WebSocket at itself, reads its power
# off that socket instead of polling, leaves a plug somebody else owns alone, and repoints one only when a person approved it.
# A fake Shelly on loopback keeps its push config in a file, dials whatever it is pointed at, and counts the polls it gets.
cd "$(dirname "$0")/.." || exit 2
B=build/dustgate-brain; F=build/fakenode; PL=build/fakeplug; P=18091; PP=18092; K=testkey; fail=0
T=$(mktemp -d)
export DUSTGATE_STATE="$T/state"   # never the real ~/.dustgate
trap 'kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
api() { curl -s -m 10 -H "X-Api-Key: $K" "$@"; }
watts() { api localhost:$P/api/status | python3 -c "import sys,json;print(int(json.load(sys.stdin)['tools']['toolX']['watts']))" 2>/dev/null; }
wait_for() { for i in $(seq 1 $2); do eval "$1" && return 0; sleep 0.5; done; return 1; }
python3 - "$T/layout.json" <<'PY'
import json,sys
d=json.load(open('../firmware/test/fixtures/twoGates.json'))
for e in d['systems'][0]['elements']:
    if e['id'] in ('gate1','gate2'): e['controllerId']='fake1'
d['machines'][0]['sensor']['outlet']['ip']='127.0.0.1'
del d['machines'][1]['sensor']
json.dump(d,open(sys.argv[1],'w'))
PY
start() {   # start <plug args...>: a fresh state, a plug, a brain with a node and the layout
  kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$T/state"; rm -f "$T"/power*; echo 0 > "$T/power"
  $PL $PP "$T/power" "$@" > "$T/plug.log" 2>&1 &
  $B --id pushbrain --key $K --port $P --ip 127.0.0.1 --broadcast 127.255.255.255 --plug-port $PP --pair fake1 > "$T/brain.log" 2>&1 &
  sleep 0.5
  $F 127.0.0.1 $P fake1 20 > "$T/f1.log" 2>&1 &
  wait_for "api localhost:$P/api/nodes | grep -q '\"online\":true'" 40
  api -X PUT --data-binary @$T/layout.json localhost:$P/api/topology >/dev/null
}
polls() { [ -f "$T/power.polls" ] && wc -c < "$T/power.polls" | tr -d ' ' || echo 0; }

echo "native plug push"
start "" "Router saw"
check "an unclaimed plug is pointed at the brain" 'wait_for "grep -q \"^1 ws://127.0.0.1:$P/shelly-rpc\" $T/power.ws 2>/dev/null" 40'
check "...and its name says who is using it" 'wait_for "[ \"\$(cat $T/power.name)\" = \"Router saw · pushbrain\" ]" 10'
check "...and it dials in" 'wait_for "grep -q \"connected its push socket\" $T/brain.log" 20'
echo 300 > "$T/power"
check "a pushed reading reaches the status" 'wait_for "[ \"\$(watts)\" -ge 250 ]" 10'
sleep 1.5; N=$(polls); echo 0 > "$T/power"; sleep 0.6; echo 400 > "$T/power"; sleep 2
check "...without the plug being polled" '[ "$(polls)" = "$N" ]'
check "...and a change shows at once" '[ "$(watts)" -ge 350 ]'
check "the plug gate opens on a pushed reading" 'grep -q "\"stateId\":\"open\"" "$T/f1.log"'

echo "a plug somebody else owns"
# 127.0.0.1 is US on loopback, so "another controller" needs another address
start "ws://10.9.9.9:1/shelly-rpc" "Saw · otherbrain"
sleep 6
check "is left pointed where it was" 'grep -q "^1 ws://10.9.9.9:1/shelly-rpc" $T/power.ws'
check "...and its name untouched" '[ "$(cat $T/power.name)" = "Saw · otherbrain" ]'
echo 300 > "$T/power"
check "...but still read, by polling" 'wait_for "[ \"\$(watts)\" -ge 250 ]" 10'
check "takeover is a deliberate act: nothing was written until asked" '! grep -q "pushbrain" $T/power.ws'
echo '{"ip":"127.0.0.1"}' > "$T/ip.json"
api -X POST --data @$T/ip.json localhost:$P/api/outlets/takeover | grep -q '"ok":true' && ok "a takeover is accepted" || bad "a takeover is accepted"
check "...and the plug now pushes to this brain" 'wait_for "grep -q \"^1 ws://127.0.0.1:$P/shelly-rpc\" $T/power.ws" 40'
check "...having remembered whose it was" 'grep -q "ws://10.9.9.9:1/shelly-rpc" "$T/state/plugs.json"'
api -X POST --data @$T/ip.json localhost:$P/api/outlets/release | grep -q '"restored":true' && ok "releasing it reports a restore" || bad "releasing it reports a restore"
check "...and the other controller has its plug back" 'grep -q "^1 ws://10.9.9.9:1/shelly-rpc" $T/power.ws'
check "...with its own name" '[ "$(cat $T/power.name)" = "Saw" ]'
echo "the brain's own address moves"
# No --ip: the address is read from a file here (on a Pi it comes from the network), and the file changes under the running brain.
# Loopback only answers on .1 on a Mac, so this checks what the plug was TOLD, which is the thing that has to move.
kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$T/state"; rm -f "$T"/power*; echo 0 > "$T/power"; echo 127.0.0.3 > "$T/myip"
$PL $PP "$T/power" "" "Router saw" > "$T/plug.log" 2>&1 &
$B --id pushbrain --key $K --port $P --ip-from-file "$T/myip" --broadcast 127.255.255.255 --plug-port $PP > "$T/brain.log" 2>&1 &
sleep 0.5
api -X PUT --data-binary @$T/layout.json localhost:$P/api/topology >/dev/null
check "a plug is pointed at the address the brain has" 'wait_for "grep -q \"^1 ws://127.0.0.3:$P/shelly-rpc\" $T/power.ws 2>/dev/null" 40'
echo 127.0.0.4 > "$T/myip"
check "the address changes: the plug is pointed at the new one" 'wait_for "grep -q \"^1 ws://127.0.0.4:$P/shelly-rpc\" $T/power.ws 2>/dev/null" 60'
check "...and the brain says so" 'grep -q "address changed 127.0.0.3 -> 127.0.0.4" $T/brain.log'
check "...without renaming the plug twice" '[ "$(cat $T/power.name)" = "Router saw · pushbrain" ]'

[ $fail = 0 ] && echo "all native plug-push checks passed" || { echo "--- brain log"; tail -40 "$T/brain.log"; echo "--- plug"; cat $T/plug.log; }
exit $fail
