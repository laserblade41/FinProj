#pragma once

#include <stdbool.h>

#define DISPLAY_TIMEOUT_MS 10000   /* backlight off after 10 s of inactivity */

int  display_module_init(void);
void display_backlight_set(bool on);
void display_wake(void);           /* reset inactivity timer, turn backlight on */
