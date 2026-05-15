#!/usr/bin/env python3
"""
pitch_replay.py — Pitch-by-pitch MLB game replay server with TUI.

Loads a complete MLB feed/live JSON and advances one pitch event per poll.
Point the tracker's base_url at this server instead of statsapi.mlb.com.

Usage:
    python pitch_replay.py fullapi_inprogress.json
    python pitch_replay.py fullapi_inprogress.json --port 8080 --loop
    python pitch_replay.py fullapi_inprogress.json --no-tui

Endpoints:
    GET  /api/v1.1/game/<pk>/feed/live   → current state, then advances cursor
    GET  /api/v1/schedule                → synthesized schedule (Live or Final)
    GET  /replay/status                  → cursor position / current state info
    POST /replay/reset                   → restart from step 0
    POST /replay/goto/<idx>              → jump cursor to step idx
    All other GET routes proxy to statsapi.mlb.com.

TUI keys:
    j / ↓       move selection down
    k / ↑       move selection up
    PgDn/PgUp   move by page
    g / Home    jump to top
    G / End     jump to bottom
    Space       toggle skip on selected step
    Enter       move server cursor to selected step
    f           snap selection to current server cursor
    r           reset server cursor to step 0
    u           clear all skips
    q / Esc     quit
"""

import argparse
import curses
import json
import logging
import threading
from pathlib import Path

import httpx
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse, Response

# ── CLI ───────────────────────────────────────────────────────────────────────

parser = argparse.ArgumentParser(description="Pitch-by-pitch MLB game replay")
parser.add_argument("game_file", help="Complete MLB feed/live JSON (e.g. fullapi_inprogress.json)")
parser.add_argument("--port",   type=int, default=8000)
parser.add_argument("--host",   default="0.0.0.0")
parser.add_argument("--loop",   action="store_true", help="Restart from step 0 when game ends")
parser.add_argument("--no-tui", action="store_true", help="Run headless without TUI")
args = parser.parse_args()

# ── Load game data ────────────────────────────────────────────────────────────

raw = json.loads(Path(args.game_file).read_text())

GAME_PK       = str(raw["gamePk"])
gd            = raw["gameData"]
all_plays     = raw["liveData"]["plays"]["allPlays"]
scoring_idxs  = raw["liveData"]["plays"]["scoringPlays"]

away_abbrev   = gd["teams"]["away"]["abbreviation"]
home_abbrev   = gd["teams"]["home"]["abbreviation"]
away_id       = gd["teams"]["away"]["id"]
home_id       = gd["teams"]["home"]["id"]
game_datetime = gd["datetime"]["dateTime"]

players_slim = {
    k: {"id": p["id"], "useLastName": p["useLastName"]}
    for k, p in gd["players"].items()
}

ORDINALS = ["0th", "1st", "2nd", "3rd", "4th", "5th", "6th",
            "7th", "8th", "9th", "10th", "11th", "12th", "13th", "14th", "15th"]

def ordinal(n: int) -> str:
    return ORDINALS[n] if 0 <= n < len(ORDINALS) else f"{n}th"


_score_after: list[tuple[int, int]] = [
    (p.get("result", {}).get("awayScore", 0),
     p.get("result", {}).get("homeScore", 0))
    for p in all_plays
]

def score_at(ab_idx: int, is_last_ev: bool) -> tuple[int, int]:
    if is_last_ev:
        return _score_after[ab_idx]
    return _score_after[ab_idx - 1] if ab_idx > 0 else (0, 0)


# ── Runner state precompute ───────────────────────────────────────────────────

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
            start  = mv.get("start")
            end    = mv.get("end")
            is_out = mv.get("isOut", False)
            if start in ("1B", "2B", "3B"):
                bases[int(start[0])] = False
            if end in ("1B", "2B", "3B") and not is_out:
                bases[int(end[0])] = True

    return states

runner_states_at_ab = compute_runner_states(all_plays)

# ── Step sequence ─────────────────────────────────────────────────────────────

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
    
    desc = ev.get("details", {}).get("description", "") or ev.get("type", {}).get("description", "")
    
    resultdesc = play["result"]["description"] if is_last_ev else ""


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
        "desc":           desc,
        "resultdesc":     resultdesc,
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
        "desc":          "half-inning break",
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
        "desc":          "game over",
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

cursor:  int      = 0
skipped: set[int] = set()


def advance():
    global cursor
    if cursor >= len(steps) - 1:
        if args.loop:
            cursor = 0
        return
    nxt = cursor + 1
    # Skip over marked steps; bound iterations to avoid infinite loop if all skipped
    for _ in range(len(steps)):
        if nxt not in skipped:
            break
        if nxt < len(steps) - 1:
            nxt += 1
        elif args.loop:
            nxt = 0
        else:
            break
    cursor = nxt


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


def apply_field_filter(obj, fields: set):
    """Recursively keep only keys whose names appear in fields (flat whitelist).
    Keys like 'ID670541' are dynamic player-map keys — pass them through."""
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

app       = FastAPI()
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
        "skipped":   sorted(skipped),
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


@app.post("/replay/goto/{idx}")
async def replay_goto(idx: int):
    global cursor
    if not (0 <= idx < len(steps)):
        raise HTTPException(status_code=400, detail="index out of range")
    cursor = idx
    return {"ok": True, "cursor": cursor}


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

# ── TUI ───────────────────────────────────────────────────────────────────────

_C_NORMAL  = 0
_C_SEL     = 1   # TUI cursor row
_C_SERVER  = 2   # current server position
_C_SKIP    = 3   # skipped step
_C_HEADER  = 4
_C_BOTH    = 5   # TUI cursor + server cursor on same row


_DESC_W = 20  # fixed width for the description column

def _col_header_row(width: int) -> str:
    # Each column separated by 2 spaces, matching _step_row exactly.
    # Away/home headers are 5 chars: abbrev(3) + 2 spaces (score digit not shown).
    base = (" " + " idx"                        # srv(1) + idx(4)
            + "  " + "type"                     # + kind(4)
            + "  " + "inn"                      # + half+inning(3)
            + "  " + "B-S-O"                    # + balls-strikes-outs(5)
            + "  " + f"{away_abbrev}  "         # + away score col(5)
            + "  " + f"{home_abbrev}  "         # + home score col(5)
            + "  " + "      "                   # + skip marker(6)
            + "  " + "description".ljust(_DESC_W)  # fixed-width description
            + "  ")                             # sep before extended description
    remaining = max(0, width - len(base) - 1)
    return (base + "extended description")[:width - 1]


def _step_row(i: int, s: dict, width: int) -> str:
    kind_tag = {"pitch": "PTCH", "intermission": "MID ", "final": "END "}.get(s["kind"], "??? ")
    half     = "T" if s["is_top"] else "B"
    count    = f"{s['balls']}-{s['strikes']}-{s['outs']}"
    away_s   = f"{away_abbrev}{s['away_score']:2d}"  # "SEA 3" — 5 chars, 1 space before digit
    home_s   = f"{home_abbrev}{s['home_score']:2d}"  # "OAK 1" — 5 chars
    srv      = "►" if i == cursor else " "
    skp      = "[skip]" if i in skipped else "      "  # 6 chars
    desc     = s.get("desc", "")
    rdesc    = s.get("resultdesc", "") or ""
    base     = (f"{srv}{i:4d}"
                f"  {kind_tag}"
                f"  {half}{s['inning']:<2d}"
                f"  {count}"
                f"  {away_s}"
                f"  {home_s}"
                f"  {skp}"
                f"  {desc[:_DESC_W]:<{_DESC_W}}"
                f"  ")
    remaining = max(0, width - len(base) - 1)
    return base + rdesc[:remaining]


def run_tui(stdscr):
    global cursor

    curses.curs_set(0)
    curses.use_default_colors()
    curses.init_pair(_C_SEL,    curses.COLOR_WHITE,  curses.COLOR_BLUE)
    curses.init_pair(_C_SERVER, curses.COLOR_GREEN,  -1)
    curses.init_pair(_C_SKIP,   curses.COLOR_RED,    -1)
    curses.init_pair(_C_HEADER, curses.COLOR_BLACK,  curses.COLOR_WHITE)
    curses.init_pair(_C_BOTH,   curses.COLOR_GREEN,  curses.COLOR_BLUE)

    curses.mousemask(curses.ALL_MOUSE_EVENTS)

    stdscr.timeout(200)   # refresh at most 5×/sec to track server cursor movement

    sel      = cursor
    view_top = 0

    HELP = " j/k:move  Spc:skip  Enter:jump  f:follow  r:reset  u:unskip-all  q:quit "

    while True:
        h, w = stdscr.getmaxyx()
        list_h = h - 3   # 1 title row + 1 column header row + 1 footer row

        # Keep selection in view
        if sel < view_top:
            view_top = sel
        elif sel >= view_top + list_h:
            view_top = sel - list_h + 1

        stdscr.erase()

        # Header
        hdr = (f" {away_abbrev} @ {home_abbrev}  gamePk={GAME_PK}"
               f"  step {cursor}/{len(steps)-1}"
               f"  skipped:{len(skipped)} ")
        stdscr.addnstr(0, 0, hdr.ljust(w), w, curses.color_pair(_C_HEADER) | curses.A_BOLD)

        # Column headers
        stdscr.addnstr(1, 0, _col_header_row(w).ljust(w - 1), w - 1,
                       curses.A_UNDERLINE | curses.A_DIM)

        # Step list
        for row in range(list_h):
            idx = view_top + row
            if idx >= len(steps):
                break
            line = _step_row(idx, steps[idx], w)
            is_sel = (idx == sel)
            is_srv = (idx == cursor)
            is_skp = (idx in skipped)

            if is_sel and is_srv:
                attr = curses.color_pair(_C_BOTH) | curses.A_BOLD
            elif is_sel:
                attr = curses.color_pair(_C_SEL)
            elif is_srv:
                attr = curses.color_pair(_C_SERVER) | curses.A_BOLD
            elif is_skp:
                attr = curses.color_pair(_C_SKIP)
            else:
                attr = curses.A_NORMAL

            stdscr.addnstr(2 + row, 0, line.ljust(w - 1), w - 1, attr)

        # Footer
        stdscr.addnstr(h - 1, 0, HELP.ljust(w), w - 1, curses.A_DIM)

        stdscr.refresh()

        key = stdscr.getch()
        if key == -1:
            continue
        elif key in (ord('q'), 27):
            break
        elif key in (curses.KEY_UP, ord('k')):
            sel = max(0, sel - 1)
        elif key in (curses.KEY_DOWN, ord('j')):
            sel = min(len(steps) - 1, sel + 1)
        elif key == curses.KEY_PPAGE:
            sel = max(0, sel - list_h)
        elif key == curses.KEY_NPAGE:
            sel = min(len(steps) - 1, sel + list_h)
        elif key in (curses.KEY_HOME, ord('g')):
            sel = 0
        elif key in (curses.KEY_END, ord('G')):
            sel = len(steps) - 1
        elif key == ord(' '):
            if sel in skipped:
                skipped.discard(sel)
            else:
                skipped.add(sel)
        elif key in (curses.KEY_ENTER, 10, 13):
            cursor = sel
        elif key == ord('f'):
            sel = cursor
        elif key == ord('r'):
            cursor = 0
            sel    = 0
        elif key == ord('u'):
            skipped.clear()
        elif key == curses.KEY_MOUSE:
            try:
                _, _, _my, _, bstate = curses.getmouse()
                if bstate & curses.BUTTON4_PRESSED:
                    sel = max(0, sel - 3)
                elif bstate & getattr(curses, "BUTTON5_PRESSED", 0x200000):
                    sel = min(len(steps) - 1, sel + 3)
                else:
                    row_idx = view_top + (_my - 2)
                    if 0 <= row_idx < len(steps):
                        sel = row_idx
                        if bstate & curses.BUTTON1_DOUBLE_CLICKED:
                            cursor = sel
            except curses.error:
                pass

# ── Startup ───────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import uvicorn

    if args.no_tui:
        print(f"\nPitch replay server → http://{args.host}:{args.port}")
        print(f"  Live feed : http://localhost:{args.port}{LIVE_BASE}")
        print(f"  Status    : http://localhost:{args.port}/replay/status")
        print(f"  Reset     : POST http://localhost:{args.port}/replay/reset")
        print(f"\nPoint the tracker at:  base_url: http://<host>:{args.port}")
        print(f"Set poll_interval low (e.g. 1s) for fast stepping.\n")
        uvicorn.run(app, host=args.host, port=args.port, log_level="warning")
    else:
        server_thread = threading.Thread(
            target=uvicorn.run,
            kwargs={"app": app, "host": args.host, "port": args.port, "log_level": "critical"},
            daemon=True,
        )
        server_thread.start()
        try:
            curses.wrapper(run_tui)
        except KeyboardInterrupt:
            pass
