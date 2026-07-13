#!/usr/bin/env python3
"""
mlb_replay_server.py — MLB Stats API replay server.

Serves saved game data (from fetch_mlb_live.sh) through the exact same URL
structure as the real MLB Stats API. Your app will not know the difference.

Usage:
    pip install fastapi uvicorn

    # Serve a saved game directory (contains snapshot_*.json + diff_*.json):
    python mlb_replay_server.py ./mlb_live_747175

    # With options:
    python mlb_replay_server.py ./mlb_live_747175 --port 8080 --speed 2.0

    # Then point your app at http://localhost:8000 instead of https://statsapi.mlb.com
    # Everything under /api/v1.1/game/<gamePk>/feed/live* is handled here.
    # Everything else is proxied transparently to the real MLB API.

Endpoints served (matching the real API exactly):
    GET /api/v1.1/game/{gamePk}/feed/live
        Returns the full game state as it existed at the current replay time.
        Supports ?timecode=YYYYMMDD_HHMMSS to jump to a specific moment.

    GET /api/v1.1/game/{gamePk}/feed/live/diffPatch
        Requires ?startTimecode=...&endTimecode=...
        Returns the diff between those two timecodes, just like the real API.

    GET /api/v1.1/game/{gamePk}/feed/live/timestamps
        Returns the list of all timecodes where the game state changed.

    GET /api/v1/schedule  (and all other /api/* paths)
        Proxied transparently to statsapi.mlb.com.
"""

import argparse
import json
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

import httpx
from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.responses import JSONResponse, Response

# ── CLI args ──────────────────────────────────────────────────────────────────

parser = argparse.ArgumentParser(description="MLB Stats API replay server")
parser.add_argument("data_dir", help="Directory containing snapshot_*.json and diff_*.json files")
parser.add_argument("--port", type=int, default=8000, help="Port to listen on (default: 8000)")
parser.add_argument("--host", default="0.0.0.0", help="Host to bind to (default: 0.0.0.0)")
parser.add_argument(
    "--speed",
    type=float,
    default=1.0,
    help="Replay speed multiplier (default: 1.0 = real time, 2.0 = double speed)",
)
parser.add_argument(
    "--loop",
    action="store_true",
    help="Loop the replay when it reaches the end",
)
args = parser.parse_args()

DATA_DIR = Path(args.data_dir).resolve()
if not DATA_DIR.exists():
    print(f"ERROR: data directory not found: {DATA_DIR}", file=sys.stderr)
    sys.exit(1)

# ── Load and index saved files ────────────────────────────────────────────────

TC_RE = re.compile(r"(\d{8}_\d{6})")  # YYYYMMDD_HHMMSS


def parse_tc(tc: str) -> datetime:
    """Parse a timecode string to a UTC datetime."""
    return datetime.strptime(tc, "%Y%m%d_%H%M%S").replace(tzinfo=timezone.utc)


def fmt_tc(dt: datetime) -> str:
    return dt.strftime("%Y%m%d_%H%M%S")


def tc_to_epoch(tc: str) -> float:
    return parse_tc(tc).timestamp()


# Find the snapshot (initial full state)
snapshots = sorted(DATA_DIR.glob("snapshot_*.json"))
if not snapshots:
    print(f"ERROR: no snapshot_*.json found in {DATA_DIR}", file=sys.stderr)
    sys.exit(1)
snapshot_path = snapshots[0]  # use the earliest snapshot

snapshot_data = json.loads(snapshot_path.read_text())
GAME_PK = str(snapshot_data.get("gamePk", ""))
if not GAME_PK:
    # Try to infer from directory name
    m = re.search(r"(\d+)", DATA_DIR.name)
    GAME_PK = m.group(1) if m else "000000"

# Extract the starting timecode from the snapshot
SNAPSHOT_TC = snapshot_data.get("metaData", {}).get("timeStamp", "")
if not SNAPSHOT_TC:
    # Fall back to filename
    m = TC_RE.search(snapshot_path.name)
    SNAPSHOT_TC = m.group(1) if m else fmt_tc(datetime.now(timezone.utc))

print(f"Loaded snapshot: {snapshot_path.name}")
print(f"  gamePk        : {GAME_PK}")
print(f"  start timecode: {SNAPSHOT_TC}")

# Find all diff files and build a sorted timeline
# Filename format: diff_+<elapsed>s_<startTC>_to_<endTC>.json
DIFF_RE = re.compile(r"diff_\+\d+s_(\d{8}_\d{6})_to_(\d{8}_\d{6})\.json")

diffs: list[dict] = []  # [{start_tc, end_tc, path}]
for f in sorted(DATA_DIR.glob("diff_*.json")):
    m = DIFF_RE.match(f.name)
    if m:
        diffs.append({"start_tc": m.group(1), "end_tc": m.group(2), "path": f})

diffs.sort(key=lambda d: tc_to_epoch(d["start_tc"]))
print(f"  diffs found   : {len(diffs)}")

# Find the most recently saved schedule file (latest by filename — timecodes sort lexicographically)
schedule_files = sorted(DATA_DIR.glob("schedule_*.json"))
if schedule_files:
    schedule_path = schedule_files[-1]
    schedule_data = json.loads(schedule_path.read_text())
    print(f"  schedule file : {schedule_path.name}")
else:
    schedule_data = None
    print("  schedule file : none found — schedule will be proxied live")

# Build ordered list of all known timecodes (snapshot + each diff endpoint)
all_timecodes: list[str] = [SNAPSHOT_TC]
for d in diffs:
    if d["end_tc"] not in all_timecodes:
        all_timecodes.append(d["end_tc"])
all_timecodes.sort()

print(f"  timecodes     : {len(all_timecodes)}")
print(f"  replay speed  : {args.speed}x")
print()

# ── Replay state ──────────────────────────────────────────────────────────────

# We maintain a single "current state" dict that we patch forward as time passes.
# Diffs are applied in order when the replay clock passes their end timecode.

class ReplayState:
    def __init__(self):
        self.reset()

    def reset(self):
        self.state: dict = json.loads(snapshot_path.read_text())
        self.applied_diffs: list[str] = []      # end_tcs of applied diffs
        self.wall_start: float = time.monotonic()
        self.game_start_epoch: float = tc_to_epoch(SNAPSHOT_TC)
        self.finished: bool = False

    def game_elapsed(self) -> float:
        """Seconds of game time that have passed since replay started."""
        wall_elapsed = time.monotonic() - self.wall_start
        return wall_elapsed * args.speed

    def current_game_epoch(self) -> float:
        return self.game_start_epoch + self.game_elapsed()

    def current_tc(self) -> str:
        return fmt_tc(datetime.fromtimestamp(self.current_game_epoch(), tz=timezone.utc))

    def advance(self):
        """Apply any diffs whose end_tc has passed in game time."""
        now_epoch = self.current_game_epoch()
        for d in diffs:
            if d["end_tc"] in self.applied_diffs:
                continue
            if tc_to_epoch(d["end_tc"]) <= now_epoch:
                patch = json.loads(d["path"].read_text())
                if patch and patch != {} and patch != []:
                    deep_merge(self.state, patch)
                self.applied_diffs.append(d["end_tc"])

        # Check if we've passed the last timecode
        if all_timecodes and tc_to_epoch(all_timecodes[-1]) <= now_epoch:
            if not self.finished:
                self.finished = True
                print("Replay reached end of recorded data.")
            if args.loop:
                print("Looping replay...")
                self.reset()

    def state_at(self, tc: str) -> dict:
        """Return a copy of state as it was at the given timecode."""
        target_epoch = tc_to_epoch(tc)
        state_copy = json.loads(snapshot_path.read_text())
        for d in diffs:
            if tc_to_epoch(d["end_tc"]) <= target_epoch:
                patch = json.loads(d["path"].read_text())
                if patch and patch != {} and patch != []:
                    deep_merge(state_copy, patch)
        return state_copy

    def diff_between(self, start_tc: str, end_tc: str) -> dict:
        """Return accumulated diff between two timecodes."""
        start_epoch = tc_to_epoch(start_tc)
        end_epoch = tc_to_epoch(end_tc)
        accumulated: dict = {}
        for d in diffs:
            d_end = tc_to_epoch(d["end_tc"])
            d_start = tc_to_epoch(d["start_tc"])
            # Include diffs that fall within the window
            if d_start >= start_epoch and d_end <= end_epoch:
                patch = json.loads(d["path"].read_text())
                if patch and patch != {} and patch != []:
                    deep_merge(accumulated, patch)
        return accumulated


def deep_merge(base: dict, patch: dict) -> None:
    """Recursively merge patch into base in-place."""
    for key, val in patch.items():
        if key in base and isinstance(base[key], dict) and isinstance(val, dict):
            deep_merge(base[key], val)
        else:
            base[key] = val


replay = ReplayState()

# ── FastAPI app ───────────────────────────────────────────────────────────────

app = FastAPI(
    title="MLB Replay Server",
    description="Serves saved MLB game data as a live API replay.",
    version="1.0.0",
)

LIVE_BASE = f"/api/v1.1/game/{GAME_PK}/feed/live"


def live_response(state: dict, current_tc: str) -> dict:
    """Stamp the state with current replay timecode and return it."""
    out = json.loads(json.dumps(state))  # deep copy
    if "metaData" not in out:
        out["metaData"] = {}
    out["metaData"]["timeStamp"] = current_tc
    out["metaData"]["wait"] = 10  # match real API's recommended poll interval
    return out


# ── /feed/live ────────────────────────────────────────────────────────────────

@app.get(LIVE_BASE)
async def get_live_feed(timecode: Optional[str] = Query(None)):
    replay.advance()

    if timecode:
        # Validate format
        if not TC_RE.fullmatch(timecode):
            raise HTTPException(status_code=400, detail="Invalid timecode format. Use YYYYMMDD_HHMMSS")
        state = replay.state_at(timecode)
        return JSONResponse(live_response(state, timecode))

    current_tc = replay.current_tc()
    return JSONResponse(live_response(replay.state, current_tc))


# ── /feed/live/diffPatch ──────────────────────────────────────────────────────

@app.get(f"{LIVE_BASE}/diffPatch")
async def get_diff_patch(
    startTimecode: str = Query(...),
    endTimecode: str = Query(...),
):
    replay.advance()

    if not TC_RE.fullmatch(startTimecode) or not TC_RE.fullmatch(endTimecode):
        raise HTTPException(status_code=400, detail="Timecodes must be YYYYMMDD_HHMMSS format")

    diff = replay.diff_between(startTimecode, endTimecode)
    return JSONResponse(diff if diff else {})


# ── /feed/live/timestamps ─────────────────────────────────────────────────────

@app.get(f"{LIVE_BASE}/timestamps")
async def get_timestamps():
    replay.advance()
    # Return only timecodes that have passed in the current replay
    now_epoch = replay.current_game_epoch()
    visible = [tc for tc in all_timecodes if tc_to_epoch(tc) <= now_epoch]
    return JSONResponse(visible)


# ── /api/v1/schedule ─────────────────────────────────────────────────────────

@app.get("/api/v1/schedule")
async def get_schedule(request: Request):
    """Serve the saved schedule file, falling back to live proxy if none recorded."""
    if schedule_data is not None:
        return JSONResponse(schedule_data)

    # Fallback: proxy to real API
    url = f"{MLB_API}/api/v1/schedule"
    if request.query_params:
        url += f"?{request.query_params}"
    async with httpx.AsyncClient(timeout=10.0) as client:
        try:
            resp = await client.get(url)
            return Response(content=resp.content, status_code=resp.status_code, headers=dict(resp.headers))
        except httpx.RequestError as e:
            raise HTTPException(status_code=502, detail=f"Upstream MLB API error: {e}")


# ── Transparent proxy for everything else ────────────────────────────────────

MLB_API = "https://statsapi.mlb.com"

@app.api_route("/{path:path}", methods=["GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"])
async def proxy(request: Request, path: str):
    url = f"{MLB_API}/{path}"
    if request.query_params:
        url += f"?{request.query_params}"

    async with httpx.AsyncClient(timeout=10.0) as client:
        try:
            resp = await client.request(
                method=request.method,
                url=url,
                headers={k: v for k, v in request.headers.items() if k.lower() != "host"},
                content=await request.body(),
            )
            return Response(
                content=resp.content,
                status_code=resp.status_code,
                headers=dict(resp.headers),
            )
        except httpx.RequestError as e:
            raise HTTPException(status_code=502, detail=f"Upstream MLB API error: {e}")


# ── Startup banner ────────────────────────────────────────────────────────────

@app.on_event("startup")
async def startup():
    print(f"MLB Replay Server running on http://{args.host}:{args.port}")
    print(f"Replaying gamePk {GAME_PK} from {SNAPSHOT_TC}")
    print()
    print("Point your app at this server instead of https://statsapi.mlb.com")
    print(f"  Live feed : http://localhost:{args.port}{LIVE_BASE}")
    print(f"  Timestamps: http://localhost:{args.port}{LIVE_BASE}/timestamps")
    print(f"  Diff patch: http://localhost:{args.port}{LIVE_BASE}/diffPatch?startTimecode=...&endTimecode=...")
    sched_status = f"served from {schedule_files[-1].name}" if schedule_data else "proxied live (no saved file)"
    print(f"  Schedule  : {sched_status}")
    print(f"  All other routes proxy transparently to statsapi.mlb.com")
    print()


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=args.host, port=args.port, log_level="warning")