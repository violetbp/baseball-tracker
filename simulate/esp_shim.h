#pragma once

// ESPHome shim layer for simulator.
// Provides ESPHome types/macros so baseball_tracker.cpp compiles without changes.
// Rendering is backed by SDL2; font metrics by SDL_ttf.

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <ctime>
#include <vector>
#include <map>
#include <algorithm>

// ---------------------------------------------------------------------------
// Logging macros (ESPHome -> stdout)
// ---------------------------------------------------------------------------

#define ESP_LOGI(tag, ...) do { fprintf(stderr, "[I] %s: ", tag); fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); } while(0)
#define ESP_LOGD(tag, ...) /* silent in sim */
#define ESP_LOGW(tag, ...) do { fprintf(stderr, "[W] %s: ", tag); fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); } while(0)
#define ESP_LOGV(tag, ...) /* silent in sim */
#define ESP_LOGCONFIG(tag, ...) do { fprintf(stderr, "[C] %s: ", tag); fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); } while(0)

// ---------------------------------------------------------------------------
// ESP32 hardware stubs
// ---------------------------------------------------------------------------

static inline uint32_t millis() {
  static auto start = SDL_GetPerformanceCounter();
  static double freq = (double)SDL_GetPerformanceFrequency();
  return (uint32_t)((double)(SDL_GetPerformanceCounter() - start) / freq * 1000.0);
}

// ---------------------------------------------------------------------------
// MLB API stub
// ---------------------------------------------------------------------------

namespace esphome {

void set_mlb_json_response(const std::string &json);

// ---------------------------------------------------------------------------
// Color
// ---------------------------------------------------------------------------

struct Color {
  uint8_t r, g, b;
  Color() : r(0), g(0), b(0) {}
  Color(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}
};

// ---------------------------------------------------------------------------
// setup_priority stub
// ---------------------------------------------------------------------------

namespace setup_priority {
  constexpr float AFTER_WIFI = 400.0f;
}

// ---------------------------------------------------------------------------
// Component base class (for override methods)
// ---------------------------------------------------------------------------

class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return setup_priority::AFTER_WIFI; }
};

// ---------------------------------------------------------------------------
// font namespace (esphome::font)
// ---------------------------------------------------------------------------

namespace font {
  class Font {
   public:
    TTF_Font *handle = nullptr;
  };
}

// ---------------------------------------------------------------------------
// time namespace (esphome::time)
// ---------------------------------------------------------------------------

namespace time {
  class RealTimeClock {
   public:
    struct Time {
      time_t timestamp;
    };
    Time utcnow() {
      Time t{};
      t.timestamp = std::time(nullptr);
      return t;
    }
  };
}

// ---------------------------------------------------------------------------
// switch_ namespace (esphome::switch_)
// ---------------------------------------------------------------------------

namespace switch_ {
  class Switch {
   public:
    void turn_on() {}
    void turn_off() {}
  };
}

// ---------------------------------------------------------------------------
// binary_sensor namespace (esphome::binary_sensor)
// ---------------------------------------------------------------------------

namespace binary_sensor {
  class BinarySensor {
   public:
    void publish_state(bool state) { (void)state; }
  };
}

// ---------------------------------------------------------------------------
// display namespace + Display class
// ---------------------------------------------------------------------------

namespace display {

enum TextAlign { TOP_LEFT = 0 };

class Display {
 public:
  Display();
  ~Display();

  void set_renderer(SDL_Renderer *renderer, TTF_Font *font, int scale);

  void clear();

  void print(int x, int y, font::Font *font, Color color, const char *text);
  void get_text_bounds(int x, int y, const char *text, font::Font *font,
                       TextAlign align, int *x_off, int *y_off, int *text_w, int *text_h);
  void line(int x0, int y0, int x1, int y1, Color color);
  void rectangle(int x, int y, int w, int h, Color color);
  void filled_rectangle(int x, int y, int w, int h, Color color);
  void circle(int x, int y, int r, Color color);
  void filled_circle(int x, int y, int r, Color color);

  void present();

  int get_width() const { return buf_w_; }
  int get_height() const { return buf_h_; }

 private:
  SDL_Renderer *renderer_{nullptr};
  int scale_{1};
  int buf_w_{128};
  int buf_h_{32};
  SDL_Surface *buf_{nullptr};
  SDL_Surface *scaled_{nullptr};
};

} // namespace display

// ---------------------------------------------------------------------------
// json namespace (esphome::json)
// ---------------------------------------------------------------------------

namespace json {

  class JsonVariant {
    std::string val_;
    bool is_null_{true};
    int int_val_{0};
    bool bool_val_{false};
    std::vector<JsonVariant> arr_;
    std::map<std::string, JsonVariant> obj_;
  public:
    JsonVariant() : is_null_(true) {}
    JsonVariant(const char *s) : val_(s), is_null_(false) {}
    JsonVariant(int i) : int_val_(i), is_null_(false) {}
    JsonVariant(bool b) : bool_val_(b), is_null_(false) {}
    JsonVariant(const std::string &s) : val_(s), is_null_(false) {}

  bool isNull() const { return is_null_; }
    size_t size() const { return arr_.size(); }
    const char* c_str() const { return val_.empty() && !is_null_ ? "" : val_.c_str(); }
    operator std::string() const { return val_; }
    operator int() const {
      if (is_null_) return 0;
      if (!val_.empty()) { try { return std::stoi(val_); } catch (...) { return 0; } }
      return int_val_;
    }
    JsonVariant operator[](size_t idx) const {
      JsonVariant v;
      if (idx < arr_.size()) { v = arr_[idx]; }
      return v;
    }
    JsonVariant operator|(const char *) const { return *this; }
    JsonVariant operator|(int default_val) const {
      JsonVariant v; v.is_null_ = is_null_; v.int_val_ = is_null_ ? default_val : int_val_;
      v.val_ = is_null_ ? std::to_string(default_val) : val_;
      return v;
    }
    void set(const std::string &key, const JsonVariant &val) { obj_[key] = val; }
    void push_back(const JsonVariant &val) { arr_.push_back(val); }
  };

  using JsonObject = JsonVariant;
  using JsonArray = JsonVariant;

} // namespace json

// Make JsonObject and JsonArray available at esphome namespace level
using json::JsonObject;
using json::JsonArray;

// Stub parse_json: always succeeds, callback receives a root that supports the MLB API structure
inline bool parse_json(const std::string &json_str, std::function<bool(JsonObject)> callback) {
  (void)json_str;
  JsonObject root;
  return callback(root);
}

// ---------------------------------------------------------------------------
// WiFi / HTTP stubs
// ---------------------------------------------------------------------------

namespace WiFi {
  class WiFiClientSecure {
   public:
    void setInsecure() {}
  };
}

namespace network {
  class NetworkClientSecure {
   public:
    void setInsecure() {}
  };
}

namespace HTTPClient {

void set_response(int code, const std::string &body);

int GET();
std::string getString();
void begin(WiFi::WiFiClientSecure&, const char*, int, const char*, bool);
void setTimeout(int ms);
void addHeader(const char*, const char*);
void end();

} // namespace HTTPClient

} // namespace esphome
