#pragma once

#include <string>
#include <string_view>

struct SimState;

/// Call once before any HTTPS. No-op if built without libcurl.
void sim_http_startup();

/// Call once at process exit.
void sim_http_shutdown();

bool sim_http_get(const std::string &url, std::string &body, std::string &err);

/// Fetch /api/v1.1/game/{game_pk}/feed/live from base_url.
bool sim_fetch_mlb_live_feed(const std::string &base_url, int game_pk,
                              std::string &body, std::string &err);

/// Fetch /api/v1/schedule?sportId=1&teamId={team_id}&hydrate=linescore,team from base_url.
bool sim_fetch_mlb_schedule(const std::string &base_url, int team_id,
                             std::string &body, std::string &err);

/// Accept schedule envelope or feed/live JSON; update SimState.
bool sim_apply_statsapi_json(SimState &sim, std::string_view json_utf8, std::string *err_out);
