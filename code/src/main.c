#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "rtc/rtc.h"
#include "pmic/pmic.h"
#include "sensor/sensor.h"
#include "touch/touch.h"
#include "display/display.h"
#include "display/watchface.h"
#include "storage/storage.h"
#include "ble/ble.h"
#include "buttons/buttons.h"
#include "expansion/expansion.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ---- Button handler ---- */

static void on_button(uint8_t btn_idx, btn_event_t event)
{
    if (btn_idx == 0 && event == BTN_SHORT_PRESS) {
        /* BTN0 short: cycle screens (only watch face for now) */
        LOG_DBG("BTN0 short");
    } else if (btn_idx == 0 && event == BTN_LONG_PRESS) {
        /* BTN0 long: reset step counter */
        sensor_reset_step_count();
        LOG_INF("Step counter reset");
    } else if (btn_idx == 1 && event == BTN_SHORT_PRESS) {
        /* BTN1 short: toggle backlight */
        LOG_DBG("BTN1 short");
    } else if (btn_idx == 1 && event == BTN_LONG_PRESS) {
        /* BTN1 long: (reserved for power menu) */
        LOG_INF("BTN1 long press");
    }
}

/* ---- Expansion module events ---- */

/* Runs on the expansion poll thread, logging only, no LVGL calls.
 * The module screen refreshes itself from its own LVGL timer. */
static void on_expansion_event(enum expansion_state state,
                               const struct expansion_info *info)
{
    switch (state) {
    case EXP_ACTIVE:
        LOG_INF("Module attached: \"%s\" (%04X:%04X)",
                info->name, info->vid, info->pid);
        break;
    case EXP_ABSENT:
        LOG_INF("Module detached");
        break;
    case EXP_ERROR:
        LOG_WRN("Module present but descriptor rejected");
        break;
    case EXP_ENUMERATING:
        LOG_DBG("Module enumerating");
        break;
    }
}

/* ---- Background update thread- 1 Hz watch-face refresh ---- */

#define UPDATE_STACK   1024
#define UPDATE_PRIO    9
#define UPDATE_PERIOD  1000   /* ms */

static void update_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    struct watch_settings settings = {
        .tz_offset_hours = 0,
        .step_goal       = 10000,
        .brightness      = 80,
    };
    storage_load_settings(&settings);

    uint8_t last_pct = 0;
    int batt_tick = 0;

    while (1) {
        /* ---- Time ---- */
        struct rtc_time t;
        rtc_get_time(&t);
        watchface_update_time(&t);

        /* ---- Battery: poll every 10 s ---- */
        if (++batt_tick >= 10) {
            batt_tick = 0;
            uint8_t pct = pmic_get_batt_percent();
            bool    chg = pmic_is_charging();
            watchface_update_battery(pct, chg);
            if (pct != last_pct) {
                ble_notify_battery(pct);
                last_pct = pct;
            }
        }

        /* ---- Steps ---- */
        watchface_update_steps(sensor_get_step_count());

        k_msleep(UPDATE_PERIOD);
    }
}

K_THREAD_DEFINE(update_tid, UPDATE_STACK,
                update_thread_fn, NULL, NULL, NULL,
                UPDATE_PRIO, 0, 0);

/* ---- Main ---- */

int main(void)
{
    int ret;

    LOG_INF("FinWatch booting...");

    ret = rtc_module_init();
    if (ret < 0) {
        LOG_WRN("RTC init: %d (time resets on power cycle)", ret);
    }

    ret = pmic_module_init();
    if (ret < 0) {
        LOG_WRN("PMIC init: %d", ret);
    }

    /* Pass display_wake as the motion callback, wrist-raise wakes screen */
    ret = sensor_module_init(display_wake);
    if (ret < 0) {
        LOG_WRN("Sensor init: %d", ret);
    }

    /*
     * DK bring-up: external SPI-NOR flash isn't populated on the DK.
     * fs_mount() hard-faults (SecureFault) rather than failing gracefully,
     * because DT_FIXED_PARTITION_ID(ext_storage_partition) collides with
     * the DK's internal storage_partition ID, a separate bug to revisit
     * once real flash hardware is wired up. Skip storage until then.
     */
#if 0
    ret = storage_module_init();
    if (ret < 0) {
        LOG_WRN("Storage init: %d", ret);
    }
#endif

    ret = buttons_module_init(on_button);
    if (ret < 0) {
        LOG_WRN("Buttons init: %d", ret);
    }

    /* Touch must be ready before display so LVGL indev is registered */
    ret = touch_module_init();
    if (ret < 0) {
        LOG_WRN("Touch init: %d", ret);
    }

    ret = display_module_init();
    if (ret < 0) {
        LOG_ERR("Display init: %d: halting", ret);
        return ret;
    }

    ret = ble_module_init();
    if (ret < 0) {
        LOG_WRN("BLE init: %d", ret);
    }

    /* Expansion port is optional hardware, never let it block boot. */
    ret = expansion_module_init(on_expansion_event);
    if (ret < 0) {
        LOG_WRN("Expansion init: %d", ret);
    }

    LOG_INF("FinWatch running");
    return 0;
}
