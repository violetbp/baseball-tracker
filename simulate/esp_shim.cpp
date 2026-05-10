#include "esp_shim.h"

namespace esphome {

// ---------------------------------------------------------------------------
// MLB API stub
// ---------------------------------------------------------------------------

static std::string g_mlb_response;
static int g_http_code = 200;
static bool g_http_consumed = false;

void set_mlb_json_response(const std::string &json) {
  g_mlb_response = json;
  g_http_code = 200;
  g_http_consumed = false;
}

// ---------------------------------------------------------------------------
// HTTP stub
// ---------------------------------------------------------------------------

namespace HTTPClient {

void begin(WiFi::WiFiClientSecure&, const char*, int, const char*, bool) {
  g_http_consumed = false;
}
void setTimeout(int) {}
void addHeader(const char*, const char*) {}
void end() { g_http_consumed = true; }

void set_response(int code, const std::string &body) {
  g_http_code = code;
  g_mlb_response = body;
  g_http_consumed = false;
}

int GET() {
  // Return non-200 so fetch_game_data_() returns early without resetting state
  // The simulator controls state via ImGui panel, not HTTP
  return 0;
}
std::string getString() {
  if (g_http_consumed) return "";
  g_http_consumed = true;
  return g_mlb_response;
}

} // namespace HTTPClient

// ---------------------------------------------------------------------------
// Display implementation backed by SDL2 + SDL_ttf
// ---------------------------------------------------------------------------

namespace display {

Display::Display() : renderer_(nullptr), scale_(1),
                     buf_(nullptr), scaled_(nullptr) {}

Display::~Display() {
  if (buf_) SDL_FreeSurface(buf_);
  if (scaled_) SDL_FreeSurface(scaled_);
}

void Display::set_renderer(SDL_Renderer *renderer, TTF_Font *font, int scale) {
  (void)font;
  renderer_ = renderer;
  scale_ = scale;

  if (buf_) SDL_FreeSurface(buf_);
  buf_ = SDL_CreateRGBSurface(0, buf_w_, buf_h_, 24, 0, 0, 0, 0);

  if (scaled_) SDL_FreeSurface(scaled_);
  scaled_ = SDL_CreateRGBSurface(0, buf_w_ * scale, buf_h_ * scale, 24, 0, 0, 0, 0);
}

void Display::clear() {
  if (!buf_) return;
  SDL_FillRect(buf_, nullptr, SDL_MapRGB(buf_->format, 0, 0, 0));
}

void Display::print(int x, int y, font::Font *font, Color color, const char *text) {
  if (!buf_ || !text || !*text) return;

  if (font && font->handle) {
    SDL_Color sdl_color = {color.r, color.g, color.b, 255};
    SDL_Surface *text_surf = TTF_RenderText_Blended(font->handle, text, sdl_color);
    if (text_surf) {
      SDL_Rect dst = {x, y, 0, 0};
      SDL_BlitSurface(text_surf, nullptr, buf_, &dst);
      SDL_FreeSurface(text_surf);
    }
  }
}

void Display::get_text_bounds(int x, int y, const char *text, font::Font *font,
                              TextAlign align, int *x_off, int *y_off, int *text_w, int *text_h) {
  (void)x; (void)y; (void)align; (void)x_off; (void)y_off;

  if (!text || !*text) {
    *text_w = 0; *text_h = 0;
    return;
  }

  if (font && font->handle) {
    TTF_SizeText(font->handle, text, text_w, text_h);
  } else {
    *text_w = static_cast<int>(strlen(text)) * 6;
    *text_h = 10;
  }
}

void Display::line(int x0, int y0, int x1, int y1, Color color) {
  if (!buf_) return;
  SDL_LockSurface(buf_);
  uint32_t *pixels = static_cast<uint32_t *>(buf_->pixels);
  int w = buf_->w, h = buf_->h;

  int dx = abs(x1 - x0), dy = abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx - dy;

  while (true) {
    if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h)
      pixels[y0 * w + x0] = (uint32_t)color.r << 16 | (uint32_t)color.g << 8 | color.b;
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 > -dy) { err -= dy; x0 += sx; }
    if (e2 < dx) { err += dx; y0 += sy; }
  }
  SDL_UnlockSurface(buf_);
}

void Display::filled_rectangle(int x, int y, int w, int h, Color color) {
  if (!buf_) return;
  SDL_LockSurface(buf_);
  uint32_t *pixels = static_cast<uint32_t *>(buf_->pixels);
  int buf_w = buf_->w;

  for (int dy = 0; dy < h; dy++) {
    for (int dx = 0; dx < w; dx++) {
      int px = x + dx, py = y + dy;
      if (px >= 0 && px < buf_w && py >= 0 && py < buf_->h)
        pixels[py * buf_w + px] = (uint32_t)color.r << 16 | (uint32_t)color.g << 8 | color.b;
    }
  }
  SDL_UnlockSurface(buf_);
}

void Display::rectangle(int x, int y, int w, int h, Color color) {
  if (!buf_) return;
  line(x, y, x + w - 1, y, color);
  line(x, y + h - 1, x + w - 1, y + h - 1, color);
  line(x, y, x, y + h - 1, color);
  line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void Display::filled_circle(int x, int y, int r, Color color) {
  if (!buf_) return;
  SDL_LockSurface(buf_);
  uint32_t *pixels = static_cast<uint32_t *>(buf_->pixels);
  int buf_w = buf_->w, buf_h = buf_->h;
  auto set_px = [&](int px, int py) {
    if (px >= 0 && px < buf_w && py >= 0 && py < buf_h)
      pixels[py * buf_w + px] = (uint32_t)color.r << 16 | (uint32_t)color.g << 8 | color.b;
  };

  int d = 3 - 2 * r;
  int dx = 0, dy = 2 * r;
  set_px(x, y + r);
  set_px(x, y - r);
  set_px(x + r, y);
  set_px(x - r, y);

  while (dx < dy) {
    if (d < 0) {
      d = d + 4 * dx + 6;
    } else {
      dy -= 2;
      d = d + 4 * (dx - dy) + 10;
    }
    dx++;
    for (int i = x - dx + 1; i <= x + dx; i++) {
      set_px(i, y + dy / 2);
      set_px(i, y - dy / 2);
    }
    if (dx != dy) {
      for (int i = x - dy + 1; i <= x + dy; i++) {
        set_px(i, y + dx);
        set_px(i, y - dx);
      }
    }
  }
  SDL_UnlockSurface(buf_);
}

void Display::circle(int x, int y, int r, Color color) {
  if (!buf_) return;
  SDL_LockSurface(buf_);
  uint32_t *pixels = static_cast<uint32_t *>(buf_->pixels);
  int buf_w = buf_->w, buf_h = buf_->h;
  auto set_px = [&](int px, int py) {
    if (px >= 0 && px < buf_w && py >= 0 && py < buf_h)
      pixels[py * buf_w + px] = (uint32_t)color.r << 16 | (uint32_t)color.g << 8 | color.b;
  };

  int d = 3 - 2 * r;
  int dx = 0, dy = 2 * r;
  set_px(x, y + r);
  set_px(x, y - r);
  set_px(x + r, y);
  set_px(x - r, y);

  while (dx < dy) {
    if (d < 0) {
      d = d + 4 * dx + 6;
    } else {
      dy -= 2;
      d = d + 4 * (dx - dy) + 10;
    }
    dx++;
    set_px(x + dx, y + dy / 2);
    set_px(x - dx, y + dy / 2);
    set_px(x + dx, y - dy / 2);
    set_px(x - dx, y - dy / 2);
    if (dx != dy) {
      set_px(x + dy, y + dx);
      set_px(x - dy, y + dx);
      set_px(x + dy, y - dx);
      set_px(x - dy, y - dx);
    }
  }
  SDL_UnlockSurface(buf_);
}

void Display::present() {
  if (!renderer_ || !buf_) return;

  SDL_BlitScaled(buf_, nullptr, scaled_, nullptr);

  SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, scaled_);
  if (!texture) return;

  SDL_Rect dst = {0, 0, scaled_->w, scaled_->h};
  SDL_RenderClear(renderer_);
  SDL_RenderCopy(renderer_, texture, nullptr, &dst);
  SDL_RenderPresent(renderer_);
  SDL_DestroyTexture(texture);
}

} // namespace display
} // namespace esphome
