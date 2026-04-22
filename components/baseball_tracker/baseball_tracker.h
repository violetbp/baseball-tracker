#pragma once

#include <string>

#include "esphome/core/component.h"
#include "esphome/core/color.h"
#include "esphome/components/display/display.h"
#include "esphome/components/font/font.h"
#include "esphome/components/time/real_time_clock.h"

namespace esphome {
namespace baseball_tracker {

// Game lifecycle states returned by MLB Stats API abstractGameState
enum class GamePhase {
  NONE,     // No game today
  PREVIEW,  // Scheduled, not started
  LIVE,     // In progress
  FINAL,    // Game over
};

struct GameState {
  GamePhase phase{GamePhase::NONE};

  // Teams
  std::string away_abbrev;
  std::string home_abbrev;
  int away_score{0};
  int home_score{0};

  // Inning
  int inning{0};
  bool is_top_inning{true};
  std::string inning_ordinal;  // "1st", "2nd", etc.

  // Count (only valid when Live)
  int balls{0};
  int strikes{0};
  int outs{0};

  // Base runners (true = occupied)
  bool runner_first{false};
  bool runner_second{false};
  bool runner_third{false};

  // Pre-game display (local start time string, e.g. "7:10 PM")
  std::string start_time_str;

  // Game primary key for potential future live-feed polling
  int game_pk{0};
};

class BaseballTracker : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Called from the display page lambda
  void draw_game();

  // Setters called from generated code
  void set_display(display::Display *display) { display_ = display; }
  void set_font(font::Font *font) { font_ = font; }
  void set_rtc(time::RealTimeClock *rtc) { rtc_ = rtc; }
  void set_team_id(int team_id) { team_id_ = team_id; }
  void set_poll_interval(uint32_t interval_ms) { poll_interval_ms_ = interval_ms; }

 protected:
  // ---- drawing helpers ----
  void draw_no_game_();
  void draw_pregame_();
  void draw_live_();
  void draw_final_();

  // Draw the compact base diamond at pixel (cx, cy) = center of the diamond
  void draw_bases_(int cx, int cy);

  // Draw a row of filled/hollow dots, returns right edge x
  int draw_dots_(int x, int y, int count, int filled, Color on_color, Color off_color);

  // Draw text centered horizontally in a given x range
  void draw_centered_text_(int x_start, int x_end, int y, const char *text, Color color);

  // ---- data fetching ----
  void fetch_game_data_();
  bool parse_response_(const std::string &json_body);

  // ---- members ----
  display::Display *display_{nullptr};
  font::Font *font_{nullptr};
  time::RealTimeClock *rtc_{nullptr};

  int team_id_{136};
  uint32_t poll_interval_ms_{30000};

  uint32_t last_poll_ms_{0};
  bool first_poll_done_{false};

  GameState state_{};

  // Colors – defined as inline helpers to avoid constexpr issues with Color
  static Color kWhite()  { return Color(255, 255, 255); }
  static Color kYellow() { return Color(255, 255,   0); }
  static Color kGreen()  { return Color(  0, 255,   0); }
  static Color kRed()    { return Color(255,   0,   0); }
  static Color kCyan()   { return Color(  0, 255, 255); }
  static Color kDim()    { return Color( 50,  50,  50); }

  // Pixel geometry constants for a 128×32 display
  static constexpr int kDisplayW = 128;
  static constexpr int kDisplayH = 32;
  static constexpr int kTopRowY  = 1;   // y baseline for top row text
  static constexpr int kBotRowY  = 18;  // y baseline for bottom row text/dots

  // Dot geometry
  static constexpr int kDotR     = 2;   // dot radius in pixels
  static constexpr int kDotStep  = 6;   // pixel spacing between dot centers
};

}  // namespace baseball_tracker
}  // namespace esphome
