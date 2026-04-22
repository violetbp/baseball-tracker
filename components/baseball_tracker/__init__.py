import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.display import Display
from esphome.components.font import Font
from esphome.components.time import RealTimeClock
from esphome.const import CONF_ID, CONF_DISPLAY_ID, CONF_TIME_ID, __version__ as ESPHOME_VERSION, Framework

_MINIMUM_ESPHOME_VERSION = "2026.3.0"

DEPENDENCIES = ["network", "display", "font", "time"]
AUTO_LOAD = ["json"]

baseball_tracker_ns = cg.esphome_ns.namespace("baseball_tracker")
BaseballTracker = baseball_tracker_ns.class_("BaseballTracker", cg.Component)

CONF_FONT_ID = "font_id"
CONF_TEAM_ID = "team_id"
CONF_POLL_INTERVAL = "poll_interval"

_MARINERS_TEAM_ID = 136


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

    await cg.register_component(var, config)

    cg.add_library("HTTPClient", None)
    cg.add_library("NetworkClientSecure", None)
    cg.add_library("WiFi", None)
