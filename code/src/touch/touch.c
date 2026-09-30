#include "touch/touch.h"
#include "display/display.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

LOG_MODULE_REGISTER(touch, LOG_LEVEL_DBG);

static const struct device *touch_dev =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_touch));

static int32_t touch_x;
static int32_t touch_y;
static bool    touch_pressed;

static void touch_input_cb(struct input_event *evt, void *user_data)
{
    ARG_UNUSED(user_data);

    switch (evt->type) {
    case INPUT_EV_ABS:
        if (evt->code == INPUT_ABS_X) touch_x = evt->value;
        if (evt->code == INPUT_ABS_Y) touch_y = evt->value;
        break;
    case INPUT_EV_KEY:
        if (evt->code == INPUT_BTN_TOUCH) {
            touch_pressed = (evt->value != 0);
            if (touch_pressed) {
                display_wake();
            }
        }
        break;
    default:
        break;
    }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_CHOSEN(zephyr_touch)), touch_input_cb, NULL);

/*
 * LVGL v9 indev API: lv_indev_drv_t / lv_indev_drv_register() were removed.
 * Use lv_indev_create() + lv_indev_set_type() + lv_indev_set_read_cb() instead.
 */
static void lvgl_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    ARG_UNUSED(indev);
    data->point.x = (lv_coord_t)touch_x;
    data->point.y = (lv_coord_t)touch_y;
    data->state   = touch_pressed ? LV_INDEV_STATE_PRESSED
                                  : LV_INDEV_STATE_RELEASED;
}

int touch_module_init(void)
{
    if (!device_is_ready(touch_dev)) {
        LOG_ERR("Touch controller not ready");
        return -ENODEV;
    }
    LOG_INF("Touch controller ready");

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lvgl_indev_read_cb);

    return 0;
}
