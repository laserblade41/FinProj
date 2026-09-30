#include "display/display.h"
#include "display/watchface.h"
#include "display/test_screen.h"
#include "display/module_screen.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

LOG_MODULE_REGISTER(display_mod, LOG_LEVEL_INF);

#define DISPLAY_STACK_SIZE  4096
#define DISPLAY_PRIORITY    7
#define DISPLAY_PERIOD_MS   16   /* ~60 fps */

static const struct device *display_dev =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static const struct device *backlight_dev =
    DEVICE_DT_GET(DT_NODELABEL(backlight));

/* Inactivity timeout — turns backlight off after DISPLAY_TIMEOUT_MS */
static struct k_work_delayable backlight_off_work;
static bool backlight_on;

static void backlight_off_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    display_backlight_set(false);
}

static void display_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        uint32_t sleep_ms = lv_timer_handler();
        k_msleep(MIN(sleep_ms, DISPLAY_PERIOD_MS));
    }
}

K_THREAD_DEFINE(display_tid, DISPLAY_STACK_SIZE,
                display_thread_fn, NULL, NULL, NULL,
                DISPLAY_PRIORITY, 0, 0);

int display_module_init(void)
{
    if (!device_is_ready(display_dev)) {
        LOG_ERR("Display not ready");
        return -ENODEV;
    }

    display_blanking_off(display_dev);

    k_work_init_delayable(&backlight_off_work, backlight_off_fn);

    if (device_is_ready(backlight_dev)) {
        regulator_enable(backlight_dev);
        backlight_on = true;
    } else {
        LOG_WRN("Backlight regulator not ready");
    }

    /*
     * Bring-up note: the inactivity timeout is only recoverable once a
     * real wake source (IMU motion or a physically wired button) calls
     * display_wake(). Kconfig for those drivers is on regardless of
     * whether the hardware is actually connected (it just reflects the
     * devicetree nodes), so it can't be used to detect "no wake source
     * wired" at compile time. Leave the timeout disabled until buttons
     * or the IMU are physically present — re-enable this once they are.
     */
#if 0
    k_work_schedule(&backlight_off_work, K_MSEC(DISPLAY_TIMEOUT_MS));
#endif

    /* LVGL is auto-initialised via SYS_INIT when CONFIG_LVGL=y.
     * Calling lv_init() here is a no-op if already done. */
    lv_init();

    int ret = watchface_init();
    if (ret < 0) {
        LOG_ERR("watchface_init failed: %d", ret);
        return ret;
    }

    ret = test_screen_init();
    if (ret < 0) {
        LOG_ERR("test_screen_init failed: %d", ret);
        return ret;
    }

    ret = module_screen_init();
    if (ret < 0) {
        LOG_ERR("module_screen_init failed: %d", ret);
        return ret;
    }

    LOG_INF("Display ready, 240×240 RGB565");
    return 0;
}

void display_backlight_set(bool on)
{
    if (!device_is_ready(backlight_dev)) {
        return;
    }
    if (on && !backlight_on) {
        regulator_enable(backlight_dev);
        backlight_on = true;
    } else if (!on && backlight_on) {
        regulator_disable(backlight_dev);
        backlight_on = false;
    }
}

void display_wake(void)
{
    display_backlight_set(true);
    /* Reset the inactivity timer on every call */
    k_work_reschedule(&backlight_off_work, K_MSEC(DISPLAY_TIMEOUT_MS));
}
