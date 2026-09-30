#include "display/watchface.h"
#include "display/test_screen.h"
#include "rtc/rtc.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <stdio.h>

LOG_MODULE_REGISTER(watchface, LOG_LEVEL_DBG);

/* All LVGL objects live on the default screen */
static lv_obj_t *scr_watchface;
static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *batt_label;
static lv_obj_t *steps_label;
static lv_obj_t *notif_label;

static void gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_LEFT) {
        lv_scr_load_anim(test_screen_get_scr(), LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    }
}

/* Notification auto-clear timer */
static lv_timer_t *notif_clear_timer;

static void notif_clear_cb(lv_timer_t *timer)
{
    lv_label_set_text(notif_label, "");
    lv_obj_add_flag(notif_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(timer);
    notif_clear_timer = NULL;
}

int watchface_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    scr_watchface = scr;

    /* Black background fills the round display */
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* ---- Time (large, centre) ---- */
    time_label = lv_label_create(scr);
    lv_obj_set_style_text_color(time_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(time_label, "00:00");
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -10);

    /* ---- Date (below time) ---- */
    date_label = lv_label_create(scr);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_label_set_text(date_label, "2000-01-01");
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 25);

    /* ---- Battery (top centre) ---- */
    batt_label = lv_label_create(scr);
    lv_obj_set_style_text_color(batt_label, lv_color_hex(0x88FF88), LV_PART_MAIN);
    lv_label_set_text(batt_label, LV_SYMBOL_BATTERY_FULL " 100%");
    lv_obj_align(batt_label, LV_ALIGN_TOP_MID, 0, 28);

    /* ---- Step count (bottom centre) ---- */
    steps_label = lv_label_create(scr);
    lv_obj_set_style_text_color(steps_label, lv_color_hex(0xFFAA33), LV_PART_MAIN);
    lv_label_set_text(steps_label, "Steps: 0");
    lv_obj_align(steps_label, LV_ALIGN_BOTTOM_MID, 0, -28);

    /* ---- Notification (centre, hidden by default) ---- */
    notif_label = lv_label_create(scr);
    lv_obj_set_style_text_color(notif_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_long_mode(notif_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(notif_label, 180);
    lv_obj_align(notif_label, LV_ALIGN_CENTER, 0, 60);
    lv_obj_add_flag(notif_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);

    LOG_INF("Watch face initialised");
    return 0;
}

lv_obj_t *watchface_get_scr(void)
{
    return scr_watchface;
}

void watchface_update_time(const struct rtc_time *t)
{
    char buf[9];   /* "255:255\0" — GCC sees uint8_t range [0,255] */
    snprintf(buf, sizeof(buf), "%02u:%02u", t->hour, t->minute);
    lv_label_set_text(time_label, buf);

    char dbuf[15]; /* "65535-255-255\0" — covers full uint16/uint8 ranges */
    snprintf(dbuf, sizeof(dbuf), "%04u-%02u-%02u", t->year, t->month, t->day);
    lv_label_set_text(date_label, dbuf);
}

void watchface_update_battery(uint8_t percent, bool charging)
{
    char buf[16];
    const char *icon = charging        ? LV_SYMBOL_CHARGE
                       : percent > 75  ? LV_SYMBOL_BATTERY_FULL
                       : percent > 50  ? LV_SYMBOL_BATTERY_3
                       : percent > 25  ? LV_SYMBOL_BATTERY_2
                       : percent > 10  ? LV_SYMBOL_BATTERY_1
                                       : LV_SYMBOL_BATTERY_EMPTY;
    snprintf(buf, sizeof(buf), "%s %u%%", icon, percent);
    lv_label_set_text(batt_label, buf);
}

void watchface_update_steps(uint32_t steps)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "Steps: %u", steps);
    lv_label_set_text(steps_label, buf);
}

void watchface_show_notification(const char *text)
{
    lv_label_set_text(notif_label, text);
    lv_obj_clear_flag(notif_label, LV_OBJ_FLAG_HIDDEN);

    /* Auto-clear after 5 seconds */
    if (notif_clear_timer) {
        lv_timer_reset(notif_clear_timer);
    } else {
        notif_clear_timer = lv_timer_create(notif_clear_cb, 5000, NULL);
        lv_timer_set_repeat_count(notif_clear_timer, 1);
    }
}
