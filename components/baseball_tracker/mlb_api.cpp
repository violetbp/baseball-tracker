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

static std::string last_name_from_full_name(const char *full) {
  if (full == nullptr || *full == '\0')
    return "";
  const char *sp = strrchr(full, ' ');
  return sp != nullptr ? std::string(sp + 1) : std::string(full);
}

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

static esp_err_t http_event_cb_(esp_http_client_event_t *evt) {
  std::string *out = (std::string *)evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    out->append((char *)evt->data, evt->data_len);
  }
  return ESP_OK;
}

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
  GameState prev = state_;

  return json::parse_json(json_body, [this, &prev](JsonObject root) -> bool {
    state_ = GameState{};

    int total_games = root["totalGames"] | 0;
    if (total_games == 0) {
      state_.phase = GamePhase::NONE;
      final_at_utc_ = 0;
      if (prev.phase != GamePhase::NONE) {
        ESP_LOGI(TAG, "No game scheduled today");
      }
      return true;
    }

    JsonArray dates = root["dates"];
    if (dates.isNull() || dates.size() == 0) {
      state_.phase = GamePhase::NONE;
      final_at_utc_ = 0;
      ESP_LOGW(TAG, "totalGames=%d but dates array is empty", total_games);
      return true;
    }

    JsonObject game = pick_schedule_game(dates);
    if (game.isNull()) {
      state_.phase = GamePhase::NONE;
      final_at_utc_ = 0;
      ESP_LOGW(TAG, "No selectable game in schedule response");
      return true;
    }

    state_.game_pk = game["gamePk"] | 0;

    {
      const char *gd = game["gameDate"] | "";
      state_.start_time_str = gd;
      if (gd[0] == '\0' || !parse_iso8601_utc(gd, &state_.game_start_utc) || state_.game_start_utc <= 0) {
        state_.has_game_start = false;
      } else {
        state_.has_game_start = true;
      }
    }

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

    if (prev.phase != state_.phase) {
      ESP_LOGI(TAG, "Game phase changed: %d → %d (%s) [gamePk=%d]",
               (int)prev.phase, (int)state_.phase, detailed_state, state_.game_pk);
    }

    JsonObject teams = game["teams"];
    state_.away_abbrev = teams["away"]["team"]["abbreviation"] | "???";
    state_.home_abbrev = teams["home"]["team"]["abbreviation"] | "???";
    state_.away_score  = teams["away"]["score"] | 0;
    state_.home_score  = teams["home"]["score"] | 0;

    if (state_.away_score != prev.away_score || state_.home_score != prev.home_score) {
      ESP_LOGI(TAG, "Score update: %s %d, %s %d",
               state_.away_abbrev.c_str(), state_.away_score,
               state_.home_abbrev.c_str(), state_.home_score);
    }

    if (state_.phase == GamePhase::PREVIEW) {
      ESP_LOGI(TAG, "Game preview: %s @ %s, start=%s",
               state_.away_abbrev.c_str(), state_.home_abbrev.c_str(), state_.start_time_str.c_str());
    }

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

    if (prev.game_pk != state_.game_pk) {
      final_at_utc_ = 0;
    }
    if (state_.phase == GamePhase::NONE || state_.phase == GamePhase::PREVIEW) {
      final_at_utc_ = 0;
    }
    if (prev.phase == GamePhase::LIVE && state_.phase == GamePhase::FINAL && rtc_ != nullptr) {
      time_t now_ts = rtc_->utcnow().timestamp;
      if (now_ts > 0) {
        final_at_utc_ = now_ts;
      }
    }

    return true;
  });
}

bool BaseballTracker::fetch_live_feed_(int game_pk) {
  if (game_pk <= 0) {
    return false;
  }

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

    {
      JsonObject status = game_data["status"];
      const char *abstract_state = status["abstractGameState"] | "";
      const char *detailed_state = status["detailedState"]     | "";
      if (detailed_state[0] != '\0') {
        state_.detailed_state = detailed_state;
      }
      if (strcmp(abstract_state, "Final") == 0) {
        const bool became_final = (state_.phase != GamePhase::FINAL);
        if (became_final) {
          ESP_LOGI(TAG, "feed/live reports Final (gamePk=%d)", state_.game_pk);
          state_.phase = GamePhase::FINAL;
        }
        if (prev.phase == GamePhase::LIVE && rtc_ != nullptr) {
          time_t now_ts = rtc_->utcnow().timestamp;
          if (now_ts > 0) {
            final_at_utc_ = now_ts;
          }
        }
      }
    }

    JsonObject teams = game_data["teams"];
    if (!teams.isNull()) {
      const char *aw = teams["away"]["abbreviation"] | "";
      const char *hm = teams["home"]["abbreviation"] | "";
      if (aw[0] != '\0') state_.away_abbrev = aw;
      if (hm[0] != '\0') state_.home_abbrev = hm;
    }

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

    JsonObject ls_teams = ls["teams"];
    if (!ls_teams.isNull()) {
      state_.away_score = ls_teams["away"]["runs"] | state_.away_score;
      state_.home_score = ls_teams["home"]["runs"] | state_.home_score;
    }

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

}  // namespace baseball_tracker
}  // namespace esphome
