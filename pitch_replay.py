#!/usr/bin/env python3
"""
pitch_replay.py — Pitch-by-pitch MLB game replay server.

Loads a complete MLB feed/live JSON and advances one pitch event per poll.
Point the tracker's base_url at this server instead of statsapi.mlb.com.

Usage:
    python pitch_replay.py fullapi_inprogress.json
    python pitch_replay.py fullapi_inprogress.json --port 8080 --loop

Endpoints:
    GET  /api/v1.1/game/<pk>/feed/live   → current state, then advances cursor
    GET  /api/v1/schedule                → synthesized schedule (Live or Final)
    GET  /replay/status                  → cursor position / current state info
    POST /replay/reset                   → restart from pitch 1
    All other GET routes proxy to statsapi.mlb.com.
"""

import argparse
import json
import sys
from pathlib import Path

import httpx
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse, Response

# ── CLI ───────────────────────────────────────────────────────────────────────

parser = argparse.ArgumentParser(description="Pitch-by-pitch MLB game replay")
parser.add_argument("game_file", help="Complete MLB feed/live JSON (e.g. fullapi_inprogress.json)")
parser.add_argument("--port",  type=int, default=8000)
parser.add_argument("--host",  default="0.0.0.0")
parser.add_argument("--loop",  action="store_true", help="Restart from pitch 1 when game ends")
args = parser.parse_args()

# ── Load game data ────────────────────────────────────────────────────────────

raw = json.loads(Path(args.game_file).read_text())

GAME_PK      = str(raw["gamePk"])
gd           = raw["gameData"]
all_plays    = raw["liveData"]["plays"]["allPlays"]
scoring_idxs = raw["liveData"]["plays"]["scoringPlays"]  # list of atBatIndex with a run scored

away_abbrev  = gd["teams"]["away"]["abbreviation"]
home_abbrev  = gd["teams"]["home"]["abbreviation"]
away_id      = gd["teams"]["away"]["id"]
home_id      = gd["teams"]["home"]["id"]
game_datetime = gd["datetime"]["dateTime"]

# Slim player map: tracker only needs id + useLastName
players_slim = {
    k: {"id": p["id"], "useLastName": p["useLastName"]}
    for k, p in gd["players"].items()
}

ORDINALS = ["0th", "1st", "2nd", "3rd", "4th", "5th", "6th",
            "7th", "8th", "9th", "10th", "11th", "12th", "13th", "14th", "15th"]

def ordinal(n: int) -> str:
    return ORDINALS[n] if 0 <= n < len(ORDINALS) else f"{n}th"


# Score after each at-bat completes (from result, which is definitive).
# Index 0 = score after at-bat 0, etc. Score before at-bat 0 = (0, 0).
_score_after: list[tuple[int, int]] = [
    (p.get("result", {}).get("awayScore", 0),
     p.get("result", {}).get("homeScore", 0))
    for p in all_plays
]

def score_at(ab_idx: int, is_last_ev: bool) -> tuple[int, int]:
    """Score to display for a pitch step.
    Show the updated score on the final pitch of an at-bat, the prior
    score for all intermediate pitches."""
    if is_last_ev:
        return _score_after[ab_idx]
    return _score_after[ab_idx - 1] if ab_idx > 0 else (0, 0)

# ── Precompute runner state at the start of each at-bat ──────────────────────
#
# Tracks bases {1,2,3: bool} by applying each completed at-bat's runner
# movements in sequence. Bases reset to empty at the start of each half-inning.
# Mid-at-bat runner movements (stolen bases, wild pitches in playEvents) are
# not modelled — runners are shown in their start-of-at-bat positions.

def compute_runner_states(plays: list) -> list[dict]:
    states = []
    bases  = {1: False, 2: False, 3: False}
    prev_half_key = None

    for play in plays:
        about    = play["about"]
        half_key = (about["inning"], about["halfInning"])

        if half_key != prev_half_key and prev_half_key is not None:
            bases = {1: False, 2: False, 3: False}
        prev_half_key = half_key

        states.append({k: v for k, v in bases.items()})

        for runner in play.get("runners", []):
            mv     = runner["movement"]
            start  = mv.get("start")   # "1B" / "2B" / "3B" / None (batter arriving)
            end    = mv.get("end")     # "1B" / "2B" / "3B" / "score" / None
            is_out = mv.get("isOut", False)
            if start in ("1B", "2B", "3B"):
                bases[int(start[0])] = False
            if end in ("1B", "2B", "3B") and not is_out:
                bases[int(end[0])] = True

    return states

runner_states_at_ab = compute_runner_states(all_plays)

# ── Build the flat step sequence ──────────────────────────────────────────────
#
# Each step is a dict of the synthesized game state to show on that poll.
# Kinds:
#   pitch        – one pitch/action event within an at-bat
#   intermission – break between half-innings (inningState Middle or End)
#   final        – game over

def make_pitch_step(ab_idx: int, ev_idx: int) -> dict:
    play    = all_plays[ab_idx]
    ev      = play["playEvents"][ev_idx]
    about   = play["about"]
    cnt     = ev["count"]
    runners = runner_states_at_ab[ab_idx]

    is_last_ev = (ev_idx == len(play["playEvents"]) - 1)
    away_sc, home_sc = score_at(ab_idx, is_last_ev)

    sp_threshold = ab_idx if is_last_ev else ab_idx - 1
    sp_count = sum(1 for s in scoring_idxs if s <= sp_threshold)

    return {
        "kind":           "pitch",
        "ab_idx":         ab_idx,
        "inning":         about["inning"],
        "is_top":         about["isTopInning"],
        "inning_state":   "Top" if about["isTopInning"] else "Bottom",
        "balls":          cnt["balls"],
        "strikes":        cnt["strikes"],
        "outs":           cnt["outs"],
        "away_score":     away_sc,
        "home_score":     home_sc,
        "runner_1":       runners[1],
        "runner_2":       runners[2],
        "runner_3":       runners[3],
        "batter_id":      play["matchup"]["batter"]["id"],
        "pitcher_id":     play["matchup"]["pitcher"]["id"],
        "abstract_state": "Live",
        "sp_count":       sp_count,
        "current_play":   play,
    }


def make_intermission_step(ab_idx: int, inning_state: str) -> dict:
    play     = all_plays[ab_idx]
    about    = play["about"]
    away_sc, home_sc = _score_after[ab_idx]
    sp_count = sum(1 for s in scoring_idxs if s <= ab_idx)

    return {
        "kind":          "intermission",
        "ab_idx":        ab_idx,
        "inning":        about["inning"],
        "is_top":        about["isTopInning"],
        "inning_state":  inning_state,
        "balls":         0,
        "strikes":       0,
        "outs":          0,
        "away_score":    away_sc,
        "home_score":    home_sc,
        "runner_1":      False,
        "runner_2":      False,
        "runner_3":      False,
        "batter_id":     0,
        "pitcher_id":    play["matchup"]["pitcher"]["id"],
        "abstract_state": "Live",
        "sp_count":      sp_count,
        "current_play":  play,
    }


def make_final_step(ab_idx: int) -> dict:
    play   = all_plays[ab_idx]
    result = play.get("result", {})
    about  = play.get("about", {})

    return {
        "kind":          "final",
        "ab_idx":        ab_idx,
        "inning":        about.get("inning", 9),
        "is_top":        about.get("isTopInning", False),
        "inning_state":  "",
        "balls":         0,
        "strikes":       0,
        "outs":          3,
        "away_score":    result.get("awayScore", 0),
        "home_score":    result.get("homeScore", 0),
        "runner_1":      False,
        "runner_2":      False,
        "runner_3":      False,
        "batter_id":     0,
        "pitcher_id":    0,
        "abstract_state": "Final",
        "sp_count":      len(scoring_idxs),
        "current_play":  play,
    }


steps: list[dict] = []

for ab_idx, play in enumerate(all_plays):
    for ev_idx in range(len(play["playEvents"])):
        steps.append(make_pitch_step(ab_idx, ev_idx))

    if ab_idx + 1 < len(all_plays):
        this_half = (play["about"]["inning"], play["about"]["halfInning"])
        next_half = (all_plays[ab_idx + 1]["about"]["inning"],
                     all_plays[ab_idx + 1]["about"]["halfInning"])
        if next_half != this_half:
            istate = "Middle" if play["about"]["isTopInning"] else "End"
            steps.append(make_intermission_step(ab_idx, istate))

steps.append(make_final_step(len(all_plays) - 1))

pitch_count = sum(1 for s in steps if s["kind"] == "pitch")
print(f"Loaded: {away_abbrev} @ {home_abbrev}  gamePk={GAME_PK}")
print(f"  {len(all_plays)} at-bats  |  {pitch_count} pitch steps  |  {len(scoring_idxs)} scoring plays")
print(f"  Total steps (with intermissions + final): {len(steps)}")

# ── Replay cursor ─────────────────────────────────────────────────────────────

cursor = 0


def advance():
    global cursor
    if cursor < len(steps) - 1:
        cursor += 1
    elif args.loop:
        cursor = 0


def current_step() -> dict:
    return steps[cursor]

# ── Response synthesis ────────────────────────────────────────────────────────


def build_live_response(step: dict) -> dict:
    offense: dict = {}
    if step["runner_1"]: offense["first"]  = {"id": 0}
    if step["runner_2"]: offense["second"] = {"id": 0}
    if step["runner_3"]: offense["third"]  = {"id": 0}
    if step["batter_id"]:
        offense["batter"] = {"id": step["batter_id"]}

    defense: dict = {}
    if step["pitcher_id"]:
        defense["pitcher"] = {"id": step["pitcher_id"]}

    linescore = {
        "currentInning":        step["inning"],
        "currentInningOrdinal": ordinal(step["inning"]),
        "isTopInning":          step["is_top"],
        "inningState":          step["inning_state"],
        "balls":                step["balls"],
        "strikes":              step["strikes"],
        "outs":                 step["outs"],
        "teams": {
            "away": {"runs": step["away_score"]},
            "home": {"runs": step["home_score"]},
        },
        "offense": offense,
        "defense": defense,
    }

    sp_indices = [s for s in scoring_idxs if s <= step["ab_idx"]]

    return {
        "gamePk": int(GAME_PK),
        "gameData": {
            "status": {
                "abstractGameState": step["abstract_state"],
                "detailedState": "Final" if step["abstract_state"] == "Final" else "In Progress",
            },
            "teams": {
                "away": {"abbreviation": away_abbrev},
                "home": {"abbreviation": home_abbrev},
            },
            "players": players_slim,
        },
        "liveData": {
            "linescore": linescore,
            "plays": {
                "scoringPlays": sp_indices,
                "currentPlay":  step["current_play"],
            },
        },
    }


def build_schedule_response(step: dict) -> dict:
    abstract = step["abstract_state"]
    return {
        "totalGames": 1,
        "dates": [{
            "games": [{
                "gamePk":    int(GAME_PK),
                "gameDate":  game_datetime,
                "status": {
                    "abstractGameState": abstract,
                    "detailedState": "Final" if abstract == "Final" else "In Progress",
                },
                "teams": {
                    "away": {
                        "score": step["away_score"],
                        "team":  {"abbreviation": away_abbrev, "id": away_id},
                    },
                    "home": {
                        "score": step["home_score"],
                        "team":  {"abbreviation": home_abbrev, "id": home_id},
                    },
                },
            }]
        }],
    }

# ── Field filter (mirrors the MLB Stats API ?fields= behaviour) ───────────────

def apply_field_filter(obj, fields: set):
    """Recursively keep only keys whose names appear in fields (flat whitelist).
    Keys like 'ID670541' are dynamic data keys (player map), not schema fields —
    pass them through so the players dict isn't wiped."""
    if isinstance(obj, dict):
        return {
            k: apply_field_filter(v, fields)
            for k, v in obj.items()
            if k in fields or (k.startswith("ID") and k[2:].isdigit())
        }
    if isinstance(obj, list):
        return [apply_field_filter(item, fields) for item in obj]
    return obj

# ── FastAPI app ───────────────────────────────────────────────────────────────

app      = FastAPI()
LIVE_BASE = f"/api/v1.1/game/{GAME_PK}/feed/live"
MLB_API   = "https://statsapi.mlb.com"


@app.get(LIVE_BASE)
async def get_live_feed(request: Request):
    step = current_step()
    resp = build_live_response(step)
    advance()
    fields_param = request.query_params.get("fields")
    if fields_param:
        fields = set(fields_param.split(","))
        resp = apply_field_filter(resp, fields)
    return JSONResponse(resp)


@app.get("/api/v1/schedule")
async def get_schedule():
    return JSONResponse(build_schedule_response(current_step()))


@app.get("/replay/status")
async def replay_status():
    s = current_step()
    return {
        "cursor":    cursor,
        "total":     len(steps),
        "kind":      s["kind"],
        "inning":    s["inning"],
        "half":      "top" if s["is_top"] else "bottom",
        "balls":     s["balls"],
        "strikes":   s["strikes"],
        "outs":      s["outs"],
        "score":     f"{away_abbrev} {s['away_score']}  {home_abbrev} {s['home_score']}",
    }


@app.post("/replay/reset")
async def replay_reset():
    global cursor
    cursor = 0
    return {"ok": True, "cursor": 0, "total": len(steps)}


@app.api_route("/{path:path}", methods=["GET"])
async def proxy(request: Request, path: str):
    url = f"{MLB_API}/{path}"
    if request.query_params:
        url += f"?{request.query_params}"
    async with httpx.AsyncClient(timeout=10.0) as client:
        try:
            resp = await client.get(
                url, headers={"User-Agent": "ESPHome-BaseballTracker/1.0"}
            )
            return Response(content=resp.content, status_code=resp.status_code)
        except httpx.RequestError as e:
            raise HTTPException(status_code=502, detail=str(e))


@app.on_event("startup")
async def startup():
    print(f"\nPitch replay server → http://{args.host}:{args.port}")
    print(f"  Live feed : http://localhost:{args.port}{LIVE_BASE}")
    print(f"  Status    : http://localhost:{args.port}/replay/status")
    print(f"  Reset     : POST http://localhost:{args.port}/replay/reset")
    print(f"\nPoint the tracker at:  base_url: http://<host>:{args.port}")
    print(f"Set poll_interval low (e.g. 1s) for fast stepping.\n")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=args.host, port=args.port, log_level="warning")
