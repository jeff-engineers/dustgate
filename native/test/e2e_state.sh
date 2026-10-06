#!/bin/bash
# Power cuts: the layout, pairings and API key are written atomically with the last good copy kept beside them, and a brain that finds
# the layout truncated (what a cut mid-save used to leave) restores the previous one instead of coming up empty.
cd "$(dirname "$0")/.." || exit 2
B=build/dustgate-brain; P=18095; K=testkey; fail=0; T=$(mktemp -d)
export DUSTGATE_STATE="$T/state"; mkdir -p "$T/state"
trap 'kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
api() { curl -s -m 10 -H "X-Api-Key: $K" "$@"; }
start() { $B --id statebrain --key $K --port $P --ip 127.0.0.1 --broadcast 127.255.255.255 > "$T/brain.log" 2>&1 & sleep 0.6; }
stop()  { kill %% 2>/dev/null; wait 2>/dev/null; }
python3 - "$T" <<'PY'
import json,sys
d=json.load(open('../firmware/test/fixtures/twoGates.json'))
json.dump(d,open(sys.argv[1]+'/a.json','w'))
d['name']='Second Layout'
json.dump(d,open(sys.argv[1]+'/b.json','w'))
PY
echo "native state files"
start
api -X PUT --data-binary @$T/a.json localhost:$P/api/topology >/dev/null
api -X PUT --data-binary @$T/b.json localhost:$P/api/topology >/dev/null
check "a saved layout is on disk whole" 'python3 -c "import json;json.load(open(\"$T/state/topology.json\"))"'
check "...with the previous one kept beside it" 'python3 -c "import json;assert json.load(open(\"$T/state/topology.json.bak\")).get(\"name\")!=\"Second Layout\""'
check "no half-written temp file is left behind" '[ -z "$(ls $T/state | grep "\.tmp$")" ]'
stop
# What a power cut mid-write used to leave: a file cut short.
head -c 300 "$T/state/topology.json" > "$T/state/cut" && mv "$T/state/cut" "$T/state/topology.json"
start
check "a truncated layout is not fatal: the previous copy is restored" 'api localhost:$P/api/topology | python3 -c "import sys,json;d=json.load(sys.stdin);assert d[\"controllers\"]"'
check "...and the damage is healed on disk" 'python3 -c "import json;json.load(open(\"$T/state/topology.json\"))"'
check "...and it says so" 'grep -q "restored the previous layout" "$T/brain.log"'
stop
check "healing did not replace the good copy with the damaged file" 'python3 -c "import json;json.load(open(\"$T/state/topology.json.bak\"))"'
: > "$T/state/topology.json"   # an EMPTY file, the other thing a cut leaves
start
check "an empty layout file restores the same way" 'api localhost:$P/api/topology | python3 -c "import sys,json;d=json.load(sys.stdin);assert d[\"controllers\"]"'
[ $fail = 0 ] && echo "all native state checks passed" || { echo "--- brain log"; tail -20 "$T/brain.log"; }
exit $fail
