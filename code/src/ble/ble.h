#pragma once

#include <stdint.h>
#include "rtc/rtc.h"

int  ble_module_init(void);
void ble_notify_battery(uint8_t percent);
void ble_set_notification_text(const char *text, uint16_t len);
