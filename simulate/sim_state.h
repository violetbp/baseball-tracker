#pragma once

#include <cstdint>
#include <string>

// Mirror of baseball_tracker.h — keep in sync with firmware
enum class GamePhase { NONE, PREVIEW, LIVE, FINAL };
enum class InningIntermissionKind { NONE, MIDDLE, END };

/// Matches firmware's GameState in baseball_tracker.h
struct GameState {
  GamePhase phase{GamePhase::NONE};
  std::string away_abbrev;
  std::string home_abbrev;
  int away_score{0};
  int home_score{0};
  int inning{0};
  bool is_top_inning{true};
  InningIntermissionKind inning_intermission{InningIntermissionKind::NONE};
  std::string inning_ordinal;
  int balls{0};
  int strikes{0};
  int outs{0};
  std::string pitcher_last;
  std::string batter_last;
  bool runner_first{false};
  bool runner_second{false};
  bool runner_third{false};
  std::string start_time_str;
  bool has_game_start{false};
  std::string detailed_state;
  int game_pk{0};
};

/// Working copy: ImGui + JSON mutate these; sim_refresh_display_model pushes into state.
struct SimState {
  // Phase
  char phase[16] = "Preview";  // "None" | "Preview" | "Live" | "Final"

  // Teams
  char away[8] = "SEA";
  int away_score = 0;
  char home[8] = "ARI";
  int home_score = 0;

  // Inning
  int inning = 0;
  char ordinal[8] = "1st";
  bool top_inning = true;
  int intermission = 0;  // 0=none, 1=middle, 2=end

  // Count
  int balls = 0;
  int strikes = 0;
  int outs = 0;

  // Players (last names)
  char pitcher[32] = "";
  char batter[32] = "";

  // Bases
  bool runner_first = false;
  bool runner_second = false;
  bool runner_third = false;

  // Pregame info
  char start_time[32] = "2026-05-05T20:10:00Z";
  char detailed_state[32] = "";

  // Synthetic live feed
  bool live_feed = false;
  bool live_running = false;
  int live_inning = 1;
  bool live_top = true;
  int live_balls = 0;
  int live_strikes = 0;
  int live_outs = 0;
  int live_away_score = 0;
  int live_home_score = 0;
  bool live_r1 = false, live_r2 = false, live_r3 = false;
  int live_tick = 0;
  int live_speed = 2;
  char live_ordinal[8] = "1st";

  // Display settings
  int scale = 4;
  bool show_grid = false;
  bool show_fps = true;

  // MLB API settings
  char base_url[128] = "https://statsapi.mlb.com";
  int team_id = 136;
  int game_pk = 0;

  // Schedule endpoint poll (discovers game by team ID)
  bool mlb_poll_schedule = false;
  int mlb_schedule_poll_ms = 300000;  // 5 min default, matches firmware cadence
  uint32_t mlb_last_schedule_poll_ticks = 0;
  bool mlb_schedule_fetch_pending = false;

  // Live feed endpoint poll
  bool mlb_poll_live = false;
  int mlb_poll_ms = 5000;
  uint32_t mlb_last_poll_ticks = 0;
  bool mlb_fetch_pending = false;

  char mlb_status[288] = "";

  GameState state{};
};

/// Copy ImGui-facing fields into sim.state for rendering.
void sim_refresh_display_model(SimState &sim);
