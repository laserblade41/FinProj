/*
 * Expansion module screen.
 *
 * Shows the enumeration state and, once a module is active, its descriptor
 * fields and live input report.
 *
 * Threading: LVGL is not thread-safe and expansion.c runs its own poll
 * thread, so this screen never touches LVGL from the expansion callback.
 * Instead an lv_timer (which runs on the display thread alongside
 * lv_timer_handler()) polls the expansion API and updates the widgets.
 */

#include "display/module_screen.h"
#include "display/test_screen.h"
#include "expansion/expansion.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <stdio.h>

LOG_MODULE_REGISTER(module_screen, LOG_LEVEL_INF);

#define REFRESH_PERIOD_MS 200

static lv_obj_t *scr_module;
static lv_obj_t *state_label;
static lv_obj_t *name_label;
static lv_obj_t *detail_label;
static lv_obj_t *report_label;
static lv_obj_t *action_btn;
static lv_obj_t *action_btn_label;

#if defined(CONFIG_FINWATCH_EXPANSION_MOCK)
static lv_obj_t *mock_btn_label;
#endif

static const char *state_text(enum expansion_state st)
{
    switch (st) {
    case EXP_ABSENT:      return "No module";
    case EXP_ENUMERATING: return "Enumerating...";
    case EXP_ACTIVE:      return "Connected";
    case EXP_ERROR:       return "Bad descriptor";
    default:              return "?";
    }
}

static void refresh_cb(lv_timer_t *timer)
{
    ARG_UNUSED(timer);

    const enum expansion_state st = expansion_get_state();

    lv_label_set_text(state_label, state_text(st));
    lv_obj_set_style_text_color(state_label,
                                (st == EXP_ACTIVE) ? lv_color_hex(0x33CC66)
                                : (st == EXP_ERROR) ? lv_color_hex(0xCC3333)
                                                    : lv_color_hex(0xAAAAAA),
                                LV_PART_MAIN);

    const struct expansion_info *info = expansion_get_info();

    if (info == NULL) {
        lv_label_set_text(name_label, "-");
        lv_label_set_text(detail_label, "");
        lv_label_set_text(report_label, "");
        lv_obj_add_flag(action_btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(name_label, info->name);

    char dbuf[48];
    snprintf(dbuf, sizeof(dbuf), "%04X:%04X  %s%s",
             info->vid, info->pid,
             (info->caps & FWMP_CAP_INPUT)  ? "IN "  : "",
             (info->caps & FWMP_CAP_OUTPUT) ? "OUT" : "");
    lv_label_set_text(detail_label, dbuf);

    /* Live input report, hex */
    uint8_t rpt[FWMP_REPORT_MAX_LEN];
    const int n = expansion_get_report(rpt, sizeof(rpt));

    if (n > 0) {
        char rbuf[3 * 8 + 1];
        int pos = 0;
        const int shown = MIN(n, 8);

        for (int i = 0; i < shown && pos < (int)sizeof(rbuf) - 3; i++) {
            pos += snprintf(&rbuf[pos], sizeof(rbuf) - pos, "%02X ", rpt[i]);
        }
        lv_label_set_text(report_label, rbuf);
    } else {
        lv_label_set_text(report_label, "");
    }

    if (info->caps & FWMP_CAP_OUTPUT) {
        lv_obj_remove_flag(action_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(action_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Send an incrementing byte so the mock's echo field visibly changes. */
static void action_btn_cb(lv_event_t *e)
{
    ARG_UNUSED(e);
    static uint8_t out;

    out++;
    const int ret = expansion_write_report(&out, 1);
    if (ret < 0) {
        LOG_WRN("output report failed: %d", ret);
    }
}

#if defined(CONFIG_FINWATCH_EXPANSION_MOCK)
/* Simulates plugging/unplugging a module on the pogo pins, so the attach,
 * enumerate and detach paths are all reachable without real hardware. */
static void mock_btn_cb(lv_event_t *e)
{
    ARG_UNUSED(e);
    const bool now_attached = !expansion_mock_is_attached();

    expansion_mock_set_attached(now_attached);
    lv_label_set_text(mock_btn_label, now_attached ? "Unplug" : "Plug in");
}
#endif

static void gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
        lv_scr_load_anim(test_screen_get_scr(), LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    }
}

/* Poll fast only while the user is actually looking at this screen. */
static void screen_loaded_cb(lv_event_t *e)
{
    ARG_UNUSED(e);
    expansion_set_foreground(true);
}

static void screen_unloaded_cb(lv_event_t *e)
{
    ARG_UNUSED(e);
    expansion_set_foreground(false);
}

int module_screen_init(void)
{
    scr_module = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_module, lv_color_hex(0x101828), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr_module, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr_module);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(title, "MODULE");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    state_label = lv_label_create(scr_module);
    lv_obj_set_style_text_color(state_label, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_label_set_text(state_label, "No module");
    lv_obj_align(state_label, LV_ALIGN_TOP_MID, 0, 56);

    name_label = lv_label_create(scr_module);
    lv_obj_set_style_text_color(name_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_label, 190);
    lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(name_label, "-");
    lv_obj_align(name_label, LV_ALIGN_CENTER, 0, -14);

    detail_label = lv_label_create(scr_module);
    lv_obj_set_style_text_color(detail_label, lv_color_hex(0x8899BB), LV_PART_MAIN);
    lv_label_set_text(detail_label, "");
    lv_obj_align(detail_label, LV_ALIGN_CENTER, 0, 6);

    report_label = lv_label_create(scr_module);
    lv_obj_set_style_text_color(report_label, lv_color_hex(0xFFAA33), LV_PART_MAIN);
    lv_label_set_text(report_label, "");
    lv_obj_align(report_label, LV_ALIGN_CENTER, 0, 28);

    action_btn = lv_button_create(scr_module);
    lv_obj_set_size(action_btn, 84, 30);
    lv_obj_align(action_btn, LV_ALIGN_BOTTOM_MID, 0, -56);
    lv_obj_add_event_cb(action_btn, action_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(action_btn, LV_OBJ_FLAG_HIDDEN);

    action_btn_label = lv_label_create(action_btn);
    lv_label_set_text(action_btn_label, "Send");
    lv_obj_center(action_btn_label);

#if defined(CONFIG_FINWATCH_EXPANSION_MOCK)
    lv_obj_t *mock_btn = lv_button_create(scr_module);
    lv_obj_set_size(mock_btn, 84, 30);
    lv_obj_align(mock_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_add_event_cb(mock_btn, mock_btn_cb, LV_EVENT_CLICKED, NULL);

    mock_btn_label = lv_label_create(mock_btn);
    lv_label_set_text(mock_btn_label,
                      expansion_mock_is_attached() ? "Unplug" : "Plug in");
    lv_obj_center(mock_btn_label);
#endif

    lv_obj_add_event_cb(scr_module, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr_module, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(scr_module, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    lv_timer_create(refresh_cb, REFRESH_PERIOD_MS, NULL);

    LOG_INF("Module screen initialised");
    return 0;
}

lv_obj_t *module_screen_get_scr(void)
{
    return scr_module;
}
