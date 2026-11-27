#include "nav_view.h"
#include "map_view.h"  // bike-os: rejoin distance
#include "board.h"
#include "display.h"
#include "ui_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// Bottom-centred banner. On a round panel the usable width shrinks towards
// the bottom. For the 480x480 design: at y=410 the visible chord is ~339 px,
// so 340 px with the bottom edge resting there has its corners just meet the
// bezel instead of being clipped (the guide's geometry table).
// Scaled for the fitted panel below; at 412x412 the chord at the banner's
// bottom edge is ~291 px, so 336 design px (288 real) keeps a small margin.
#define BANNER_W UI_SC(336)
#define BANNER_H UI_SC(84)
#define BANNER_RADIUS UI_SC(16)
#define BANNER_X ((BOARD_LCD_W - BANNER_W) / 2)
#define BANNER_BOTTOM UI_SC(410)
#define BANNER_Y (BANNER_BOTTOM - BANNER_H)
#define ICON_CX UI_SC(44)    // banner-local centre of the maneuver icon
#define ICON_CY (BANNER_H / 2)
#define ICON_D UI_SC(64)
#define TEXT_X UI_SC(88)     // banner-local left edge of the text column
#define TEXT_W (BANNER_W - TEXT_X - UI_SC(12))
#define STALE_MS 6000         // no update for this long -> treat as ended

#define COL_BG 0x1B2027
#define COL_ICON 0xF1F3F4
#define COL_STREET 0xFFFFFF
#define COL_DIST 0xA8B0B8
#define COL_WARN 0xF28B82
#define MAX_DIST_LABEL 8

// ---- persistent state (survives map screen rebuilds) ----
static nav_state_t s_st;
static char s_street[NAV_TEXT_MAX + 1];
static bool s_have;
static uint32_t s_last_ms;
static bool s_minimal;
static bool s_suppressed;
static uint16_t s_limit;
static bool s_mph;
static uint32_t s_total_m;  // longest remaining distance seen this trip

// ---- UI ----
static lv_obj_t *s_banner, *s_lbl_street, *s_lbl_dist, *s_lbl_left;
// bike-os: minimal display layout - street name and ETA at the top, turn and
// distance at the bottom (the bottom band no longer carries the street).
static lv_obj_t *s_top, *s_top_street, *s_top_eta;
#define TOP_Y 50                 // below the bezel's narrow top: the chord is ~300 px at y 70
#define TOP_W 300
#define TOP_H 76
static void (*s_click_cb)(void);
// bike-os: turn card for a drive with no route line (NAV_F_TURN_ONLY): a large
// arrow, the distance, the street over two lines and the ETA line, centred for
// the round panel. Not clickable, so swipes reach the map's faces underneath.
static lv_obj_t *s_card, *s_card_dist, *s_card_street, *s_card_eta;
// Real panel pixels (412 x 412), not design space: checked against the circle.
#define CARD_ICON_CY 118     // the arrow spans ~42..194 at CARD_ICON_PCT
#define CARD_ICON_PCT 210
#define CARD_DIST_Y 200
#define CARD_STREET_Y 258
#define CARD_STREET_W 300    // the chord is ~338 px at the second line's foot (y 324)
#define CARD_ETA_Y 336
#define CARD_ETA_W 270       // ~274 px at y 360

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// "450 m", "1.2 km" - compact, so it fits next to the street name.
static void format_dist(char *out, size_t cap, uint16_t m)
{
    if (m >= 1000) snprintf(out, cap, "%u.%u km", m / 1000, (m % 1000) / 100);
    else snprintf(out, cap, "%u m", m);
}

// ---------------------------------------------------------------- icons
// Authored in a 100x100 box centred on (cx, cy), then scaled by ICON_PCT so a
// single knob shrinks every icon without touching the text.
#define ICON_PCT 90
// The same icons at this scale preview the turn after next ("then").
#define THEN_PCT 44
static int s_icon_pct = ICON_PCT;
static int ip(int v) { return (v * s_icon_pct) / 100; }

// A stroke is a thick line with rounded caps; heads are triangles. Lines must
// use lv_draw_line (true rotated capsules) - axis-aligned rects only work for
// horizontal/vertical strokes and turn into blobs on every diagonal.
typedef struct {
    lv_color_t color;
    int w;
} icon_pen_t;

static void pen_init(icon_pen_t *p, lv_color_t col, int w)
{
    p->color = col;
    p->w = w;
}

// Straight segment from (x1,y1) to (x2,y2) with rounded caps.
static void seg(lv_layer_t *l, const icon_pen_t *p, int x1, int y1, int x2, int y2)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = p->color;
    d.opa = LV_OPA_COVER;
    d.width = p->w;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = x1; d.p1.y = y1;
    d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(l, &d);
}

static void arrow_head(lv_layer_t *l, const icon_pen_t *p, int x, int y, float dir_deg)
{
    // Triangle pointing along dir_deg (0 = up, 90 = right).
    float a = dir_deg * (float)M_PI / 180.0f;
    float s = sinf(a), c = cosf(a);
    const int tip = ip(22), back = ip(12), wid = ip(13);
    lv_point_precise_t v[3];
    v[0].x = x + s * tip;      v[0].y = y - c * tip;
    v[1].x = x - s * back + c * wid; v[1].y = y + c * back + s * wid;
    v[2].x = x - s * back - c * wid; v[2].y = y + c * back - s * wid;
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = p->color;
    t.opa = LV_OPA_COVER;
    t.p[0] = v[0]; t.p[1] = v[1]; t.p[2] = v[2];
    lv_draw_triangle(l, &t);
}

// shaft + head, the base of every straight-ahead icon
static void straight(lv_layer_t *l, const icon_pen_t *p, int cx, int cy)
{
    seg(l, p, cx, cy + ip(34), cx, cy - ip(12));
    arrow_head(l, p, cx, cy - ip(14), 0.0f);
}

static void draw_icon(lv_layer_t *l, int cx, int cy, uint8_t type)
{
    icon_pen_t p;
    bool warn = type == NAV_OFF_ROUTE;
    pen_init(&p, lv_color_hex(warn ? COL_WARN : COL_ICON), ip(13));
    int w = p.w;

    switch (type) {
    case NAV_CONTINUE:
        straight(l, &p, cx, cy);
        break;
    case NAV_SLIGHT_LEFT:
        seg(l, &p, cx, cy + ip(36), cx, cy - ip(6));
        seg(l, &p, cx, cy - ip(6), cx - ip(16), cy - ip(22));
        arrow_head(l, &p, cx - ip(18), cy - ip(24), -40.0f);
        break;
    case NAV_LEFT:
        seg(l, &p, cx, cy + ip(36), cx, cy - ip(8));
        seg(l, &p, cx, cy - ip(8), cx - ip(18), cy - ip(8));
        arrow_head(l, &p, cx - ip(20), cy - ip(8), -90.0f);
        break;
    case NAV_SHARP_LEFT:
        seg(l, &p, cx, cy + ip(36), cx, cy - ip(6));
        seg(l, &p, cx, cy - ip(6), cx - ip(20), cy + ip(10));
        arrow_head(l, &p, cx - ip(22), cy + ip(12), -130.0f);
        break;
    case NAV_SLIGHT_RIGHT:
        seg(l, &p, cx, cy + ip(36), cx, cy - ip(6));
        seg(l, &p, cx, cy - ip(6), cx + ip(16), cy - ip(22));
        arrow_head(l, &p, cx + ip(18), cy - ip(24), 40.0f);
        break;
    case NAV_RIGHT:
        seg(l, &p, cx, cy + ip(36), cx, cy - ip(8));
        seg(l, &p, cx, cy - ip(8), cx + ip(18), cy - ip(8));
        arrow_head(l, &p, cx + ip(20), cy - ip(8), 90.0f);
        break;
    case NAV_SHARP_RIGHT:
        seg(l, &p, cx, cy + ip(36), cx, cy - ip(6));
        seg(l, &p, cx, cy - ip(6), cx + ip(20), cy + ip(10));
        arrow_head(l, &p, cx + ip(22), cy + ip(12), 130.0f);
        break;
    case NAV_KEEP_LEFT:
        seg(l, &p, cx + ip(10), cy + ip(36), cx + ip(10), cy - ip(6));
        seg(l, &p, cx + ip(10), cy - ip(6), cx - ip(18), cy - ip(6));
        arrow_head(l, &p, cx - ip(20), cy - ip(6), -90.0f);
        break;
    case NAV_KEEP_RIGHT:
        seg(l, &p, cx - ip(10), cy + ip(36), cx - ip(10), cy - ip(6));
        seg(l, &p, cx - ip(10), cy - ip(6), cx + ip(18), cy - ip(6));
        arrow_head(l, &p, cx + ip(20), cy - ip(6), 90.0f);
        break;
    case NAV_UTURN: {
        // Up, back down, then a downward head: a hairpin.
        seg(l, &p, cx + ip(14), cy + ip(36), cx + ip(14), cy - ip(14));
        seg(l, &p, cx + ip(14), cy - ip(14), cx - ip(10), cy - ip(14));
        seg(l, &p, cx - ip(10), cy - ip(14), cx - ip(10), cy + ip(8));
        arrow_head(l, &p, cx - ip(10), cy + ip(10), 180.0f);
        break;
    }
    case NAV_ROUNDABOUT: {
        // A ring with a gap on the right, and an exit arrow out of the top right.
        lv_draw_arc_dsc_t a;
        lv_draw_arc_dsc_init(&a);
        a.color = p.color;
        a.opa = LV_OPA_COVER;
        a.width = p.w;
        a.center.x = cx;
        a.center.y = cy;
        a.radius = ip(22);
        a.start_angle = 0;
        a.end_angle = 270;
        lv_draw_arc(l, &a);
        seg(l, &p, cx + ip(12), cy - ip(12), cx + ip(26), cy - ip(22));
        arrow_head(l, &p, cx + ip(27), cy - ip(23), 40.0f);
        break;
    }
    case NAV_OFF_ROUTE: {
        // Exclamation: this is the one icon that must read as "stop".
        seg(l, &p, cx, cy - ip(28), cx, cy + ip(2));
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = p.color;
        d.bg_opa = LV_OPA_COVER;
        d.radius = LV_RADIUS_CIRCLE;
        lv_area_t dot = {cx - w, cy + ip(14), cx + w, cy + ip(14) + w * 2};
        lv_draw_rect(l, &d, &dot);
        break;
    }
    case NAV_ARRIVE:
    case NAV_NONE:
    default: {
        // Arrival: a filled disc, so "you are here" can't be mistaken for a turn.
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = p.color;
        d.bg_opa = LV_OPA_COVER;
        d.radius = LV_RADIUS_CIRCLE;
        int r = ip(26);
        lv_area_t a = {cx - r, cy - r, cx + r, cy + r};
        lv_draw_rect(l, &d, &a);
        break;
    }
    }
}

// ---------------------------------------------------------------- banner

// The line under the distance. The phone sends the road being entered, or its
// own instruction when the road has no name. If it sends nothing at all, say
// what the maneuver is: that is always true, where "unnamed road" was noise.
static const char *street_line(void)
{
    static const char *const words[NAV_MANEUVER_MAX + 1] = {
        [NAV_NONE] = "",
        [NAV_CONTINUE] = "Continue",
        [NAV_SLIGHT_LEFT] = "Slight left",
        [NAV_LEFT] = "Turn left",
        [NAV_SHARP_LEFT] = "Sharp left",
        [NAV_SLIGHT_RIGHT] = "Slight right",
        [NAV_RIGHT] = "Turn right",
        [NAV_SHARP_RIGHT] = "Sharp right",
        [NAV_KEEP_LEFT] = "Keep left",
        [NAV_KEEP_RIGHT] = "Keep right",
        [NAV_UTURN] = "Make a U-turn",
        [NAV_ROUNDABOUT] = "At the roundabout",
        [NAV_OFF_ROUTE] = "Finding a new route",
        [NAV_ARRIVE] = "Your destination",
    };
    if (s_street[0]) return s_street;
    return s_st.maneuver <= NAV_MANEUVER_MAX ? words[s_st.maneuver] : "";
}

// Two turns close together: the one after next is previewed beside the next,
// so a quick left-then-right is seen as one move. Only when the second follows
// within THEN_MAX_M of the first; further apart, it gets its own turn in time.
#define THEN_MAX_M 250
#define THEN_CX (BANNER_W - 34)
#define THEN_CY 52
#define THEN_ROOM 56   // width given up by the text beside it

static bool then_shown(void)
{
    return s_st.maneuver != NAV_ARRIVE && s_st.maneuver != NAV_OFF_ROUTE &&
           s_st.after_maneuver != NAV_NONE && s_st.after_maneuver <= NAV_MANEUVER_MAX &&
           s_st.after_dist_m <= THEN_MAX_M;
}

// bike-os: "ETA 10:50 . 27 min . 10.7 km" (the clock part only once the phone
// has sent its time).
static void eta_text(char *eta, size_t cap)
{
    char left[20], mins[16];
    eta[0] = 0;
    uint32_t rm = s_st.remaining_m;
    if (rm >= 1000) snprintf(left, sizeof(left), "%.1f km", rm / 1000.0f);
    else snprintf(left, sizeof(left), "%u m", (unsigned)rm);
    int mn = (s_st.remaining_s + 30) / 60;
    if (mn >= 60) snprintf(mins, sizeof(mins), "%dh %02dm", mn / 60, mn % 60);
    else snprintf(mins, sizeof(mins), "%d min", mn);
    uint32_t now = map_view_clock_now();
    if (now && s_st.maneuver != NAV_ARRIVE) {
        uint32_t m = (now + s_st.remaining_s) / 60 % 1440;
        unsigned h = (unsigned)(m / 60), mm = (unsigned)(m % 60);
        if (map_view_clock_24h()) snprintf(eta, cap, "ETA %02u:%02u " LV_SYMBOL_BULLET " ", h, mm);
        else snprintf(eta, cap, "ETA %u:%02u%s " LV_SYMBOL_BULLET " ", h % 12 ? h % 12 : 12, mm, h < 12 ? "am" : "pm");
    }
    size_t o = strlen(eta);
    snprintf(eta + o, cap - o, "%s " LV_SYMBOL_BULLET " %s", mins, left);
}

static void refresh(void)
{
    if (!s_banner) return;
    bool active = s_have && (s_st.flags & NAV_F_ACTIVE) &&
                  (now_ms() - s_last_ms) < STALE_MS;
    // bike-os: a drive with no route line gets the turn card in place of the banner.
    bool card = active && !s_suppressed && (s_st.flags & NAV_F_TURN_ONLY);
    if (s_card) lv_obj_set_hidden(s_card, !card);
    lv_obj_set_hidden(s_banner, !active || s_suppressed || card);
    if (s_top) lv_obj_set_hidden(s_top, !active || s_suppressed || !s_minimal || card);  // bike-os
    if (!active) return;
    if (card) {
        char buf[MAX_DIST_LABEL + 4] = "";
        if (s_st.maneuver == NAV_ARRIVE) snprintf(buf, sizeof(buf), "Arrived");
        // 0 is "not known yet" (the first step of a trip), not "now".
        else if (s_st.dist_m) format_dist(buf, sizeof(buf), s_st.dist_m);
        lv_label_set_text(s_card_dist, buf);
        char eta[64];
        eta_text(eta, sizeof(eta));
        lv_label_set_text(s_card_eta, eta);
        return;
    }
    if (s_top && s_minimal) {  // bike-os: "ETA 10:50 . 27 min . 10.7 km"
        char eta[64];
        eta_text(eta, sizeof(eta));
        lv_label_set_text(s_top_eta, eta);
    }

    char buf[MAX_DIST_LABEL + 4];
    if (s_st.maneuver == NAV_ARRIVE) {
        snprintf(buf, sizeof(buf), "Arrived");
    } else if (s_st.maneuver == NAV_OFF_ROUTE) {
        // bike-os: how far back to the route (the dots on the map lead there)
        int rj = map_view_rejoin_m();
        if (rj >= 0) format_dist(buf, sizeof(buf), (uint16_t)(rj > 65535 ? 65535 : rj));
        else snprintf(buf, sizeof(buf), "Off route");
    } else {
        format_dist(buf, sizeof(buf), s_st.dist_m);
    }
    lv_label_set_text(s_lbl_dist, buf);

    uint32_t rm = s_st.remaining_m;
    char left[24];
    if (rm >= 10000) snprintf(left, sizeof(left), "%.1f km left", rm / 1000.0f);
    else snprintf(left, sizeof(left), "%u m left", (unsigned)rm);
    lv_label_set_text(s_lbl_left, left);
    // The preview takes the right-hand end of the banner: the text narrows to
    // make room, and the distance-left figure (which lives there) steps aside.
    bool then = then_shown();
    lv_obj_set_hidden(s_lbl_left, s_minimal || then);
    static int then_was = -1;
    if (then_was != (int)then || lv_obj_get_width(s_lbl_street) != TEXT_W - (then ? THEN_ROOM : 0)) {
        then_was = then;
        lv_obj_set_width(s_lbl_street, TEXT_W - (then ? THEN_ROOM : 0));
        lv_obj_set_width(s_lbl_dist, TEXT_W - (then ? THEN_ROOM : 0));
        lv_obj_invalidate(s_banner);
    }
}

// Normal: a dark box, street over distance. Minimal: no box, a large distance
// over a dimmer street name.
static void layout(void)
{
    if (!s_banner) return;
    // Minimal: no box of its own. The map screen lays a translucent black band
    // under it that runs to the bottom of the screen (nav_view_banner_top).
    lv_obj_set_style_bg_opa(s_banner, s_minimal ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    // bike-os: minimal = turn icon + a large distance only; the street moved to the top.
    lv_obj_set_style_text_font(s_lbl_dist,
                               s_minimal ? &lv_font_montserrat_48 : &lv_font_montserrat_28, 0);
    lv_obj_align(s_lbl_dist, LV_ALIGN_TOP_LEFT, TEXT_X, s_minimal ? (BANNER_H - 56) / 2 : UI_SC(40));
    lv_obj_set_hidden(s_lbl_street, s_minimal);
    lv_obj_align(s_lbl_street, LV_ALIGN_TOP_LEFT, TEXT_X, s_minimal ? UI_SC(54) : UI_SC(10));
    lv_obj_set_style_text_color(s_lbl_street, lv_color_hex(s_minimal ? COL_DIST : COL_STREET), 0);
    lv_obj_invalidate(s_banner);
}

int nav_view_banner_top(void)
{
    return BANNER_Y;
}

void nav_view_set_suppressed(bool on)
{
    s_suppressed = on;
    refresh();
}

void nav_view_set_limit(bool mph, uint16_t limit)
{
    s_mph = mph;
    s_limit = limit;
}

uint16_t nav_view_limit(void)
{
    return s_limit;
}

bool nav_view_mph(void)
{
    return s_mph;
}

int nav_view_dist_m(void)
{
    return nav_view_active() ? s_st.dist_m : -1;
}

uint8_t nav_view_maneuver(void)
{
    return nav_view_active() ? s_st.maneuver : NAV_NONE;
}

int nav_view_remaining_m(void)
{
    return nav_view_active() ? (int)s_st.remaining_m : -1;
}

void nav_view_set_minimal(bool on)
{
    s_minimal = on;
    layout();
    refresh();
}

int nav_view_progress_permille(void)
{
    if (!nav_view_active() || !s_total_m) return -1;
    if (s_st.maneuver == NAV_ARRIVE) return 1000;
    return 1000 - (int)((uint64_t)s_st.remaining_m * 1000u / s_total_m);
}

static void banner_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_target(e);
    // Draw events use ABSOLUTE (screen) coordinates: the layer's clip area is
    // the object's absolute coords (see map_view's marker). Drawing at the
    // banner-local icon point falls outside that clip and vanishes, so offset
    // by the banner's own position.
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    draw_icon(layer, c.x1 + ICON_CX, c.y1 + ICON_CY, s_st.maneuver);
    if (then_shown()) {
        // "then" over a small copy of the following turn's icon, at the right.
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.text = "then";
        d.font = &lv_font_montserrat_14;
        d.color = lv_color_hex(COL_DIST);
        d.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t a = {c.x1 + THEN_CX - 30, c.y1 + 6, c.x1 + THEN_CX + 30, c.y1 + 24};
        lv_draw_label(layer, &d, &a);
        s_icon_pct = THEN_PCT;
        draw_icon(layer, c.x1 + THEN_CX, c.y1 + THEN_CY, s_st.after_maneuver);
        s_icon_pct = ICON_PCT;
    }
}

// bike-os: the turn card's arrow, large, above the distance.
static void card_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_target(e), &c);
    s_icon_pct = CARD_ICON_PCT;
    draw_icon(layer, c.x1 + BOARD_LCD_W / 2, c.y1 + CARD_ICON_CY, s_st.maneuver);
    s_icon_pct = ICON_PCT;
}

static lv_obj_t *card_label(const lv_font_t *font, uint32_t col, int y, int w, int lines)
{
    lv_obj_t *l = lv_label_create(s_card);
    lv_obj_set_size(l, w, font->line_height * lines);
    lv_label_set_long_mode(l, lines > 1 ? LV_LABEL_LONG_MODE_DOTS : LV_LABEL_LONG_MODE_SCROLL);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, "");
    return l;
}

static void banner_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_click_cb) s_click_cb();
}

void nav_view_set_click_cb(void (*cb)(void))
{
    s_click_cb = cb;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t col, int y,
                          lv_label_long_mode_t mode)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, TEXT_W);
    lv_label_set_long_mode(l, mode);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, TEXT_X, y);  // banner-local
    return l;
}

void nav_view_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    s_banner = lv_obj_create(scr);
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_size(s_banner, BANNER_W, BANNER_H);
    lv_obj_set_pos(s_banner, BANNER_X, BANNER_Y);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_banner, BANNER_RADIUS, 0);
    lv_obj_set_clickable(s_banner, true);
    lv_obj_add_event_cb(s_banner, banner_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_banner, banner_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    // Street name is a single line that scrolls (ping-pong) only when it is
    // too long for the column; short names stay put. Distance/remaining clip.
    s_lbl_street = mk_label(s_banner, &lv_font_montserrat_20, COL_STREET, 10,
                            LV_LABEL_LONG_SCROLL);
    s_lbl_dist = mk_label(s_banner, &lv_font_montserrat_28, COL_ICON, 40,
                          LV_LABEL_LONG_DOT);
    s_lbl_left = mk_label(s_banner, &lv_font_montserrat_14, COL_DIST, 52,
                          LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_left, "");
    // The box is right-aligned, but the text inside defaults to left-aligned
    // and would draw at the same x as the big distance, overlapping it.
    lv_obj_set_style_text_align(s_lbl_left, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_lbl_left, LV_ALIGN_TOP_RIGHT, -UI_SC(12), UI_SC(52));  // banner-local

    // bike-os: top block for the minimal display (street over ETA), centred so
    // the round edge never clips it.
    s_top = lv_obj_create(scr);
    lv_obj_remove_style_all(s_top);
    lv_obj_set_size(s_top, TOP_W, TOP_H);
    lv_obj_align(s_top, LV_ALIGN_TOP_MID, 0, TOP_Y);
    lv_obj_set_clickable(s_top, false);
    s_top_street = lv_label_create(s_top);
    lv_obj_set_width(s_top_street, TOP_W - 20);
    lv_label_set_long_mode(s_top_street, LV_LABEL_LONG_MODE_SCROLL);
    lv_obj_set_style_text_font(s_top_street, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_top_street, lv_color_hex(COL_STREET), 0);
    lv_obj_set_style_text_align(s_top_street, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_top_street, LV_ALIGN_TOP_MID, 0, 6);
    lv_label_set_text(s_top_street, street_line());
    s_top_eta = lv_label_create(s_top);
    lv_obj_set_size(s_top_eta, TOP_W, lv_font_montserrat_20.line_height);   /* one line */
    lv_label_set_long_mode(s_top_eta, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_top_eta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_top_eta, lv_color_hex(COL_DIST), 0);
    lv_obj_set_style_text_align(s_top_eta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_top_eta, LV_ALIGN_TOP_MID, 0, 42);
    lv_label_set_text(s_top_eta, "");
    lv_obj_set_hidden(s_top, true);

    // bike-os: the turn card covers the whole round panel in black.
    s_card = lv_obj_create(scr);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, BOARD_LCD_W, BOARD_LCD_H);
    lv_obj_set_pos(s_card, 0, 0);
    lv_obj_set_style_bg_color(s_card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_card, card_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    s_card_dist = card_label(&lv_font_montserrat_48, COL_ICON, CARD_DIST_Y, CARD_STREET_W, 1);
    s_card_street = card_label(&lv_font_montserrat_28, COL_STREET, CARD_STREET_Y, CARD_STREET_W, 2);
    lv_label_set_text(s_card_street, street_line());
    s_card_eta = card_label(&lv_font_montserrat_20, COL_DIST, CARD_ETA_Y, CARD_ETA_W, 1);
    lv_obj_set_hidden(s_card, true);

    layout();
    refresh();
}

void nav_view_destroy(void)
{
    s_banner = s_lbl_street = s_lbl_dist = s_lbl_left = NULL;
    s_top = s_top_street = s_top_eta = NULL;  // bike-os
    s_card = s_card_dist = s_card_street = s_card_eta = NULL;  // bike-os
}

// bike-os: bottom edge of the top block (the map shades the route under it).
int nav_view_top_bottom(void)
{
    return TOP_Y + TOP_H;
}

void nav_view_update(const nav_state_t *st, const char *street)
{
    // STATE and TEXT are separate writes, so either half may be absent: main.c
    // passes NULL for the one it is not updating.
    if (!st && !street) return;

    if (st) {
#if CONFIG_NAV_DEBUG_BLE
        static bool logged = false;
        if (!logged) {
            logged = true;
            ESP_LOGW("nav", "first state: active=%d maneuver=%u dist=%u after=%u+%u rem=%um/%us",
                     st->flags & NAV_F_ACTIVE, st->maneuver, st->dist_m, st->after_maneuver,
                     st->after_dist_m, (unsigned)st->remaining_m, st->remaining_s);
        }
#endif
        s_st = *st;
        s_have = true;
        if (!(st->flags & NAV_F_ACTIVE)) {
            s_total_m = 0;
        } else if (st->remaining_m > s_total_m) {
            s_total_m = st->remaining_m;
        }
        s_last_ms = now_ms();
        // Changing the maneuver must repaint it: lv_label_set_text below only
        // invalidates the label areas, so the icon would otherwise stay stale.
        if (s_banner) lv_obj_invalidate(s_banner);
        if (s_card) lv_obj_invalidate(s_card);  // bike-os: the card's arrow too
    }
    if (street) {
        // Same Latin-only rule as the media title: drop what we cannot draw.
        size_t o = 0;
        for (const unsigned char *p = (const unsigned char *)street;
             *p && o < NAV_TEXT_MAX; p++) {
            if (*p >= 0x80 && *p < 0xA0) continue;
            if (*p < 0x20 || *p == 0x7F) {
                if (o) s_street[o++] = ' ';
                continue;
            }
            s_street[o++] = (char)*p;
        }
        while (o > 0 && s_street[o - 1] == ' ') o--;
        s_street[o] = '\0';
    }
    if (s_lbl_street) lv_label_set_text(s_lbl_street, street_line());
    if (s_top_street) lv_label_set_text(s_top_street, street_line());  // bike-os
    if (s_card_street) lv_label_set_text(s_card_street, street_line());  // bike-os
    // Always re-evaluate visibility: a stop STATE carries no street, and
    // refresh() is what hides the banner (and applies staleness). Calling it
    // only from the street branch left the banner up forever after a stop.
    refresh();
}

// Re-evaluate visibility on the GUI loop. refresh() is idempotent and only
// touches the banner when it is built.
void nav_view_tick(void)
{
    refresh();
}

bool nav_view_active(void)
{
    return s_have && (s_st.flags & NAV_F_ACTIVE) && (now_ms() - s_last_ms) < STALE_MS;
}

uint16_t nav_view_remaining_s(void)
{
    return nav_view_active() ? s_st.remaining_s : 0;
}

void nav_view_clear(void)
{
    s_have = false;
    s_total_m = 0;
    s_limit = 0;
    s_street[0] = '\0';
    if (s_banner) {
        if (s_lbl_street) lv_label_set_text(s_lbl_street, "");
        lv_obj_set_hidden(s_banner, true);
    }
}
