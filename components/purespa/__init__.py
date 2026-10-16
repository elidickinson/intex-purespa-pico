import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, select, switch, text_sensor
from esphome.const import CONF_ID

AUTO_LOAD = ["text_sensor", "switch", "climate", "select"]

CONF_CLIMATE = "climate"
CONF_POWER = "power"
CONF_FILTER = "filter"
CONF_BUBBLE = "bubble"
CONF_SANITIZER = "sanitizer"
CONF_SANITIZER_HOURS = "sanitizer_hours"
CONF_ERROR_TEXT = "error_text"

purespa_ns = cg.esphome_ns.namespace("purespa")

PureSpa = purespa_ns.class_("PureSpa", cg.Component)
PureSpaClimate = purespa_ns.class_("PureSpaClimate", climate.Climate)
PureSpaSwitch = purespa_ns.class_("PureSpaSwitch", switch.Switch)
PureSpaSelect = purespa_ns.class_("PureSpaSelect", select.Select)

CONFIG_SCHEMA = cv.COMPONENT_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(PureSpa),
        cv.Optional(CONF_CLIMATE): climate.climate_schema(PureSpaClimate),
        cv.Optional(CONF_POWER): switch.switch_schema(PureSpaSwitch),
        cv.Optional(CONF_FILTER): switch.switch_schema(PureSpaSwitch),
        cv.Optional(CONF_BUBBLE): switch.switch_schema(PureSpaSwitch),
        cv.Optional(CONF_SANITIZER): switch.switch_schema(PureSpaSwitch),
        cv.Optional(CONF_SANITIZER_HOURS): select.select_schema(PureSpaSelect),
        cv.Optional(CONF_ERROR_TEXT): text_sensor.text_sensor_schema(),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_CLIMATE in config:
        clim = await climate.new_climate(config[CONF_CLIMATE])
        cg.add(var.set_climate(clim))

    if CONF_POWER in config:
        sw = await switch.new_switch(config[CONF_POWER])
        cg.add(var.set_switch_power(sw))

    if CONF_FILTER in config:
        sw = await switch.new_switch(config[CONF_FILTER])
        cg.add(var.set_switch_filter(sw))

    if CONF_BUBBLE in config:
        sw = await switch.new_switch(config[CONF_BUBBLE])
        cg.add(var.set_switch_bubble(sw))

    if CONF_SANITIZER in config:
        sw = await switch.new_switch(config[CONF_SANITIZER])
        cg.add(var.set_switch_sanitizer(sw))

    if CONF_SANITIZER_HOURS in config:
        hours = await select.new_select(config[CONF_SANITIZER_HOURS], options=["off", "3h", "5h", "8h"])
        cg.add(var.set_sanitizer_hours_select(hours))

    if CONF_ERROR_TEXT in config:
        tx = await text_sensor.new_text_sensor(config[CONF_ERROR_TEXT])
        cg.add(var.set_error_text_sensor(tx))
