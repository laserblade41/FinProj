#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <lvgl.h>

struct rtc_time;

int  watchface_init(void);
void watchface_update_time(const struct rtc_time *t);
void watchface_update_battery(uint8_t percent, bool charging);
void watchface_update_steps(uint32_t steps);
void watchface_show_notification(const char *text);
lv_obj_t *watchface_get_scr(void);
