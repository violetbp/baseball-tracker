#pragma once

#include <ctime>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/color.h"
#include "esphome/components/display/display.h"
#include "esphome/components/font/font.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/switch/switch.h"
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

  // Pre-game display (raw ISO8601 from API, also used to parse first pitch in UTC)
  std::string start_time_str;
  // Parsed first pitch in UTC; used for T−N auto page
  time_t game_start_utc{0};
  bool has_game_start{false};

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
  void set_auto_baseball_page(bool e) { auto_baseball_page_ = e; }
  void set_auto_page_lead_sec(uint32_t s) { auto_page_lead_sec_ = s; }
  void set_baseball_page_switch(switch_::Switch *s) { baseball_page_switch_ = s; }
  void set_game_in_progress_sensor(binary_sensor::BinarySensor *s) { game_in_progress_sensor_ = s; }

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
  // Draw with right edge at x_end (e.g. align to 126)
  void draw_right_aligned_text_(int x_end, int y, const char *text, Color color);

  // ---- data fetching ----
  void fetch_game_data_();
  bool parse_response_(const std::string &json_body);
  static bool parse_iso8601_utc(const char *iso, time_t *out);
  // Auto baseball page: on from (first pitch − lead) through until Final / no game
  void try_auto_baseball_page_();
  bool should_auto_show_baseball_() const;
  void update_game_in_progress_sensor_();

  // ---- members ----
  display::Display *display_{nullptr};
  font::Font *font_{nullptr};
  time::RealTimeClock *rtc_{nullptr};

  int team_id_{136};
  uint32_t poll_interval_ms_{30000};

  uint32_t last_poll_ms_{0};
  bool first_poll_done_{false};

  // Auto page (T−N before first pitch through end of play)
  bool auto_baseball_page_{false};
  uint32_t auto_page_lead_sec_{300};
  switch_::Switch *baseball_page_switch_{nullptr};
  bool last_auto_show_cmd_{false};
  uint32_t last_auto_logic_ms_{0};

  // Optional: "game in progress" = LIVE
  binary_sensor::BinarySensor *game_in_progress_sensor_{nullptr};
  bool last_published_in_progress_{false};
  bool in_progress_sensor_published_{false};

  GameState state_{};

  // Colors – defined as inline helpers to avoid constexpr issues with Color
  static Color kWhite()  { return Color(255, 255, 255); }
  static Color kYellow() { return Color(255, 255,   0); }
  static Color kGreen()  { return Color(  0, 255,   0); }
  static Color kRed()    { return Color(255,   0,   0); }
  static Color kCyan()   { return Color(  0, 255, 255); }
  static Color kDim()    { return Color( 80,  80,  80); }

  // Pixel geometry for a 128×32 display — three visual rows
  static constexpr int kDisplayW  = 128;
  static constexpr int kDisplayH  = 32;
  static constexpr int kRow1Y = 1;  // line 1: teams, scores, inning
  static constexpr int kRow2Y = 9;  // line 2: balls-strikes text only (live)
  // Line 3: base diamond + out dots; outs are right-anchored; diamond leaves a gap
  // before the first out dot.
  static constexpr int kOutDotsY  = 23;  // vertical center of out circles
  static constexpr int kDiamondCY = 20;  // draw_bases_ centre (1st/3rd sit at kOutDotsY)
  static constexpr int kOutsFirstX  = 100;  // x of leftmost out-dot centre (group toward right edge)
  static constexpr int kDiamondOutPadding  = 5;  // min px gap between diamond and first out dot
  static constexpr int kRow2RightX = 126;  // B–S count right-align edge (see draw_right_aligned)

  // Dot geometry (outs indicator, line 3)
  static constexpr int kDotR    = 3;  // dot radius in pixels
  static constexpr int kDotStep = 8;  // pixel spacing between dot centers
};

}  // namespace baseball_tracker
}  // namespace esphome
