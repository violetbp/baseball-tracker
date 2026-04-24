#include "baseball_tracker.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ctime>
#include <cstdlib>
#include <cstring>

#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"

// ISO8601 in UTC: mktime with TZ=UTC
static time_t utc_time_from_tm(struct tm *tp) {
  const char *prev = getenv("TZ");
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t ret = mktime(tp);
  if (prev != nullptr) {
    setenv("TZ", prev, 1);
  } else {
    unsetenv("TZ");
  }
  tzset();
  return ret;
}

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

  // Poll faster during live; slow down the rest to be polite to the API.
  uint32_t effective_interval = poll_interval_ms_;
  if (state_.phase != GamePhase::LIVE) {
    effective_interval = 5 * 60 * 1000;  // 5 min when not live
  }

  if (!first_poll_done_ || (now - last_poll_ms_) >= effective_interval) {
    ESP_LOGD(TAG, "Polling MLB API (interval=%u ms, phase=%d)", effective_interval, (int)state_.phase);
    fetch_game_data_();
    last_poll_ms_ = now;
    first_poll_done_ = true;
  }

  // 1Hz: auto page + binary_sensor (cheap)
  if (now - last_auto_logic_ms_ >= 1000) {
    last_auto_logic_ms_ = now;
    try_auto_baseball_page_();
    update_game_in_progress_sensor_();
  }
}

void BaseballTracker::dump_config() {
  ESP_LOGCONFIG(TAG, "Baseball Tracker:");
  ESP_LOGCONFIG(TAG, "  Team ID: %d", team_id_);
  ESP_LOGCONFIG(TAG, "  Poll interval: %u ms", poll_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Auto baseball page: %s", auto_baseball_page_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Auto lead before start: %u s", auto_page_lead_sec_);
  ESP_LOGCONFIG(TAG, "  Linked baseball page switch: %s", baseball_page_switch_ == nullptr ? "no" : "yes");
  ESP_LOGCONFIG(TAG, "  Game in progress binary sensor: %s", game_in_progress_sensor_ == nullptr ? "no" : "yes");
}

// ---------------------------------------------------------------------------
// HTTP fetch
// ---------------------------------------------------------------------------

void BaseballTracker::fetch_game_data_() {
  char path[128];
  snprintf(path, sizeof(path), MLB_SCHEDULE_PATH, team_id_);

  ESP_LOGD(TAG, "GET https://%s%s", MLB_API_HOST, path);
  uint32_t t0 = millis();

  WiFiClientSecure client;
  client.setInsecure();  // matches verify_ssl: false in the firmware

  HTTPClient http;
  http.begin(client, MLB_API_HOST, 443, path, true);
  http.setTimeout(8000);
  http.addHeader("User-Agent", "ESPHome-BaseballTracker/1.0");

  int code = http.GET();
  uint32_t elapsed = millis() - t0;

  if (code != 200) {
    ESP_LOGW(TAG, "HTTP GET failed after %u ms: code=%d", elapsed, code);
    http.end();
    return;
  }

  std::string body = http.getString().c_str();
  http.end();

  ESP_LOGD(TAG, "HTTP 200 in %u ms, body=%u bytes", elapsed, (unsigned)body.size());

  if (!parse_response_(body)) {
    ESP_LOGW(TAG, "Failed to parse MLB API response (body_len=%u)", (unsigned)body.size());
  }
}

// ---------------------------------------------------------------------------
// JSON parsing
// ---------------------------------------------------------------------------

bool BaseballTracker::parse_response_(const std::string &json_body) {
  // Snapshot current state so we can log only what changed
  GameState prev = state_;

  return json::parse_json(json_body, [this, &prev](JsonObject root) -> bool {
    state_ = GameState{};

    int total_games = root["totalGames"] | 0;
    if (total_games == 0) {
      state_.phase = GamePhase::NONE;
      if (prev.phase != GamePhase::NONE) {
        ESP_LOGI(TAG, "No game scheduled today");
      }
      return true;
    }

    JsonArray dates = root["dates"];
    if (dates.isNull() || dates.size() == 0) {
      state_.phase = GamePhase::NONE;
      ESP_LOGW(TAG, "totalGames=%d but dates array is empty", total_games);
      return true;
    }

    // Pick the first game of the first date (today's game)
    JsonObject game = dates[0]["games"][0];
    if (game.isNull()) {
      state_.phase = GamePhase::NONE;
      ESP_LOGW(TAG, "Games array unexpectedly empty");
      return true;
    }

    state_.game_pk = game["gamePk"] | 0;

    // First pitch time (all phases): ISO8601 UTC
    {
      const char *gd = game["gameDate"] | "";
      state_.start_time_str = gd;
      if (gd[0] == '\0' || !parse_iso8601_utc(gd, &state_.game_start_utc) || state_.game_start_utc <= 0) {
        state_.has_game_start = false;
      } else {
        state_.has_game_start = true;
      }
    }

    // Abstract game state: "Preview", "Live", "Final"
    const char *abstract_state = game["status"]["abstractGameState"] | "Preview";
    const char *detailed_state = game["status"]["detailedState"]     | "";
    if (strcmp(abstract_state, "Live") == 0) {
      state_.phase = GamePhase::LIVE;
    } else if (strcmp(abstract_state, "Final") == 0) {
      state_.phase = GamePhase::FINAL;
    } else {
      state_.phase = GamePhase::PREVIEW;
    }

    // Log phase transitions
    if (prev.phase != state_.phase) {
      ESP_LOGI(TAG, "Game phase changed: %d → %d (%s) [gamePk=%d]",
               (int)prev.phase, (int)state_.phase, detailed_state, state_.game_pk);
    }

    // Teams
    JsonObject teams = game["teams"];
    state_.away_abbrev = teams["away"]["team"]["abbreviation"] | "???";
    state_.home_abbrev = teams["home"]["team"]["abbreviation"] | "???";
    state_.away_score  = teams["away"]["score"] | 0;
    state_.home_score  = teams["home"]["score"] | 0;

    // Log score changes
    if (state_.away_score != prev.away_score || state_.home_score != prev.home_score) {
      ESP_LOGI(TAG, "Score update: %s %d, %s %d",
               state_.away_abbrev.c_str(), state_.away_score,
               state_.home_abbrev.c_str(), state_.home_score);
    }

    if (state_.phase == GamePhase::PREVIEW) {
      ESP_LOGI(TAG, "Game preview: %s @ %s, start=%s",
               state_.away_abbrev.c_str(), state_.home_abbrev.c_str(), state_.start_time_str.c_str());
    }

    // Linescore (present for Live and Final)
    JsonObject ls = game["linescore"];
    if (ls.isNull()) {
      ESP_LOGD(TAG, "No linescore in response (phase=%d)", (int)state_.phase);
    } else {
      state_.inning         = ls["currentInning"] | 0;
      state_.inning_ordinal = ls["currentInningOrdinal"] | "";
      state_.is_top_inning  = ls["isTopInning"] | true;
      state_.balls          = ls["balls"] | 0;
      state_.strikes        = ls["strikes"] | 0;
      state_.outs           = ls["outs"] | 0;

      // Log inning changes at INFO; count/bases at VERBOSE
      if (state_.inning != prev.inning || state_.is_top_inning != prev.is_top_inning) {
        ESP_LOGI(TAG, "Inning: %s %s (%d outs)",
                 state_.is_top_inning ? "Top" : "Bottom",
                 state_.inning_ordinal.c_str(),
                 state_.outs);
      }

      JsonObject offense = ls["offense"];
      if (!offense.isNull()) {
        state_.runner_first  = !offense["first"].isNull();
        state_.runner_second = !offense["second"].isNull();
        state_.runner_third  = !offense["third"].isNull();
      }

      ESP_LOGD(TAG, "%s @ %s  %d-%d  %s%s  B%d S%d O%d  bases:[%s%s%s]",
               state_.away_abbrev.c_str(), state_.home_abbrev.c_str(),
               state_.away_score, state_.home_score,
               state_.is_top_inning ? "T" : "B", state_.inning_ordinal.c_str(),
               state_.balls, state_.strikes, state_.outs,
               state_.runner_first  ? "1" : "-",
               state_.runner_second ? "2" : "-",
               state_.runner_third  ? "3" : "-");

      ESP_LOGV(TAG, "Count detail — balls=%d strikes=%d outs=%d  runners: 1st=%d 2nd=%d 3rd=%d",
               state_.balls, state_.strikes, state_.outs,
               (int)state_.runner_first, (int)state_.runner_second, (int)state_.runner_third);
    }

    return true;
  });
}

// ---------------------------------------------------------------------------
// Public draw entry point
// ---------------------------------------------------------------------------

void BaseballTracker::draw_game() {
  if (display_ == nullptr || font_ == nullptr) {
    ESP_LOGW(TAG, "draw_game() called but display or font is not set");
    return;
  }

  ESP_LOGV(TAG, "draw_game() phase=%d", (int)state_.phase);

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
  draw_centered_text_(0, kDisplayW, kRow2Y, "NO GAME TODAY", kDim());
}

// ---------------------------------------------------------------------------
// Drawing: pre-game / scheduled
// ---------------------------------------------------------------------------

void BaseballTracker::draw_pregame_() {
  // Row 1: "ATH @ SEA" centered
  char top[32];
  snprintf(top, sizeof(top), "%s @ %s", state_.away_abbrev.c_str(), state_.home_abbrev.c_str());
  draw_centered_text_(0, kDisplayW, kRow1Y, top, kWhite());

  // Row 2: start time (UTC ISO8601 → "HH:MM UTC") centered
  // The API returns e.g. "2026-04-22T20:10:00Z"
  const std::string &dt = state_.start_time_str;
  char time_buf[16] = "TBD";
  if (dt.size() >= 16) {
    snprintf(time_buf, sizeof(time_buf), "%c%c:%c%c UTC",
             dt[11], dt[12], dt[14], dt[15]);
  }
  draw_centered_text_(0, kDisplayW, kRow2Y, time_buf, kYellow());
}

// ---------------------------------------------------------------------------
// Drawing: live game
// ---------------------------------------------------------------------------

void BaseballTracker::draw_live_() {
  auto *d = display_;

  // ---- Layout (3 lines) ----
  // Line 1: [away+score] ... [inning] ... [home+score]
  // Line 2: [B–S] right-aligned
  // Line 3: [diamond] ... [padding] ... [out dots]

  // --- Line 1: away team + score (left) ---
  char away_buf[16];
  snprintf(away_buf, sizeof(away_buf), "%s  %d", state_.away_abbrev.c_str(), state_.away_score);
  d->print(2, kRow1Y, font_, kCyan(), away_buf);

  // --- Line 1: home team + score (right) ---
  char home_buf[16];
  snprintf(home_buf, sizeof(home_buf), "%d  %s", state_.home_score, state_.home_abbrev.c_str());
  draw_centered_text_(78, 126, kRow1Y, home_buf, kCyan());

  // --- Line 1: inning centered between the two scores ---
  // Show "^ 3rd" or "v 2nd" (no "top/bot" word), and place arrow next to the batting team
  char inn_buf[8];
  snprintf(inn_buf, sizeof(inn_buf), "%s %s",
          //  state_.is_top_inning ? "^" : "v",
          "",
           state_.inning_ordinal.c_str());
  if (state_.is_top_inning) {
    // Away team batting, show "^ 3rd" left of center, blank in center
    draw_centered_text_(38, 59, kRow1Y, "^", kYellow());
    draw_centered_text_(60, 68, kRow1Y, inn_buf, kYellow());
  } else {
    // Home team batting, show "v 3rd" right of center, blank in center
    draw_centered_text_(38, 59, kRow1Y, inn_buf, kYellow());
    draw_centered_text_(60, 68, kRow1Y, "v", kYellow());

  }

  // --- Line 2: balls-strikes, right-aligned (same right edge as home block on line 1) ---
  char count_buf[8];
  snprintf(count_buf, sizeof(count_buf), "%d-%d", state_.balls, state_.strikes);
  draw_right_aligned_text_(kRow2RightX, kRow2Y, count_buf, kWhite());

  // --- Line 3: diamond (left of outs) + out dots (right) —
  // First out dot’s left: kOutsFirstX - kDotR; keep kDiamondOutPadding after diamond’s right.
  static constexpr int d13x = 7;
  static constexpr int base_pad = 2;
  int diamond_right = d13x + base_pad;  // right extent from cx to 1st-base tile edge
  int diamond_cx = kOutsFirstX - kDotR - kDiamondOutPadding - diamond_right;
  draw_bases_(diamond_cx, kDiamondCY);

  draw_dots_(kOutsFirstX, kOutDotsY, 3, state_.outs, kRed(), kDim());
}

// ---------------------------------------------------------------------------
// Drawing: final score
// ---------------------------------------------------------------------------

void BaseballTracker::draw_final_() {
  // Row 1: away score (left) ... "FINAL" (center) ... home score (right)
  char away_buf[12], home_buf[12];
  snprintf(away_buf, sizeof(away_buf), "%s  %d", state_.away_abbrev.c_str(), state_.away_score);
  snprintf(home_buf, sizeof(home_buf), "%d  %s", state_.home_score, state_.home_abbrev.c_str());

  display_->print(2, kRow1Y, font_, kWhite(), away_buf);
  draw_centered_text_(40, 88, kRow1Y, "FINAL", kYellow());
  draw_centered_text_(78, 126, kRow1Y, home_buf, kWhite());

  // Row 2: final inning centered (e.g. "9th" or "10th" for extra innings)
  draw_centered_text_(0, kDisplayW, kRow2Y, state_.inning_ordinal.c_str(), kDim());
}

// ---------------------------------------------------------------------------
// Helper: draw base diamond
// ---------------------------------------------------------------------------
// Enlarged diamond (~14px wide, ~12px tall) so it reads well on 128×32.
// (cx, cy) is the “fold” between the 2nd-base ray and the 3rd/1st line:
//
//            2nd
//         (cx, cy-7)
//    3rd           1st
// (cx-7,cy+3)   (cx+7,cy+3)
//         \   /
//        home
//     (cx, cy+5)
//
// Each base pad is 5×5 px (filled = runner on, hollow = empty).

void BaseballTracker::draw_bases_(int cx, int cy) {
  auto *d = display_;

  static constexpr int d2y = -7;   // 2nd base offset (up)
  static constexpr int d13x = 7;  // 3rd/1st horizontal offset
  static constexpr int d13y = 3;  // 3rd/1st vertical offset
  static constexpr int homey = 5;  // home plate offset (down)
  static constexpr int pad = 2;  // base pad half-size → 5×5 total

  struct Base {
    int dx, dy;
    bool occupied;
  } bases[3] = {
    { 0, d2y, state_.runner_second },   // 2nd
    {-d13x, d13y, state_.runner_third}, // 3rd
    { d13x, d13y, state_.runner_first}, // 1st
  };

  // Connect bases + home
  int x2 = cx, y2 = cy + d2y;
  int x3 = cx - d13x, y3 = cy + d13y;
  int x1 = cx + d13x, y1 = cy + d13y;
  int xh = cx, yh = cy + homey;

  d->line(x2, y2, x3, y3, kDim());
  d->line(x2, y2, x1, y1, kDim());
  d->line(x3, y3, xh, yh, kDim());
  d->line(x1, y1, xh, yh, kDim());

  for (auto &b : bases) {
    int bx = cx + b.dx;
    int by = cy + b.dy;
    if (b.occupied) {
      d->filled_rectangle(bx - pad, by - pad, 2 * pad + 1, 2 * pad + 1, kYellow());
    } else {
      d->rectangle(bx - pad, by - pad, 2 * pad + 1, 2 * pad + 1, kDim());
    }
  }

  // Home: small 3×3 dim square (reads better than a single pixel)
  d->rectangle(xh - 1, yh - 1, 3, 3, kDim());
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

void BaseballTracker::draw_right_aligned_text_(int x_end, int y, const char *text, Color color) {
  if (display_ == nullptr || font_ == nullptr) return;

  int text_w = 0, text_h = 0, x_off = 0, y_off = 0;
  display_->get_text_bounds(0, 0, text, font_, display::TextAlign::TOP_LEFT,
                            &x_off, &y_off, &text_w, &text_h);

  int draw_x = x_end - text_w;
  if (draw_x < 0) {
    draw_x = 0;
  }
  display_->print(draw_x, y, font_, color, text);
}

// ---------------------------------------------------------------------------
// ISO-8601 UTC (MLB: 2026-04-22T20:10:00Z)
// ---------------------------------------------------------------------------

bool BaseballTracker::parse_iso8601_utc(const char *iso, time_t *out) {
  if (iso == nullptr || *iso == 0) {
    return false;
  }
  char buf[32];
  strncpy(buf, iso, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  if (char *p = strchr(buf, '.')) {
    *p = 0;
  }
  int y, mo, d, h, mi, s;
  if (sscanf(buf, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 6) {
    return false;
  }
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_sec = s;
  *out = utc_time_from_tm(&t);
  return *out > 0;
}

bool BaseballTracker::should_auto_show_baseball_() const {
  if (state_.phase == GamePhase::NONE) {
    return false;
  }
  if (state_.phase == GamePhase::FINAL) {
    return false;
  }
  if (state_.phase == GamePhase::LIVE) {
    return true;
  }
  // PREVIEW: show from (start − lead) until first pitch (stays in PREVIEW until go)
  if (state_.phase == GamePhase::PREVIEW) {
    if (rtc_ == nullptr || !state_.has_game_start) {
      return false;
    }
    time_t now_ts = rtc_->utcnow().timestamp;
    if (now_ts < 1) {  // clock not set / invalid
      return false;
    }
    time_t t0 = state_.game_start_utc - static_cast<time_t>(auto_page_lead_sec_);
    return now_ts >= t0;
  }
  return false;
}

void BaseballTracker::try_auto_baseball_page_() {
  if (!auto_baseball_page_ || baseball_page_switch_ == nullptr) {
    return;
  }
  if (rtc_ == nullptr) {
    return;
  }

  bool want = this->should_auto_show_baseball_();
  if (want == last_auto_show_cmd_) {
    return;
  }
  if (want) {
    baseball_page_switch_->turn_on();
    ESP_LOGI(TAG, "Auto page: show baseball (T−%us window / live started)", auto_page_lead_sec_);
  } else {
    baseball_page_switch_->turn_off();
    ESP_LOGI(TAG, "Auto page: return to transit (no game, final, or before window)");
  }
  last_auto_show_cmd_ = want;
}

void BaseballTracker::update_game_in_progress_sensor_() {
  if (game_in_progress_sensor_ == nullptr) {
    return;
  }
  bool live = (state_.phase == GamePhase::LIVE);
  if (in_progress_sensor_published_ && live == last_published_in_progress_) {
    return;
  }
  game_in_progress_sensor_->publish_state(live);
  last_published_in_progress_ = live;
  in_progress_sensor_published_ = true;
}

}  // namespace baseball_tracker
}  // namespace esphome
