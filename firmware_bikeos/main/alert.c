#include "alert.h"

#include <string.h>
#include "ui_theme.h"
#include "sys.h"
#include "app_manager.h"

#define SOUND_MAX_MS (90 * 1000)   /* stop beeping after 90 s; the overlay stays */

static lv_obj_t  *s_ov, *s_ring;
static alert_cb_t s_cb;
static lv_timer_t *s_tmr;
static uint32_t   s_start;

bool alert_active(void) { return s_ov != NULL; }

static void ring_anim(void *obj, int32_t v) { lv_obj_set_style_arc_opa(obj, (lv_opa_t)v, LV_PART_MAIN); }

static void tick(lv_timer_t *t)
{
    if (lv_tick_elaps(s_start) > SOUND_MAX_MS) audio_stop();
}

void alert_close(alert_action_t a)
{
    if (!s_ov) return;
    alert_cb_t cb = s_cb;
    audio_stop();
    lv_anim_delete(s_ring, ring_anim);
    lv_obj_delete(s_ov);
    if (s_tmr) lv_timer_delete(s_tmr);
    s_ov = s_ring = NULL;
    s_tmr = NULL;
    s_cb = NULL;
    if (cb) cb(a);
}

static void btn_cb(lv_event_t *e) { alert_close((alert_action_t)(intptr_t)lv_event_get_user_data(e)); }

void alert_show(const char *title, const char *big, uint32_t color, snd_t sound,
                const char *secondary_label, alert_cb_t cb)
{
    if (s_ov) alert_close(ALERT_DISMISS);
    am_screen_wake();
    s_cb = cb;
    s_start = lv_tick_get();

    s_ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ov);
    lv_obj_set_size(s_ov, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_ov, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(s_ov, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_ov, true);      /* block the app underneath */
    lv_obj_set_scrollable(s_ov, false);

    s_ring = lv_arc_create(s_ov);
    lv_obj_set_size(s_ring, UI_W - 6, UI_H - 6);
    lv_obj_center(s_ring);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_style(s_ring, NULL, LV_PART_INDICATOR);
    lv_obj_set_clickable(s_ring, false);
    lv_obj_set_style_arc_width(s_ring, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(color), LV_PART_MAIN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ring);
    lv_anim_set_exec_cb(&a, ring_anim);
    lv_anim_set_values(&a, LV_OPA_20, LV_OPA_COVER);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_reverse_duration(&a, 500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_obj_t *t = lv_label_create(s_ov);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(color), 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -96);

    lv_obj_t *b = lv_label_create(s_ov);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(b, UI_COL_TEXT, 0);
    lv_label_set_text(b, big ? big : "");
    lv_obj_align(b, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *ok = ui_round_btn(s_ov, 84, lv_color_hex(color), LV_SYMBOL_OK, &lv_font_montserrat_32);
    lv_obj_add_event_cb(ok, btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ALERT_DISMISS);
    if (secondary_label) {
        lv_obj_align(ok, LV_ALIGN_CENTER, 70, 104);
        lv_obj_t *sec = lv_button_create(s_ov);
        lv_obj_set_size(sec, 120, 64);
        lv_obj_set_style_radius(sec, 32, 0);
        lv_obj_set_style_bg_color(sec, lv_color_hex(0x33363D), 0);
        lv_obj_set_ext_click_area(sec, 10);
        lv_obj_align(sec, LV_ALIGN_CENTER, -66, 104);
        lv_obj_t *sl = lv_label_create(sec);
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_20, 0);
        lv_label_set_text(sl, secondary_label);
        lv_obj_center(sl);
        lv_obj_add_event_cb(sec, btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ALERT_SECONDARY);
    } else {
        lv_obj_align(ok, LV_ALIGN_CENTER, 0, 104);
    }

    if (sound != SND_NONE) audio_play(sound);
    s_tmr = lv_timer_create(tick, 1000, NULL);
}
