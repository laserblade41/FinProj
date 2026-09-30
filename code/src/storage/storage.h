#pragma once

#include <stdint.h>

struct watch_settings {
    int8_t   tz_offset_hours;  /* UTC offset -12..+14 */
    uint16_t step_goal;
    uint8_t  brightness;       /* 0–100 */
};

int  storage_module_init(void);
int  storage_load_settings(struct watch_settings *s);
int  storage_save_settings(const struct watch_settings *s);
