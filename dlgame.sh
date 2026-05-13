#!/usr/bin/env bash
# dlgame.sh — Download a complete MLB live feed JSON for use with pitch_replay.py.
#
# Usage:
#   ./dlgame.sh --last       # latest Final Mariners game
#   ./dlgame.sh <gamePk>     # specific game
#
# Saves a single JSON file to:
#   gamereplay/mlb_live_<gamePk>_<DATE>_<AWAY>@<HOME>_<awayScore>-<homeScore>.json

set -euo pipefail

[[ $# -lt 1 ]] && { echo "Usage: $0 [--last | <gamePk>]" >&2; exit 1; }

BASE_URL="https://statsapi.mlb.com"
OUT_FILE=""
GAME_PK=""

# ── resolve gamePk ────────────────────────────────────────────────────────────

if [[ "$1" == "--last" || "$1" == "-L" ]]; then
    echo "Looking up last Mariners game..."
    GAME_INFO=$(python3 - <<'PYEOF'
import urllib.request, json, sys
from datetime import datetime, timedelta, timezone

now   = datetime.now(timezone.utc)
end   = now.strftime('%Y-%m-%d')
start = (now - timedelta(days=14)).strftime('%Y-%m-%d')
url   = (f"https://statsapi.mlb.com/api/v1/schedule"
         f"?sportId=1&teamId=136&startDate={start}&endDate={end}&hydrate=linescore,team")
try:
    with urllib.request.urlopen(url, timeout=10) as r:
        data = json.load(r)
except Exception as e:
    print(f"ERROR: {e}", file=sys.stderr); sys.exit(1)

best = None; best_date = ""
for date_obj in data.get("dates", []):
    for game in date_obj.get("games", []):
        if game["status"]["abstractGameState"] == "Final":
            gdate = date_obj.get("date", "")
            if gdate >= best_date:
                best = game; best_date = gdate

if best is None:
    print("ERROR: No recent Final Mariners game found", file=sys.stderr); sys.exit(1)

away    = best["teams"]["away"]["team"]["abbreviation"]
home    = best["teams"]["home"]["team"]["abbreviation"]
away_sc = best["teams"]["away"].get("score", 0)
home_sc = best["teams"]["home"].get("score", 0)
print(f"{best['gamePk']} {best_date} {away} {home} {away_sc} {home_sc}")
PYEOF
    )
    read -r GAME_PK GAME_DATE AWAY HOME AWAY_SC HOME_SC <<< "$GAME_INFO"
    echo "  ${AWAY} ${AWAY_SC} @ ${HOME} ${HOME_SC}  (${GAME_DATE})  gamePk=${GAME_PK}"
    OUT_FILE="gamereplay/mlb_live_${GAME_PK}_${GAME_DATE}_${AWAY}@${HOME}_${AWAY_SC}-${HOME_SC}.json"
else
    GAME_PK="$1"
fi

# ── download ──────────────────────────────────────────────────────────────────

TMP=$(mktemp /tmp/mlb_game_XXXXXX.json)
trap 'rm -f "$TMP"' EXIT

echo "Downloading gamePk=${GAME_PK}..."
if ! curl -fsSL "${BASE_URL}/api/v1.1/game/${GAME_PK}/feed/live" -o "$TMP"; then
    echo "ERROR: Failed to download gamePk=${GAME_PK}" >&2
    exit 1
fi

# ── derive filename from response when gamePk was given directly ──────────────

if [[ -z "$OUT_FILE" ]]; then
    META=$(python3 - "$TMP" <<'PYEOF'
import json, sys
d  = json.load(open(sys.argv[1]))
gd = d["gameData"]
away    = gd["teams"]["away"]["abbreviation"]
home    = gd["teams"]["home"]["abbreviation"]
date    = gd.get("datetime", {}).get("dateTime", "")[:10]
ls      = d.get("liveData", {}).get("linescore", {}).get("teams", {})
away_sc = ls.get("away", {}).get("runs", 0)
home_sc = ls.get("home", {}).get("runs", 0)
print(f"{d['gamePk']} {date} {away} {home} {away_sc} {home_sc}")
PYEOF
    )
    read -r GAME_PK GAME_DATE AWAY HOME AWAY_SC HOME_SC <<< "$META"
    echo "  ${AWAY} ${AWAY_SC} @ ${HOME} ${HOME_SC}  (${GAME_DATE})"
    OUT_FILE="gamereplay/mlb_live_${GAME_PK}_${GAME_DATE}_${AWAY}@${HOME}_${AWAY_SC}-${HOME_SC}.json"
fi

mkdir -p gamereplay
mv "$TMP" "$OUT_FILE"
echo "Saved: ${OUT_FILE}  ($(wc -c < "$OUT_FILE") bytes)"
echo ""
echo "Run with:"
echo "  python pitch_replay.py ${OUT_FILE} --port 8080"
