#include "app_manager.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ui_theme.h"
#include "sys.h"
#include "apps/registry.h"
#include "alert.h"
#include "settings.h"
#include "cfg.h"
#include "home.h"
#include "ride.h"
#include "nav_core.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"

static const char *TAG = "am";

static lv_obj_t    *s_menu_scr;
static lv_obj_t    *s_home_scr;          /* clock face after boot; tap -> menu */
static bool         s_on_home;           /* home is showing (s_cur is NULL then) */
static bool         s_app_from_home;     /* the open app came straight from home */
static lv_obj_t    *s_list;
static lv_obj_t    *s_clock_lbl;
static lv_obj_t    *s_bat_lbl;
static lv_obj_t    *s_status_lbl[APP_MAX];
static const app_t *s_cur;
static uint32_t     s_last_nav_ms;        /* debounce while a screen animation runs */

#define NAV_GUARD_MS 350
#define MENU_IDLE_HOME_MS 30000   /* untouched menu falls back to home */

static void refresh_menu_status(void);

const app_t *am_current(void) { return s_cur; }

/* ------------------------------------------------------------------ */
/* "Coming soon" placeholder for apps that are listed but not built   */
/* ------------------------------------------------------------------ */
static void soon_create(lv_obj_t *scr, const app_t *self)
{
    lv_obj_t *ic = lv_label_create(scr);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(self->color), 0);
    lv_label_set_text(ic, self->symbol);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *t = lv_label_create(scr);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_label_set_text(t, self->name);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *s = lv_label_create(scr);
    lv_obj_set_style_text_color(s, UI_COL_MUTED, 0);
    lv_label_set_text(s, "Coming soon\nSwipe right or tap PWR to go back");
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s, LV_ALIGN_CENTER, 0, 80);
}

/* ------------------------------------------------------------------ */
/* Navigation                                                          */
/* ------------------------------------------------------------------ */
static bool nav_allowed(void)
{
    uint32_t now = lv_tick_get();
    if (now - s_last_nav_ms < NAV_GUARD_MS) return false;
    s_last_nav_ms = now;
    return true;
}

static void app_gesture_cb(lv_event_t *e)
{
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_RIGHT) am_home();
}

static const app_t *s_prev;     /* app that was open before an app->app switch */

const app_t *am_find(const char *name)
{
    for (int i = 0; i < g_app_count; i++) if (strcmp(g_apps[i]->name, name) == 0) return g_apps[i];
    return NULL;
}

void am_open(const app_t *app)
{
    if (!app || app == s_cur) return;
    bool from_app = s_cur != NULL;
    if (!from_app && !nav_allowed()) return;
    if (!from_app) s_app_from_home = s_on_home;
    s_on_home = false;
    ESP_LOGW(TAG, "open %s", app->name);
    s_prev = from_app ? s_cur : NULL;
    if (from_app && s_cur->ready && s_cur->destroy) s_cur->destroy();

    lv_obj_t *scr = ui_screen_create();
    if (!app->no_swipe_back) lv_obj_add_event_cb(scr, app_gesture_cb, LV_EVENT_GESTURE, NULL);
    s_cur = app;
    if (app->instant) {
        /* views that build on lv_screen_active() need the screen loaded first */
        lv_obj_t *old = lv_screen_active();
        lv_screen_load(scr);
        if (from_app && old != s_menu_scr && old != s_home_scr) lv_obj_delete(old);
        if (app->ready && app->create) app->create(scr, app);
        return;
    }
    if (app->ready && app->create) app->create(scr, app);
    else soon_create(scr, app);
    /* app -> app: the old app screen is deleted after the animation */
    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 200, 0, from_app);
}

void am_back(void)
{
    const app_t *p = s_prev;
    s_prev = NULL;
    if (p) am_open(p);
    else if (s_app_from_home) am_show_home();
    else am_home();
}

/* ------------------------------------------------------------------ */
/* Home screen                                                          */
/* ------------------------------------------------------------------ */
void am_show_menu(void)
{
    if (!s_on_home || !nav_allowed()) return;
    ESP_LOGW(TAG, "menu from home");
    s_on_home = false;
    refresh_menu_status();
    lv_display_trigger_activity(NULL);
    lv_screen_load_anim(s_menu_scr, LV_SCREEN_LOAD_ANIM_MOVE_TOP, 200, 0, false);
}

void am_show_home(void)
{
    if (s_on_home) return;
    ESP_LOGW(TAG, "home from %s", s_cur ? s_cur->name : "menu");
    lv_obj_t *old = lv_screen_active();
    bool app = s_cur != NULL;
    if (app && s_cur->ready && s_cur->destroy) s_cur->destroy();
    s_cur = NULL;
    s_prev = NULL;
    s_on_home = true;
    home_refresh();
    /* an app screen is freed after the animation; the menu is kept */
    lv_screen_load_anim(s_home_scr, LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM, 200, 0, app && old != s_menu_scr);
}

bool am_on_home(void) { return s_on_home; }

static void home_tap(void) { am_show_menu(); }
static void home_nav(void) { am_open(am_find("Navigation")); }

static void menu_gesture_cb(lv_event_t *e)
{
    (void)e;
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) am_show_home();
}

void am_home(void)
{
    if (!s_cur || !nav_allowed()) return;
    ESP_LOGW(TAG, "home from %s", s_cur->name);
    if (s_cur->ready && s_cur->destroy) s_cur->destroy();
    for (int i = 0; i < g_app_count; i++) {      /* re-centre the menu on the app we left */
        if (g_apps[i] == s_cur) {
            lv_obj_scroll_to_view(lv_obj_get_child(s_list, i), LV_ANIM_OFF);
            lv_obj_send_event(s_list, LV_EVENT_SCROLL, NULL);
            break;
        }
    }
    s_cur = NULL;
    s_prev = NULL;
    refresh_menu_status();
    /* auto_del = true: the app screen is freed when the animation ends */
    lv_screen_load_anim(s_menu_scr, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 200, 0, true);
}

/* ------------------------------------------------------------------ */
/* Menu (round, vertically scrolling, items bend along the bezel)      */
/* ------------------------------------------------------------------ */
static void list_scroll_cb(lv_event_t *e)
{
    lv_obj_t *cont = (lv_obj_t *)lv_event_get_target(e);
    lv_area_t ca;
    lv_obj_get_coords(cont, &ca);
    int32_t cy = ca.y1 + lv_area_get_height(&ca) / 2;
    int32_t r  = UI_H / 2;

    uint32_t n = lv_obj_get_child_count(cont);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(cont, i);
        lv_area_t a;
        lv_obj_get_coords(child, &a);
        int32_t dy = LV_ABS((a.y1 + lv_area_get_height(&a) / 2) - cy);
        int32_t x;
        if (dy >= r) {
            x = r;
        } else {
            lv_sqrt_res_t res;
            lv_sqrt((uint32_t)(r * r - dy * dy), &res, 0x8000);
            x = r - res.i;
        }
        lv_obj_set_style_translate_x(child, x, 0);
        lv_opa_t fade = (lv_opa_t)lv_map(dy, 0, r, 0, 200);
        lv_obj_set_style_opa(child, LV_OPA_COVER - fade, 0);
    }
}

static void item_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    /* Tapping an item that is not centred just scrolls it to the middle */
    lv_obj_t *item = (lv_obj_t *)lv_event_get_current_target(e);
    lv_area_t a, ca;
    lv_obj_get_coords(item, &a);
    lv_obj_get_coords(s_list, &ca);
    int32_t off = (a.y1 + lv_area_get_height(&a) / 2) - (ca.y1 + lv_area_get_height(&ca) / 2);
    if (LV_ABS(off) > 40) {
        lv_obj_scroll_to_view(item, LV_ANIM_ON);
        return;
    }
    am_open(g_apps[idx]);
}

static lv_obj_t *make_item(lv_obj_t *parent, int idx)
{
    const app_t *app = g_apps[idx];

    lv_obj_t *it = lv_obj_create(parent);
    lv_obj_set_size(it, 300, 88);
    lv_obj_set_style_radius(it, 44, 0);
    lv_obj_set_style_bg_color(it, UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_color(it, lv_color_hex(0x2A2D35), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(it, 0, 0);
    lv_obj_set_style_pad_all(it, 0, 0);
    lv_obj_set_scrollable(it, false);
    lv_obj_set_snappable(it, true); lv_obj_set_clickable(it, true);
    lv_obj_add_event_cb(it, item_click_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *ic = lv_obj_create(it);
    lv_obj_set_size(ic, 68, 68);
    lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(app->color), 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_scrollable(ic, false); lv_obj_set_clickable(ic, false);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *sym = lv_label_create(ic);
    lv_obj_set_style_text_font(sym, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(sym, lv_color_black(), 0);
    lv_label_set_text(sym, app->symbol);
    lv_obj_center(sym);

    lv_obj_t *name = lv_label_create(it);
    lv_obj_set_style_text_color(name, UI_COL_TEXT, 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
    lv_label_set_text(name, app->name);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 94, -12);

    lv_obj_t *st = lv_label_create(it);
    lv_obj_set_style_text_font(st, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(st, app->ready ? UI_COL_ACCENT : UI_COL_MUTED, 0);
    lv_label_set_text(st, app->ready ? "" : "soon");
    lv_obj_align(st, LV_ALIGN_LEFT_MID, 94, 16);
    s_status_lbl[idx] = st;
    return it;
}

static void build_menu(void)
{
    s_menu_scr = ui_screen_create();

    s_list = lv_obj_create(s_menu_scr);
    lv_obj_set_size(s_list, UI_W, UI_H);
    lv_obj_center(s_list);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_list, 14, 0);
    /* top/bottom padding lets the first and last items reach the centre */
    lv_obj_set_style_pad_ver(s_list, UI_H / 2 - 44, 0);
    lv_obj_set_style_pad_left(s_list, 40, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_list, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_list, list_scroll_cb, LV_EVENT_SCROLL, NULL);

    for (int i = 0; i < g_app_count; i++) make_item(s_list, i);

    /* header: clock + battery, floating over the list */
    s_clock_lbl = lv_label_create(s_menu_scr);
    lv_obj_set_style_text_font(s_clock_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_clock_lbl, "--:--");
    lv_obj_align(s_clock_lbl, LV_ALIGN_TOP_MID, 0, 22);

    s_bat_lbl = lv_label_create(s_menu_scr);
    lv_obj_set_style_text_font(s_bat_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_bat_lbl, UI_COL_MUTED, 0);
    lv_label_set_text(s_bat_lbl, "");
    lv_obj_align(s_bat_lbl, LV_ALIGN_BOTTOM_MID, 0, -20);

    /* start centred on the first ready app (stopwatch for now) */
    int start = 0;
    for (int i = 0; i < g_app_count; i++) if (g_apps[i]->ready) { start = i; break; }
    lv_obj_update_layout(s_list);
    lv_obj_scroll_to_view(lv_obj_get_child(s_list, start), LV_ANIM_OFF);
    lv_obj_send_event(s_list, LV_EVENT_SCROLL, NULL);
}

static void refresh_menu_status(void)
{
    datetime_t t;
    sys_get_time(&t);
    lv_label_set_text_fmt(s_clock_lbl, "%02d:%02d", t.hour % 24, t.minute % 60);

    int pct = sys_battery_pct();
    if (pct < 0) lv_label_set_text(s_bat_lbl, LV_SYMBOL_USB);
    else lv_label_set_text_fmt(s_bat_lbl, "%s %d%%",
                               pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3 :
                               pct > 30 ? LV_SYMBOL_BATTERY_2 : pct > 10 ? LV_SYMBOL_BATTERY_1 :
                               LV_SYMBOL_BATTERY_EMPTY, pct);

    for (int i = 0; i < g_app_count; i++) {
        const app_t *a = g_apps[i];
        if (!a->ready || !a->status) continue;
        const char *s = a->status();
        const char *cur = lv_label_get_text(s_status_lbl[i]);
        if (strcmp(cur, s ? s : "") != 0) lv_label_set_text(s_status_lbl[i], s ? s : "");
    }
}

/* ------------------------------------------------------------------ */
/* Event pump: hardware keys -> navigation / app                        */
/* ------------------------------------------------------------------ */
static void show_power_off(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(l, 12, 0);
    lv_label_set_text(l, sys_on_battery_latch() ? "Power off" : "On USB power");
    lv_obj_center(l);
    lv_refr_now(NULL);
}

/* ------------------------------------------------------------------ */
/* Screen timeout                                                       */
/* ------------------------------------------------------------------ */
static bool      s_off;
static lv_obj_t *s_wake_catcher;      /* swallows the touch that wakes the screen */

bool am_screen_off(void) { return s_off; }

void am_screen_wake(void)
{
    lv_display_trigger_activity(NULL);
    if (!s_off) return;
    s_off = false;
    sys_set_backlight(settings_brightness());
    /* a key wake: nothing is pressing the catcher, drop it now */
    lv_indev_t *in = lv_indev_get_next(NULL);
    bool pressing = in && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED;
    if (s_wake_catcher && !pressing) { lv_obj_delete(s_wake_catcher); s_wake_catcher = NULL; }
}

/* The waking touch must not reach the app underneath: the catcher stays until
   that finger is lifted. */
static void catcher_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) { am_screen_wake(); return; }
    if (s_wake_catcher && !s_off) { lv_obj_delete_async(s_wake_catcher); s_wake_catcher = NULL; }
}

/* Backlight off; the next touch (caught, so it reaches no app) or key wakes it. */
static void screen_off_now(void)
{
    if (s_off) return;
    s_off = true;
    sys_set_backlight(0);
    if (!s_cur && !s_on_home) {          /* menu: wake up on the home screen */
        s_on_home = true;
        home_refresh();
        lv_screen_load(s_home_scr);
    }
    s_wake_catcher = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wake_catcher);
    lv_obj_set_size(s_wake_catcher, UI_W, UI_H);
    lv_obj_set_clickable(s_wake_catcher, true);
    lv_obj_add_event_cb(s_wake_catcher, catcher_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_wake_catcher, catcher_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_wake_catcher, catcher_cb, LV_EVENT_PRESS_LOST, NULL);
}

/* The app keeping the screen on, or NULL. */
static const app_t *awake_holder(void)
{
    for (int i = 0; i < g_app_count; i++)
        if (g_apps[i]->ready && g_apps[i]->keep_awake && g_apps[i]->keep_awake()) return g_apps[i];
    return NULL;
}

static void screen_power_tick(void)
{
    uint16_t tmo = cfg_screen_timeout_s();
    if (s_off || tmo == 0 || alert_active()) return;
    if (awake_holder()) { lv_display_trigger_activity(NULL); return; }
    if (lv_display_get_inactive_time(NULL) < (uint32_t)tmo * 1000) return;
    screen_off_now();
}

#if CONFIG_BIKEOS_DEBUG_DIAG
static void diag_tick(void)
{
    static uint32_t last_fixes;
    uint32_t fixes = ride_fix_count();
    const app_t *hold = awake_holder();
    const ride_summary_t *r = ride_current();
    ESP_LOGW("diag", "fixes+%u phone=%ucm/s eff=%ucm/s moving=%d nav=%d conn=%d | ride=%d dist=%um pts=%u store=%d | "
             "screen off=%d idle=%us tmo=%us hold=%s alert=%d home=%d | ram int=%u dma=%u big=%u",
             (unsigned)(fixes - last_fixes), ride_phone_cms(), ride_speed_cms(), ride_moving_now(),
             nav_core_navigating(), nav_core_connected(), (int)ride_state(), (unsigned)r->distance_m,
             (unsigned)r->points, ride_storage_ok(), s_off,
             (unsigned)(lv_display_get_inactive_time(NULL) / 1000), cfg_screen_timeout_s(),
             hold ? hold->name : "-", alert_active(), s_on_home,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    last_fixes = fixes;
}
#endif

/* ------------------------------------------------------------------ */
/* Event pump: hardware keys -> alert / navigation / app                */
/* ------------------------------------------------------------------ */
static void open_centred(void)
{
    lv_area_t ca;
    lv_obj_get_coords(s_list, &ca);
    int32_t cy = ca.y1 + lv_area_get_height(&ca) / 2, best = LV_COORD_MAX;
    int pick = 0;
    for (int i = 0; i < g_app_count; i++) {
        lv_area_t a;
        lv_obj_get_coords(lv_obj_get_child(s_list, i), &a);
        int32_t d = LV_ABS(a.y1 + lv_area_get_height(&a) / 2 - cy);
        if (d < best) { best = d; pick = i; }
    }
    am_open(g_apps[pick]);
}

static void pump_cb(lv_timer_t *t)
{
    static uint32_t n;
    uint32_t ev = sys_take_events();

    if (ev & SYS_EVT_SHUTDOWN) {
        show_power_off();
        vTaskDelay(pdMS_TO_TICKS(600));
        sys_power_off();
    }

    /* a key press while the screen is off only wakes it */
    if (s_off && (ev & (SYS_EVT_PWR_SHORT | SYS_EVT_BOOT_SHORT | SYS_EVT_BOOT_LONG))) {
        am_screen_wake();
        ev = 0;
    } else if (ev) {
        lv_display_trigger_activity(NULL);
    }

    if (alert_active()) {
        if (ev & (SYS_EVT_PWR_SHORT | SYS_EVT_BOOT_SHORT)) alert_close(ALERT_DISMISS);
        else if (ev & SYS_EVT_BOOT_LONG) alert_close(ALERT_SECONDARY);
        ev = 0;
    }

    /* PWR tap: app -> menu -> home -> screen off */
    if (ev & SYS_EVT_PWR_SHORT) {
        if (s_cur) am_home();
        else if (!s_on_home) am_show_home();
        else screen_off_now();
    }
    if (ev & (SYS_EVT_BOOT_SHORT | SYS_EVT_BOOT_LONG)) {
        app_key_t k = (ev & SYS_EVT_BOOT_LONG) ? APP_KEY_SECONDARY : APP_KEY_PRIMARY;
        if (s_cur) {
            bool used = s_cur->ready && s_cur->on_key && s_cur->on_key(k);
            if (!used) am_home();
        } else if (s_on_home) {
            if (k == APP_KEY_PRIMARY) am_show_menu();
        } else if (k == APP_KEY_PRIMARY) {
            open_centred();
        }
    }

    if (n % 2 == 0) {          /* 100 ms: background work for every app */
        for (int i = 0; i < g_app_count; i++)
            if (g_apps[i]->ready && g_apps[i]->bg_tick) g_apps[i]->bg_tick();
        screen_power_tick();
    }
    if (!s_cur && !s_on_home && (n % 10) == 0) refresh_menu_status();   /* ~every 0.5 s */
    if (s_on_home && (n % 4) == 0) home_refresh();                       /* 200 ms: seconds ring */
#if CONFIG_BIKEOS_DEBUG_DIAG
    if (n % 100 == 0) diag_tick();                                       /* 5 s */
#endif
    if (!s_cur && !s_on_home && !s_off &&
        lv_display_get_inactive_time(NULL) > MENU_IDLE_HOME_MS) am_show_home();
    n++;
}

void am_init(void)
{
    lv_display_t *d = lv_display_get_default();
    lv_display_set_theme(d, lv_theme_default_init(d, lv_palette_main(LV_PALETTE_AMBER),
                                               lv_palette_main(LV_PALETTE_RED), true,
                                               &lv_font_montserrat_16));
    build_menu();
    lv_obj_add_event_cb(s_menu_scr, menu_gesture_cb, LV_EVENT_GESTURE, NULL);
    refresh_menu_status();
    s_home_scr = home_create(home_tap, home_nav);
    s_on_home = true;
    lv_screen_load_anim(s_home_scr, LV_SCREEN_LOAD_ANIM_FADE_IN, 300, 0, false);
    sys_set_backlight(settings_brightness());
    lv_timer_create(pump_cb, 50, NULL);
}
