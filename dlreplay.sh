#!/usr/bin/env bash
# fetch_mlb_live.sh — Poll an MLB live game feed using the native diffPatch API.
#                     Also records the schedule endpoint periodically.
#
# Usage:
#   ./fetch_mlb_live.sh <gamePk> [team_id] [output_dir]
#
# Examples:
#   ./fetch_mlb_live.sh 747175
#   ./fetch_mlb_live.sh 747175 116
#   ./fetch_mlb_live.sh 747175 116 ./game_data
#
# How it works:
#   1. Fetches + saves the schedule once at startup, then every ~30s if it changes.
#   2. Fetches the full live feed once to get the initial game state + timestamp.
#   3. Every 2s, calls /feed/live/diffPatch?startTimecode=<last>&endTimecode=<now>
#   4. Saves a file only when the diff is non-empty (i.e. something actually changed).
#
# Timecode format: YYYYMMDD_HHMMSS (UTC) — as required by the MLB API.
# Files saved:
#   schedule_<timecode>.json                    — schedule snapshots (saved only when changed)
#   snapshot_+0s_<timecode>.json                — initial full game state
#   diff_+<elapsed>s_<start>_to_<end>.json      — incremental diffs

set -euo pipefail

# ── args ──────────────────────────────────────────────────────────────────────
if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <gamePk> [team_id] [output_dir]" >&2
    echo "  gamePk    : MLB game ID" >&2
    echo "  team_id   : MLB team ID for schedule (default: 116)" >&2
    echo "  output_dir: where to save files (default: ./mlb_live_<gamePk>)" >&2
    exit 1
fi

GAME_PK="$1"
TEAM_ID="${2:-116}"
OUT_DIR="${3:-./mlb_live_${GAME_PK}}"
INTERVAL=10
SCHEDULE_INTERVAL=30  # re-check schedule every N seconds

FIELDS="metaData,timeStamp,wait,gamePk,gameData,status,abstractGameState,detailedState,teams,away,home,name,id,liveData,linescore,currentInning,currentInningOrdinal,inningHalf,isTopInning,outs,teams,runs,offense,first,second,third,batter,pitcher,plays,currentPlay,matchup,batter,useLastName,pitcher,useLastName,count,balls,strikes,outs"

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