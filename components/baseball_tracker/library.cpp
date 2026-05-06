#include "library.h"
#include "baseball_tracker.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace esphome {
namespace baseball_tracker {

time_t utc_time_from_tm(struct tm *tp) {
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
 
std::string to_upper(std::string s) {
  for (char &ch : s) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return s;
}

void to_upper_in_place(char *s) {
  if (s == nullptr) {
    return;
  }
  for (; *s; ++s) {
    *s = static_cast<char>(std::toupper(static_cast<unsigned char>(*s)));
  }
}

char *to_upper(const char *in, char *out, std::size_t out_size) {
  if (out == nullptr || out_size == 0) {
    return nullptr;
  }
  if (in == nullptr) {
    out[0] = '\0';
    return out;
  }

  std::size_t i = 0;
  for (; in[i] != '\0' && i + 1 < out_size; ++i) {
    out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(in[i])));
  }
  out[i] = '\0';
  return out;
}

// ---------------------------------------------------------------------------
// Helper: draw a row of filled/hollow dots
// filled can be higher than count
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

void BaseballTracker::draw_text_max_width_(int x, int y, int max_width_px, const char *text, Color color) {
  if (display_ == nullptr || font_ == nullptr)
    return;
  if (text == nullptr)
    return;

  char work[48];
  strncpy(work, text, sizeof(work) - 1);
  work[sizeof(work) - 1] = '\0';
  if (work[0] == '\0') {
    return;
  }
  for (;;) {
    int tw, th, xo, yo;
    display_->get_text_bounds(0, 0, work, font_, display::TextAlign::TOP_LEFT, &xo, &yo, &tw, &th);
    if (tw <= max_width_px) {
      display_->print(x, y, font_, color, work);
      return;
    }
    size_t len = strlen(work);
    if (len <= 1) {
      return;
    }
    work[len - 1] = '\0';
  }
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

}  // namespace baseball_tracker
}  // namespace esphome
