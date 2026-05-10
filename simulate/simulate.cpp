#define IMGUI_DEFINE_MATH_FUNCTIONS
#define SDL_MAIN_HANDLED
#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_sdlrenderer2.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <cmath>
#include <string>
#include <string_view>
#include <algorithm>

#include "sim_state.h"
#include "mlb_live.h"


// ---------------------------------------------------------------------------
// Display geometry — source of truth is baseball_tracker.h constants
// ---------------------------------------------------------------------------

static constexpr int kDisplayW = 128;
static constexpr int kDisplayH = 32;
static constexpr int kDefaultWindowPixelScale = 4;
static constexpr int kRow1Y = 1;
static constexpr int kRow2Y = 11;        // firmware: kRow2Y = 11
static constexpr int kPregameRow3Y = 21; // firmware: kPregameRow3Y = 21
static constexpr int kOutDotsY = 24;
static constexpr int kDiamondCY = 20;
static constexpr int kOutsFirstX = 114;  // firmware: kOutsFirstX = 114
static constexpr int kDiamondOutPadding = 5;
static constexpr int kRow2RightX = 126;
static constexpr int kDotR = 3;
static constexpr int kDotStep = 8;
static constexpr int kLiveBatterNameMaxW = 68 + 15;

// ---------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------

static SDL_Color kWhite  = {255, 255, 255, 255};
static SDL_Color kYellow = {255, 255,   0, 255};
static SDL_Color kRed    = {255,   0,   0, 255};
static SDL_Color kCyan   = {  0, 255, 255, 255};
static SDL_Color kDim    = { 80,  80,  80, 255};

static constexpr Uint32 kFramebufferFormat = SDL_PIXELFORMAT_ARGB8888;

static SDL_Surface *make_display_surface(int w, int h) {
  return SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, kFramebufferFormat);
}

static Uint32 map_color(SDL_Surface *surf, const SDL_Color &c) {
  return SDL_MapRGBA(surf->format, c.r, c.g, c.b, c.a);
}

static void put_pixel_locked(SDL_Surface *surf, int x, int y, Uint32 px) {
  Uint8 *row = static_cast<Uint8 *>(surf->pixels) + y * surf->pitch;
  std::memcpy(row + static_cast<size_t>(x) * 4u, &px, sizeof(px));
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

// Use UTF-8 rendering so Unicode glyphs (▲ ▼) work if font supports them
static void draw_text(SDL_Surface *buf, int x, int y, TTF_Font *font,
                      const SDL_Color &color, const char *text) {
  SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, color);
  if (s) {
    SDL_Rect dst = {x, y, 0, 0};
    SDL_BlitSurface(s, nullptr, buf, &dst);
    SDL_FreeSurface(s);
  }
}

static void draw_centered_text(SDL_Surface *buf, int x_start, int x_end, int y,
                                TTF_Font *font, const SDL_Color &color, const char *text) {
  int tw = 0, th = 0;
  TTF_SizeUTF8(font, text, &tw, &th);
  int center = (x_start + x_end) / 2;
  draw_text(buf, center - tw / 2, y, font, color, text);
}

static void draw_right_aligned_text(SDL_Surface *buf, int x_end, int y,
                                     TTF_Font *font, const SDL_Color &color, const char *text) {
  int tw = 0, th = 0;
  TTF_SizeUTF8(font, text, &tw, &th);
  int draw_x = x_end - tw;
  if (draw_x < 0) draw_x = 0;
  draw_text(buf, draw_x, y, font, color, text);
}

// Truncate text to fit within max_w pixels
static void draw_text_max_width(SDL_Surface *buf, int x, int y, int max_w,
                                 TTF_Font *font, const SDL_Color &color, const char *text) {
  if (!text || !*text || max_w <= 0) return;
  char work[64];
  strncpy(work, text, sizeof(work) - 1);
  work[sizeof(work) - 1] = '\0';
  for (;;) {
    if (!*work) return;
    int tw = 0, th = 0;
    TTF_SizeUTF8(font, work, &tw, &th);
    if (tw <= max_w) {
      draw_text(buf, x, y, font, color, work);
      return;
    }
    size_t len = strlen(work);
    if (len <= 1) return;
    work[len - 1] = '\0';
  }
}

static void draw_line(SDL_Surface *buf, int x0, int y0, int x1, int y1, const SDL_Color &color) {
  SDL_LockSurface(buf);
  const Uint32 px = map_color(buf, color);
  const int w = buf->w, h = buf->h;
  int dx = abs(x1 - x0), dy = abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx - dy;
  while (true) {
    if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h)
      put_pixel_locked(buf, x0, y0, px);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 > -dy) { err -= dy; x0 += sx; }
    if (e2 < dx)  { err += dx; y0 += sy; }
  }
  SDL_UnlockSurface(buf);
}

static void draw_filled_rect(SDL_Surface *buf, int x, int y, int w, int h, const SDL_Color &color) {
  SDL_LockSurface(buf);
  const Uint32 px = map_color(buf, color);
  const int bw = buf->w, bh = buf->h;
  for (int dy = 0; dy < h; dy++) {
    for (int dx = 0; dx < w; dx++) {
      int px_x = x + dx, py = y + dy;
      if (px_x >= 0 && px_x < bw && py >= 0 && py < bh)
        put_pixel_locked(buf, px_x, py, px);
    }
  }
  SDL_UnlockSurface(buf);
}

static void draw_rect(SDL_Surface *buf, int x, int y, int w, int h, const SDL_Color &color) {
  draw_line(buf, x, y, x + w - 1, y, color);
  draw_line(buf, x, y + h - 1, x + w - 1, y + h - 1, color);
  draw_line(buf, x, y, x, y + h - 1, color);
  draw_line(buf, x + w - 1, y, x + w - 1, y + h - 1, color);
}

static void draw_filled_circle(SDL_Surface *buf, int x, int y, int radius, const SDL_Color &color) {
  SDL_LockSurface(buf);
  const Uint32 pxcol = map_color(buf, color);
  const int bw = buf->w, bh = buf->h;
  auto set_px = [&](int px, int py) {
    if (px >= 0 && px < bw && py >= 0 && py < bh)
      put_pixel_locked(buf, px, py, pxcol);
  };
  int d = 3 - 2 * radius, dx = 0, dy = 2 * radius;
  set_px(x, y + radius); set_px(x, y - radius);
  set_px(x + radius, y); set_px(x - radius, y);
  while (dx < dy) {
    if (d < 0) { d = d + 4 * dx + 6; }
    else { dy -= 2; d = d + 4 * (dx - dy) + 10; }
    dx++;
    for (int i = x - dx + 1; i <= x + dx; i++) { set_px(i, y + dy / 2); set_px(i, y - dy / 2); }
    if (dx != dy) {
      for (int i = x - dy + 1; i <= x + dy; i++) { set_px(i, y + dx); set_px(i, y - dx); }
    }
  }
  SDL_UnlockSurface(buf);
}

static void draw_circle(SDL_Surface *buf, int x, int y, int radius, const SDL_Color &color) {
  SDL_LockSurface(buf);
  const Uint32 pxcol = map_color(buf, color);
  const int bw = buf->w, bh = buf->h;
  auto set_px = [&](int px, int py) {
    if (px >= 0 && px < bw && py >= 0 && py < bh)
      put_pixel_locked(buf, px, py, pxcol);
  };
  int d = 3 - 2 * radius, dx = 0, dy = 2 * radius;
  set_px(x, y + radius); set_px(x, y - radius);
  set_px(x + radius, y); set_px(x - radius, y);
  while (dx < dy) {
    if (d < 0) { d = d + 4 * dx + 6; }
    else { dy -= 2; d = d + 4 * (dx - dy) + 10; }
    dx++;
    set_px(x + dx, y + dy / 2); set_px(x - dx, y + dy / 2);
    set_px(x + dx, y - dy / 2); set_px(x - dx, y - dy / 2);
    if (dx != dy) {
      set_px(x + dy, y + dx); set_px(x - dy, y + dx);
      set_px(x + dy, y - dx); set_px(x - dy, y - dx);
    }
  }
  SDL_UnlockSurface(buf);
}

// ---------------------------------------------------------------------------
// Phase renderers — match firmware draw_ui.cpp layout exactly
// ---------------------------------------------------------------------------

static void draw_no_game(SDL_Surface *buf, TTF_Font *font) {
  draw_centered_text(buf, 0, kDisplayW, kRow2Y, font, kDim, "NO GAME TODAY");
}

static void draw_pregame(SDL_Surface *buf, TTF_Font *font, const GameState &s) {
  char top[32];
  snprintf(top, sizeof(top), "%s @ %s", s.away_abbrev.c_str(), s.home_abbrev.c_str());
  draw_centered_text(buf, 0, kDisplayW, kRow1Y, font, kWhite, top);

  // Start time: show raw UTC from API string or "TBD"
  const std::string &dt = s.start_time_str;
  char time_buf[16] = "TBD";
  if (dt.size() >= 16) {
    snprintf(time_buf, sizeof(time_buf), "%c%c:%c%c UTC",
             dt[11], dt[12], dt[14], dt[15]);
  }
  draw_centered_text(buf, 0, kDisplayW, kRow2Y, font, kYellow, time_buf);

  // Row 3: detailedState (e.g. "Postponed", "Rain Delay") — skip if empty or "In Progress"
  if (!s.detailed_state.empty() &&
      strcasecmp(s.detailed_state.c_str(), "In Progress") != 0) {
    draw_centered_text(buf, 0, kDisplayW, kPregameRow3Y, font, kDim, s.detailed_state.c_str());
  }
}

static void draw_bases(SDL_Surface *buf, int cx, int cy, bool r1, bool r2, bool r3) {
  static constexpr int dist = 8;
  static constexpr int pad = 2;

  int x2 = cx, y2 = cy - dist;
  int x3 = cx - dist, y3 = cy;
  int x1 = cx + dist, y1 = cy;
  int xh = cx, yh = cy + dist;

  draw_line(buf, x2, y2, x3, y3, kDim);
  draw_line(buf, x2, y2, x1, y1, kDim);
  draw_line(buf, x3, y3, xh, yh, kDim);
  draw_line(buf, x1, y1, xh, yh, kDim);

  // 2nd base
  if (r2) draw_filled_rect(buf, cx - pad, cy - dist - pad, 2 * pad + 1, 2 * pad + 1, kYellow);
  else     draw_rect(buf,        cx - pad, cy - dist - pad, 2 * pad + 1, 2 * pad + 1, kDim);
  // 3rd base
  if (r3) draw_filled_rect(buf, cx - dist - pad, cy - pad, 2 * pad + 1, 2 * pad + 1, kYellow);
  else     draw_rect(buf,        cx - dist - pad, cy - pad, 2 * pad + 1, 2 * pad + 1, kDim);
  // 1st base
  if (r1) draw_filled_rect(buf, cx + dist - pad, cy - pad, 2 * pad + 1, 2 * pad + 1, kYellow);
  else     draw_rect(buf,        cx + dist - pad, cy - pad, 2 * pad + 1, 2 * pad + 1, kDim);
  // Home plate
  draw_rect(buf, xh - 1, yh - 1, 3, 3, kDim);
}

// count = number of dot slots; filled = number to fill (mirrors firmware draw_dots_)
static void draw_outs(SDL_Surface *buf, int x, int count, int filled) {
  for (int i = 0; i < count; i++) {
    int cx = x + i * kDotStep;
    if (i < filled) draw_filled_circle(buf, cx, kOutDotsY, kDotR, kRed);
    else             draw_circle(buf,        cx, kOutDotsY, kDotR, kDim);
  }
}

static void draw_live(SDL_Surface *buf, TTF_Font *font, const GameState &s) {
  // Row 1 left: away team + score
  char away_buf[16];
  snprintf(away_buf, sizeof(away_buf), "%s  %d", s.away_abbrev.c_str(), s.away_score);
  draw_text(buf, 2, kRow1Y, font, kCyan, away_buf);

  // Row 1 right: home score + team
  char home_buf[16];
  snprintf(home_buf, sizeof(home_buf), "%d  %s", s.home_score, s.home_abbrev.c_str());
  draw_right_aligned_text(buf, 126, kRow1Y, font, kCyan, home_buf);

  // Row 1 center: inning indicator
  int balls = s.balls, strikes = s.strikes, outs = s.outs;
  bool r1 = s.runner_first, r2 = s.runner_second, r3 = s.runner_third;
  if (s.inning_intermission != InningIntermissionKind::NONE) {
    balls = strikes = outs = 0;
    r1 = r2 = r3 = false;
    const char *prefix = s.inning_intermission == InningIntermissionKind::MIDDLE ? "mid" : "end";
    char label_buf[20];
    if (!s.inning_ordinal.empty())
      snprintf(label_buf, sizeof(label_buf), "%s %s", prefix, s.inning_ordinal.c_str());
    else
      snprintf(label_buf, sizeof(label_buf), "%s %d", prefix, s.inning);
    draw_centered_text(buf, 40, 88, kRow1Y, font, kYellow, label_buf);
  } else {
    // Leading space creates separation from the arrow, matching firmware's "%s %s" with ""
    char inn_buf[16];
    snprintf(inn_buf, sizeof(inn_buf), " %s", s.inning_ordinal.c_str());
    if (s.is_top_inning) {
      draw_centered_text(buf, 38,  59, kRow1Y, font, kYellow, "\xe2\x96\xb2");  // ▲ UTF-8
      draw_centered_text(buf, 54,  74, kRow1Y, font, kYellow, inn_buf);
    } else {
      draw_centered_text(buf, 54,  74, kRow1Y, font, kYellow, inn_buf);
      draw_centered_text(buf, 69,  90, kRow1Y, font, kYellow, "\xe2\x96\xbc");  // ▼ UTF-8
    }
  }

  // Row 2: pitcher name (left, truncated to make room for count) + B-S count (right)
  char count_buf[8];
  snprintf(count_buf, sizeof(count_buf), "%d-%d", balls, strikes);
  {
    int count_w = 0, count_h = 0;
    TTF_SizeUTF8(font, count_buf, &count_w, &count_h);
    int max_p_w = kRow2RightX - 2 - count_w - 3;
    if (max_p_w < 8) max_p_w = 8;
    char p_line[24];
    const char *pl = s.pitcher_last.empty() ? "--" : s.pitcher_last.c_str();
    snprintf(p_line, sizeof(p_line), "P: %s", pl);
    draw_text_max_width(buf, 2, kRow2Y, max_p_w, font, kDim, p_line);
  }
  draw_right_aligned_text(buf, kRow2RightX, kRow2Y, font, kWhite, count_buf);

  // Row 3 (kPregameRow3Y): batter name
  if (s.inning_intermission == InningIntermissionKind::NONE) {
    char b_line[28];
    const char *bl = s.batter_last.empty() ? "--" : s.batter_last.c_str();
    snprintf(b_line, sizeof(b_line), "B: %s", bl);
    draw_text_max_width(buf, 2, kPregameRow3Y, kLiveBatterNameMaxW, font, kCyan, b_line);
  }

  // Base diamond + 2 out dots (matching firmware's draw_dots_ count=2)
  static constexpr int d13x = 8;
  static constexpr int base_pad = 2;
  int diamond_right = d13x + base_pad;
  int diamond_cx = kOutsFirstX - kDotR - kDiamondOutPadding - diamond_right;
  draw_bases(buf, diamond_cx, kDiamondCY, r1, r2, r3);
  draw_outs(buf, kOutsFirstX, 2, outs);
}

static void draw_final(SDL_Surface *buf, TTF_Font *font, const GameState &s) {
  char away_buf[12], home_buf[12];
  snprintf(away_buf, sizeof(away_buf), "%s  %d", s.away_abbrev.c_str(), s.away_score);
  snprintf(home_buf, sizeof(home_buf), "%d  %s", s.home_score, s.home_abbrev.c_str());

  draw_text(buf, 2, kRow1Y, font, kWhite, away_buf);
  draw_centered_text(buf, 40, 88, kRow1Y, font, kYellow, "FINAL");
  draw_centered_text(buf, 78, 126, kRow1Y, font, kWhite, home_buf);

  draw_centered_text(buf, 0, kDisplayW, kRow2Y, font, kDim, s.inning_ordinal.c_str());
}

// ---------------------------------------------------------------------------
// Sample schedule JSON builder for the JSON injection panel
// ---------------------------------------------------------------------------

static std::string build_mlb_json(
  int game_pk,
  const char *phase, const char *away, int away_score,
  const char *home, int home_score,
  int inning, const char *ordinal, bool top_inning,
  int balls, int strikes, int outs,
  bool r1, bool r2, bool r3,
  const char *start_time = "2026-05-05T20:10:00Z"
) {
  // Individual base runners (not all-or-nothing)
  std::ostringstream bases_oss;
  bases_oss << "\"offense\":{"
            << "\"first\":"  << (r1 ? "{\"id\":699912}" : "null") << ","
            << "\"second\":" << (r2 ? "{\"id\":699913}" : "null") << ","
            << "\"third\":"  << (r3 ? "{\"id\":699914}" : "null")
            << "}";
  std::string bases = bases_oss.str();

  std::string detailed = std::string(phase) == "Live" ? "In Progress" :
                         std::string(phase) == "Final" ? "Official" : "Scheduled";
  std::string status_code = std::string(phase) == "Live" ? "I" :
                            std::string(phase) == "Final" ? "F" : "S";

  std::ostringstream oss;
  oss << "{\"totalGames\":1,\"dates\":[{\"games\":[{"
      << "\"gamePk\":" << game_pk << ",\"gameDate\":\"" << start_time << "\","
      << "\"status\":{\"abstractGameState\":\"" << phase << "\","
      << "\"detailedState\":\"" << detailed << "\","
      << "\"statusCode\":\"" << status_code << "\"},"
      << "\"teams\":{"
      << "\"away\":{\"team\":{\"abbreviation\":\"" << away << "\"},\"score\":" << away_score << "},"
      << "\"home\":{\"team\":{\"abbreviation\":\"" << home << "\"},\"score\":" << home_score << "}},"
      << "\"linescore\":{"
      << "\"currentInning\":" << inning
      << ",\"currentInningOrdinal\":\"" << ordinal << "\","
      << "\"isTopInning\":" << (top_inning ? "true" : "false")
      << ",\"balls\":" << balls
      << ",\"strikes\":" << strikes
      << ",\"outs\":" << outs
      << "," << bases
      << "\"}}}]}]}\n";
  return oss.str();
}

// ---------------------------------------------------------------------------
// sim_refresh_display_model — push SimState into GameState for rendering
// ---------------------------------------------------------------------------

void sim_refresh_display_model(SimState &sim) {
  sim.state = GameState{};
  if      (strcmp(sim.phase, "Live")  == 0) sim.state.phase = GamePhase::LIVE;
  else if (strcmp(sim.phase, "Final") == 0) sim.state.phase = GamePhase::FINAL;
  else if (strcmp(sim.phase, "None")  == 0) sim.state.phase = GamePhase::NONE;
  else                                       sim.state.phase = GamePhase::PREVIEW;

  sim.state.away_abbrev = sim.away;
  sim.state.home_abbrev = sim.home;
  sim.state.away_score  = sim.away_score;
  sim.state.home_score  = sim.home_score;
  sim.state.inning      = sim.inning;
  sim.state.is_top_inning = sim.top_inning;
  sim.state.inning_intermission =
      sim.intermission == 1 ? InningIntermissionKind::MIDDLE :
      sim.intermission == 2 ? InningIntermissionKind::END :
                              InningIntermissionKind::NONE;
  sim.state.inning_ordinal = sim.ordinal;
  sim.state.balls   = sim.balls;
  sim.state.strikes = sim.strikes;
  sim.state.outs    = sim.outs;
  sim.state.pitcher_last = sim.pitcher;
  sim.state.batter_last  = sim.batter;
  sim.state.runner_first  = sim.runner_first;
  sim.state.runner_second = sim.runner_second;
  sim.state.runner_third  = sim.runner_third;
  sim.state.start_time_str = sim.start_time;
  sim.state.has_game_start = true;
  sim.state.game_pk = sim.game_pk;
  sim.state.detailed_state = sim.detailed_state;
}

// ---------------------------------------------------------------------------
// Synthetic live animation
// ---------------------------------------------------------------------------

static void format_inning_ordinal(int inning, char *buf, size_t cap) {
  if (cap == 0) return;
  inning = std::clamp(inning, 1, 999);
  int teen = inning % 100;
  int d = inning % 10;
  const char *suf = "th";
  if (teen < 10 || teen > 20) {
    if (d == 1) suf = "st";
    else if (d == 2) suf = "nd";
    else if (d == 3) suf = "rd";
  }
  snprintf(buf, cap, "%d%s", inning, suf);
}

static void copy_synthetic_live_into_editor_and_refresh(SimState &sim) {
  format_inning_ordinal(sim.live_inning, sim.live_ordinal, sizeof(sim.live_ordinal));
  snprintf(sim.phase, sizeof(sim.phase), "Live");
  sim.away_score = sim.live_away_score;
  sim.home_score = sim.live_home_score;
  strncpy(sim.ordinal, sim.live_ordinal, sizeof(sim.ordinal));
  sim.ordinal[sizeof(sim.ordinal) - 1] = '\0';
  sim.inning    = sim.live_inning;
  sim.top_inning = sim.live_top;
  sim.balls     = sim.live_balls;
  sim.strikes   = sim.live_strikes;
  sim.outs      = sim.live_outs;
  sim.runner_first  = sim.live_r1;
  sim.runner_second = sim.live_r2;
  sim.runner_third  = sim.live_r3;
  sim.intermission  = 0;
  sim.pitcher[0]    = '\0';
  sim.batter[0]     = '\0';
  sim_refresh_display_model(sim);
}

static void advance_live_feed(SimState &sim) {
  sim.live_tick++;
  if (sim.live_tick < sim.live_speed) return;
  sim.live_tick = 0;

  sim.live_strikes++;
  if (sim.live_strikes >= 3) {
    sim.live_strikes = 0;
    sim.live_balls = 0;
    sim.live_outs++;
    if (sim.live_outs >= 3) {
      sim.live_outs = 0;
      bool was_top = sim.live_top;
      sim.live_top = !sim.live_top;
      if (!was_top && sim.live_top) sim.live_inning++;
    }
  }

  if (rand() % 12 == 0) {
    if (sim.live_top) sim.live_away_score += (rand() % 3) + 1;
    else              sim.live_home_score += (rand() % 3) + 1;
  }
  if (rand() % 6 == 0)  sim.live_r1 = !sim.live_r1;
  if (rand() % 8 == 0)  sim.live_r2 = !sim.live_r2;
  if (rand() % 12 == 0) sim.live_r3 = !sim.live_r3;
}

// ---------------------------------------------------------------------------
// MLB network pumps
// ---------------------------------------------------------------------------

static void run_one_schedule_poll(SimState &sim) {
  std::string body, err;
  if (!sim_fetch_mlb_schedule(std::string(sim.base_url), sim.team_id, body, err)) {
    snprintf(sim.mlb_status, sizeof(sim.mlb_status), "Schedule HTTP: %s", err.c_str());
    return;
  }
  std::string perr;
  if (!sim_apply_statsapi_json(sim, body, &perr)) {
    snprintf(sim.mlb_status, sizeof(sim.mlb_status), "Schedule JSON: %s", perr.c_str());
    return;
  }
  // status message is set by sim_apply_statsapi_json inside try_apply_schedule_root
}

static void run_one_live_poll(SimState &sim) {
  std::string body, err;
  if (!sim_fetch_mlb_live_feed(std::string(sim.base_url), sim.game_pk, body, err)) {
    snprintf(sim.mlb_status, sizeof(sim.mlb_status), "Live HTTP: %s", err.c_str());
    return;
  }
  std::string perr;
  if (!sim_apply_statsapi_json(sim, body, &perr)) {
    snprintf(sim.mlb_status, sizeof(sim.mlb_status), "Live JSON: %s", perr.c_str());
    return;
  }
  snprintf(sim.mlb_status, sizeof(sim.mlb_status), "Live OK (%zu bytes)", body.size());
}

static void pump_mlb_network(SimState &sim, Uint32 ticks) {
  // Schedule poll
  bool sched_manual = sim.mlb_schedule_fetch_pending;
  if (sched_manual) sim.mlb_schedule_fetch_pending = false;
  bool sched_timer = sim.mlb_poll_schedule && sim.team_id > 0 &&
    (sim.mlb_last_schedule_poll_ticks == 0 ||
     ticks - sim.mlb_last_schedule_poll_ticks >= (Uint32)std::max(5000, sim.mlb_schedule_poll_ms));

  if (sched_manual || sched_timer) {
    sim.mlb_last_schedule_poll_ticks = ticks;
    run_one_schedule_poll(sim);
  }

  // Live feed poll
  bool live_manual = sim.mlb_fetch_pending;
  if (live_manual) sim.mlb_fetch_pending = false;
  bool live_timer = sim.mlb_poll_live && sim.game_pk > 0 &&
    (sim.mlb_last_poll_ticks == 0 ||
     ticks - sim.mlb_last_poll_ticks >= (Uint32)std::max(500, sim.mlb_poll_ms));

  if (live_manual || live_timer) {
    sim.mlb_last_poll_ticks = ticks;
    run_one_live_poll(sim);
  }
}

// ---------------------------------------------------------------------------
// ImGui Control Panel
// ---------------------------------------------------------------------------

static void render_control_panel(SimState &sim) {
  // ---- Game State ----
  if (ImGui::CollapsingHeader("Game State", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text("Phase:");
    ImGui::SameLine();
    static const char *phases[] = {"None", "Preview", "Live", "Final"};
    int phase_idx = 1;
    if      (strcmp(sim.phase, "None")  == 0) phase_idx = 0;
    else if (strcmp(sim.phase, "Live")  == 0) phase_idx = 2;
    else if (strcmp(sim.phase, "Final") == 0) phase_idx = 3;
    if (ImGui::Combo("##phase", &phase_idx, phases, 4)) {
      strncpy(sim.phase, phases[phase_idx], sizeof(sim.phase));
      sim.phase[sizeof(sim.phase) - 1] = '\0';
      sim_refresh_display_model(sim);
    }

    ImGui::Text("Teams:");
    ImGui::SameLine();
    if (ImGui::InputText("##away", sim.away, sizeof(sim.away))) sim_refresh_display_model(sim);
    ImGui::SameLine();
    ImGui::TextUnformatted("@");
    ImGui::SameLine();
    if (ImGui::InputText("##home", sim.home, sizeof(sim.home))) sim_refresh_display_model(sim);

    ImGui::Text("Scores:");
    ImGui::SameLine();
    if (ImGui::InputInt("##away_score", &sim.away_score, 1, 4)) sim_refresh_display_model(sim);
    ImGui::SameLine();
    if (ImGui::InputInt("##home_score", &sim.home_score, 1, 4)) sim_refresh_display_model(sim);

    ImGui::Text("Inning:");
    ImGui::SameLine();
    if (ImGui::InputInt("##inning", &sim.inning, 1, 99)) sim_refresh_display_model(sim);
    ImGui::SameLine();
    if (ImGui::InputText("##ordinal", sim.ordinal, sizeof(sim.ordinal))) sim_refresh_display_model(sim);
    ImGui::SameLine();
    if (ImGui::Checkbox("Top", &sim.top_inning)) sim_refresh_display_model(sim);

    ImGui::Text("Intermission:");
    ImGui::SameLine();
    static const char *intermissions[] = {"None", "Middle", "End"};
    if (ImGui::Combo("##intermission", &sim.intermission, intermissions, 3))
      sim_refresh_display_model(sim);

    ImGui::Text("Count:");
    ImGui::SameLine();
    if (ImGui::InputInt("##balls",   &sim.balls,   0, 9)) sim_refresh_display_model(sim);
    ImGui::SameLine();
    if (ImGui::InputInt("##strikes", &sim.strikes, 0, 9)) sim_refresh_display_model(sim);
    ImGui::SameLine();
    if (ImGui::InputInt("##outs",    &sim.outs,    0, 3)) sim_refresh_display_model(sim);

    ImGui::Text("Bases:");
    if (ImGui::Checkbox("1st", &sim.runner_first))  sim_refresh_display_model(sim);
    ImGui::SameLine();
    if (ImGui::Checkbox("2nd", &sim.runner_second)) sim_refresh_display_model(sim);
    ImGui::SameLine();
    if (ImGui::Checkbox("3rd", &sim.runner_third))  sim_refresh_display_model(sim);

    ImGui::Text("Pitcher:");
    ImGui::SameLine();
    if (ImGui::InputText("##pitcher", sim.pitcher, sizeof(sim.pitcher))) sim_refresh_display_model(sim);

    ImGui::Text("Batter:");
    ImGui::SameLine();
    if (ImGui::InputText("##batter", sim.batter, sizeof(sim.batter))) sim_refresh_display_model(sim);

    ImGui::Text("DetailedState:");
    ImGui::SameLine();
    if (ImGui::InputText("##detailstate", sim.detailed_state, sizeof(sim.detailed_state)))
      sim_refresh_display_model(sim);

    ImGui::Text("Start Time:");
    ImGui::SameLine();
    if (ImGui::InputText("##start_time", sim.start_time, sizeof(sim.start_time)))
      sim_refresh_display_model(sim);
  }

  // ---- MLB Stats API ----
  if (ImGui::CollapsingHeader("MLB Stats API", ImGuiTreeNodeFlags_None)) {
    ImGui::Text("Base URL:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##base_url", sim.base_url, sizeof(sim.base_url));
    ImGui::TextDisabled("Set to http://host:8000 for gamereplay server");

    ImGui::Separator();
    ImGui::Text("Team ID:");
    ImGui::SameLine();
    ImGui::InputInt("##team_id", &sim.team_id);

    ImGui::Checkbox("Poll Schedule (by team ID)", &sim.mlb_poll_schedule);
    ImGui::SliderInt("Schedule interval (ms)", &sim.mlb_schedule_poll_ms, 5000, 600000);
    if (ImGui::Button("Fetch schedule now", ImVec2(-1, 0)))
      sim.mlb_schedule_fetch_pending = true;

    ImGui::Separator();
    ImGui::Text("gamePk:");
    ImGui::SameLine();
    ImGui::InputInt("##game_pk", &sim.game_pk);

    static bool prev_mlb_poll = false;
    ImGui::Checkbox("Poll feed/live", &sim.mlb_poll_live);
    if (sim.mlb_poll_live && !prev_mlb_poll) {
      sim.live_feed = false;
      sim.live_running = false;
    }
    prev_mlb_poll = sim.mlb_poll_live;
    ImGui::SliderInt("Live poll interval (ms)", &sim.mlb_poll_ms, 1000, 60000);
    if (ImGui::Button("Fetch live now", ImVec2(-1, 0)))
      sim.mlb_fetch_pending = true;

    ImGui::Separator();
    ImGui::TextWrapped("%s", sim.mlb_status[0] ? sim.mlb_status : "(no status yet)");
  }

  // ---- JSON Injection ----
  if (ImGui::CollapsingHeader("JSON Injection", ImGuiTreeNodeFlags_None)) {
    static char json_buf[262144] = {};
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.1f, 1));
    ImGui::InputTextMultiline("##json", json_buf, sizeof(json_buf),
                              ImVec2(-1, -80), ImGuiInputTextFlags_None);
    ImGui::PopStyleColor();
    if (ImGui::Button("Apply JSON", ImVec2(-1, 0))) {
      std::string err;
      if (sim_apply_statsapi_json(sim, std::string_view(json_buf), &err))
        snprintf(sim.mlb_status, sizeof(sim.mlb_status), "Applied pasted JSON");
      else
        snprintf(sim.mlb_status, sizeof(sim.mlb_status), "JSON error: %s", err.c_str());
    }
    if (ImGui::Button("Generate sample JSON from state", ImVec2(-1, 0))) {
      std::string sample = build_mlb_json(
          sim.game_pk,
          strcmp(sim.phase, "Live") == 0 ? "Live" :
          strcmp(sim.phase, "Final") == 0 ? "Final" : "Preview",
          sim.away, sim.away_score,
          sim.home, sim.home_score,
          std::max(1, sim.inning > 0 ? sim.inning : 1), sim.ordinal, sim.top_inning,
          sim.balls, sim.strikes, sim.outs,
          sim.runner_first, sim.runner_second, sim.runner_third,
          sim.start_time);
      snprintf(json_buf, sizeof(json_buf), "%s", sample.c_str());
    }
  }

  // ---- Synthetic Live Feed ----
  if (ImGui::CollapsingHeader("Synthetic Live Feed", ImGuiTreeNodeFlags_None)) {
    ImGui::BeginDisabled(sim.mlb_poll_live);
    ImGui::Checkbox("Auto-advance live feed", &sim.live_feed);
    ImGui::EndDisabled();
    if (sim.mlb_poll_live)
      ImGui::TextDisabled("Disabled while MLB poll is active.");

    static bool prev_lf = false;
    bool rising = sim.live_feed && !prev_lf;
    if (rising) {
      sim.live_running = true;
      sim.live_tick = 0;
      sim.live_inning = std::max(1, sim.inning > 0 ? sim.inning : 1);
      sim.live_top    = sim.top_inning;
      sim.live_balls   = sim.balls;
      sim.live_strikes = sim.strikes;
      sim.live_outs    = sim.outs;
      sim.live_away_score = sim.away_score;
      sim.live_home_score = sim.home_score;
      sim.live_r1 = sim.runner_first;
      sim.live_r2 = sim.runner_second;
      sim.live_r3 = sim.runner_third;
      copy_synthetic_live_into_editor_and_refresh(sim);
    }
    prev_lf = sim.live_feed;
    if (!sim.live_feed) sim.live_running = false;

    ImGui::SliderInt("Frames per advance", &sim.live_speed, 1, 24);
    ImGui::TextDisabled("Higher = slower");

    char live_buf[64];
    snprintf(live_buf, sizeof(live_buf), "%s %d  %s  %d-%d  O%d",
             sim.live_top ? "Top" : "Bot", sim.live_inning, sim.live_ordinal,
             sim.live_balls, sim.live_strikes, sim.live_outs);
    ImGui::TextColored(ImVec4(1, 1, 0, 1), "Live: %s", live_buf);
    ImGui::Text("Score: %s %d - %d %s", sim.away, sim.live_away_score, sim.live_home_score, sim.home);

    if (ImGui::Button("Advance one step", ImVec2(-1, 0))) {
      advance_live_feed(sim);
      copy_synthetic_live_into_editor_and_refresh(sim);
    }
    if (ImGui::Button("Sync live -> display", ImVec2(-1, 0)))
      copy_synthetic_live_into_editor_and_refresh(sim);
  }

  // ---- Display Settings ----
  if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_None)) {
    ImGui::SliderInt("Pixel scale", &sim.scale, 1, 16);
    ImGui::Checkbox("Show Grid", &sim.show_grid);
    ImGui::Checkbox("Show FPS",  &sim.show_fps);
    ImGui::TextDisabled("Canvas: %d x %d px", sim.scale * kDisplayW, sim.scale * kDisplayH);
  }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  srand((unsigned)time(nullptr));

  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    fprintf(stderr, "SDL2 init failed: %s\n", SDL_GetError());
    return 1;
  }
  if (TTF_Init() < 0) {
    fprintf(stderr, "SDL_ttf init failed: %s\n", TTF_GetError());
    return 1;
  }

  const int initial_canvas_w = kDisplayW * kDefaultWindowPixelScale;
  const int initial_canvas_h = kDisplayH * kDefaultWindowPixelScale;
  const int panel_w = 320;
  int window_w = initial_canvas_w + panel_w + 16;
  int window_h = std::max(initial_canvas_h + 16, 400);

  SDL_Window *window = SDL_CreateWindow(
    "Baseball Tracker Simulator",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    window_w, window_h,
    SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
  );
  if (!window) {
    fprintf(stderr, "SDL window creation failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_RaiseWindow(window);

  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    fprintf(stderr, "SDL renderer creation failed: %s\n", SDL_GetError());
    return 1;
  }

  // Font search order: project fonts/, then deps imgui fonts, then system fallback
  const char *font_paths[] = {
    "fonts/Pixolletta8px.ttf",
    "../fonts/Pixolletta8px.ttf",
    "../../fonts/Pixolletta8px.ttf",
    "simulate/fonts/Pixolletta8px.ttf",
    "../deps/imgui/misc/fonts/Roboto-Medium.ttf",
    "deps/imgui/misc/fonts/Roboto-Medium.ttf",
    "../../deps/imgui/misc/fonts/Roboto-Medium.ttf",
    nullptr
  };
  TTF_Font *font = nullptr;
  for (int i = 0; font_paths[i]; i++) {
    font = TTF_OpenFont(font_paths[i], 10);
    if (font) break;
  }
  if (!font) {
    fprintf(stderr, "Warning: Pixolletta8px not found; trying system DejaVu\n");
    font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 10);
  }
  if (!font) {
    fprintf(stderr, "Error: no usable font found.\n");
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  if (!ImGui_ImplSDLRenderer2_Init(renderer)) {
    fprintf(stderr, "ImGui SDL_Renderer backend init failed\n");
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  {
    ImGuiIO &io = ImGui::GetIO();
    const char *imgui_font_paths[] = {
        "fonts/Pixolletta8px.ttf",
        "../fonts/Pixolletta8px.ttf",
        "simulate/fonts/Pixolletta8px.ttf",
        "../deps/imgui/misc/fonts/Roboto-Medium.ttf",
        "deps/imgui/misc/fonts/Roboto-Medium.ttf",
        "../../deps/imgui/misc/fonts/Roboto-Medium.ttf",
        nullptr,
    };
    for (int i = 0; imgui_font_paths[i]; ++i) {
      FILE *probe = fopen(imgui_font_paths[i], "rb");
      if (!probe) continue;
      fclose(probe);
      ImFont *f = io.Fonts->AddFontFromFileTTF(imgui_font_paths[i], 14.0f);
      if (f) break;
    }
  }

  sim_http_startup();

  SimState sim;
  sim_refresh_display_model(sim);

  bool running = true;
  int frame_count = 0;
  double fps = 0, fps_timer = 0;

  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type == SDL_QUIT) running = false;
    }

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window, &win_w, &win_h);
    if (win_w <= 0) win_w = window_w;
    if (win_h <= 0) win_h = window_h;

    if (sim.mlb_poll_live) {
      sim.live_running = false;
    }

    Uint32 ticks = SDL_GetTicks();
    pump_mlb_network(sim, ticks);

    if (!sim.mlb_poll_live && sim.live_running) {
      advance_live_feed(sim);
      copy_synthetic_live_into_editor_and_refresh(sim);
    }

    sim_refresh_display_model(sim);

    SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
    SDL_RenderClear(renderer);

    // Render 128×32 canvas at sim.scale pixels per logical pixel
    int canvas_w = sim.scale * kDisplayW;
    int canvas_h = sim.scale * kDisplayH;

    SDL_Surface *buf = make_display_surface(kDisplayW, kDisplayH);
    if (buf) {
      SDL_FillRect(buf, nullptr, SDL_MapRGBA(buf->format, 0, 0, 0, 255));

      switch (sim.state.phase) {
        case GamePhase::NONE:    draw_no_game(buf, font); break;
        case GamePhase::PREVIEW: draw_pregame(buf, font, sim.state); break;
        case GamePhase::LIVE:    draw_live(buf, font, sim.state); break;
        case GamePhase::FINAL:   draw_final(buf, font, sim.state); break;
      }

      // Optional pixel grid overlay
      if (sim.show_grid && sim.scale >= 4) {
        SDL_Color grid_color = {40, 40, 40, 255};
        for (int gx = 0; gx < kDisplayW; gx++)
          for (int gy = 0; gy < kDisplayH; gy++) {
            // Mark grid by darkening the 1-pixel border of each logical pixel
            // (just mark right/bottom edge by drawing a dim line on the surface)
            // For simplicity: draw grid on the scaled texture instead
            (void)gx; (void)gy; (void)grid_color;
            break;
          }
      }

      SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, buf);
      SDL_FreeSurface(buf);
      if (texture) {
        SDL_Rect dst = {0, 0, canvas_w, canvas_h};
        SDL_RenderCopy(renderer, texture, nullptr, &dst);

        // Pixel grid: draw lines on the SDL renderer at scaled positions
        if (sim.show_grid && sim.scale >= 4) {
          SDL_SetRenderDrawColor(renderer, 40, 40, 40, 200);
          for (int gx = 0; gx <= kDisplayW; gx++)
            SDL_RenderDrawLine(renderer, gx * sim.scale, 0, gx * sim.scale, canvas_h);
          for (int gy = 0; gy <= kDisplayH; gy++)
            SDL_RenderDrawLine(renderer, 0, gy * sim.scale, canvas_w, gy * sim.scale);
        }

        SDL_DestroyTexture(texture);
      }
    }

    // ImGui panel — positioned right of the canvas with a small gap
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    frame_count++;
    fps_timer += 1.0 / 60.0;
    if (fps_timer >= 1.0) {
      fps = (double)frame_count / fps_timer;
      frame_count = 0;
      fps_timer = 0;
    }

    float panel_x = (float)(canvas_w + 8);
    float panel_h = (float)win_h - 20.0f;
    if (panel_h < 200) panel_h = 200;
    ImGui::SetNextWindowPos(ImVec2(panel_x, 10), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(310, panel_h), ImGuiCond_Always);
    ImGui::Begin("Control Panel", nullptr,
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_HorizontalScrollbar);

    if (sim.show_fps) {
      ImGui::TextColored(ImVec4(0, 1, 0, 1), "FPS: %.1f", fps);
      ImGui::Separator();
    }

    render_control_panel(sim);

    ImGui::End();
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
  }

  if (font) TTF_CloseFont(font);
  TTF_Quit();
  sim_http_shutdown();
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
