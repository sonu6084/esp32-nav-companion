/*
 * Alarm
 *
 *  List:    up to 6 alarms (time, days, on/off switch); tap a row to edit; + to add
 *  Editor:  hour/minute rollers, S M T W T F S toggles (none = once), 🗑 delete, ✓ save
 *  Keys:    list:   BOOT tap = add            editor: BOOT tap = save, BOOT hold = cancel
 *  Ringing: alert over any app;  ✓ dismiss   "Snooze" = 9 min
 *
 * Alarms live in NVS (settings.c). bg_tick() checks the RTC once per minute.
 * A one-off alarm (no days) switches itself off after it rings.
 */
#include <stdio.h>
#include <string.h>
#include "app_manager.h"
#include "ui_theme.h"
#include "sys.h"
#include "settings.h"
#include "cfg.h"
#include "alert.h"

#define COLOR      0xFF7043
#define SNOOZE_MIN 9

static const char *const DAY1[] = {"S", "M", "T", "W", "T", "F", "S"};
static const char *const DAY3[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static int  s_snooze_tod = -1;      /* minute of the day a snoozed alarm rings again */
static long s_last_key = -1;
static char s_ring_label[8];

/* minutes since 2000-01-01-ish; only used for equality / ordering */
static long minute_key(const datetime_t *t)
{
    return ((((long)t->year * 13 + t->month) * 32 + t->day) * 24 + t->hour) * 60 + t->minute;
}

static void days_text(uint8_t d, char *buf, size_t n)
{
    if (d == 0) { snprintf(buf, n, "Once"); return; }
    if (d == 0x7F) { snprintf(buf, n, "Every day"); return; }
    if (d == 0x3E) { snprintf(buf, n, "Mon - Fri"); return; }
    if (d == 0x41) { snprintf(buf, n, "Weekends"); return; }
    buf[0] = 0;
    for (int i = 1; i <= 7; i++) {             /* list Monday first */
        int b = i % 7;
        if (d & (1 << b)) {
            size_t l = strlen(buf);
            snprintf(buf + l, n - l, "%s%s", l ? " " : "", DAY3[b]);
        }
    }
}

/* ------------------------------ UI ------------------------------ */
static struct {
    bool alive;
    lv_obj_t *scr, *list, *add, *empty;
    lv_obj_t *ed, *r_h, *r_m, *day_btn[7];
    int ed_idx;                 /* slot being edited */
    uint8_t ed_days;
} ui;

static void build_list(void);

static void row_click_cb(lv_event_t *e);
static void switch_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    alarm_t a = cfg_alarms()[i];
    a.enabled = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    cfg_set_alarm(i, &a);
}

static void ed_close(bool save)
{
    if (!ui.ed) return;
    if (save) {
        alarm_t a = { .enabled = true, .hour = lv_roller_get_selected(ui.r_h),
                      .minute = lv_roller_get_selected(ui.r_m), .days = ui.ed_days };
        cfg_set_alarm(ui.ed_idx, &a);
    }
    lv_obj_delete(ui.ed);
    ui.ed = NULL;
    build_list();
}

static void ed_save_cb(lv_event_t *e) { ed_close(true); }
static void ed_del_cb(lv_event_t *e)
{
    cfg_set_alarm(ui.ed_idx, NULL);
    ed_close(false);
}
static void day_cb(lv_event_t *e)
{
    int b = (int)(intptr_t)lv_event_get_user_data(e);
    ui.ed_days ^= (1 << b);
    bool on = ui.ed_days & (1 << b);
    lv_obj_set_style_bg_color(ui.day_btn[b], on ? lv_color_hex(COLOR) : UI_COL_SURFACE, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(ui.day_btn[b], 0), on ? lv_color_black() : UI_COL_TEXT, 0);
}

static void ed_open(int idx)
{
    if (idx < 0 || ui.ed) return;
    ui.ed_idx = idx;
    const alarm_t *a = &cfg_alarms()[idx];
    bool is_new = a->hour == 0xFF;
    datetime_t now;
    sys_get_time(&now);
    int h = is_new ? (now.hour + 1) % 24 : a->hour, m = is_new ? 0 : a->minute;
    ui.ed_days = is_new ? 0x3E : a->days;

    ui.ed = lv_obj_create(ui.scr);
    lv_obj_remove_style_all(ui.ed);
    lv_obj_set_size(ui.ed, UI_W, UI_H);
    lv_obj_set_style_bg_color(ui.ed, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(ui.ed, LV_OPA_COVER, 0);
    lv_obj_set_clickable(ui.ed, true);
    lv_obj_set_scrollable(ui.ed, false);

    lv_obj_t *t = lv_label_create(ui.ed);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR), 0);
    lv_label_set_text(t, is_new ? "NEW ALARM" : "EDIT ALARM");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 36);

    static char oh[24 * 3], om[60 * 3];
    ui.r_h = ui_roller(ui.ed, ui_num_opts(oh, 0, 23, true), 3, &lv_font_montserrat_32, 90);
    lv_obj_align(ui.r_h, LV_ALIGN_CENTER, -58, -62);
    ui.r_m = ui_roller(ui.ed, ui_num_opts(om, 0, 59, true), 3, &lv_font_montserrat_32, 90);
    lv_obj_align(ui.r_m, LV_ALIGN_CENTER, 58, -62);
    lv_obj_t *c = lv_label_create(ui.ed);
    lv_obj_set_style_text_font(c, &lv_font_montserrat_32, 0);
    lv_label_set_text(c, ":");
    lv_obj_align(c, LV_ALIGN_CENTER, 0, -64);
    lv_roller_set_selected(ui.r_h, h, LV_ANIM_OFF);
    lv_roller_set_selected(ui.r_m, m, LV_ANIM_OFF);

    for (int i = 0; i < 7; i++) {              /* Monday first */
        int b = (i + 1) % 7;
        bool on = ui.ed_days & (1 << b);
        lv_obj_t *d = ui_round_btn(ui.ed, 40, on ? lv_color_hex(COLOR) : UI_COL_SURFACE, DAY1[b], &lv_font_montserrat_16);
        lv_obj_set_style_text_color(lv_obj_get_child(d, 0), on ? lv_color_black() : UI_COL_TEXT, 0);
        lv_obj_set_ext_click_area(d, 2);
        lv_obj_align(d, LV_ALIGN_CENTER, -138 + i * 46, 50);
        lv_obj_add_event_cb(d, day_cb, LV_EVENT_CLICKED, (void *)(intptr_t)b);
        ui.day_btn[b] = d;
    }

    lv_obj_t *del = ui_round_btn(ui.ed, 70, UI_COL_RED, LV_SYMBOL_TRASH, &lv_font_montserrat_28);
    lv_obj_align(del, LV_ALIGN_CENTER, -62, 136);
    lv_obj_add_event_cb(del, ed_del_cb, LV_EVENT_CLICKED, NULL);
    if (is_new) lv_obj_set_hidden(del, true);
    lv_obj_t *ok = ui_round_btn(ui.ed, 70, UI_COL_GREEN, LV_SYMBOL_OK, &lv_font_montserrat_28);
    lv_obj_align(ok, LV_ALIGN_CENTER, is_new ? 0 : 62, 136);
    lv_obj_add_event_cb(ok, ed_save_cb, LV_EVENT_CLICKED, NULL);
}

static int free_slot(void)
{
    for (int i = 0; i < ALARM_MAX; i++) if (cfg_alarms()[i].hour == 0xFF) return i;
    return -1;
}

static void add_cb(lv_event_t *e) { ed_open(free_slot()); }
static void row_click_cb(lv_event_t *e) { ed_open((int)(intptr_t)lv_event_get_user_data(e)); }

static void build_list(void)
{
    lv_obj_clean(ui.list);
    const alarm_t *al = cfg_alarms();

    /* sort slots by time for display */
    int order[ALARM_MAX], n = 0;
    for (int i = 0; i < ALARM_MAX; i++) if (al[i].hour != 0xFF) order[n++] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && al[order[j]].hour * 60 + al[order[j]].minute <
                                 al[order[j - 1]].hour * 60 + al[order[j - 1]].minute; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }

    for (int k = 0; k < n; k++) {
        int i = order[k];
        lv_obj_t *row = lv_obj_create(ui.list);
        lv_obj_set_size(row, 300, 74);
        lv_obj_set_style_radius(row, 37, 0);
        lv_obj_set_style_bg_color(row, UI_COL_SURFACE, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_scrollable(row, false);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *tl = lv_label_create(row);
        lv_obj_set_style_text_font(tl, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(tl, al[i].enabled ? UI_COL_TEXT : UI_COL_MUTED, 0);
        lv_label_set_text_fmt(tl, "%02d:%02d", al[i].hour, al[i].minute);
        lv_obj_align(tl, LV_ALIGN_LEFT_MID, 26, -9);
        char d[40];
        days_text(al[i].days, d, sizeof d);
        lv_obj_t *dl = lv_label_create(row);
        lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(dl, al[i].enabled ? lv_color_hex(COLOR) : UI_COL_MUTED, 0);
        lv_label_set_text(dl, d);
        lv_obj_align(dl, LV_ALIGN_LEFT_MID, 28, 20);

        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 64, 34);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -20, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_ext_click_area(sw, 8);
        if (al[i].enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }

    if (n == 0) lv_obj_set_hidden(ui.empty, false);
    else lv_obj_set_hidden(ui.empty, true);
    if (free_slot() < 0) lv_obj_set_hidden(ui.add, true);
    else lv_obj_set_hidden(ui.add, false);
}

static void al_create(lv_obj_t *scr, const app_t *self)
{
    memset(&ui, 0, sizeof ui);
    ui.alive = true;
    ui.scr = scr;

    lv_obj_t *t = lv_label_create(scr);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR), 0);
    lv_label_set_text(t, "ALARMS");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 36);

    ui.list = lv_obj_create(scr);
    lv_obj_set_size(ui.list, 320, 262);
    lv_obj_align(ui.list, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_bg_opa(ui.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.list, 0, 0);
    lv_obj_set_style_pad_all(ui.list, 2, 0);
    lv_obj_set_style_pad_row(ui.list, 10, 0);
    lv_obj_set_flex_flow(ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui.list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(ui.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.list, LV_SCROLLBAR_MODE_OFF);

    ui.empty = lv_label_create(scr);
    lv_obj_set_style_text_color(ui.empty, UI_COL_MUTED, 0);
    lv_obj_set_style_text_align(ui.empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(ui.empty, "No alarms\nTap + to add one");
    lv_obj_align(ui.empty, LV_ALIGN_CENTER, 0, -20);

    ui.add = ui_round_btn(scr, 64, lv_color_hex(COLOR), LV_SYMBOL_PLUS, &lv_font_montserrat_28);
    lv_obj_align(ui.add, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_add_event_cb(ui.add, add_cb, LV_EVENT_CLICKED, NULL);

    build_list();
}

static void al_destroy(void) { memset(&ui, 0, sizeof ui); }

static bool al_on_key(app_key_t k)
{
    if (ui.ed) ed_close(k == APP_KEY_PRIMARY);
    else if (k == APP_KEY_PRIMARY) ed_open(free_slot());
    else return false;
    return true;
}

/* ------------------------------ ringing ------------------------------ */
static void ring_cb(alert_action_t a)
{
    if (a == ALERT_SECONDARY) {
        datetime_t now;
        sys_get_time(&now);
        s_snooze_tod = (now.hour * 60 + now.minute + SNOOZE_MIN) % (24 * 60);
    }
}

static void ring(const char *label)
{
    snprintf(s_ring_label, sizeof s_ring_label, "%s", label);
    alert_show("ALARM", s_ring_label, COLOR, SND_ALARM, "Snooze", ring_cb);
}

static void al_bg_tick(void)
{
    datetime_t now;
    sys_get_time(&now);
    long key = minute_key(&now);
    if (key == s_last_key) return;
    bool first = s_last_key < 0;
    s_last_key = key;
    if (first) return;                       /* don't ring for the boot minute */

    if (s_snooze_tod >= 0 && now.hour * 60 + now.minute == s_snooze_tod) {
        s_snooze_tod = -1;
        char b[8];
        snprintf(b, sizeof b, "%02d:%02d", now.hour, now.minute);
        ring(b);
        return;
    }

    int dow = sys_weekday(now.year, now.month ? now.month : 1, now.day ? now.day : 1);
    const alarm_t *al = cfg_alarms();
    for (int i = 0; i < ALARM_MAX; i++) {
        const alarm_t *a = &al[i];
        if (a->hour == 0xFF || !a->enabled) continue;
        if (a->hour != now.hour || a->minute != now.minute) continue;
        if (a->days && !(a->days & (1 << dow))) continue;
        if (a->days == 0) {                   /* one-off: switch off after ringing */
            alarm_t off = *a;
            off.enabled = false;
            cfg_set_alarm(i, &off);
            if (ui.alive && !ui.ed) build_list();
        }
        s_snooze_tod = -1;
        char b[8];
        snprintf(b, sizeof b, "%02d:%02d", a->hour, a->minute);
        ring(b);
        return;
    }
}

static const char *al_status(void)
{
    static char buf[24];
    datetime_t now;
    sys_get_time(&now);
    int dow = sys_weekday(now.year, now.month ? now.month : 1, now.day ? now.day : 1);
    int now_m = now.hour * 60 + now.minute;
    int best = 1 << 30, bi = -1, bd = 0;
    const alarm_t *al = cfg_alarms();
    for (int i = 0; i < ALARM_MAX; i++) {
        const alarm_t *a = &al[i];
        if (a->hour == 0xFF || !a->enabled) continue;
        for (int d = 0; d < 8; d++) {        /* first matching day from today */
            int wd = (dow + d) % 7;
            int m = d * 1440 + a->hour * 60 + a->minute - now_m;
            if (m <= 0) continue;
            if (a->days && !(a->days & (1 << wd))) continue;
            if (m < best) { best = m; bi = i; bd = wd; }
            break;
        }
    }
    if (bi < 0) return NULL;
    if (best < 24 * 60) snprintf(buf, sizeof buf, LV_SYMBOL_BELL " %02d:%02d", al[bi].hour, al[bi].minute);
    else snprintf(buf, sizeof buf, LV_SYMBOL_BELL " %s %02d:%02d", DAY3[bd], al[bi].hour, al[bi].minute);
    return buf;
}

const app_t app_alarm = {
    .name    = "Alarm",
    .symbol  = LV_SYMBOL_BELL,
    .color   = COLOR,
    .create  = al_create,
    .destroy = al_destroy,
    .on_key  = al_on_key,
    .status  = al_status,
    .bg_tick = al_bg_tick,
    .ready   = true,
};
