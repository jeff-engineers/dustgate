#!/usr/bin/env bash
# restore-topology.sh — put a saved shop layout back on the device.
#
#   bash tools/restore-topology.sh                  # newest backup
#   bash tools/restore-topology.sh path/to/doc.json # a specific one
#   bash tools/restore-topology.sh --list           # what's been saved
#
# deploy.sh takes a backup before every filesystem flash (it erases topology.json
# along with the Angular bundle) and restores it at the end. This is the manual
# half: for when the board hadn't rejoined WiFi by the time the deploy finished,
# or when you want to roll back to an older layout.
#
# DUSTGATE_HOST overrides where to send it (default: the hostname in tools/.env,
# or dustgate). Backups live in .dustgate-backups/ and are gitignored.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BACKUP_DIR="$SCRIPT_DIR/.dustgate-backups"
ENV_FILE="$SCRIPT_DIR/tools/.env"

HOSTNAME_CFG="${HOSTNAME_CFG:-}"
if [[ -z "$HOSTNAME_CFG" && -f "$ENV_FILE" ]]; then
  while IFS='=' read -r key val; do
    [[ "$key" =~ ^#.*$ || -z "$key" ]] && continue
    val="${val%%#*}"
    val="${val%"${val##*[![:space:]]}"}"
    [[ "$key" == "HOSTNAME" ]] && HOSTNAME_CFG="$val"
  done < "$ENV_FILE"
fi
HOST="${DUSTGATE_HOST:-${HOSTNAME_CFG:-dustgate}.local}"

if [[ "${1:-}" == "--list" ]]; then
  if [[ -d "$BACKUP_DIR" ]] && compgen -G "$BACKUP_DIR/topology-*.json" >/dev/null; then
    ls -lh "$BACKUP_DIR"/topology-*.json
  else
    echo "No backups in ${BACKUP_DIR#$SCRIPT_DIR/} yet."
  fi
  exit 0
fi

DOC="${1:-}"
if [[ -z "$DOC" ]]; then
  # Newest by name, which is newest by time: the files are timestamp-named.
  DOC="$(ls "$BACKUP_DIR"/topology-*.json 2>/dev/null | tail -1 || true)"
fi
if [[ -z "$DOC" || ! -f "$DOC" ]]; then
  echo "No backup to restore. Try: bash tools/restore-topology.sh --list"
  exit 1
fi

# Refuse to send something that isn't a document. A truncated or error body PUT
# at the device is worse than no restore: the firmware would reject it, but a
# half-valid one could adopt and leave the shop routing something unintended.
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if isinstance(d,dict) else 1)' "$DOC" \
  || { echo "✗ ${DOC} isn't a JSON object — not sending it."; exit 1; }

echo "▶ Restoring ${DOC#$SCRIPT_DIR/} → $HOST"

KEY="$(curl -fsS --max-time 5 "http://$HOST/api/info" 2>/dev/null \
        | python3 -c 'import json,sys; print(json.load(sys.stdin).get("apiKey",""))' 2>/dev/null || true)"
if [[ -z "$KEY" ]]; then
  echo "✗ Couldn't reach $HOST. Is it powered and on WiFi?"
  echo "  If mDNS is being unhelpful, try: DUSTGATE_HOST=192.168.x.y bash tools/restore-topology.sh"
  exit 1
fi

RESP="$(curl -sS --max-time 15 -X PUT -H "X-Api-Key: $KEY" \
          -H 'Content-Type: application/json' --data-binary "@$DOC" \
          "http://$HOST/api/topology" 2>&1 || true)"

if [[ "$RESP" == *'"ok":true'* ]]; then
  echo "✓ Restored. The device re-adopts it on its next loop pass."
else
  echo "✗ Device didn't accept it: $RESP"
  exit 1
fi
