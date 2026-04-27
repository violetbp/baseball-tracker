#include "baseball_tracker.h"
#include "library.h"
#include <ctime>
#include <cstring>
#include <strings.h>
#include <unordered_map>
#include <lwip/sockets.h>
#include "esp_http_client.h"
#include "esphome/components/network/util.h"

#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"

namespace esphome {
namespace baseball_tracker {

static const char *const TAG = "baseball_tracker";

// Last name from MLB "fullName" (e.g. "J.P. Crawford" -> "Crawford")
static std::string last_name_from_full_name(const char *full) {
  if (full == nullptr || *full == '\0')
    return "";
  const char *sp = strrchr(full, ' ');
  return sp != nullptr ? std::string(sp + 1) : std::string(full);
}

// ---------------------------------------------------------------------------
// MLB Stats API endpoints.
//
// 1. Schedule (discovery): tells us if today has a game, the gamePk, first
//    pitch, abstract/detailed state. Cheap and works for any team_id.
//
// 2. Per-game live feed: richer/fresher data while a game is LIVE. Requires a
//    known gamePk. We pull only the leaves we need via `fields=` so the
//    response stays small enough to parse on-device. In particular we keep
//    `players,useLastName` so we can resolve batter/pitcher names from the
//    integer id returned in `liveData.linescore.{offense.batter,defense.pitcher}`.
// ---------------------------------------------------------------------------
static const char *const MLB_SCHEDULE_PATH =
    "/api/v1/schedule?sportId=1&teamId=%d&hydrate=linescore,team";

static const char *const MLB_LIVE_FEED_PATH =
    "/api/v1.1/game/%d/feed/live?fields="
    "metaData,timeStamp,gamePk,"
    "gameData,status,abstractGameState,detailedState,statusCode,"
    "datetime,dateTime,"
    "teams,away,home,abbreviation,id,"
    "players,useLastName,"
    "liveData,linescore,currentInning,currentInningOrdinal,isTopInning,"
    "inningHalf,inningState,balls,strikes,outs,runs,"
    "offense,defense,first,second,third,batter,pitcher";

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void BaseballTracker::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Baseball Tracker (team_id=%d)", team_id_);
  // First fetch runs from loop() so we only mark first_poll_done_ after a successful response.
}
void BaseballTracker::loop() {
  uint32_t now = millis();

  if (!esphome::network::is_connected()) {
    last_schedule_poll_ms_ = 0;
    last_live_poll_ms_ = 0;
    wifi_connected_at_ms_ = 0;  // reset holdoff
    return;
  }

  // Record when we first saw the network come up
  if (wifi_connected_at_ms_ == 0) {
    wifi_connected_at_ms_ = now;
  }

  // Wait 2 seconds after connection before making any HTTP requests.
  // The TCP/IP stack needs a moment to fully initialize after association.
  static constexpr uint32_t kNetworkHoldoffMs = 2000;
  if ((now - wifi_connected_at_ms_) < kNetworkHoldoffMs) {
    return;
  }


  // Guard: don't attempt HTTP if network isn't up
  if (!esphome::network::is_connected()) {
    last_schedule_poll_ms_ = 0;
    last_live_poll_ms_ = 0;
    return;
  }

  // ---- Schedule (discovery) ----
  static constexpr uint32_t kScheduleSlowMs = 5 * 60 * 1000;
  uint32_t schedule_interval = first_poll_done_
      ? kScheduleSlowMs
      : poll_interval_ms_;
  bool need_schedule = (last_schedule_poll_ms_ == 0)
      || ((now - last_schedule_poll_ms_) >= schedule_interval);

  if (need_schedule) {
    ESP_LOGD(TAG, "Polling schedule (interval=%u ms, phase=%d, first_ok=%s)",
             schedule_interval, (int)state_.phase, first_poll_done_ ? "yes" : "no");
    bool ok = fetch_schedule_data_();
    last_schedule_poll_ms_ = now;
    if (ok) {
      first_poll_done_ = true;
    }
  }

  // ---- feed/live (fast updates while LIVE) ----
  bool need_live = first_poll_done_
      && state_.phase == GamePhase::LIVE
      && state_.game_pk > 0
      && (last_live_poll_ms_ == 0
          || (now - last_live_poll_ms_) >= poll_interval_ms_);

  if (need_live) {
    ESP_LOGD(TAG, "Polling feed/live (interval=%u ms, gamePk=%d)",
             poll_interval_ms_, state_.game_pk);
    fetch_live_feed_(state_.game_pk);
    last_live_poll_ms_ = now;
  }

  // 1Hz: auto page + binary_sensor (cheap, no network)
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

void BaseballTracker::set_team_id_and_refresh(int team_id) {
  if (team_id <= 0) {
    return;
  }
  if (team_id_ == team_id) {
    return;
  }

  ESP_LOGI(TAG, "Team changed: %d → %d (refresh now)", team_id_, team_id);
  team_id_ = team_id;
  last_mlb_status_log_ms_ = 0;

  first_poll_done_ = false;
  last_schedule_poll_ms_ = 0;
  last_live_poll_ms_ = 0;
  bool ok = fetch_schedule_data_();
  uint32_t now = millis();
  last_schedule_poll_ms_ = now;
  if (ok) {
    first_poll_done_ = true;
    // If the new team is already LIVE, kick a feed/live fetch immediately so
    // the UI doesn't lag a full poll_interval after a switch.
    if (state_.phase == GamePhase::LIVE && state_.game_pk > 0) {
      fetch_live_feed_(state_.game_pk);
      last_live_poll_ms_ = now;
    }
  }
}

// ---------------------------------------------------------------------------
// HTTP fetch helpers
// ---------------------------------------------------------------------------

namespace {

static bool starts_with_(const std::string &s, const char *prefix) {
  size_t n = strlen(prefix);
  return s.size() >= n && s.compare(0, n, prefix) == 0;
}

static std::string join_url_(const std::string &base, const char *path) {
  if (base.empty()) {
    return std::string(path);
  }
  if (path == nullptr || *path == '\0') {
    return base;
  }
  bool base_slash = base.back() == '/';
  bool path_slash = *path == '/';
  if (base_slash && path_slash) {
    return base + (path + 1);
  }
  if (!base_slash && !path_slash) {
    return base + "/" + path;
  }
  return base + path;
}

// esp_http_client event callback for collecting response body
static esp_err_t http_event_cb_(esp_http_client_event_t *evt) {
  std::string *out = (std::string *)evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    out->append((char *)evt->data, evt->data_len);
  }
  return ESP_OK;
}

// Issue a GET to base_url + path and return the body in `out`.
// Returns true only on HTTP 200 with a non-empty body.
bool http_get_json_(const std::string &base_url, const char *path, std::string *out, const char *log_tag) {
  std::string url = join_url_(base_url, path);
  ESP_LOGD(log_tag, "GET %s", url.c_str());
  uint32_t t0 = millis();

  esp_http_client_config_t config{};
  config.url = url.c_str();
  config.cert_pem = nullptr;
  config.skip_cert_common_name_check = true;
  config.timeout_ms = 8000;
  config.method = HTTP_METHOD_GET;
  config.event_handler = http_event_cb_;
  config.user_data = out;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGW(log_tag, "Failed to init HTTP client");
    return false;
  }

  esp_http_client_set_header(client, "User-Agent", "ESPHome-BaseballTracker/1.0");

  esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    ESP_LOGW(log_tag, "HTTP perform failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }

  int status_code = esp_http_client_get_status_code(client);
  uint32_t elapsed = millis() - t0;

  if (status_code != 200) {
    ESP_LOGW(log_tag, "HTTP GET failed after %u ms: code=%d (url=%s)", elapsed, status_code, url.c_str());
    esp_http_client_cleanup(client);
    return false;
  }

  ESP_LOGD(log_tag, "HTTP 200 in %u ms, body=%u bytes", elapsed, (unsigned) out->size());
  esp_http_client_cleanup(client);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Schedule endpoint: discovery (gamePk, first pitch, abstract phase, …).
// ---------------------------------------------------------------------------

bool BaseballTracker::fetch_schedule_data_() {
  char path[128];
  snprintf(path, sizeof(path), MLB_SCHEDULE_PATH, team_id_);

  std::string body;
  if (!http_get_json_(base_url_, path, &body, TAG)) {
    return false;
  }

  if (!parse_schedule_response_(body)) {
    ESP_LOGW(TAG, "Failed to parse MLB schedule response (body_len=%u)", (unsigned) body.size());
    return false;
  }

  uint32_t now_ms = millis();
  if (state_.phase != GamePhase::LIVE || (now_ms - last_mlb_status_log_ms_) >= 30000) {
    ESP_LOGI(TAG, "MLB: gamePk=%d %s @ %s phase=%d (%s)", state_.game_pk, state_.away_abbrev.c_str(),
             state_.home_abbrev.c_str(), (int) state_.phase, state_.detailed_state.c_str());
    last_mlb_status_log_ms_ = now_ms;
  }
  return true;
}

// ---------------------------------------------------------------------------
// JSON parsing
// ---------------------------------------------------------------------------

// Prefer LIVE, else earliest-start PREVIEW, else latest FINAL (first games[] entry is often
// an earlier completed game when a doubleheader or split squad day exists).
static JsonObject pick_schedule_game(JsonArray dates) {
  if (dates.isNull() || dates.size() == 0) {
    return JsonObject();
  }

  JsonObject any_live;
  JsonObject best_preview;
  time_t preview_start = 0;
  bool have_preview = false;

  JsonObject best_final;
  time_t final_start = 0;
  int final_pk = 0;
  bool have_final = false;

  for (JsonVariant date_var : dates) {
    JsonObject date_obj = date_var.as<JsonObject>();
    JsonArray games = date_obj["games"];
    if (games.isNull()) {
      continue;
    }
    for (JsonVariant game_var : games) {
      JsonObject g = game_var.as<JsonObject>();
      if (g.isNull()) {
        continue;
      }

      const char *abs = g["status"]["abstractGameState"] | "";
      if (strcmp(abs, "Live") == 0) {
        if (any_live.isNull()) {
          any_live = g;
        }
        continue;
      }
      if (strcmp(abs, "Final") == 0) {
        const char *gd = g["gameDate"] | "";
        time_t t = 0;
        BaseballTracker::parse_iso8601_utc(gd, &t);
        int pk = g["gamePk"] | 0;
        if (!have_final || t > final_start || (t == final_start && pk > final_pk)) {
          best_final = g;
          final_start = t;
          final_pk = pk;
          have_final = true;
        }
        continue;
      }
      // Preview / other non-final (scheduled, warmup as preview, etc.)
      {
        const char *gd = g["gameDate"] | "";
        time_t t = 0;
        BaseballTracker::parse_iso8601_utc(gd, &t);
        if (!have_preview) {
          best_preview = g;
          preview_start = t;
          have_preview = true;
        } else if (t > 0 && (preview_start == 0 || t < preview_start)) {
          best_preview = g;
          preview_start = t;
        }
      }
    }
  }

  if (!any_live.isNull()) {
    return any_live;
  }
  if (have_preview) {
    return best_preview;
  }
  if (have_final) {
    return best_final;
  }

  JsonArray g0 = dates[0]["games"].as<JsonArray>();
  if (!g0.isNull() && g0.size() > 0) {
    return g0[0].as<JsonObject>();
  }
  return JsonObject();
}

bool BaseballTracker::parse_schedule_response_(const std::string &json_body) {
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

    JsonObject game = pick_schedule_game(dates);
    if (game.isNull()) {
      state_.phase = GamePhase::NONE;
      ESP_LOGW(TAG, "No selectable game in schedule response");
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
    state_.detailed_state = detailed_state;
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

      state_.pitcher_last.clear();
      state_.batter_last.clear();
      JsonObject defense = ls["defense"];
      if (!defense.isNull() && !defense["pitcher"].isNull()) {
        const char *pn = defense["pitcher"]["fullName"] | "";
        state_.pitcher_last = last_name_from_full_name(pn);
      }
      JsonObject offense = ls["offense"];
      if (!offense.isNull()) {
        state_.runner_first  = !offense["first"].isNull();
        state_.runner_second = !offense["second"].isNull();
        state_.runner_third  = !offense["third"].isNull();
        if (!offense["batter"].isNull()) {
          const char *bn = offense["batter"]["fullName"] | "";
          state_.batter_last = last_name_from_full_name(bn);
        }
      }

      const char *inning_state = ls["inningState"] | "";
      state_.inning_intermission = InningIntermissionKind::NONE;
      if (strcasecmp(inning_state, "Middle") == 0) {
        state_.inning_intermission = InningIntermissionKind::MIDDLE;
      } else if (strcasecmp(inning_state, "End") == 0) {
        state_.inning_intermission = InningIntermissionKind::END;
      }
      if (state_.inning_intermission != InningIntermissionKind::NONE) {
        state_.outs = 0;
        state_.batter_last.clear();
        state_.runner_first = state_.runner_second = state_.runner_third = false;
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

      // Log inning changes at INFO (after intermission handling)
      if (state_.inning != prev.inning || state_.is_top_inning != prev.is_top_inning ||
          state_.inning_intermission != prev.inning_intermission) {
        if (state_.inning_intermission == InningIntermissionKind::MIDDLE) {
          ESP_LOGI(TAG, "Inning: Mid %s", state_.inning_ordinal.c_str());
        } else if (state_.inning_intermission == InningIntermissionKind::END) {
          ESP_LOGI(TAG, "Inning: End %s", state_.inning_ordinal.c_str());
        } else {
          ESP_LOGI(TAG, "Inning: %s %s (%d outs)",
                   state_.is_top_inning ? "Top" : "Bottom",
                   state_.inning_ordinal.c_str(),
                   state_.outs);
        }
      }
    }

    return true;
  });
}

// ---------------------------------------------------------------------------
// feed/live endpoint: rich live state for a known gamePk.
// ---------------------------------------------------------------------------

bool BaseballTracker::fetch_live_feed_(int game_pk) {
  if (game_pk <= 0) {
    return false;
  }

  // Path is long because the fields= leaf-allowlist is baked in. ~480 chars.
  char path[512];
  snprintf(path, sizeof(path), MLB_LIVE_FEED_PATH, game_pk);

  std::string body;
  if (!http_get_json_(base_url_, path, &body, TAG)) {
    return false;
  }

  if (!parse_live_feed_response_(body)) {
    ESP_LOGW(TAG, "Failed to parse feed/live response (gamePk=%d, body_len=%u)", game_pk,
             (unsigned) body.size());
    return false;
  }
  return true;
}

bool BaseballTracker::parse_live_feed_response_(const std::string &json_body) {
  GameState prev = state_;

  return json::parse_json(json_body, [this, &prev](JsonObject root) -> bool {
    JsonObject game_data = root["gameData"];
    JsonObject live_data = root["liveData"];
    if (game_data.isNull() || live_data.isNull()) {
      ESP_LOGW(TAG, "feed/live: missing gameData or liveData");
      return false;
    }

    // ----- gameData.status -----
    {
      JsonObject status = game_data["status"];
      const char *abstract_state = status["abstractGameState"] | "";
      const char *detailed_state = status["detailedState"]     | "";
      if (detailed_state[0] != '\0') {
        state_.detailed_state = detailed_state;
      }
      // Honor a FINAL transition from feed/live so we stop fast-polling sooner
      // than the next 5-min schedule refresh.
      if (strcmp(abstract_state, "Final") == 0 && state_.phase != GamePhase::FINAL) {
        ESP_LOGI(TAG, "feed/live reports Final (gamePk=%d)", state_.game_pk);
        state_.phase = GamePhase::FINAL;
      }
    }

    // ----- gameData.teams.{away,home}.abbreviation -----
    JsonObject teams = game_data["teams"];
    if (!teams.isNull()) {
      const char *aw = teams["away"]["abbreviation"] | "";
      const char *hm = teams["home"]["abbreviation"] | "";
      if (aw[0] != '\0') state_.away_abbrev = aw;
      if (hm[0] != '\0') state_.home_abbrev = hm;
    }

    // ----- gameData.players: id -> useLastName lookup -----
    // Keys look like "ID643338"; we key the map by the integer id leaf so the
    // batter/pitcher resolution below doesn't depend on the prefix string.
    std::unordered_map<int, std::string> player_last;
    JsonObject players = game_data["players"];
    if (!players.isNull()) {
      for (JsonPair kv : players) {
        JsonObject p = kv.value().as<JsonObject>();
        if (p.isNull()) {
          continue;
        }
        int pid = p["id"] | 0;
        const char *last = p["useLastName"] | "";
        if (pid > 0 && last[0] != '\0') {
          player_last.emplace(pid, std::string(last));
        }
      }
      ESP_LOGV(TAG, "feed/live: built player map with %u entries", (unsigned) player_last.size());
    }

    auto resolve_last = [&player_last](int id) -> std::string {
      if (id <= 0) return std::string();
      auto it = player_last.find(id);
      return it == player_last.end() ? std::string() : it->second;
    };

    // ----- liveData.linescore -----
    JsonObject ls = live_data["linescore"];
    if (ls.isNull()) {
      ESP_LOGD(TAG, "feed/live: no linescore");
      return true;
    }

    state_.inning         = ls["currentInning"] | state_.inning;
    state_.inning_ordinal = ls["currentInningOrdinal"] | state_.inning_ordinal.c_str();
    state_.is_top_inning  = ls["isTopInning"] | state_.is_top_inning;
    state_.balls          = ls["balls"]   | 0;
    state_.strikes        = ls["strikes"] | 0;
    state_.outs           = ls["outs"]    | 0;

    // Score lives under linescore.teams in the live feed (vs game-level teams.score
    // in the schedule).
    JsonObject ls_teams = ls["teams"];
    if (!ls_teams.isNull()) {
      state_.away_score = ls_teams["away"]["runs"] | state_.away_score;
      state_.home_score = ls_teams["home"]["runs"] | state_.home_score;
    }

    // Runners on base + current batter id
    int batter_id = 0;
    JsonObject offense = ls["offense"];
    if (!offense.isNull()) {
      state_.runner_first  = !offense["first"].isNull();
      state_.runner_second = !offense["second"].isNull();
      state_.runner_third  = !offense["third"].isNull();
      batter_id = offense["batter"]["id"] | 0;
    } else {
      state_.runner_first = state_.runner_second = state_.runner_third = false;
    }

    int pitcher_id = 0;
    JsonObject defense = ls["defense"];
    if (!defense.isNull()) {
      pitcher_id = defense["pitcher"]["id"] | 0;
    }

    state_.batter_last  = resolve_last(batter_id);
    state_.pitcher_last = resolve_last(pitcher_id);

    // Inning state ("Middle" / "End" between half-innings)
    const char *inning_state = ls["inningState"] | "";
    state_.inning_intermission = InningIntermissionKind::NONE;
    if (strcasecmp(inning_state, "Middle") == 0) {
      state_.inning_intermission = InningIntermissionKind::MIDDLE;
    } else if (strcasecmp(inning_state, "End") == 0) {
      state_.inning_intermission = InningIntermissionKind::END;
    }
    if (state_.inning_intermission != InningIntermissionKind::NONE) {
      state_.outs = 0;
      state_.batter_last.clear();
      state_.runner_first = state_.runner_second = state_.runner_third = false;
    }

    if (state_.away_score != prev.away_score || state_.home_score != prev.home_score) {
      ESP_LOGI(TAG, "Score update (live): %s %d, %s %d", state_.away_abbrev.c_str(), state_.away_score,
               state_.home_abbrev.c_str(), state_.home_score);
    }

    if (state_.inning != prev.inning || state_.is_top_inning != prev.is_top_inning ||
        state_.inning_intermission != prev.inning_intermission) {
      if (state_.inning_intermission == InningIntermissionKind::MIDDLE) {
        ESP_LOGI(TAG, "Inning: Mid %s", state_.inning_ordinal.c_str());
      } else if (state_.inning_intermission == InningIntermissionKind::END) {
        ESP_LOGI(TAG, "Inning: End %s", state_.inning_ordinal.c_str());
      } else {
        ESP_LOGI(TAG, "Inning: %s %s (%d outs)", state_.is_top_inning ? "Top" : "Bottom",
                 state_.inning_ordinal.c_str(), state_.outs);
      }
    }

    ESP_LOGD(TAG, "feed/live: %s @ %s %d-%d  %s%s  B%d S%d O%d  bases:[%s%s%s]  P:%s AB:%s",
             state_.away_abbrev.c_str(), state_.home_abbrev.c_str(), state_.away_score, state_.home_score,
             state_.is_top_inning ? "T" : "B", state_.inning_ordinal.c_str(),
             state_.balls, state_.strikes, state_.outs,
             state_.runner_first  ? "1" : "-",
             state_.runner_second ? "2" : "-",
             state_.runner_third  ? "3" : "-",
             state_.pitcher_last.empty() ? "--" : state_.pitcher_last.c_str(),
             state_.batter_last.empty()  ? "--" : state_.batter_last.c_str());

    return true;
  });
}

// ---------------------------------------------------------------------------
// Public draw entry point
// ---------------------------------------------------------------------------
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

  // Row 2: first pitch in local 24h (device timezone from YAML `time:` / `timezone:`)
  char time_buf[16] = "TBD";
  if (state_.has_game_start && state_.game_start_utc > 0) {
    ESPTime local = ESPTime::from_epoch_local(state_.game_start_utc);
    if (local.is_valid()) {
      local.strftime(time_buf, sizeof(time_buf), "%H:%M");
    }
  }
  draw_centered_text_(0, kDisplayW, kRow2Y, time_buf, kYellow());

  // Row 3: status text when MLB sends something other than "In Progress"
  if (!state_.detailed_state.empty() && strcasecmp(state_.detailed_state.c_str(), "In Progress") != 0) {
    char detail_buf[24];
    snprintf(detail_buf, sizeof(detail_buf), "%s", state_.detailed_state.c_str());
    draw_centered_text_(0, kDisplayW, kPregameRow3Y, detail_buf, kDim());
  }
}

// ---------------------------------------------------------------------------
// Drawing: live game
// ---------------------------------------------------------------------------

void BaseballTracker::draw_live_() {
  auto *d = display_;

  // ---- Layout (3 lines) ----
  // Line 1: [away+score] ... [inning] ... [home+score]
  // Line 2: [P: pitcher's last, truncated] (left) + [B–S] right
  // Line 3: [AB: batter's last, truncated] (left) + [diamond] + [out dots] on the right

  // --- Line 1: away team + score (left) ---
  char away_buf[16];
  snprintf(away_buf, sizeof(away_buf), "%s  %d", state_.away_abbrev.c_str(), state_.away_score);
  d->print(2, kRow1Y, font_, kCyan(), away_buf);

  // --- Line 1: home team + score (right) ---
  char home_buf[16];
  snprintf(home_buf, sizeof(home_buf), "%d  %s", state_.home_score, state_.home_abbrev.c_str());
  draw_right_aligned_text_(126, kRow1Y, home_buf, kCyan());

  // --- Line 1: inning centered between the two scores ---
  if (state_.inning_intermission != InningIntermissionKind::NONE) {
    const char *prefix = state_.inning_intermission == InningIntermissionKind::MIDDLE ? "mid" : "end";
    char label_buf[20];
    if (!state_.inning_ordinal.empty()) {
      snprintf(label_buf, sizeof(label_buf), "%s %s", prefix, state_.inning_ordinal.c_str());
    } else {
      snprintf(label_buf, sizeof(label_buf), "%s %d", prefix, state_.inning);
    }
    draw_centered_text_(40, 88, kRow1Y, label_buf, kYellow());

    //also reset the count and runners when in intermission
    state_.balls = 0;
    state_.strikes = 0;
    state_.outs = 0;
    state_.runner_first = state_.runner_second = state_.runner_third = false;
    // state_.batter_last.clear();
    // state_.pitcher_last.clear();
  } else {
    // Show "▲ 3rd" or "▼ 2nd"; arrow next to the batting team
    char inn_buf[8];
    snprintf(inn_buf, sizeof(inn_buf), "%s %s",
            "",
             state_.inning_ordinal.c_str());
    if (state_.is_top_inning) {
      draw_centered_text_(38, 59, kRow1Y, "▲", kYellow());
      draw_centered_text_(54, 74, kRow1Y, inn_buf, kYellow());
    } else {
      draw_centered_text_(54, 74, kRow1Y, inn_buf, kYellow());
      draw_centered_text_(128 - 59, 128 - 38, kRow1Y, "▼", kYellow());
    }
  }

  // --- Line 2: pitcher last name (left) + balls-strikes (right) ---
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

  // --- Line 3 (top): current batter; bottom half still has diamond+outs below ---
  {
    const char *bl = state_.batter_last.empty() ? "--" : state_.batter_last.c_str();
    char b_line[28];
    snprintf(b_line, sizeof(b_line), "AB: %s", bl);
    draw_text_max_width_(2, kPregameRow3Y, kLiveBatterNameMaxW, b_line, kCyan());
  }

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

// ---------------------------------------------------------------------------
// HA team select
// ---------------------------------------------------------------------------

static const TeamSelect::TeamOpt kMlbTeams[] = {
    {"Arizona Diamondbacks", 109},
    {"Atlanta Braves", 144},
    {"Baltimore Orioles", 110},
    {"Boston Red Sox", 111},
    {"Chicago Cubs", 112},
    {"Chicago White Sox", 145},
    {"Cincinnati Reds", 113},
    {"Cleveland Guardians", 114},
    {"Colorado Rockies", 115},
    {"Detroit Tigers", 116},
    {"Houston Astros", 117},
    {"Kansas City Royals", 118},
    {"Los Angeles Angels", 108},
    {"Los Angeles Dodgers", 119},
    {"Miami Marlins", 146},
    {"Milwaukee Brewers", 158},
    {"Minnesota Twins", 142},
    {"New York Mets", 121},
    {"New York Yankees", 147},
    {"Oakland Athletics", 133},
    {"Philadelphia Phillies", 143},
    {"Pittsburgh Pirates", 134},
    {"San Diego Padres", 135},
    {"San Francisco Giants", 137},
    {"Seattle Mariners", 136},
    {"St. Louis Cardinals", 138},
    {"Tampa Bay Rays", 139},
    {"Texas Rangers", 140},
    {"Toronto Blue Jays", 141},
    {"Washington Nationals", 120},
};

const TeamSelect::TeamOpt *TeamSelect::find_by_name_(const std::string &name) {
  for (const auto &t : kMlbTeams) {
    if (name == t.name) {
      return &t;
    }
  }
  return nullptr;
}

const TeamSelect::TeamOpt *TeamSelect::find_by_id_(int team_id) {
  for (const auto &t : kMlbTeams) {
    if (team_id == t.team_id) {
      return &t;
    }
  }
  return nullptr;
}

void TeamSelect::setup() {
  this->traits.set_options({
      "Arizona Diamondbacks",
      "Atlanta Braves",
      "Baltimore Orioles",
      "Boston Red Sox",
      "Chicago Cubs",
      "Chicago White Sox",
      "Cincinnati Reds",
      "Cleveland Guardians",
      "Colorado Rockies",
      "Detroit Tigers",
      "Houston Astros",
      "Kansas City Royals",
      "Los Angeles Angels",
      "Los Angeles Dodgers",
      "Miami Marlins",
      "Milwaukee Brewers",
      "Minnesota Twins",
      "New York Mets",
      "New York Yankees",
      "Oakland Athletics",
      "Philadelphia Phillies",
      "Pittsburgh Pirates",
      "San Diego Padres",
      "San Francisco Giants",
      "Seattle Mariners",
      "St. Louis Cardinals",
      "Tampa Bay Rays",
      "Texas Rangers",
      "Toronto Blue Jays",
      "Washington Nationals",
  });

  if (restore_value_) {
    pref_ = global_preferences->make_preference<int>(this->get_object_id_hash());
    pref_ready_ = true;
    int saved_team_id = 0;
    if (pref_.load(&saved_team_id) && saved_team_id > 0 && tracker_ != nullptr) {
      tracker_->set_team_id(saved_team_id);
      if (auto *found = find_by_id_(saved_team_id)) {
        this->publish_state(found->name);
        return;
      }
    }
  }

  // Publish an initial state that matches the current configured team_id (best effort),
  // otherwise fall back to the first option.
  if (tracker_ != nullptr) {
    if (auto *found = find_by_id_(tracker_->get_team_id())) {
      this->publish_state(found->name);
      return;
    }
  }
  this->publish_state(kMlbTeams[0].name);
}

void TeamSelect::control(const std::string &value) {
  // Always publish the option HA picked.
  this->publish_state(value);

  if (tracker_ == nullptr) {
    return;
  }
  if (auto *found = find_by_name_(value)) {
    if (restore_value_ && pref_ready_) {
      int id = found->team_id;
      pref_.save(&id);
    }
    tracker_->set_team_id_and_refresh(found->team_id);
  }
}

}  // namespace baseball_tracker
}  // namespace esphome
