#include "pmic/pmic.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pmic, LOG_LEVEL_INF);

static const struct device *charger_dev =
    DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));
static bool charger_ready;

int pmic_module_init(void)
{
    if (!device_is_ready(charger_dev)) {
        LOG_ERR("npm1300 charger not ready");
        return -ENODEV;
    }
    charger_ready = true;
    LOG_INF("npm1300 PMIC ready");
    return 0;
}

/*
 *  Until a fuel-gauge node is added to the DT
 * (nordic,npm1300-fuel-gauge) and CONFIG_FUEL_GAUGE=y is wired up properly,
 * we return a static placeholder so the rest of the app compiles and runs.
 *
 * TODO: replace with fuel_gauge_get_prop(FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE)
 *       once nordic,npm1300-fuel-gauge is confirmed in the DT.
 */
uint8_t pmic_get_batt_percent(void)
{
    return 50U; /* placeholder until fuel gauge is wired */
}

bool pmic_is_charging(void)
{
    if (!charger_ready) {
        return false;
    }

    union charger_propval val = {0};

    if (charger_get_prop(charger_dev, CHARGER_PROP_STATUS, &val) < 0) {
        return false;
    }
    return (val.status == CHARGER_STATUS_CHARGING);
}
