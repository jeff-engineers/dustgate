#!/bin/bash
# Sweep a routing constant across every sample layout in router.bench.ts.
#
# The costs in route-grid.ts are module-level constants — deliberately, they are
# read on every edge the search relaxes — so there is nothing to override at run
# time. This copies the routing directory, patches the copy, and runs the bench
# there, which keeps a sweep from leaving a half-edited constant in the tree.
#
#     ./routing-sweep.sh TURN 32 48 64 80 96
#     ./routing-sweep.sh CELL 108 126 144
#
# Anything the bench measures can move when a constant does, so read the whole
# table and not just the column you are aiming at: on 2026-09-07 raising TURN past
# 64 bought a bend by climbing over the collector, which shows up in `above` and
# nowhere else.
set -e
cd "$(dirname "$0")"
NAME=$1; shift
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
for V in "$@"; do
  D="$WORK/$V"; mkdir -p "$D/src"
  cp src/app/build/routing/*.ts "$D/src/"
  rm -f "$D/src/router.spec.ts"
  perl -pi -e "s/^(const|export const) $NAME = [0-9]+;/\$1 $NAME = $V;/" "$D/src/route-grid.ts" "$D/src/geometry.ts" "$D/src/router.ts"
  if ! grep -qE "^(const|export const) $NAME = $V;" "$D/src/"*.ts; then
    echo "no constant named $NAME in the routing sources" >&2; exit 1
  fi
  echo "═══ $NAME = $V"
  ./node_modules/.bin/tsc "$D/src/"*.ts --outDir "$D/out" --module commonjs \
    --target es2022 --moduleResolution node --strict --skipLibCheck
  node "$D/out/router.bench.js"
  echo
done
