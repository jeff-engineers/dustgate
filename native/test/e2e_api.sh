#!/bin/bash
# The rest of the API, against the native brain and fake nodes: manual switches, a jog, boards that knock, the log,
# and staging a firmware image and telling a node to pull it.
cd "$(dirname "$0")/.." || exit 2
B=build/dustgate-brain; F=build/fakenode; P=18083; T=$(mktemp -d); fail=0; K=testkey
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
api() { curl -s -m 5 -H "X-Api-Key: $K" "$@"; }
code() { curl -s -m 5 -o /dev/null -w "%{http_code}" -H "X-Api-Key: $K" "$@"; }
body() { echo "$1" > "$T/body.json"; }
mkdir -p "$T/state"
$B --id apibrain --key $K --port $P --ip 127.0.0.1 --broadcast 127.255.255.255 --state "$T/state" --pair fake1 > "$T/brain.log" 2>&1 &
sleep 0.5
$F 127.0.0.1 $P fake1 40 > "$T/f1.log" 2>&1 &
python3 - "$T/layout.json" <<'PY'
import json,sys
d=json.load(open('../firmware/test/fixtures/twoGates.json'))
for e in d['systems'][0]['elements']:
    if e['id'] in ('gate1','gate2'): e['controllerId']='fake1'
json.dump(d,open(sys.argv[1],'w'))
PY
for i in $(seq 1 30); do api localhost:$P/api/nodes | grep -q '"online":true' && break; sleep 0.3; done
echo "native API"
api -X PUT --data-binary @$T/layout.json localhost:$P/api/topology >/dev/null

# manual switches
body '{"toolId":"nope","on":true}'
check "an unknown tool is a 404" '[ "$(code -X POST --data @$T/body.json localhost:$P/api/tool)" = 404 ]'
api -X POST -d '{"toolId":"toolX","on":true}' localhost:$P/api/tool >/dev/null
for i in $(seq 1 30); do grep -q '"stateId":"open"' "$T/f1.log" && break; sleep 0.3; done
check "switching a tool on by hand opens its gate on its node" 'grep -q "\"selectorId\":\"gate1\".*\"stateId\":\"open\"" "$T/f1.log"'
api -X POST -d '{"toolId":"toolX","on":false}' localhost:$P/api/tool >/dev/null

# a jog goes to the board that owns the channel (once the gate move before it has landed: a jog waits its turn)
sleep 2
api -X POST -d '{"controllerId":"fake1","channel":1,"angle":45}' localhost:$P/api/servo/jog >/dev/null
for i in $(seq 1 20); do grep -q '"angle":45' "$T/f1.log" && break; sleep 0.3; done
check "a jog reaches the node as an angle on that channel" 'grep -q "\"angle\":45" "$T/f1.log"'
body '{"channel":0,"angle":10}'
check "a jog with no board is a 501 (this brain has no servos of its own)" '[ "$(code -X POST --data @$T/body.json localhost:$P/api/servo/jog)" = 501 ]'
body '{"controllerId":"ghost","channel":0,"angle":10}'
check "a jog to a board that is not paired is a 502" '[ "$(code -X POST --data @$T/body.json localhost:$P/api/servo/jog)" = 502 ]'

# a board nobody paired knocks, and shows up in discovery
$F 127.0.0.1 $P stranger 3 > "$T/s.log" 2>&1 &
sleep 2
check "an unpaired board that dials in is listed by discovery" 'api localhost:$P/api/nodes/discover | grep -q "\"host\":\"stranger\""'
api -X POST -d '{"host":"stranger","name":"New board"}' localhost:$P/api/nodes/pair >/dev/null
check "...and is no longer offered once paired" '! api localhost:$P/api/nodes/discover | grep -q "\"host\":\"stranger\""'
api -X POST -d '{"host":"stranger","remove":true}' localhost:$P/api/nodes/pair >/dev/null

# the console and the link log
check "the console is readable with a cursor" 'api -D "$T/h.txt" "localhost:$P/api/serial?from=0" | grep -q "dialled in" && grep -qi "x-serial-next" "$T/h.txt"'
check "the link log has the node coming up" 'api localhost:$P/api/linklog | grep -q "\"ev\":\"link_up\".*\"node\":\"fake1\"\|\"node\":\"fake1\".*\"ev\":\"link_up\""'

# staging an image, and a node pulling it
head -c 150000 /dev/urandom > "$T/img.bin"
MD5=$(md5 -q "$T/img.bin" 2>/dev/null || md5sum "$T/img.bin" | cut -d" " -f1)
check "an upload that does not match its MD5 is refused" '[ "$(code -X POST -H "X-Fw: test 1" -H "X-Md5: 00000000000000000000000000000000" --data-binary @$T/img.bin "localhost:$P/api/node-image?kind=pwm")" = 400 ]'
check "an image is staged" '[ "$(api -X POST -H "X-Fw: test 1" -H "X-Md5: $MD5" -H "Expect:" --data-binary @$T/img.bin "localhost:$P/api/node-image?kind=pwm")" = "{\"ok\":true}" ]'
check "the staged image is served, byte for byte, with no key (a node has none)" '[ "$(curl -s -m 5 localhost:$P/node-pwm.bin | md5 -q 2>/dev/null || curl -s localhost:$P/node-pwm.bin | md5sum | cut -d" " -f1)" = "$MD5" ]'
check "the node list says an update is offered" 'api localhost:$P/api/nodes | grep -q "\"image\":\"test 1\""'
# a node is told to update only when the shop is quiet: wait out the blower's coast-down
for i in $(seq 1 40); do api localhost:$P/api/status | grep -q '"collectorOn":false' && break; sleep 0.5; done
api -X POST -d '{"id":"fake1"}' localhost:$P/api/nodes/update >/dev/null
for i in $(seq 1 20); do grep -q '"t":"OTA"' "$T/f1.log" && break; sleep 0.3; done
check "telling a node to update sends it the OTA order with the image's path, size and MD5" 'grep "\"t\":\"OTA\"" "$T/f1.log" | grep -q "/node-pwm.bin" && grep "\"t\":\"OTA\"" "$T/f1.log" | grep -q "150000" && grep "\"t\":\"OTA\"" "$T/f1.log" | grep -q "$MD5"'
body '{"id":"ghost"}'
check "an update for a board that is not paired is a 409" '[ "$(code -X POST --data @$T/body.json localhost:$P/api/nodes/update)" = 409 ]'

# the slider and reset
check "the slider routes say this brain has no rack" '[ "$(code -X POST localhost:$P/api/home)" = 501 ]'
api -X POST localhost:$P/api/reset-all >/dev/null
check "reset-all clears the layout and the pairings" '[ "$(code localhost:$P/api/topology)" = 404 ] && [ "$(api localhost:$P/api/nodes | python3 -c "import sys,json;print(len(json.load(sys.stdin)[\"nodes\"]))")" = 0 ]'
[ $fail = 0 ] && echo "all native API checks passed" || { echo "--- brain log"; tail -40 "$T/brain.log"; }
exit $fail
