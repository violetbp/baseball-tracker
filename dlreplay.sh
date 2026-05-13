#!/usr/bin/env bash
# dlreplay.sh — Poll an MLB live game feed using the native diffPatch API.
#               Also records the schedule endpoint periodically.
#
# Usage:
#   ./dlreplay.sh --last                         # auto-find last Mariners game → gamereplay/
#   ./dlreplay.sh <gamePk> [team_id] [output_dir]
#
# Examples:
#   ./dlreplay.sh --last
#   ./dlreplay.sh 747175
#   ./dlreplay.sh 747175 136
#   ./dlreplay.sh 747175 136 ./game_data
#
# How it works:
#   1. Fetches + saves the schedule once at startup, then every ~30s if it changes.
#   2. Fetches the full live feed once to get the initial game state + timestamp.
#   3. Every 2s, calls /feed/live/diffPatch?startTimecode=<last>&endTimecode=<now>
#   4. Saves a file only when the diff is non-empty (i.e. something actually changed).
#
# --last: queries the Mariners schedule for the most recent Final game and sets
#   the output dir to gamereplay/mlb_live_<gamePk>_<DATE>_<AWAY>@<HOME>_<score>.
#
# Timecode format: YYYYMMDD_HHMMSS (UTC) — as required by the MLB API.
# Files saved:
#   schedule_<timecode>.json                    — schedule snapshots (saved only when changed)
#   snapshot_+0s_<timecode>.json                — initial full game state
#   diff_+<elapsed>s_<start>_to_<end>.json      — incremental diffs

set -euo pipefail

# ── args ──────────────────────────────────────────────────────────────────────

USE_LAST=0
if [[ "${1:-}" == "--last" || "${1:-}" == "-L" ]]; then
    USE_LAST=1
    shift
elif [[ $# -lt 1 ]]; then
    echo "Usage: $0 [--last] <gamePk> [team_id] [output_dir]" >&2
    echo "  --last    : auto-find last Mariners game, save to gamereplay/" >&2
    echo "  gamePk    : MLB game ID" >&2
    echo "  team_id   : MLB team ID for schedule (default: 136)" >&2
    echo "  output_dir: where to save files (default: ./mlb_live_<gamePk>)" >&2
    exit 1
fi

GAME_PK="${1:-}"
TEAM_ID="${2:-136}"
OUT_DIR="${3:-}"

if [[ "$USE_LAST" == "1" ]]; then
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

best = None
best_date = ""
for date_obj in data.get("dates", []):
    for game in date_obj.get("games", []):
        if game["status"]["abstractGameState"] == "Final":
            gdate = date_obj.get("date", "")
            if gdate >= best_date:
                best = game
                best_date = gdate

if best is None:
    print("ERROR: No recent Final Mariners game found", file=sys.stderr); sys.exit(1)

pk      = best["gamePk"]
away    = best["teams"]["away"]["team"]["abbreviation"]
home    = best["teams"]["home"]["team"]["abbreviation"]
away_sc = best["teams"]["away"].get("score", 0)
home_sc = best["teams"]["home"].get("score", 0)
print(f"{pk} {best_date} {away} {home} {away_sc} {home_sc}")
PYEOF
    )
    GAME_PK=$(echo "$GAME_INFO" | awk '{print $1}')
    GAME_DATE=$(echo "$GAME_INFO" | awk '{print $2}')
    AWAY_ABBR=$(echo "$GAME_INFO" | awk '{print $3}')
    HOME_ABBR=$(echo "$GAME_INFO" | awk '{print $4}')
    AWAY_SC=$(echo "$GAME_INFO"   | awk '{print $5}')
    HOME_SC=$(echo "$GAME_INFO"   | awk '{print $6}')
    OUT_DIR="gamereplay/mlb_live_${GAME_PK}_${GAME_DATE}_${AWAY_ABBR}@${HOME_ABBR}_${AWAY_SC}-${HOME_SC}"
    echo "  gamePk=${GAME_PK}  ${AWAY_ABBR} ${AWAY_SC} @ ${HOME_ABBR} ${HOME_SC}  (${GAME_DATE})"
    echo "  → ${OUT_DIR}"
    echo ""
fi

[[ -z "$GAME_PK" ]] && { echo "ERROR: no gamePk" >&2; exit 1; }
[[ -z "$OUT_DIR" ]] && OUT_DIR="./mlb_live_${GAME_PK}"
INTERVAL=10
SCHEDULE_INTERVAL=30  # re-check schedule every N seconds

FIELDS="metaData,timeStamp,gamePk,
gameData,status,abstractGameState,detailedState,statusCode,
teams,away,home,abbreviation,id,
players,useLastName,
liveData,linescore,currentInning,currentInningOrdinal,isTopInning,
inningHalf,inningState,balls,strikes,outs,runs,
offense,defense,first,second,third,batter,pitcher,
plays,scoringPlays,
currentPlay,result,description,event,about,isScoringPlay,atBatIndex,halfInning"
FIELDS="${FIELDS//$'\n'/}"  # strip newlines so the URL stays valid




BASE="https://statsapi.mlb.com/api/v1.1/game/${GAME_PK}/feed/live?fields=${FIELDS}"
SCHEDULE_URL="https://statsapi.mlb.com/api/v1/schedule?sportId=1&teamId=${TEAM_ID}&hydrate=linescore,team"

START_TS=$(date +%s)
mkdir -p "$OUT_DIR"

TMP_FILE=$(mktemp /tmp/mlb_live_XXXXXX.json)
TMP_SCHED=$(mktemp /tmp/mlb_sched_XXXXXX.json)
trap 'rm -f "$TMP_FILE" "$TMP_SCHED"; echo ""; echo "Stopped."' EXIT

# ── helpers ───────────────────────────────────────────────────────────────────

now_utc() {
    date -u +"%Y%m%d_%H%M%S"
}

elapsed() {
    echo $(( $(date +%s) - START_TS ))
}

fetch() {
    curl -fsSL "$1" -o "$2" 2>/dev/null
}

# ── schedule fetch function ───────────────────────────────────────────────────
LAST_SCHED_FILE=""

save_schedule() {
    if ! fetch "$SCHEDULE_URL" "$TMP_SCHED"; then
        echo "[schedule] ERROR: fetch failed — skipping"
        return
    fi

    if [[ -n "$LAST_SCHED_FILE" ]] && diff -q "$TMP_SCHED" "$LAST_SCHED_FILE" > /dev/null 2>&1; then
        return  # no change, stay quiet
    fi

    local TC
    TC=$(now_utc)
    local SCHED_FILE="${OUT_DIR}/schedule_${TC}.json"
    cp "$TMP_SCHED" "$SCHED_FILE"
    LAST_SCHED_FILE="$SCHED_FILE"
    echo "[schedule] Saved: $(basename "$SCHED_FILE")"
}

# ── step 1: initial schedule snapshot ────────────────────────────────────────
echo "Fetching schedule for teamId=${TEAM_ID}..."
save_schedule

# ── step 2: initial live feed snapshot ───────────────────────────────────────
echo "Fetching initial snapshot for gamePk=${GAME_PK}..."
if ! fetch "${BASE}" "$TMP_FILE"; then
    echo "ERROR: Could not reach live feed. Check gamePk=${GAME_PK} and try again." >&2
    exit 1
fi

LAST_TC=$(python3 -c "
import json, sys
d = json.load(open('$TMP_FILE'))
print(d['metaData']['timeStamp'])
" 2>/dev/null || true)

if [[ -z "$LAST_TC" ]]; then
    echo "ERROR: Could not read metaData.timeStamp from response. Game may not be active." >&2
    cat "$TMP_FILE" >&2
    exit 1
fi

SNAP_FILE="${OUT_DIR}/snapshot_+0s_${LAST_TC}.json"
cp "$TMP_FILE" "$SNAP_FILE"
echo "Saved snapshot: $SNAP_FILE"
echo "Starting timecode: $LAST_TC"
echo ""
echo "Polling every ${INTERVAL}s (schedule every ${SCHEDULE_INTERVAL}s) — Ctrl+C to stop"
echo ""

FETCH_COUNT=0
SAVE_COUNT=0
LAST_SCHED_CHECK=$(date +%s)

# ── step 3: polling loop ──────────────────────────────────────────────────────
while true; do
    sleep "$INTERVAL"

    FETCH_COUNT=$(( FETCH_COUNT + 1 ))
    NOW_TC=$(now_utc)
    ELAPSED=$(elapsed)

    # Re-check schedule periodically
    NOW_WALL=$(date +%s)
    if (( NOW_WALL - LAST_SCHED_CHECK >= SCHEDULE_INTERVAL )); then
        save_schedule
        LAST_SCHED_CHECK=$NOW_WALL
    fi

    DIFF_URL="${BASE}/diffPatch&startTimecode=${LAST_TC}&endTimecode=${NOW_TC}"

    if ! fetch "$DIFF_URL" "$TMP_FILE"; then
        printf "[+%ss] ERROR: fetch failed (fetch #%d) — retrying\r" "$ELAPSED" "$FETCH_COUNT"
        continue
    fi

    DIFF_CONTENT=$(cat "$TMP_FILE")
    if [[ "$DIFF_CONTENT" == "{}" || "$DIFF_CONTENT" == "[]" || -z "$DIFF_CONTENT" ]]; then
        printf "[+%ss] No change %s → %s (fetch #%d, %d saved)\r" \
            "$ELAPSED" "$LAST_TC" "$NOW_TC" "$FETCH_COUNT" "$SAVE_COUNT"
    else
        SAVE_COUNT=$(( SAVE_COUNT + 1 ))
        OUT_FILE="${OUT_DIR}/diff_+${ELAPSED}s_${LAST_TC}_to_${NOW_TC}.json"
        cp "$TMP_FILE" "$OUT_FILE"
        echo "[+${ELAPSED}s] Change! Saved: $(basename "$OUT_FILE") (${SAVE_COUNT} diffs total)"
        LAST_TC="$NOW_TC"
    fi
done