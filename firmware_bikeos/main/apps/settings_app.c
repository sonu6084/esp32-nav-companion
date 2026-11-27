/*
 * Settings
 *
 *  Brightness, Volume (with a test beep), Screen off after (Never/15s/30s/1m/2m/5m),
 *  Keep screen on while navigating, Set time & date, Phone (pair status / forget),
 *  About: board, memory and storage, sensors, firmware (about.c).
 *  Everything is saved to NVS immediately.
 *  Keys: BOOT tap = scroll down one row   BOOT hold = scroll up
 */
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "app_manager.h"
#include "ui_theme.h"
#include "sys.h"
#include "settings.h"
#include "cfg.h"
#include "audio.h"
#include "ui_settime.h"
#include "about.h"
#include "nav_core.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define COLOR       0x9E9E9E

static const uint16_t TMO[] = {0, 15, 30, 60, 120, 300};
static const char *const TMO_TXT[] = {"Never", "15 s", "30 s", "1 min", "2 min", "5 min"};

static struct {
    bool alive;
    lv_obj_t *scr, *list, *bri_val, *vol_val, *tmo_btn, *phone_lbl, *forget_btn, *ab_board, *ab_usage, *ab_sensors, *ab_fw;
    lv_timer_t *tmr;
} ui;

static lv_obj_t *row(const char *title)
{
    lv_obj_t *r = lv_obj_create(ui.list);
    lv_obj_set_size(r, 290, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(r, 22, 0);
    lv_obj_set_style_bg_color(r, UI_COL_SURFACE, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 14, 0);
    lv_obj_set_style_pad_row(r, 10, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(r, false);
    lv_obj_t *t = lv_label_create(r);
    lv_obj_set_style_text_color(t, UI_COL_MUTED, 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_label_set_text(t, title);
    return r;
}

static lv_obj_t *slider(lv_obj_t *r, int min, int max, int val)
{
    lv_obj_t *s = lv_slider_create(r);
    lv_obj_set_width(s, LV_PCT(100));
    lv_obj_set_height(s, 14);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, UI_COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, UI_COL_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 14);
    return s;
}

static void bri_cb(lv_event_t *e)
{
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    sys_set_backlight(v);                                    /* live preview */
    lv_label_set_text_fmt(ui.bri_val, "Brightness  %d%%", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_set_brightness(v);   /* nav/settings.c: same value the phone app sets */
}
static void vol_cb(lv_event_t *e)
{
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    lv_label_set_text_fmt(ui.vol_val, "Volume  %d%%", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) { cfg_set_volume(v); audio_play(SND_TURN); }
}

static int tmo_index(void)
{
    for (int i = 0; i < 6; i++) if (TMO[i] == cfg_screen_timeout_s()) return i;
    return 0;
}
static void tmo_cb(lv_event_t *e)
{
    int i = (tmo_index() + 1) % 6;
    cfg_set_screen_timeout_s(TMO[i]);
    lv_label_set_text(lv_obj_get_child(ui.tmo_btn, 0), TMO_TXT[i]);
}
static void navon_cb(lv_event_t *e)
{
    cfg_set_keep_on_nav(lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED));
}
static void settime_cb(lv_event_t *e) { ui_settime_open(ui.scr, NULL); }
static void forget_cb(lv_event_t *e)  { nav_core_forget_phone(); }

static void refresh(void)
{
    if (!settings_paired())
        lv_label_set_text_fmt(ui.phone_lbl, "%s  " LV_SYMBOL_BULLET "  not paired\nOpen Navigation to pair", settings_device_name());
    else
        lv_label_set_text_fmt(ui.phone_lbl, "%s  " LV_SYMBOL_BULLET "  %s", settings_device_name(),
                              nav_core_connected() ? "connected" : "paired");
    if (settings_paired()) lv_obj_remove_state(ui.forget_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(ui.forget_btn, LV_STATE_DISABLED);

    static char buf[400];
    about_usage(buf, sizeof buf);
    lv_label_set_text(ui.ab_usage, buf);
    about_sensors(buf, sizeof buf);
    lv_label_set_text(ui.ab_sensors, buf);
    about_firmware(buf, sizeof buf);
    lv_label_set_text(ui.ab_fw, buf);
}

static void tick_cb(lv_timer_t *t) { if (!ui_settime_active()) refresh(); }

static lv_obj_t *value_label(lv_obj_t *r)
{
    lv_obj_t *l = lv_label_create(r);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    return l;
}

static lv_obj_t *about_label(lv_obj_t *r)
{
    lv_obj_t *l = lv_label_create(r);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_line_space(l, 3, 0);
    return l;
}

static void st_create(lv_obj_t *scr, const app_t *self)
{
    memset(&ui, 0, sizeof ui);
    ui.alive = true;
    ui.scr = scr;

    ui.list = lv_obj_create(scr);
    lv_obj_set_size(ui.list, UI_W, UI_H);
    lv_obj_set_style_bg_opa(ui.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.list, 0, 0);
    lv_obj_set_style_pad_ver(ui.list, 70, 0);
    lv_obj_set_style_pad_row(ui.list, 12, 0);
    lv_obj_set_flex_flow(ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui.list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(ui.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.list, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *title = lv_label_create(ui.list);
    lv_obj_set_style_text_color(title, UI_COL_ACCENT, 0);
    lv_label_set_text(title, "SETTINGS");

    lv_obj_t *r = row("DISPLAY");
    ui.bri_val = value_label(r);
    lv_label_set_text_fmt(ui.bri_val, "Brightness  %d%%", settings_brightness());
    lv_obj_t *s = slider(r, 5, 100, settings_brightness());
    lv_obj_add_event_cb(s, bri_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, bri_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *rt = lv_obj_create(r);
    lv_obj_remove_style_all(rt);
    lv_obj_set_size(rt, LV_PCT(100), 44);
    lv_obj_t *tl = lv_label_create(rt);
    lv_label_set_text(tl, "Screen off after");
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 0, 0);
    ui.tmo_btn = ui_pill_btn(rt, TMO_TXT[tmo_index()], 96, 40, lv_color_hex(0x33363D));
    lv_obj_align(ui.tmo_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(ui.tmo_btn, tmo_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rn = lv_obj_create(r);
    lv_obj_remove_style_all(rn);
    lv_obj_set_size(rn, LV_PCT(100), 40);
    lv_obj_t *nl = lv_label_create(rn);
    lv_label_set_text(nl, "Stay on while\nnavigating");
    lv_obj_align(nl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *sw = lv_switch_create(rn);
    lv_obj_set_size(sw, 60, 32);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw, UI_COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (cfg_keep_on_nav()) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, navon_cb, LV_EVENT_VALUE_CHANGED, NULL);

    r = row("SOUND");
    ui.vol_val = value_label(r);
    lv_label_set_text_fmt(ui.vol_val, "Volume  %d%%", cfg_volume());
    s = slider(r, 0, 100, cfg_volume());
    lv_obj_add_event_cb(s, vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, vol_cb, LV_EVENT_RELEASED, NULL);

    r = row("TIME");
    lv_obj_t *b = ui_pill_btn(r, LV_SYMBOL_EDIT "  Set time & date", LV_PCT(100), 44, lv_color_hex(0x33363D));
    lv_obj_add_event_cb(b, settime_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *hint = lv_label_create(r);
    lv_obj_set_style_text_color(hint, UI_COL_MUTED, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(hint, "Also synced from the phone");

    r = row("PHONE");
    ui.phone_lbl = lv_label_create(r);
    lv_obj_set_width(ui.phone_lbl, LV_PCT(100));
    lv_label_set_long_mode(ui.phone_lbl, LV_LABEL_LONG_MODE_WRAP);
    ui.forget_btn = ui_pill_btn(r, LV_SYMBOL_CLOSE "  Forget phone", LV_PCT(100), 44, lv_color_hex(0x5A2020));
    lv_obj_set_style_bg_opa(ui.forget_btn, LV_OPA_30, LV_STATE_DISABLED);
    lv_obj_add_event_cb(ui.forget_btn, forget_cb, LV_EVENT_CLICKED, NULL);

    /* About: four sections, the board's own figures read every second. */
    about_open();
    static char boardtxt[240];
    about_board(boardtxt, sizeof boardtxt);
    ui.ab_board = about_label(row("ABOUT " LV_SYMBOL_BULLET " BOARD"));
    lv_label_set_text(ui.ab_board, boardtxt);
    ui.ab_usage = about_label(row("MEMORY & STORAGE"));
    ui.ab_sensors = about_label(row("SENSORS"));
    ui.ab_fw = about_label(row("FIRMWARE"));

    refresh();
    ui.tmr = lv_timer_create(tick_cb, 1000, NULL);
}

static void st_destroy(void)
{
    ui_settime_close(false);
    if (ui.tmr) lv_timer_delete(ui.tmr);
    about_close();
    memset(&ui, 0, sizeof ui);
}

static bool st_on_key(app_key_t k)
{
    if (ui_settime_active()) { ui_settime_close(k == APP_KEY_PRIMARY); return true; }
    lv_obj_scroll_by_bounded(ui.list, 0, k == APP_KEY_PRIMARY ? -120 : 120, LV_ANIM_ON);
    return true;
}

const app_t app_settings = {
    .name    = "Settings",
    .symbol  = LV_SYMBOL_SETTINGS,
    .color   = COLOR,
    .create  = st_create,
    .destroy = st_destroy,
    .on_key  = st_on_key,
    .ready   = true,
};
