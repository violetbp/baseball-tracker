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
#include "esphome/core/application.h"
#include "esphome/components/json/json_util.h"

namespace esphome {
namespace baseball_tracker {

static const char *const TAG = "baseball_tracker";

void BaseballTracker::setup() {
  using_real_api_ = (base_url_.find("statsapi.mlb.com") != std::string::npos);
  ESP_LOGCONFIG(TAG, "Setting up Baseball Tracker (team_id=%d, real_api=%s)",
                team_id_, using_real_api_ ? "yes" : "no (custom server)");
}

void BaseballTracker::loop() {
  uint32_t now = millis();

  if (!esphome::network::is_connected()) {
    last_schedule_poll_ms_ = 0;
    last_live_poll_ms_ = 0;
    wifi_connected_at_ms_ = 0;
    return;
  }

  if (wifi_connected_at_ms_ == 0) {
    wifi_connected_at_ms_ = now;
  }

  static constexpr uint32_t kNetworkHoldoffMs = 2000;
  if ((now - wifi_connected_at_ms_) < kNetworkHoldoffMs) {
    return;
  }

  if (!esphome::network::is_connected()) {
    last_schedule_poll_ms_ = 0;
    last_live_poll_ms_ = 0;
    return;
  }

  static constexpr uint32_t kScheduleSlowMs = 5 * 60 * 1000;
  uint32_t schedule_interval = first_poll_done_
      ? kScheduleSlowMs
      : poll_interval_ms_;
  bool need_schedule = (last_schedule_poll_ms_ == 0)
      || ((now - last_schedule_poll_ms_) >= schedule_interval);

  if (need_schedule) {
    ESP_LOGD(TAG, "Polling schedule (interval=%u ms, phase=%d, first_ok=%s)",
             schedule_interval, (int)pending_state_.phase, first_poll_done_ ? "yes" : "no");
    bool ok = fetch_schedule_data_();
    last_schedule_poll_ms_ = now;
    if (ok) {
      first_poll_done_ = true;
      pending_updated_at_ms_ = now;
    }
  }

  uint32_t live_interval = (using_real_api_ && poll_interval_ms_ < kRealApiMinPollMs)
      ? kRealApiMinPollMs
      : poll_interval_ms_;

  bool need_live = first_poll_done_
      && pending_state_.phase == GamePhase::LIVE
      && pending_state_.game_pk > 0
      && (last_live_poll_ms_ == 0
          || (now - last_live_poll_ms_) >= live_interval);

  if (need_live) {
    ESP_LOGD(TAG, "Polling feed/live (interval=%u ms%s, gamePk=%d)",
             live_interval, using_real_api_ ? " [real API]" : "", pending_state_.game_pk);
    bool ok = fetch_live_feed_(pending_state_.game_pk);
    last_live_poll_ms_ = now;
    if (ok) {
      pending_updated_at_ms_ = now;
    }
  }

  // Promote pending_state_ to state_ (what draw_game renders) after the configured delay.
  if (pending_updated_at_ms_ > 0) {
    if (display_delay_ms_ == 0 || (now - pending_updated_at_ms_) >= display_delay_ms_) {
      state_ = pending_state_;
      pending_updated_at_ms_ = 0;
    }
  }

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
  ESP_LOGCONFIG(TAG, "  Post-final auto-show: %u s", auto_page_post_final_sec_);
  ESP_LOGCONFIG(TAG, "  Linked baseball page switch: %s", baseball_page_switch_ == nullptr ? "no" : "yes");
  ESP_LOGCONFIG(TAG, "  Game in progress binary sensor: %s", game_in_progress_sensor_ == nullptr ? "no" : "yes");
}

void BaseballTracker::set_base_url_and_refresh(const std::string &url) {
  if (url == base_url_) return;
  ESP_LOGI(TAG, "Server changed → %s", url.c_str());
  base_url_ = url;
  using_real_api_ = (url.find("statsapi.mlb.com") != std::string::npos);
  first_poll_done_ = false;
  last_schedule_poll_ms_ = 0;
  last_live_poll_ms_ = 0;
}

void BaseballTracker::set_team_id_and_refresh(int team_id) {
  if (team_id < 0) {
    return;
  }
  if (team_id_ == team_id) {
    return;
  }

  ESP_LOGI(TAG, "Team changed: %d → %d (refresh now)", team_id_, team_id);
  team_id_ = team_id;
  last_mlb_status_log_ms_ = 0;
  final_at_utc_ = 0;

  first_poll_done_ = false;
  last_schedule_poll_ms_ = 0;
  last_live_poll_ms_ = 0;
  bool ok = fetch_schedule_data_();
  uint32_t now = millis();
  last_schedule_poll_ms_ = now;
  if (ok) {
    first_poll_done_ = true;
    if (pending_state_.phase == GamePhase::LIVE && pending_state_.game_pk > 0) {
      App.feed_wdt();
      fetch_live_feed_(pending_state_.game_pk);
      last_live_poll_ms_ = now;
    }
    // Explicit team change: show the new state immediately regardless of delay.
    state_ = pending_state_;
    pending_updated_at_ms_ = 0;
  }
}

bool BaseballTracker::should_auto_show_baseball_() const {
  if (pending_state_.phase == GamePhase::NONE) {
    return false;
  }
  if (pending_state_.phase == GamePhase::FINAL) {
    if (final_at_utc_ == 0 || rtc_ == nullptr || auto_page_post_final_sec_ == 0) {
      return false;
    }
    time_t now_ts = rtc_->utcnow().timestamp;
    if (now_ts < 1) {
      return false;
    }
    return now_ts < final_at_utc_ + static_cast<time_t>(auto_page_post_final_sec_);
  }
  if (pending_state_.phase == GamePhase::LIVE) {
    return true;
  }
  if (pending_state_.phase == GamePhase::PREVIEW) {
    if (rtc_ == nullptr || !pending_state_.has_game_start) {
      return false;
    }
    time_t now_ts = rtc_->utcnow().timestamp;
    if (now_ts < 1) {
      return false;
    }
    time_t t0 = pending_state_.game_start_utc - static_cast<time_t>(auto_page_lead_sec_);
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
    if (pending_state_.phase == GamePhase::FINAL) {
      ESP_LOGI(TAG, "Auto page: show baseball (post-final score for %u s)", auto_page_post_final_sec_);
    } else {
      ESP_LOGI(TAG, "Auto page: show baseball (T−%us window / live)", auto_page_lead_sec_);
    }
  } else {
    baseball_page_switch_->turn_off();
    ESP_LOGI(TAG, "Auto page: return to transit");
  }
  last_auto_show_cmd_ = want;
}

void BaseballTracker::update_game_in_progress_sensor_() {
  if (game_in_progress_sensor_ == nullptr) {
    return;
  }
  bool live = (pending_state_.phase == GamePhase::LIVE);
  if (in_progress_sensor_published_ && live == last_published_in_progress_) {
    return;
  }
  game_in_progress_sensor_->publish_state(live);
  last_published_in_progress_ = live;
  in_progress_sensor_published_ = true;
}

static const char *const kMlbApiLabel = "MLB Stats API";
static const char *const kMlbApiUrl   = "https://statsapi.mlb.com";

void ServerSelect::setup() {
  this->traits.set_options({kMlbApiLabel, dev_url_.c_str()});

  if (restore_value_) {
    pref_ = global_preferences->make_preference<uint8_t>(this->get_object_id_hash());
    pref_ready_ = true;
    uint8_t saved = 0;
    if (pref_.load(&saved) && tracker_ != nullptr) {
      bool use_dev = (saved == 1);
      tracker_->set_base_url(use_dev ? dev_url_ : kMlbApiUrl);
      this->publish_state(use_dev ? dev_url_ : kMlbApiLabel);
      return;
    }
  }

  // Default: reflect whichever URL the YAML configured
  bool on_dev = tracker_ != nullptr && tracker_->get_base_url() == dev_url_;
  this->publish_state(on_dev ? dev_url_ : kMlbApiLabel);
}

void ServerSelect::control(const std::string &value) {
  this->publish_state(value);
  if (tracker_ == nullptr) return;

  bool use_dev = (value == dev_url_);
  if (restore_value_ && pref_ready_) {
    uint8_t idx = use_dev ? 1 : 0;
    pref_.save(&idx);
  }
  tracker_->set_base_url_and_refresh(use_dev ? dev_url_ : kMlbApiUrl);
}

static const TeamSelect::TeamOpt kMlbTeams[] = {
    {"Any", 0},
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
      "Any",
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
    if (pref_.load(&saved_team_id) && saved_team_id >= 0 && tracker_ != nullptr) {
      tracker_->set_team_id(saved_team_id);
      if (auto *found = find_by_id_(saved_team_id)) {
        this->publish_state(found->name);
        return;
      }
    }
  }

  if (tracker_ != nullptr) {
    if (auto *found = find_by_id_(tracker_->get_team_id())) {
      this->publish_state(found->name);
      return;
    }
  }
  this->publish_state(kMlbTeams[0].name);
}

void TeamSelect::control(const std::string &value) {
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

void PollDelayNumber::setup() {
  auto traits = number::NumberTraits();
  traits.set_min_value(0.0f);
  traits.set_max_value(60000.0f);
  traits.set_step(100.0f);
  this->traits = traits;

  float initial = 0.0f;
  if (restore_value_) {
    pref_ = global_preferences->make_preference<uint32_t>(this->get_object_id_hash());
    pref_ready_ = true;
    uint32_t saved = 0;
    if (pref_.load(&saved)) {
      initial = static_cast<float>(saved);
      if (tracker_ != nullptr) {
        tracker_->set_display_delay_ms(saved);
      }
    }
  } else if (tracker_ != nullptr) {
    initial = static_cast<float>(tracker_->get_display_delay_ms());
  }
  this->publish_state(initial);
}

void PollDelayNumber::control(float value) {
  this->publish_state(value);
  uint32_t ms = static_cast<uint32_t>(value);
  if (restore_value_ && pref_ready_) {
    pref_.save(&ms);
  }
  if (tracker_ != nullptr) {
    tracker_->set_display_delay_ms(ms);
  }
}

}  // namespace baseball_tracker
}  // namespace esphome
