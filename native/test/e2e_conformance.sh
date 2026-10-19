#!/bin/bash
# The topology contract suite (shared/device-model/topology-conformance.js) against THIS brain — the same suite the mock
# passes, so the native brain's routing is held to the spec rather than to its own tests. Its gates live on a fake node
# (--gates-on), since this brain has no pads; its tools are driven with --sim (/api/sim/tool, the mock's own route).
cd "$(dirname "$0")/.." || exit 2
B=build/dustgate-brain; F=build/fakenode; P=18095; K=dev-mock-key-1234; T=$(mktemp -d)
export DUSTGATE_STATE="$T/state"   # never the real ~/.dustgate
trap 'kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
$B --id confbrain --key $K --port $P --ip 127.0.0.1 --broadcast 127.255.255.255 --plug-port 18096 --pair fake1 --sim > "$T/brain.log" 2>&1 &
sleep 0.5
$F 127.0.0.1 $P fake1 300 > "$T/f1.log" 2>&1 &
for i in $(seq 1 40); do curl -s -m 2 -H "X-Api-Key: $K" localhost:$P/api/nodes | grep -q '"online":true' && break; sleep 0.3; done
echo "native topology conformance"
node ../shared/device-model/topology-conformance.js http://localhost:$P --gates-on fake1; rc=$?
[ $rc = 0 ] || { echo "--- brain log"; tail -30 "$T/brain.log"; }
exit $rc
