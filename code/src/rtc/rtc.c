#include "rtc/rtc.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rtc, LOG_LEVEL_INF);

/* nRF RTC0 peripheral — clocked by 32.768 kHz LFXO crystal */
#define RTC_DEV DEVICE_DT_GET(DT_NODELABEL(rtc0))

static const struct device *rtc_dev;
static volatile uint32_t epoch;   /* seconds since 2000-01-01 00:00:00 UTC */

static void tick_cb(const struct device *dev, uint8_t chan, uint32_t ticks,
                    void *user_data)
{
    ARG_UNUSED(ticks);
    ARG_UNUSED(user_data);

    epoch++;

    struct counter_alarm_cfg cfg = {
        .callback = tick_cb,
        .ticks    = counter_us_to_ticks(dev, 1000000U), /* 1 second */
        .flags    = 0,
    };
    counter_set_channel_alarm(dev, chan, &cfg);
}

int rtc_module_init(void)
{
    rtc_dev = RTC_DEV;
    if (!device_is_ready(rtc_dev)) {
        LOG_ERR("RTC device not ready");
        return -ENODEV;
    }

    int ret = counter_start(rtc_dev);
    if (ret < 0 && ret != -EALREADY) {
        LOG_ERR("counter_start failed: %d", ret);
        return ret;
    }

    struct counter_alarm_cfg cfg = {
        .callback = tick_cb,
        .ticks    = counter_us_to_ticks(rtc_dev, 1000000U),
        .flags    = 0,
    };
    return counter_set_channel_alarm(rtc_dev, 0, &cfg);
}

uint32_t rtc_get_epoch(void)
{
    return epoch;
}

void rtc_set_epoch(uint32_t unix_epoch)
{
    epoch = unix_epoch;
    LOG_INF("RTC set to epoch %u", unix_epoch);
}

/* Minimal days-in-month table (no leap-year correction for simplicity) */
static const uint8_t days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/* Convert seconds since 2000-01-01 to broken-down time */
void rtc_get_time(struct rtc_time *t)
{
    uint32_t s = epoch;
    t->second = s % 60; s /= 60;
    t->minute = s % 60; s /= 60;
    t->hour   = s % 24; s /= 24;

    /* Days since 2000-01-01 */
    uint32_t days = s;
    uint16_t year = 2000;
    while (true) {
        bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        uint32_t days_in_year = leap ? 366 : 365;
        if (days < days_in_year) break;
        days -= days_in_year;
        year++;
    }
    t->year = year;

    bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    uint8_t m = 1;
    for (; m <= 12; m++) {
        uint8_t dim = days_in_month[m - 1] + (leap && m == 2 ? 1 : 0);
        if (days < dim) break;
        days -= dim;
    }
    t->month = m;
    t->day   = (uint8_t)(days + 1);
}
