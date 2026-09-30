#include "buttons/buttons.h"
#include "display/display.h"

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_DBG);

#define LONG_PRESS_MS  600U

static btn_callback_t user_cb;

/* Per-button press tracking */
static struct {
    uint16_t     code;
    int64_t      press_time_ms;
    bool         pressed;
} btn_state[] = {
    { .code = INPUT_KEY_0 },
    { .code = INPUT_KEY_1 },
};

static void btn_input_cb(struct input_event *evt, void *user_data)
{
    ARG_UNUSED(user_data);

    if (evt->type != INPUT_EV_KEY) {
        return;
    }

    for (int i = 0; i < ARRAY_SIZE(btn_state); i++) {
        if (evt->code != btn_state[i].code) {
            continue;
        }

        if (evt->value) {
            /* Press: always wake the display */
            display_wake();
            btn_state[i].pressed = true;
            btn_state[i].press_time_ms = k_uptime_get();
        } else if (btn_state[i].pressed) {
            /* Release */
            btn_state[i].pressed = false;
            int64_t held = k_uptime_get() - btn_state[i].press_time_ms;
            btn_event_t type = (held >= LONG_PRESS_MS)
                               ? BTN_LONG_PRESS : BTN_SHORT_PRESS;
            LOG_DBG("BTN%d %s", i, type == BTN_LONG_PRESS ? "LONG" : "SHORT");
            if (user_cb) {
                user_cb((uint8_t)i, type);
            }
        }
        break;
    }
}

/* Listen to all input events (first arg = NULL) to catch gpio-keys events */
INPUT_CALLBACK_DEFINE(NULL, btn_input_cb, NULL);

int buttons_module_init(btn_callback_t cb)
{
    user_cb = cb;
    LOG_INF("Buttons ready");
    return 0;
}
