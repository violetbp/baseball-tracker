# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Baseball Tracker is an ESPHome external component for an ESP32-S3 that displays live MLB game data on a 128×32 hub75 LED matrix. It is designed to slot into the [Eastside Urbanism Transit Tracker](https://github.com/EastsideUrbanism/transit-tracker) firmware as an additional page.

## Development Environment

All development happens inside a Nix flake shell:

```sh
nix develop
```

This provides ESPHome, PlatformIO, SDL2, nlohmann/json, libcurl, and all other dependencies.

## Common Commands

**Flash to device:**
```sh
esphome run firmware/local-tracker-vars.yaml --device /dev/ttyACM0
```

**Build and run the desktop simulator:**
```sh
make -C simulate
./simulate/bin/baseball-tracker-sim
```

**Run the game replay server** (emulates MLB Stats API locally):
```sh
python gamereplay/replay.py ./gamereplay/mlb_live_<gamePk> --port 8000
```
Point the firmware at `http://<host>:8000` via the `base_url` config option to test against saved game data without hitting the real API.

**Download a live game feed for replay:**
```sh
./gamereplay/dlreplay.sh
```

## Architecture

### Component layout (`components/baseball_tracker/`)

| File | Role |
|---|---|
| `baseball_tracker.h` | Core class definitions: `GameState`, `BaseballTracker`, `TeamSelect` |
| `baseball_tracker.cpp` | `setup()` / `loop()`: polling cadence, WiFi gating, auto-page logic |
| `mlb_api.cpp` | MLB Stats API HTTP fetches and JSON parsing |
| `draw_ui.cpp` | All display rendering (128×32 pixel layout) |
| `library.h/.cpp` | Utilities: `utc_time_from_tm()`, `to_upper()` |
| `__init__.py` | ESPHome Python codegen — config schema, validation, entity registration |

### Game lifecycle

States: `NONE` → `PREVIEW` → `LIVE` → `FINAL`

Two independent poll timers run in `loop()`:
- **Schedule poll** (slow, ~5 min): discovers game_pk, start time, PREVIEW/FINAL state.
- **Live feed poll** (fast, configurable default 5 s): only active when state is `LIVE`. Fetches count, runners, inning, and score from the live feed endpoint.

### Display rendering

`draw_game()` in `draw_ui.cpp` dispatches to one of four renderers based on `GameState::status`. The 128×32 canvas fits team abbreviations, scores, inning (with top/bottom arrow), a 3×3 base runner diamond, and ball/strike/out dot indicators.

### Simulator (`simulate/`)

The simulator compiles the same rendering code (`draw_ui.cpp`) against `esp_shim.h` stubs and drives it with SDL2 + Dear ImGui. Use it to test UI changes without flashing hardware. It has two modes:
- **Manual controls** — adjust game state via ImGui sliders/checkboxes.
- **Live MLB API tab** — polls the real MLB API with curl.

`esp_shim.h` provides minimal stubs for ESPHome display/font/sensor types so hardware code compiles on Linux.

### ESPHome integration

`__init__.py` validates ESPHome ≥ 2026.3.0 and Arduino framework (not IDF). It registers the component, a binary sensor (`game_in_progress`), and a `TeamSelect` entity (30 MLB teams). Required config keys: `team_id`, `display`, `font`.

### MLB API

Uses the free, unauthenticated MLB Stats API:
- Schedule: `/api/v1/schedule?sportId=1&teamId=<id>&hydrate=linescore,team`
- Live feed: `/api/v1.1/game/<gamePk>/feed/live` (with field-filter params)

Field selection details are documented in `livedatatofetch.md`.
