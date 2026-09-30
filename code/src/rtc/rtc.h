#pragma once

#include <stdint.h>

struct rtc_time {
    uint16_t year;
    uint8_t  month;   /* 1–12 */
    uint8_t  day;     /* 1–31 */
    uint8_t  hour;    /* 0–23 */
    uint8_t  minute;  /* 0–59 */
    uint8_t  second;  /* 0–59 */
};

int  rtc_module_init(void);
void rtc_get_time(struct rtc_time *t);
void rtc_set_epoch(uint32_t unix_epoch);
uint32_t rtc_get_epoch(void);
