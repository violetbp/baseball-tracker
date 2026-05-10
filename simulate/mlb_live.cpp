#include "mlb_live.h"
#include "sim_state.h"

#include <nlohmann/json.hpp>

#ifndef SIM_WITHOUT_CURL
#include <curl/curl.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <unordered_map>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#ifndef SIM_WITHOUT_CURL

namespace {

bool g_curl_global_ready = false;
CURL *g_easy = nullptr;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *out = static_cast<std::string *>(userdata);
  const size_t add = size * nmemb;
  if (add == 0) return 0;
  out->append(ptr, add);
  return add;
}

}  // namespace

void sim_http_startup() {
  if (g_curl_global_ready) return;
  CURLcode g = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (g != CURLE_OK) return;
  g_easy = curl_easy_init();
  if (!g_easy) {
    curl_global_cleanup();
    return;
  }
  g_curl_global_ready = true;
}

void sim_http_shutdown() {
  if (g_easy) {
    curl_easy_cleanup(g_easy);
    g_easy = nullptr;
  }
  if (g_curl_global_ready) {
    curl_global_cleanup();
    g_curl_global_ready = false;
  }
}

bool sim_http_get(const std::string &url, std::string &body, std::string &err) {
  sim_http_startup();
  if (!g_easy) {
    err = "libcurl failed to initialize";
    return false;
  }

  body.clear();
  body.reserve(std::min<size_t>(512 * 1024, 8 * 1024 * 1024));

  curl_easy_reset(g_easy);
  curl_easy_setopt(g_easy, CURLOPT_URL, url.c_str());
  curl_easy_setopt(g_easy, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(g_easy, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(g_easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(g_easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(g_easy, CURLOPT_TIMEOUT, 25L);
  curl_easy_setopt(g_easy, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(g_easy, CURLOPT_USERAGENT, "baseball-tracker-sim/1.0");
#if defined(CURLSSLOPT_NATIVE_CA)
  curl_easy_setopt(g_easy, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
  curl_easy_setopt(g_easy, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)(16 * 1024 * 1024));

  CURLcode res = curl_easy_perform(g_easy);
  if (res != CURLE_OK) {
    err = curl_easy_strerror(res);
    return false;
  }

  long code = 0;
  curl_easy_getinfo(g_easy, CURLINFO_RESPONSE_CODE, &code);
  if (code < 200 || code >= 300) {
    err = "HTTP " + std::to_string(code);
    return false;
  }
  return true;
}

#else  // SIM_WITHOUT_CURL

void sim_http_startup() {}
void sim_http_shutdown() {}

bool sim_http_get(const std::string &, std::string &, std::string &err) {
  err = "curl not available (use nix develop)";
  return false;
}

#endif  // SIM_WITHOUT_CURL

bool sim_fetch_mlb_live_feed(const std::string &base_url, int game_pk,
                              std::string &body, std::string &err) {
  if (game_pk <= 0) {
    err = "game_pk must be > 0";
    return false;
  }
  static const char kFields[] =
      "metaData,timeStamp,gamePk,"
      "gameData,status,abstractGameState,detailedState,statusCode,"
      "datetime,dateTime,"
      "teams,away,home,abbreviation,id,"
      "players,useLastName,"
      "liveData,linescore,currentInning,currentInningOrdinal,isTopInning,"
      "inningHalf,inningState,balls,strikes,outs,runs,teams,"
      "offense,defense,first,second,third,batter,pitcher";

  // Trim trailing slash from base_url
  std::string base = base_url;
  while (!base.empty() && base.back() == '/') base.pop_back();

  char path[768];
  int nw = snprintf(path, sizeof(path),
                    "%s/api/v1.1/game/%d/feed/live?fields=%s",
                    base.c_str(), game_pk, kFields);
  if (nw < 0 || (size_t)nw >= sizeof(path)) {
    err = "URL overflow";
    return false;
  }
  return sim_http_get(path, body, err);
}

bool sim_fetch_mlb_schedule(const std::string &base_url, int team_id,
                             std::string &body, std::string &err) {
  if (team_id <= 0) {
    err = "team_id must be > 0";
    return false;
  }

  std::string base = base_url;
  while (!base.empty() && base.back() == '/') base.pop_back();

  char path[256];
  int nw = snprintf(path, sizeof(path),
                    "%s/api/v1/schedule?sportId=1&teamId=%d&hydrate=linescore,team",
                    base.c_str(), team_id);
  if (nw < 0 || (size_t)nw >= sizeof(path)) {
    err = "URL overflow";
    return false;
  }
  return sim_http_get(path, body, err);
}

// ---------------------------------------------------------------------------
// JSON parsing
// ---------------------------------------------------------------------------

using json = nlohmann::json;

static void copy_abbrev_safe(char (&dst)[8], const json &abbr) {
  if (!abbr.is_string()) return;
  std::string s = abbr.get<std::string>();
  if (s.size() > sizeof(dst) - 1) s = s.substr(0, sizeof(dst) - 1);
  std::memcpy(dst, s.c_str(), s.size());
  dst[s.size()] = '\0';
}

static void copy_team_abbr(const json &side, char (&dst)[8]) {
  if (!side.is_object()) return;
  auto team_it = side.find("team");
  if (team_it != side.end() && team_it->is_object()) {
    if (auto ab = team_it->find("abbreviation"); ab != team_it->end())
      copy_abbrev_safe(dst, *ab);
    return;
  }
  if (auto ab = side.find("abbreviation"); ab != side.end())
    copy_abbrev_safe(dst, *ab);
}

static std::string last_name_from_full(const std::string &full) {
  auto sp = full.rfind(' ');
  return sp != std::string::npos ? full.substr(sp + 1) : full;
}

static void sync_linescore_fields(SimState &sim, const json &ls) {
  if (!ls.is_object()) return;

  sim.inning = ls.value("currentInning", sim.inning);

  if (auto ord = ls.find("currentInningOrdinal"); ord != ls.end() && ord->is_string()) {
    std::string o = ord->get<std::string>();
    snprintf(sim.ordinal, sizeof(sim.ordinal), "%s", o.c_str());
    snprintf(sim.live_ordinal, sizeof(sim.live_ordinal), "%s", sim.ordinal);
    sim.live_inning = std::max(1, ls.value("currentInning", sim.live_inning));
  }

  if (auto top = ls.find("isTopInning"); top != ls.end() && top->is_boolean()) {
    sim.top_inning = top->get<bool>();
    sim.live_top = sim.top_inning;
  }

  sim.balls   = ls.value("balls",   sim.balls);
  sim.strikes = ls.value("strikes", sim.strikes);
  sim.outs    = ls.value("outs",    sim.outs);
  sim.live_balls   = sim.balls;
  sim.live_strikes = sim.strikes;
  sim.live_outs    = sim.outs;

  // Intermission detection
  std::string inning_state_s;
  if (auto is_it = ls.find("inningState"); is_it != ls.end() && is_it->is_string())
    inning_state_s = is_it->get<std::string>();
  const char *inning_state = inning_state_s.c_str();

  if (strcasecmp(inning_state, "Middle") == 0) {
    sim.intermission = 1;
    sim.outs = sim.live_outs = 0;
    sim.runner_first = sim.runner_second = sim.runner_third = false;
    sim.live_r1 = sim.live_r2 = sim.live_r3 = false;
  } else if (strcasecmp(inning_state, "End") == 0) {
    sim.intermission = 2;
    sim.outs = sim.live_outs = 0;
    sim.runner_first = sim.runner_second = sim.runner_third = false;
    sim.live_r1 = sim.live_r2 = sim.live_r3 = false;
  } else {
    sim.intermission = 0;
  }

  // Base runners from offense object
  if (auto off = ls.find("offense"); off != ls.end() && off->is_object()) {
    sim.runner_first  = off->find("first")  != off->end() && !(*off)["first"].is_null();
    sim.runner_second = off->find("second") != off->end() && !(*off)["second"].is_null();
    sim.runner_third  = off->find("third")  != off->end() && !(*off)["third"].is_null();
    sim.live_r1 = sim.runner_first;
    sim.live_r2 = sim.runner_second;
    sim.live_r3 = sim.runner_third;

    // Batter name (schedule format: fullName in offense.batter)
    if (auto batter_it = off->find("batter"); batter_it != off->end() && batter_it->is_object()) {
      std::string name;
      if (auto ln = batter_it->find("useLastName"); ln != batter_it->end() && ln->is_string())
        name = ln->get<std::string>();
      else if (auto fn = batter_it->find("fullName"); fn != batter_it->end() && fn->is_string())
        name = last_name_from_full(fn->get<std::string>());
      if (!name.empty())
        snprintf(sim.batter, sizeof(sim.batter), "%s", name.c_str());
    }
  } else {
    sim.runner_first = sim.runner_second = sim.runner_third = false;
    sim.live_r1 = sim.live_r2 = sim.live_r3 = false;
  }

  // Pitcher name (schedule format: fullName in defense.pitcher)
  if (auto def = ls.find("defense"); def != ls.end() && def->is_object()) {
    if (auto pitch = def->find("pitcher"); pitch != def->end() && pitch->is_object()) {
      std::string name;
      if (auto ln = pitch->find("useLastName"); ln != pitch->end() && ln->is_string())
        name = ln->get<std::string>();
      else if (auto fn = pitch->find("fullName"); fn != pitch->end() && fn->is_string())
        name = last_name_from_full(fn->get<std::string>());
      if (!name.empty())
        snprintf(sim.pitcher, sizeof(sim.pitcher), "%s", name.c_str());
    }
  }

  // Scores from linescore teams (live feed has runs here)
  if (auto lt = ls.find("teams"); lt != ls.end() && lt->is_object()) {
    if (auto a = lt->find("away"); a != lt->end() && a->is_object()) {
      sim.away_score = a->value("runs", sim.away_score);
      sim.live_away_score = sim.away_score;
    }
    if (auto h = lt->find("home"); h != lt->end() && h->is_object()) {
      sim.home_score = h->value("runs", sim.home_score);
      sim.live_home_score = sim.home_score;
    }
  }

  if (sim.intermission != 0) {
    sim.batter[0] = '\0';
  }
}

static void apply_game_object(SimState &sim, const json &game) {
  if (!game.is_object()) return;

  int pk = game.value("gamePk", 0);
  if (pk > 0) sim.game_pk = pk;

  std::string gd = game.value("gameDate", "");
  if (!gd.empty())
    snprintf(sim.start_time, sizeof(sim.start_time), "%s", gd.c_str());

  std::string abs = "Preview";
  if (auto st = game.find("status"); st != game.end() && st->is_object())
    abs = st->value("abstractGameState", "Preview");

  if (abs == "Live")        snprintf(sim.phase, sizeof(sim.phase), "Live");
  else if (abs == "Final")  snprintf(sim.phase, sizeof(sim.phase), "Final");
  else                      snprintf(sim.phase, sizeof(sim.phase), "Preview");

  // detailed_state
  if (auto st = game.find("status"); st != game.end() && st->is_object()) {
    std::string det = st->value("detailedState", "");
    snprintf(sim.detailed_state, sizeof(sim.detailed_state), "%s", det.c_str());
  }

  if (auto tm = game.find("teams"); tm != game.end() && tm->is_object()) {
    if (auto aw = tm->find("away"); aw != tm->end()) copy_team_abbr(*aw, sim.away);
    if (auto hm = tm->find("home"); hm != tm->end()) copy_team_abbr(*hm, sim.home);
    if (auto aw = tm->find("away"); aw != tm->end() && aw->is_object())
      sim.away_score = aw->value("score", sim.away_score);
    if (auto hm = tm->find("home"); hm != tm->end() && hm->is_object())
      sim.home_score = hm->value("score", sim.home_score);
    sim.live_away_score = sim.away_score;
    sim.live_home_score = sim.home_score;
  }

  sim.pitcher[0] = '\0';
  sim.batter[0] = '\0';

  if (auto ls_it = game.find("linescore"); ls_it != game.end() && !ls_it->is_null())
    sync_linescore_fields(sim, *ls_it);

  sim_refresh_display_model(sim);
}

static std::optional<json> pick_schedule_game(const json &dates) {
  if (!dates.is_array() || dates.empty()) return std::nullopt;

  std::optional<json> any_live;
  std::optional<json> best_preview;
  std::string preview_start;
  bool have_preview = false;
  std::optional<json> best_final;
  std::string final_start;
  int final_pk = 0;
  bool have_final = false;

  for (const auto &date_var : dates) {
    if (!date_var.is_object()) continue;
    auto gm_it = date_var.find("games");
    if (gm_it == date_var.end() || !gm_it->is_array()) continue;
    for (const auto &g : *gm_it) {
      if (!g.is_object()) continue;
      std::string abs;
      if (auto st = g.find("status"); st != g.end() && st->is_object())
        abs = st->value("abstractGameState", "");
      if (abs == "Live") {
        if (!any_live.has_value()) any_live = g;
        continue;
      }
      if (abs == "Final") {
        std::string gdv = g.value("gameDate", "");
        int pk = g.value("gamePk", 0);
        if (!have_final || gdv > final_start || (gdv == final_start && pk > final_pk)) {
          best_final = g; final_start = gdv; final_pk = pk; have_final = true;
        }
        continue;
      }
      {
        std::string gdv = g.value("gameDate", "");
        if (!have_preview) {
          best_preview = g; preview_start = gdv; have_preview = true;
        } else if (!gdv.empty() && (preview_start.empty() || gdv < preview_start)) {
          best_preview = g; preview_start = gdv;
        }
      }
    }
  }

  if (any_live.has_value()) return any_live;
  if (have_preview) return best_preview;
  if (have_final) return best_final;

  // Fallback: first game in first date
  const json &d0 = dates.at(0);
  if (!d0.is_object()) return std::nullopt;
  auto g0 = d0.find("games");
  if (g0 != d0.end() && g0->is_array() && !g0->empty()) return g0->at(0);
  return std::nullopt;
}

static bool try_apply_schedule_root(SimState &sim, const json &root, std::string *err_out) {
  int totalGames = root.value("totalGames", -1);
  if (totalGames == 0) {
    snprintf(sim.phase, sizeof(sim.phase), "None");
    sim.away_score = sim.home_score = 0;
    snprintf(sim.mlb_status, sizeof(sim.mlb_status), "schedule: no game today");
    sim_refresh_display_model(sim);
    return true;
  }

  auto date_it = root.find("dates");
  if (date_it == root.end() || !date_it->is_array()) {
    if (err_out) *err_out = "schedule: missing dates[]";
    return false;
  }
  auto sel = pick_schedule_game(*date_it);
  if (!sel.has_value()) {
    if (err_out) *err_out = "schedule: no selectable game";
    return false;
  }
  apply_game_object(sim, sel.value());
  snprintf(sim.mlb_status, sizeof(sim.mlb_status), "schedule OK — %s @ %s  gamePk=%d",
           sim.away, sim.home, sim.game_pk);
  return true;
}

static bool try_apply_live_feed_root(SimState &sim, const json &root, std::string *err_out) {
  auto gd_it = root.find("gameData");
  auto ld_it = root.find("liveData");
  if (gd_it == root.end() || ld_it == root.end() ||
      !gd_it->is_object() || !ld_it->is_object()) {
    if (err_out) *err_out = "feed/live: need gameData + liveData";
    return false;
  }
  const json &gd = *gd_it;
  const json &ld = *ld_it;

  int pk = root.value("gamePk", 0);
  if (pk > 0) sim.game_pk = pk;

  // Phase
  {
    std::string abs = "Preview";
    if (auto st = gd.find("status"); st != gd.end() && st->is_object()) {
      abs = st->value("abstractGameState", "Preview");
      std::string det = st->value("detailedState", "");
      if (!det.empty())
        snprintf(sim.detailed_state, sizeof(sim.detailed_state), "%s", det.c_str());
    }
    if (abs == "Live")        snprintf(sim.phase, sizeof(sim.phase), "Live");
    else if (abs == "Final")  snprintf(sim.phase, sizeof(sim.phase), "Final");
    else                      snprintf(sim.phase, sizeof(sim.phase), "Preview");
  }

  // Team abbreviations
  if (auto tm = gd.find("teams"); tm != gd.end() && tm->is_object()) {
    if (auto aw = tm->find("away"); aw != tm->end()) copy_team_abbr(*aw, sim.away);
    if (auto hm = tm->find("home"); hm != tm->end()) copy_team_abbr(*hm, sim.home);
  }

  // Build player id -> last name map from gameData.players
  std::unordered_map<int, std::string> player_last;
  if (auto players_it = gd.find("players"); players_it != gd.end() && players_it->is_object()) {
    for (auto &[key, pval] : players_it->items()) {
      if (!pval.is_object()) continue;
      int pid = pval.value("id", 0);
      std::string last = pval.value("useLastName", "");
      if (last.empty()) {
        std::string full = pval.value("fullName", "");
        if (!full.empty()) last = last_name_from_full(full);
      }
      if (pid > 0 && !last.empty())
        player_last.emplace(pid, last);
    }
  }

  auto ls_it = ld.find("linescore");
  if (ls_it == ld.end() || ls_it->is_null()) {
    sim_refresh_display_model(sim);
    if (err_out) *err_out = "feed/live: no linescore (partial)";
    return true;
  }

  const json &ls = *ls_it;
  sim.pitcher[0] = '\0';
  sim.batter[0] = '\0';
  sync_linescore_fields(sim, ls);

  // Resolve batter/pitcher IDs via player map (live feed format)
  auto resolve = [&](int id) -> std::string {
    if (id <= 0) return {};
    auto it = player_last.find(id);
    return it != player_last.end() ? it->second : std::string{};
  };

  int batter_id = 0, pitcher_id = 0;
  if (auto off = ls.find("offense"); off != ls.end() && off->is_object()) {
    if (auto b = off->find("batter"); b != off->end() && b->is_object())
      batter_id = b->value("id", 0);
  }
  if (auto def = ls.find("defense"); def != ls.end() && def->is_object()) {
    if (auto p = def->find("pitcher"); p != def->end() && p->is_object())
      pitcher_id = p->value("id", 0);
  }

  if (auto name = resolve(batter_id); !name.empty())
    snprintf(sim.batter, sizeof(sim.batter), "%s", name.c_str());
  if (auto name = resolve(pitcher_id); !name.empty())
    snprintf(sim.pitcher, sizeof(sim.pitcher), "%s", name.c_str());

  if (sim.intermission != 0) sim.batter[0] = '\0';

  sim_refresh_display_model(sim);
  snprintf(sim.mlb_status, sizeof(sim.mlb_status), "feed/live OK (%zu bytes)", (size_t)0);
  return true;
}

bool sim_apply_statsapi_json(SimState &sim, std::string_view json_utf8, std::string *err_out) {
  if (json_utf8.empty()) {
    if (err_out) *err_out = "empty JSON";
    return false;
  }
  try {
    json root = json::parse(json_utf8);
    if (root.contains("liveData") && root.contains("gameData"))
      return try_apply_live_feed_root(sim, root, err_out);
    if (root.contains("dates"))
      return try_apply_schedule_root(sim, root, err_out);
    if (err_out) *err_out = "Unrecognized JSON (need schedule dates[] or feed/live envelope)";
    return false;
  } catch (const std::exception &e) {
    if (err_out) *err_out = e.what();
    return false;
  }
}
