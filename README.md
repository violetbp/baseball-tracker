# ESPHome Baseball Tracker

An ESPHome external component that displays live MLB game data on a 128×32 hub75 LED matrix. Designed as a drop-in addition to the [Eastside Urbanism Transit Tracker](https://github.com/EastsideUrbanism/transit-tracker) firmware, adding a baseball page that cycles alongside the transit schedule.

## What it shows

### Live game

```
┌────────────────────────────────────────────────────────────────────────────────┐
│ ATH   ^1    [◆]   B●●○○  S●○○  O●●○   SEA                                    │
│  2    st    base         count          0                                       │
└────────────────────────────────────────────────────────────────────────────────┘
```

- **Away / Home teams** with current score (cyan abbreviation, white score)
- **Inning indicator** — `^` for Top, `v` for Bottom, plus ordinal ("1st", "2nd" …)
- **Base diamond** — yellow squares for occupied bases, dim outlines for empty
- **Count** — green dots for balls (max 4), yellow dots for strikes (max 3), red dots for outs (max 3)

### Pre-game

```
ATH vs SEA
20:10 UTC
```

### Final

```
ATH 2        FINAL        SEA 0
                 1st
```

### No game today

```
        NO GAME TODAY
```

## Hardware

This component targets the same hardware as the Transit Tracker:

| Part | Details |
|------|---------|
| MCU | Adafruit Matrix Portal S3 (ESP32-S3) |
| Display | 64×32 RGB LED matrix × 2 (wired as 128×32) |
| Framework | Arduino (required) |

No extra wiring is needed beyond the existing transit tracker setup.

## Installation

### 1. Fork / host this repo

Push this repository to your GitHub account (e.g. `github.com/YOUR_USER/esphome-baseball-tracker`).

### 2. Add the external component to your firmware YAML

```yaml
external_components:
  # … existing entries …
  - source: github://YOUR_USER/esphome-baseball-tracker@main
    refresh: 0s
```

### 3. Add the `baseball_tracker:` block

```yaml
baseball_tracker:
  id: baseball
  team_id: 136        # 136 = Seattle Mariners (default; omit for Mariners)
  poll_interval: 30s  # refresh rate during live games; auto-slows to 5 min otherwise
  # Optional: auto-show the baseball display from N minutes before first pitch (UTC vs device clock)
  # through the end of the game; turn off after Final. Requires a template switch id (see firmware).
  auto_baseball_page: true
  auto_page_lead: 5min
  baseball_page_switch: baseball_display_switch
  # Optional: Home Assistant sees this while the game is Live
  game_in_progress:
    name: "MLB game in progress"
```

`font_id` and `display_id` default to the first font and display declared in the config — the Pixolletta font and `matrix` display from transit-tracker.yaml are picked up automatically.

### 4. Add the display page

Merge this page into the `pages:` list inside your `display:` block:

```yaml
display:
  - platform: hub75
    id: matrix
    # … existing options …
    pages:
      - id: transit_schedule
        lambda: id(tracker).draw_schedule();
      - id: baseball_score          # ← add this
        lambda: id(baseball).draw_game();
      - id: ip_address_page
        lambda: # … existing …
```

Press both buttons simultaneously on the Matrix Portal S3 to cycle between pages (this is existing transit-tracker behavior).

## Finding your team ID

The component uses the free, unauthenticated MLB Stats API. To find any team's ID:

```
https://statsapi.mlb.com/api/v1/teams?sportId=1
```

Common team IDs:

| Team | ID |
|------|----|
| Seattle Mariners | 136 |
| Los Angeles Dodgers | 119 |
| New York Yankees | 147 |
| Boston Red Sox | 111 |
| San Francisco Giants | 137 |
| Chicago Cubs | 112 |

## Data source

```
GET https://statsapi.mlb.com/api/v1/schedule
    ?sportId=1&teamId={team_id}&hydrate=linescore,team
```

No API key required. The component polls this endpoint every `poll_interval` seconds during live games and automatically backs off to every 5 minutes between games. The MLB Stats API is free and has no documented rate limits for reasonable polling.

## Configuration reference

```yaml
baseball_tracker:
  id: baseball

  # MLB team ID (default: 136 = Seattle Mariners)
  team_id: 136

  # How often to refresh during a live game.
  # Automatically becomes 5 min when no game is in progress.
  poll_interval: 30s

  # (Optional) Explicit display and font IDs — only needed if you have
  # multiple displays or fonts declared.
  display_id: matrix
  font_id: pixolletta
```

## Full merged firmware example

See [`firmware/baseball-tracker.yaml`](firmware/baseball-tracker.yaml) for a complete, ready-to-flash YAML that merges the baseball page into the Eastside Urbanism transit-tracker firmware.

## License

MIT
