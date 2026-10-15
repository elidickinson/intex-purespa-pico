import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, switch, text_sensor
from esphome.const import CONF_ID

DEPENDENCIES = ["text_sensor", "switch", "climate"]

CONF_CLIMATE = "climate"
CONF_POWER = "power"
CONF_FILTER = "filter"
CONF_BUBBLE = "bubble"
CONF_ERROR_TEXT = "error_text"

purespa_ns = cg.esphome_ns.namespace("purespa")

PureSpa = purespa_ns.class_("PureSpa", cg.Component)
PureSpaClimate = purespa_ns.class_("PureSpaClimate", climate.Climate)
PureSpaSwitch = purespa_ns.class_("PureSpaSwitch", switch.Switch)

CONFIG_SCHEMA = cv.COMPONENT_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(PureSpa),
        cv.Optional(CONF_CLIMATE): climate.climate_schema(PureSpaClimate),
        cv.Optional(CONF_POWER): switch.switch_schema(PureSpaSwitch),
        cv.Optional(CONF_FILTER): switch.switch_schema(PureSpaSwitch),
        cv.Optional(CONF_BUBBLE): switch.switch_schema(PureSpaSwitch),
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

    if CONF_ERROR_TEXT in config:
        tx = await text_sensor.new_text_sensor(config[CONF_ERROR_TEXT])
        cg.add(var.set_error_text_sensor(tx))
