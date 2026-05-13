#include "baseball_tracker.h"
#include "library.h"
#include <ctime>
#include <cstring>
#include <strings.h>
#include "esphome/components/network/util.h"

#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"

namespace esphome {
namespace baseball_tracker {

static const char *const TAG = "baseball_tracker";

void HOT BaseballTracker::draw_game() {
  if (display_ == nullptr || font_ == nullptr) {
    ESP_LOGW(TAG, "draw_game() called but display or font is not set");
    return;
  }

  if (!esphome::network::is_connected()) {
    draw_centered_text_(0, 128, kRow2Y, "Waiting for network", kYellow());
    return;
  }

  if (!this->rtc_->now().is_valid()) {
    draw_centered_text_(0, 128, kRow2Y, "Waiting for time sync", kYellow());
    return;
  }

  if (!first_poll_done_) {
    draw_centered_text_(0, 128, kRow2Y, "Loading...", kDim());
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

void BaseballTracker::draw_no_game_() {
  draw_centered_text_(0, kDisplayW, kRow2Y, "NO GAME TODAY", kDim());
}

void BaseballTracker::draw_pregame_() {
  char top[32];
  snprintf(top, sizeof(top), "%s @ %s", state_.away_abbrev.c_str(), state_.home_abbrev.c_str());
  draw_centered_text_(0, kDisplayW, kRow1Y, top, kWhite());

  char time_buf[16] = "TBD";
  if (state_.has_game_start && state_.game_start_utc > 0) {
    ESPTime local = ESPTime::from_epoch_local(state_.game_start_utc);
    if (local.is_valid()) {
      local.strftime(time_buf, sizeof(time_buf), "%H:%M");
    }
  }
  draw_centered_text_(0, kDisplayW, kRow2Y, time_buf, kYellow());

  if (!state_.detailed_state.empty() && strcasecmp(state_.detailed_state.c_str(), "In Progress") != 0) {
    char detail_buf[24];
    snprintf(detail_buf, sizeof(detail_buf), "%s", state_.detailed_state.c_str());
    draw_centered_text_(0, kDisplayW, kPregameRow3Y, detail_buf, kDim());
  }
}

void BaseballTracker::draw_live_() {
  auto *d = display_;

  char away_buf[16];
  snprintf(away_buf, sizeof(away_buf), "%s  %d", state_.away_abbrev.c_str(), state_.away_score);
  d->print(2, kRow1Y, font_, kCyan(), away_buf);

  char home_buf[16];
  snprintf(home_buf, sizeof(home_buf), "%d  %s", state_.home_score, state_.home_abbrev.c_str());
  draw_right_aligned_text_(127, kRow1Y, home_buf, kCyan());

  if (state_.inning_intermission != InningIntermissionKind::NONE) {
    const char *prefix = state_.inning_intermission == InningIntermissionKind::MIDDLE ? "mid" : "end";
    char label_buf[20];
    if (!state_.inning_ordinal.empty()) {
      snprintf(label_buf, sizeof(label_buf), "%s %s", prefix, state_.inning_ordinal.c_str());
    } else {
      snprintf(label_buf, sizeof(label_buf), "%s %d", prefix, state_.inning);
    }
    draw_centered_text_(40, 88, kRow1Y, label_buf, kYellow());

    state_.balls = 0;
    state_.strikes = 0;
    state_.outs = 0;
    state_.runner_first = state_.runner_second = state_.runner_third = false;
  } else {
    char inn_buf[8];
    snprintf(inn_buf, sizeof(inn_buf), "%s %s", "", state_.inning_ordinal.c_str());
    if (state_.is_top_inning) {
      draw_centered_text_(38, 59, kRow1Y, "▲", kYellow());
      draw_centered_text_(54, 74, kRow1Y, inn_buf, kYellow());
    } else {
      draw_centered_text_(54, 74, kRow1Y, inn_buf, kYellow());
      draw_centered_text_(128 - 59, 128 - 38, kRow1Y, "▼", kYellow());
    }
  }

  // Advance queue: when the current play (spectacle + scroll) finishes, load the next one.
  // scoring_play_end_ms is computed once on first render to avoid per-frame get_text_bounds.
  if (!state_.scoring_play_text.empty()) {
    if (state_.scoring_play_end_ms == 0) {
      int spec_ms = (state_.scoring_play_type == ScoringPlayType::GRAND_SLAM) ? kGSlamSpectacleMs
                  : (state_.scoring_play_type == ScoringPlayType::HOME_RUN)   ? kHRSpectacleMs : 0;
      int tw = 0, th = 0, xo = 0, yo = 0;
      display_->get_text_bounds(0, 0, state_.scoring_play_text.c_str(), font_,
                                display::TextAlign::TOP_LEFT, &xo, &yo, &tw, &th);
      if (tw < 0) tw = 0;
      uint32_t sp_ms = (uint32_t)((tw + kDisplayW) * 1000 / kScrollPxPerSec);
      if (sp_ms < 1) sp_ms = 1;
      state_.scoring_play_end_ms = state_.scoring_play_started_ms + (uint32_t)spec_ms + sp_ms;
    }
    if (millis() >= state_.scoring_play_end_ms) {
      if (!state_.scoring_play_queue.empty()) {
        state_.scoring_play_text       = state_.scoring_play_queue.front();
        state_.scoring_play_type       = state_.scoring_play_type_queue.front();
        state_.scoring_play_started_ms = millis();
        state_.scoring_play_end_ms     = 0;
        state_.scoring_play_queue.erase(state_.scoring_play_queue.begin());
        state_.scoring_play_type_queue.erase(state_.scoring_play_type_queue.begin());
      } else {
        state_.scoring_play_text.clear();
      }
    }
  }

  // Determine display mode for rows 2-3
  bool in_spectacle  = false;
  bool showing_scroll = false;
  uint32_t play_elapsed    = 0;
  int      cur_spectacle_ms = 0;
  if (!state_.scoring_play_text.empty()) {
    play_elapsed      = millis() - state_.scoring_play_started_ms;
    cur_spectacle_ms  = (state_.scoring_play_type == ScoringPlayType::GRAND_SLAM) ? kGSlamSpectacleMs
                      : (state_.scoring_play_type == ScoringPlayType::HOME_RUN)   ? kHRSpectacleMs : 0;
    if (cur_spectacle_ms > 0 && play_elapsed < (uint32_t)cur_spectacle_ms) {
      in_spectacle = true;
    } else {
      showing_scroll = true;
    }
  }

  // Spectacle replaces rows 2-3 entirely; row 1 is already drawn above
  if (in_spectacle) {
    draw_spectacle_(play_elapsed, state_.scoring_play_type);
    return;
  }

  // Row 2: pitcher name + ball-strike count
  char count_buf[8];
  snprintf(count_buf, sizeof(count_buf), "%d-%d", state_.balls, state_.strikes);
  {
    int count_w, count_h, cxo, cyo;
    display_->get_text_bounds(0, 0, count_buf, font_, display::TextAlign::TOP_LEFT, &cxo, &cyo, &count_w,
                              &count_h);
    if (count_w < 0)
      count_w = 0;
    int max_p_w = kRow2RightX - 2 - count_w - 3;
    if (max_p_w < 8)
      max_p_w = 8;
    char p_line[24];
    const char *pl = state_.pitcher_last.empty() ? "--" : state_.pitcher_last.c_str();
    snprintf(p_line, sizeof(p_line), "P: %s", pl);
    draw_text_max_width_(2, kRow2Y, max_p_w, p_line, kDim());
  }
  draw_right_aligned_text_(kRow2RightX, kRow2Y, count_buf, kWhite());

  // Row 3: scrolling play description, or batter/diamond/outs
  if (showing_scroll) {
    uint32_t scroll_elapsed = play_elapsed - (uint32_t)cur_spectacle_ms;
    int scroll_x = kDisplayW - (int)(scroll_elapsed * kScrollPxPerSec / 1000);
    display_->print(scroll_x, kPregameRow3Y, font_, kYellow(), state_.scoring_play_text.c_str());
  } else {
    {
      const char *bl = state_.batter_last.empty() ? "--" : state_.batter_last.c_str();
      char b_line[28];
      snprintf(b_line, sizeof(b_line), "B: %s", bl);
      draw_text_max_width_(2, kPregameRow3Y, kLiveBatterNameMaxW, b_line, kCyan());
    }

    static constexpr int d13x = 8;
    static constexpr int base_pad = 2;
    int diamond_right = d13x + base_pad;
    int diamond_cx = kOutsFirstX - kDotR - kDiamondOutPadding - diamond_right;
    draw_bases_(diamond_cx, kDiamondCY);

    draw_dots_(kOutsFirstX, kOutDotsY, 2, state_.outs, kRed(), kDim());
  }
}

void BaseballTracker::draw_final_() {
  char away_buf[12], home_buf[12];
  snprintf(away_buf, sizeof(away_buf), "%s  %d", state_.away_abbrev.c_str(), state_.away_score);
  snprintf(home_buf, sizeof(home_buf), "%d  %s", state_.home_score, state_.home_abbrev.c_str());

  display_->print(2, kRow1Y, font_, kWhite(), away_buf);
  draw_centered_text_(40, 88, kRow1Y, "FINAL", kYellow());
  draw_centered_text_(78, 126, kRow1Y, home_buf, kWhite());

  draw_centered_text_(0, kDisplayW, kRow2Y, state_.inning_ordinal.c_str(), kDim());
}

void BaseballTracker::draw_spectacle_(uint32_t elapsed_ms, ScoringPlayType type) {
  auto *d = display_;
  bool flash_on = (elapsed_ms / 125) % 2 == 0;
  Color fg = flash_on ? kYellow() : kWhite();

  const char *label = (type == ScoringPlayType::GRAND_SLAM) ? "GRAND SLAM!" : "HR!";
  int text_w = 0, text_h = 0, xo = 0, yo = 0;
  display_->get_text_bounds(0, 0, label, font_, display::TextAlign::TOP_LEFT, &xo, &yo, &text_w, &text_h);
  if (text_w < 0) text_w = 0;
  int label_x = (kDisplayW - text_w) / 2;
  display_->print(label_x, kRow2Y, font_, fg, label);

  if (type == ScoringPlayType::GRAND_SLAM && flash_on) {
    d->rectangle(0, 0, kDisplayW, kDisplayH, kYellow());
  }
}

void BaseballTracker::draw_bases_(int cx, int cy) {
  auto *d = display_;

  static constexpr int dist = 8;  // equilateral diamond: same offset to each base from center
  static constexpr int pad = 2;

  struct Base {
    int dx, dy;
    bool occupied;
  } bases[3] = {
    { 0, -dist, state_.runner_second },
    {-dist, 0, state_.runner_third},
    { dist, 0, state_.runner_first},
  };

  int x2 = cx, y2 = cy - dist;
  int x3 = cx - dist, y3 = cy;
  int x1 = cx + dist, y1 = cy;
  int xh = cx, yh = cy + dist;

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

  d->rectangle(xh - 1, yh - 1, 3, 3, kDim());
}

}  // namespace baseball_tracker
}  // namespace esphome
