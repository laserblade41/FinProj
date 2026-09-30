#pragma once

#include <stdint.h>

typedef enum {
    BTN_SHORT_PRESS,
    BTN_LONG_PRESS,
} btn_event_t;

typedef void (*btn_callback_t)(uint8_t btn_idx, btn_event_t event);

int  buttons_module_init(btn_callback_t cb);
