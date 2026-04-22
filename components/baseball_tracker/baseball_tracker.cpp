#include "baseball_tracker.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"

namespace esphome {
namespace baseball_tracker {

static const char *const TAG = "baseball_tracker";

// ---------------------------------------------------------------------------
// MLB Stats API – single endpoint that returns everything we need.
// ?hydrate=linescore,team pulls live count, base runners, and team abbrevs.
// ---------------------------------------------------------------------------
static const char *const MLB_API_HOST = "statsapi.mlb.com";
static const char *const MLB_SCHEDULE_PATH =
    "/api/v1/schedule?sportId=1&teamId=%d&hydrate=linescore,team";

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void BaseballTracker::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Baseball Tracker (team_id=%d)", team_id_);
}

void BaseballTracker::loop() {
  uint32_t now = millis();

  // First poll happens immediately once; subsequent polls respect the interval.
  // When the game is not live we slow down to 5 minutes to be polite to the API.
  uint32_t effective_interval = poll_interval_ms_;
  if (state_.phase != GamePhase::LIVE) {
    effective_interval = 5 * 60 * 1000;  // 5 min when not live
  }

  if (!first_poll_done_ || (now - last_poll_ms_) >= effective_interval) {
    fetch_game_data_();
    last_poll_ms_ = now;
    first_poll_done_ = true;
  }
}

void BaseballTracker::dump_config() {
  ESP_LOGCONFIG(TAG, "Baseball Tracker:");
  ESP_LOGCONFIG(TAG, "  Team ID: %d", team_id_);
  ESP_LOGCONFIG(TAG, "  Poll interval: %u ms", poll_interval_ms_);
}

// ---------------------------------------------------------------------------
// HTTP fetch
// ---------------------------------------------------------------------------

void BaseballTracker::fetch_game_data_() {
  char path[128];
  snprintf(path, sizeof(path), MLB_SCHEDULE_PATH, team_id_);

  WiFiClientSecure client;
  client.setInsecure();  // matches verify_ssl: false in the firmware

  HTTPClient http;
  http.begin(client, MLB_API_HOST, 443, path, true);
  http.setTimeout(8000);
  http.addHeader("User-Agent", "ESPHome-BaseballTracker/1.0");

  int code = http.GET();
  if (code != 200) {
    ESP_LOGW(TAG, "HTTP GET failed: %d", code);
    http.end();
    return;
  }

  std::string body = http.getString().c_str();
  http.end();

  if (!parse_response_(body)) {
    ESP_LOGW(TAG, "Failed to parse MLB API response");
  }
}

// ---------------------------------------------------------------------------
// JSON parsing
// ---------------------------------------------------------------------------

bool BaseballTracker::parse_response_(const std::string &json_body) {
  return json::parse_json(json_body, [this](JsonObject root) -> bool {
    // Reset to a clean state each parse cycle
    state_ = GameState{};

    int total_games = root["totalGames"] | 0;
    if (total_games == 0) {
      state_.phase = GamePhase::NONE;
      return true;
    }

    JsonArray dates = root["dates"];
    if (dates.isNull() || dates.size() == 0) {
      state_.phase = GamePhase::NONE;
      return true;
    }

    // Pick the first game of the first date (today's game)
    JsonObject game = dates[0]["games"][0];
    if (game.isNull()) {
      state_.phase = GamePhase::NONE;
      return true;
    }

    state_.game_pk = game["gamePk"] | 0;

    // Abstract game state: "Preview", "Live", "Final"
    const char *abstract_state = game["status"]["abstractGameState"] | "Preview";
    if (strcmp(abstract_state, "Live") == 0) {
      state_.phase = GamePhase::LIVE;
    } else if (strcmp(abstract_state, "Final") == 0) {
      state_.phase = GamePhase::FINAL;
    } else {
      state_.phase = GamePhase::PREVIEW;
    }

    // Teams
    JsonObject teams = game["teams"];
    state_.away_abbrev = teams["away"]["team"]["abbreviation"] | "???";
    state_.home_abbrev = teams["home"]["team"]["abbreviation"] | "???";
    state_.away_score  = teams["away"]["score"] | 0;
    state_.home_score  = teams["home"]["score"] | 0;

    // Pre-game: store a human-readable start time (UTC from API → local via RTC)
    if (state_.phase == GamePhase::PREVIEW) {
      const char *game_date = game["gameDate"] | "";
      // Store raw UTC string for now; we'll format it at draw time if RTC available
      state_.start_time_str = game_date;
    }

    // Linescore (present for Live and Final)
    JsonObject ls = game["linescore"];
    if (!ls.isNull()) {
      state_.inning         = ls["currentInning"] | 0;
      state_.inning_ordinal = ls["currentInningOrdinal"] | "";
      state_.is_top_inning  = ls["isTopInning"] | true;
      state_.balls          = ls["balls"] | 0;
      state_.strikes        = ls["strikes"] | 0;
      state_.outs           = ls["outs"] | 0;

      JsonObject offense = ls["offense"];
      if (!offense.isNull()) {
        state_.runner_first  = !offense["first"].isNull();
        state_.runner_second = !offense["second"].isNull();
        state_.runner_third  = !offense["third"].isNull();
      }
    }

    ESP_LOGD(TAG, "Parsed: phase=%d %s%d %d-%d B%d S%d O%d bases=%d%d%d",
             (int)state_.phase,
             state_.is_top_inning ? "T" : "B",
             state_.inning,
             state_.away_score, state_.home_score,
             state_.balls, state_.strikes, state_.outs,
             (int)state_.runner_first, (int)state_.runner_second, (int)state_.runner_third);

    return true;
  });
}

// ---------------------------------------------------------------------------
// Public draw entry point
// ---------------------------------------------------------------------------

void BaseballTracker::draw_game() {
  if (display_ == nullptr || font_ == nullptr) return;

  switch (state_.phase) {
    case GamePhase::NONE:    draw_no_game_();  break;
    case GamePhase::PREVIEW: draw_pregame_();  break;
    case GamePhase::LIVE:    draw_live_();     break;
    case GamePhase::FINAL:   draw_final_();    break;
  }
}

// ---------------------------------------------------------------------------
// Drawing: no game today
// ---------------------------------------------------------------------------

void BaseballTracker::draw_no_game_() {
  draw_centered_text_(0, kDisplayW, 11, "NO GAME TODAY", kDim());
}

// ---------------------------------------------------------------------------
// Drawing: pre-game / scheduled
// ---------------------------------------------------------------------------

void BaseballTracker::draw_pregame_() {
  // Top row: "SEA  vs  ATH"
  char top[32];
  snprintf(top, sizeof(top), "%s vs %s", state_.away_abbrev.c_str(), state_.home_abbrev.c_str());
  draw_centered_text_(0, kDisplayW, kTopRowY, top, kWhite());

  // Bottom row: start time (UTC ISO8601 → strip to "HH:MM UTC")
  // The API returns e.g. "2026-04-22T20:10:00Z"
  const std::string &dt = state_.start_time_str;
  char time_buf[16] = "TBD";
  if (dt.size() >= 16) {
    // Extract HH:MM from position 11
    snprintf(time_buf, sizeof(time_buf), "%c%c:%c%c UTC",
             dt[11], dt[12], dt[14], dt[15]);
  }
  draw_centered_text_(0, kDisplayW, kBotRowY, time_buf, kYellow());
}

// ---------------------------------------------------------------------------
// Drawing: live game
// ---------------------------------------------------------------------------

void BaseballTracker::draw_live_() {
  auto *d = display_;

  // ---- Layout regions ----
  // [0..29]   away team + score
  // [30..49]  inning indicator
  // [50..77]  base diamond
  // [78..127] home team + score

  // --- Away team (left) ---
  char away_buf[16];
  snprintf(away_buf, sizeof(away_buf), "%s", state_.away_abbrev.c_str());
  d->print(2, kTopRowY, font_, kCyan(), away_buf);

  char away_score_buf[8];
  snprintf(away_score_buf, sizeof(away_score_buf), "%d", state_.away_score);
  d->print(2, kBotRowY, font_, kWhite(), away_score_buf);

  // --- Home team (right) ---
  char home_buf[16];
  snprintf(home_buf, sizeof(home_buf), "%s", state_.home_abbrev.c_str());
  draw_centered_text_(98, 127, kTopRowY, home_buf, kCyan());

  char home_score_buf[8];
  snprintf(home_score_buf, sizeof(home_score_buf), "%d", state_.home_score);
  draw_centered_text_(98, 127, kBotRowY, home_score_buf, kWhite());

  // --- Inning indicator (center-left block: x=30..49) ---
  // Arrow char + ordinal number stacked
  const char *arrow = state_.is_top_inning ? "^" : "v";
  char inn_buf[8];
  snprintf(inn_buf, sizeof(inn_buf), "%s%d", arrow, state_.inning);
  draw_centered_text_(30, 50, kTopRowY, inn_buf, kYellow());

  // Inning ordinal suffix ("st", "nd", etc.) below the number
  // Extract suffix from ordinal string (last 2 chars)
  const std::string &ord = state_.inning_ordinal;
  if (ord.size() >= 2) {
    std::string suffix = ord.substr(ord.size() - 2);
    draw_centered_text_(30, 50, kBotRowY, suffix.c_str(), kYellow());
  }

  // --- Base diamond (x=51..77, centered at x=64, y=16) ---
  draw_bases_(64, 16);

  // --- Count: B / S / O dots (x=78..97) ---
  // Arrange three groups stacked in the right portion
  // Balls (4 max) on top row
  int bx = 78;
  int by = kTopRowY + 2;
  d->print(bx, kTopRowY, font_, kDim(), "B");
  draw_dots_(bx + 7, by, 4, state_.balls, kGreen(), kDim());

  // Strikes (3 max) middle
  d->print(bx, kBotRowY, font_, kDim(), "S");
  draw_dots_(bx + 7, kBotRowY + 2, 3, state_.strikes, kYellow(), kDim());

  // Outs (3 max) shown as small red dots beside S row
  d->print(bx + 24, kBotRowY, font_, kDim(), "O");
  draw_dots_(bx + 31, kBotRowY + 2, 3, state_.outs, kRed(), kDim());
}

// ---------------------------------------------------------------------------
// Drawing: final score
// ---------------------------------------------------------------------------

void BaseballTracker::draw_final_() {
  auto *d = display_;

  // Top row: "ATH 2  FINAL  SEA 5"
  char left_buf[12], right_buf[12];
  snprintf(left_buf,  sizeof(left_buf),  "%s %d", state_.away_abbrev.c_str(), state_.away_score);
  snprintf(right_buf, sizeof(right_buf), "%s %d", state_.home_abbrev.c_str(), state_.home_score);

  d->print(2, kTopRowY, font_, kWhite(), left_buf);
  draw_centered_text_(40, 88, kTopRowY, "FINAL", kYellow());
  draw_centered_text_(90, 127, kTopRowY, right_buf, kWhite());

  // Bottom row: inning total
  char inn_buf[16];
  snprintf(inn_buf, sizeof(inn_buf), "%s", state_.inning_ordinal.c_str());
  draw_centered_text_(0, kDisplayW, kBotRowY, inn_buf, kDim());
}

// ---------------------------------------------------------------------------
// Helper: draw base diamond
// ---------------------------------------------------------------------------
// Compact diamond layout (7×7 pixels, centered at cx,cy):
//
//        2nd       (cx, cy-3)
//    3rd   1st     (cx-4,cy+1) (cx+4,cy+1)
//
// Each base is drawn as a 2×2 filled square (on) or 1×1 dot (off).

void BaseballTracker::draw_bases_(int cx, int cy) {
  auto *d = display_;

  struct Base {
    int dx, dy;
    bool occupied;
  } bases[3] = {
    {  0, -4, state_.runner_second },  // 2nd
    { -5,  2, state_.runner_third  },  // 3rd
    {  5,  2, state_.runner_first  },  // 1st
  };

  for (auto &b : bases) {
    int bx = cx + b.dx;
    int by = cy + b.dy;
    if (b.occupied) {
      // Filled 3×3 square for occupied base
      d->filled_rectangle(bx - 1, by - 1, 3, 3, kYellow());
    } else {
      // Hollow 3×3 square for empty base
      d->rectangle(bx - 1, by - 1, 3, 3, kDim());
    }
  }

  // Draw diamond outline connecting the bases with single-pixel lines
  // Top-left diagonal: 2nd → 3rd
  d->line(cx, cy - 3, cx - 4, cy + 2, kDim());
  // Top-right diagonal: 2nd → 1st
  d->line(cx, cy - 3, cx + 4, cy + 2, kDim());
  // Bottom line: 3rd → home plate area
  d->line(cx - 4, cy + 2, cx, cy + 5, kDim());
  // Bottom line: 1st → home plate area
  d->line(cx + 4, cy + 2, cx, cy + 5, kDim());
  // Home plate dot
  d->draw_pixel_at(cx, cy + 5, kDim());
}

// ---------------------------------------------------------------------------
// Helper: draw a row of filled/hollow dots
// ---------------------------------------------------------------------------

int BaseballTracker::draw_dots_(int x, int y, int count, int filled, Color on_color, Color off_color) {
  auto *d = display_;
  for (int i = 0; i < count; i++) {
    int cx = x + i * kDotStep;
    if (i < filled) {
      d->filled_circle(cx, y, kDotR, on_color);
    } else {
      d->circle(cx, y, kDotR, off_color);
    }
  }
  return x + count * kDotStep;
}

// ---------------------------------------------------------------------------
// Helper: draw text centered in an x range
// ---------------------------------------------------------------------------

void BaseballTracker::draw_centered_text_(int x_start, int x_end, int y, const char *text, Color color) {
  if (display_ == nullptr || font_ == nullptr) return;

  int text_w = 0, text_h = 0, x_off = 0, y_off = 0;
  display_->get_text_bounds(0, 0, text, font_, display::TextAlign::TOP_LEFT,
                            &x_off, &y_off, &text_w, &text_h);

  int center = (x_start + x_end) / 2;
  int draw_x = center - text_w / 2;
  display_->print(draw_x, y, font_, color, text);
}

}  // namespace baseball_tracker
}  // namespace esphome
