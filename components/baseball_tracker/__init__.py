import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, select, switch as sw
from esphome.components.display import Display
from esphome.components.font import Font
from esphome.components.time import RealTimeClock
from esphome.const import (
    CONF_ID,
    CONF_DISPLAY_ID,
    CONF_RESTORE_VALUE,
    CONF_TIME_ID,
    __version__ as ESPHOME_VERSION,
    Framework,
)

_MINIMUM_ESPHOME_VERSION = "2026.3.0"

DEPENDENCIES = ["network", "display", "font", "time", "binary_sensor", "switch", "select"]
AUTO_LOAD = ["json"]

baseball_tracker_ns = cg.esphome_ns.namespace("baseball_tracker")
BaseballTracker = baseball_tracker_ns.class_("BaseballTracker", cg.Component)
TeamSelect = baseball_tracker_ns.class_("TeamSelect", select.Select, cg.Component)

CONF_FONT_ID = "font_id"
CONF_TEAM_ID = "team_id"
CONF_POLL_INTERVAL = "poll_interval"
CONF_AUTO_BASEBALL_PAGE = "auto_baseball_page"
CONF_AUTO_PAGE_LEAD = "auto_page_lead"
CONF_AUTO_PAGE_POST_FINAL = "auto_page_post_final"
CONF_BASEBALL_PAGE_SWITCH = "baseball_page_switch"
CONF_GAME_IN_PROGRESS = "game_in_progress"
CONF_TEAM_SELECT = "team_select"
CONF_BASE_URL = "base_url"

_MARINERS_TEAM_ID = 136

_MLB_TEAM_OPTIONS = [
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
]


def validate_esphome_version(obj):
    if cv.Version.parse(ESPHOME_VERSION) < cv.Version.parse(_MINIMUM_ESPHOME_VERSION):
        raise cv.Invalid(
            "The baseball_tracker component requires ESPHome version "
            f"{_MINIMUM_ESPHOME_VERSION} or later."
        )
    return obj


CONFIG_SCHEMA = cv.All(
    validate_esphome_version,
    cv.only_with_framework(frameworks=Framework.ARDUINO),
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BaseballTracker),
            cv.GenerateID(CONF_DISPLAY_ID): cv.use_id(Display),
            cv.GenerateID(CONF_FONT_ID): cv.use_id(Font),
            cv.GenerateID(CONF_TIME_ID): cv.use_id(RealTimeClock),
            cv.Optional(CONF_TEAM_ID, default=_MARINERS_TEAM_ID): cv.positive_int,
            cv.Optional(CONF_POLL_INTERVAL, default="30s"): cv.positive_time_period_milliseconds,
            # Override the MLB API host for testing (e.g. http://10.0.3.29:8080).
            # Default is the real MLB Stats API.
            cv.Optional(CONF_BASE_URL, default="https://statsapi.mlb.com"): cv.url,
            cv.Optional(CONF_AUTO_BASEBALL_PAGE, default=False): cv.boolean,
            cv.Optional(CONF_AUTO_PAGE_LEAD, default="5min"): cv.positive_time_period,
            cv.Optional(CONF_AUTO_PAGE_POST_FINAL, default="5min"): cv.positive_time_period,
            cv.Optional(CONF_BASEBALL_PAGE_SWITCH): cv.use_id(sw.Switch),
            cv.Optional(CONF_GAME_IN_PROGRESS): binary_sensor.binary_sensor_schema(
                device_class="running"
            ),
            cv.Optional(CONF_TEAM_SELECT): select.select_schema(TeamSelect).extend(
                {
                    cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])

    drawing_display = await cg.get_variable(config[CONF_DISPLAY_ID])
    cg.add(var.set_display(drawing_display))

    font = await cg.get_variable(config[CONF_FONT_ID])
    cg.add(var.set_font(font))

    time = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_rtc(time))

    cg.add(var.set_team_id(config[CONF_TEAM_ID]))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
    cg.add(var.set_base_url(config[CONF_BASE_URL]))

    cg.add(var.set_auto_baseball_page(config[CONF_AUTO_BASEBALL_PAGE]))
    lead = config[CONF_AUTO_PAGE_LEAD]
    lead_sec = max(0, int(lead.total_milliseconds // 1000))
    cg.add(var.set_auto_page_lead_sec(lead_sec))

    post_final = config[CONF_AUTO_PAGE_POST_FINAL]
    post_final_sec = max(0, int(post_final.total_milliseconds // 1000))
    cg.add(var.set_auto_page_post_final_sec(post_final_sec))

    if config.get(CONF_BASEBALL_PAGE_SWITCH):
        swi = await cg.get_variable(config[CONF_BASEBALL_PAGE_SWITCH])
        cg.add(var.set_baseball_page_switch(swi))

    if CONF_GAME_IN_PROGRESS in config and config[CONF_GAME_IN_PROGRESS]:
        bs = await binary_sensor.new_binary_sensor(config[CONF_GAME_IN_PROGRESS])
        cg.add(var.set_game_in_progress_sensor(bs))

    if CONF_TEAM_SELECT in config and config[CONF_TEAM_SELECT]:
        sel_conf = config[CONF_TEAM_SELECT]
        sel = cg.new_Pvariable(sel_conf[CONF_ID])
        cg.add(sel.set_tracker(var))
        cg.add(sel.set_restore_value(sel_conf[CONF_RESTORE_VALUE]))
        await cg.register_component(sel, sel_conf)
        await select.register_select(sel, sel_conf, options=_MLB_TEAM_OPTIONS)

    await cg.register_component(var, config)
