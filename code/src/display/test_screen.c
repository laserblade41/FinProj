#include "display/test_screen.h"
#include "display/watchface.h"
#include "display/module_screen.h"

#include <zephyr/logging/log.h>
#include <lvgl.h>

LOG_MODULE_REGISTER(test_screen, LOG_LEVEL_INF);

static lv_obj_t *scr_test;
static lv_obj_t *box;

/* Tap the box to flip its color, confirming touch (not just swipe) works */
static void box_click_cb(lv_event_t *e)
{
    ARG_UNUSED(e);
    static bool green = true;
    green = !green;
    lv_obj_set_style_bg_color(box, green ? lv_color_hex(0x33CC66) : lv_color_hex(0xCC3333),
                              LV_PART_MAIN);
}

static void gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);

    if (dir == LV_DIR_RIGHT) {
        lv_scr_load_anim(watchface_get_scr(), LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    } else if (dir == LV_DIR_LEFT) {
        lv_scr_load_anim(module_screen_get_scr(), LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    }
}

int test_screen_init(void)
{
    scr_test = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_test, lv_color_hex(0x202040), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr_test, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr_test);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(title, "TEST");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *hint = lv_label_create(scr_test);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, 180);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint, "Right: watch face\nLeft: module");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 70);

    /* Tappable box to verify plain touch (press/release), separate from swipe gestures */
    box = lv_obj_create(scr_test);
    lv_obj_remove_style_all(box);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(box, 80, 80);
    lv_obj_set_style_radius(box, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x33CC66), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 10);
    lv_obj_add_event_cb(box, box_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(scr_test, gesture_cb, LV_EVENT_GESTURE, NULL);

    LOG_INF("Test screen initialised");
    return 0;
}

lv_obj_t *test_screen_get_scr(void)
{
    return scr_test;
}
