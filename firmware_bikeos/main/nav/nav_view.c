#include "nav_view.h"
#include "map_view.h"  // bike-os: rejoin distance
#include "bos_style.h"  // bike-os 0.11: the turn card
#include "board.h"
#include "display.h"
#include "ui_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

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
static lv_obj_t *s_card;
static uint32_t s_card_key;  // what the card shows, folded: a change means redraw
// bike-os 0.11 "night" layout, in real panel pixels (412 x 412), checked against the
// circle: clock 40, green arrow (and the limit sign) ~112, the distance on a baseline at
// 268, "onto" street 300, arrival and distance left 342, a progress ring on the rim.
#define CARD_CLOCK_Y 40
#define CARD_ICON_CY 116
#define CARD_ICON_PCT 150
#define CARD_NUM_BASE 268
#define CARD_STREET_Y 300
#define CARD_STREET_W 300
#define CARD_ETA_Y 344
#define CARD_RING_R 201
#define CARD_RING_W 6
static uint32_t s_icon_col;  // 0: the default; the card draws its arrow green
// bike-os 0.11: flyover / tunnel badge (road badge, below)
typedef enum { RK_NONE, RK_FLYOVER, RK_BELOW, RK_TUNNEL } road_kind_t;
static road_kind_t road_kind(const char *s);

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
    pen_init(&p, lv_color_hex(warn ? COL_WARN : s_icon_col ? s_icon_col : COL_ICON), ip(13));
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
    // bike-os: a drive with no route line gets the turn card in place of the banner, and
    // every drive ends on its "You've arrived" card.
    bool card = active && !s_suppressed && ((s_st.flags & NAV_F_TURN_ONLY) || s_st.maneuver == NAV_ARRIVE);
    if (s_card) lv_obj_set_hidden(s_card, !card);
    lv_obj_set_hidden(s_banner, !active || s_suppressed || card);
    if (s_top) lv_obj_set_hidden(s_top, !active || s_suppressed || !s_minimal || card);  // bike-os
    if (!active) return;
    if (card) {
        // Redraw only when something on it changed: refresh() runs every GUI loop.
        uint32_t k = s_st.maneuver;
        k = k * 65599u + s_st.dist_m;
        k = k * 65599u + s_st.remaining_m / 100;
        k = k * 65599u + (map_view_clock_now() / 60);
        k = k * 65599u + s_limit;
        k = k * 65599u + (uint32_t)nav_view_progress_permille();
        for (const char *c = s_street; *c; c++) k = k * 31u + (uint8_t)*c;
        if (k != s_card_key) {
            s_card_key = k;
            lv_obj_invalidate(s_card);
        }
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
    bool then = then_shown() || road_kind(street_line()) != RK_NONE;   // bike-os 0.11: or the badge
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

// ---------------------------------------------------------------- road badge
// bike-os 0.11: whether the step takes a flyover, stays below one, or goes through a
// tunnel, read from the turn's own words (Google Maps' instruction, or the road name or
// instruction Vela sends). Shown as a small round badge by the arrow.

static bool has_word(const char *low, const char *w)
{
    return strstr(low, w) != NULL;
}

static road_kind_t road_kind(const char *s)
{
    char low[NAV_TEXT_MAX + 2];
    size_t n = 0;
    for (; s[n] && n < sizeof low - 1; n++) low[n] = (char)tolower((unsigned char)s[n]);
    low[n] = 0;
    if (!n) return RK_NONE;
    // below first: "avoid the flyover" also says "flyover"
    if (has_word(low, "avoid the flyover") || has_word(low, "below the flyover") || has_word(low, "under the flyover") ||
        has_word(low, "service road") || has_word(low, "stay below") || has_word(low, "avoid the overpass"))
        return RK_BELOW;
    if (has_word(low, "tunnel") || has_word(low, "underpass") || has_word(low, "subway"))
        return RK_TUNNEL;
    if (has_word(low, "flyover") || has_word(low, "overpass") || has_word(low, "elevated") ||
        has_word(low, "overbridge") || has_word(low, "bridge"))
        return RK_FLYOVER;
    return RK_NONE;
}

static void badge_line(lv_layer_t *l, int x1, int y1, int x2, int y2, int w, uint32_t col)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(col);
    d.width = w;
    d.round_start = d.round_end = 1;
    d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(l, &d);
}

// A round badge of radius [r] at (x, y): a bridge deck on pillars, or a tunnel mouth.
static void draw_road_badge(lv_layer_t *l, int x, int y, int r, road_kind_t k)
{
    if (k == RK_NONE) return;
    uint32_t ring = k == RK_BELOW ? BOS_WARN : k == RK_TUNNEL ? BOS_SOFT : BOS_ROUTE;
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.radius = LV_RADIUS_CIRCLE;
    bg.bg_color = lv_color_hex(0x14171C);
    bg.bg_opa = LV_OPA_COVER;
    bg.border_color = lv_color_hex(ring);
    bg.border_width = r >= 20 ? 3 : 2;
    bg.border_opa = LV_OPA_COVER;
    lv_area_t a = {x - r, y - r, x + r, y + r};
    lv_draw_rect(l, &bg, &a);
    int u = r * 10 / 24;   // the icon is drawn for r = 24 and scaled
    if (k == RK_TUNNEL) {
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_white();
        arc.width = r >= 20 ? 4 : 3;
        arc.center.x = x;
        arc.center.y = y + u / 2;
        arc.radius = u + u / 3;
        arc.start_angle = 180;
        arc.end_angle = 360;
        lv_draw_arc(l, &arc);
        badge_line(l, x - u - u / 2, y + u / 2, x + u + u / 2, y + u / 2, r >= 20 ? 3 : 2, 0xFFFFFF);
        return;
    }
    // deck, two pillars, the ground
    int deck = y - u / 2, ground = y + u;
    badge_line(l, x - u - u / 2, deck, x + u + u / 2, deck, r >= 20 ? 4 : 3, 0xFFFFFF);
    badge_line(l, x - u / 2 - 1, deck, x - u / 2 - 1, ground, r >= 20 ? 3 : 2, 0xFFFFFF);
    badge_line(l, x + u / 2 + 1, deck, x + u / 2 + 1, ground, r >= 20 ? 3 : 2, 0xFFFFFF);
    badge_line(l, x - u - u / 2, ground, x + u + u / 2, ground, 2, k == RK_BELOW ? BOS_WARN : 0x5F6670);
    if (k == RK_BELOW) {   // a short arrow on the ground, under the deck: stay below
        badge_line(l, x - u + 1, ground - u / 3, x + u - 1, ground - u / 3, 2, BOS_WARN);
    }
}

static const char *road_kind_caption(road_kind_t k)
{
    return k == RK_FLYOVER ? "TAKE FLYOVER" : k == RK_BELOW ? "STAY BELOW" : k == RK_TUNNEL ? "TUNNEL" : "";
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
    s_icon_col = BOS_GO;  // bike-os 0.11: the next turn is green everywhere
    draw_icon(layer, c.x1 + ICON_CX, c.y1 + ICON_CY, s_st.maneuver);
    s_icon_col = 0;
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
    } else {
        // bike-os 0.11: the flyover / tunnel badge takes the right-hand end instead
        draw_road_badge(layer, c.x1 + THEN_CX, c.y1 + BANNER_H / 2, 16, road_kind(street_line()));
    }
}

// ---------------------------------------------------------------- turn card
// bike-os 0.11. Everything on it is drawn here, so the number and its unit share a
// baseline and "onto" and the arrival time take their own colours.

static void card_text(lv_layer_t *l, const char *s, const lv_font_t *f, uint32_t col,
                      int x1, int y_top, int w, lv_text_align_t al)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = s;
    d.font = f;
    d.color = lv_color_hex(col);
    d.align = al;
    lv_area_t a = {x1, y_top, x1 + w - 1, y_top + f->line_height * 2};
    lv_draw_label(l, &d, &a);
}

static int text_w(const char *s, const lv_font_t *f)
{
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

// The top of a line of [f] whose glyphs sit on [base].
static int top_for_base(const lv_font_t *f, int base)
{
    return base - (f->line_height - f->base_line);
}

static void card_disc(lv_layer_t *l, int x, int y, int r, uint32_t col)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(col);
    d.bg_opa = LV_OPA_COVER;
    lv_area_t a = {x - r, y - r, x + r, y + r};
    lv_draw_rect(l, &d, &a);
}

static void card_ring(lv_layer_t *l, int x, int y, int r, int w, int a0, int a1, uint32_t col, bool round)
{
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.color = lv_color_hex(col);
    a.width = w;
    a.center.x = x;
    a.center.y = y;
    a.radius = r;
    a.start_angle = a0;
    a.end_angle = a1;
    a.rounded = round;
    lv_draw_arc(l, &a);
}

// A road sign: white disc, red ring, the number in black.
static void card_limit(lv_layer_t *l, int x, int y, int r, uint16_t limit)
{
    card_disc(l, x, y, r, 0xFFFFFF);
    card_ring(l, x, y, r - 2, 5, 0, 360, BOS_LIMIT, false);
    char b[8];
    snprintf(b, sizeof b, "%u", limit);
    const lv_font_t *f = limit >= 100 ? &lv_font_montserrat_20 : &lv_font_montserrat_24;
    card_text(l, b, f, 0x000000, x - r, y - f->line_height / 2, 2 * r, LV_TEXT_ALIGN_CENTER);
}

// The instruction itself, not a road: no "onto" in front of it.
static bool is_instruction(const char *s)
{
    static const char *const verbs[] = {"Head", "Turn", "Keep", "Continue", "Slight", "Sharp", "Take",
                                        "Make", "At ", "Merge", "Exit", "Your", "Arrive", "Finding", "Go "};
    for (size_t i = 0; i < sizeof verbs / sizeof verbs[0]; i++)
        if (strncmp(s, verbs[i], strlen(verbs[i])) == 0) return true;
    return false;
}

static void fmt_clock(char *out, size_t cap, uint32_t epoch)
{
    uint32_t m = epoch / 60 % 1440;
    unsigned h = m / 60, mm = m % 60;
    if (map_view_clock_24h()) snprintf(out, cap, "%02u:%02u", h, mm);
    else snprintf(out, cap, "%u:%02u", h % 12 ? h % 12 : 12, mm);
}

static void card_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_target(e), &c);
    const int ox = c.x1, oy = c.y1, cx = ox + BOARD_LCD_W / 2;
    char b[48];

    // the trip so far, round the rim
    card_ring(layer, cx, oy + BOARD_LCD_H / 2, CARD_RING_R, CARD_RING_W, 0, 360, BOS_TRACK, false);
    int p = nav_view_progress_permille();
    if (p > 0) card_ring(layer, cx, oy + BOARD_LCD_H / 2, CARD_RING_R, CARD_RING_W, 270, 270 + 360 * p / 1000, BOS_GO, true);

    uint32_t now = map_view_clock_now();
    if (now) {
        fmt_clock(b, sizeof b, now);
        card_text(layer, b, &lv_font_montserrat_20, BOS_SOFT, cx - 80, oy + CARD_CLOCK_Y - 12, 160, LV_TEXT_ALIGN_CENTER);
    }

    if (s_st.maneuver == NAV_ARRIVE) {
        // You've arrived: a pin in a green ring, the place under it.
        int py = oy + 140;
        card_ring(layer, cx, py, 52, 4, 0, 360, BOS_GO, false);
        card_disc(layer, cx, py - 8, 18, BOS_LIMIT);
        lv_draw_triangle_dsc_t tr;
        lv_draw_triangle_dsc_init(&tr);
        tr.color = lv_color_hex(BOS_LIMIT);
        tr.p[0].x = cx - 15; tr.p[0].y = py - 1;
        tr.p[1].x = cx + 15; tr.p[1].y = py - 1;
        tr.p[2].x = cx;      tr.p[2].y = py + 26;
        lv_draw_triangle(layer, &tr);
        card_disc(layer, cx, py - 8, 7, 0x000000);
        card_text(layer, "You've arrived", &lv_font_montserrat_28, BOS_TEXT, ox + 56, oy + 222, 300, LV_TEXT_ALIGN_CENTER);
        if (s_street[0])
            card_text(layer, s_street, &lv_font_montserrat_20, BOS_MUTED, ox + 66, oy + 264, 280, LV_TEXT_ALIGN_CENTER);
        return;
    }

    // the turn, green, with the limit beside it when known
    int ax = s_limit ? cx - 34 : cx;
    s_icon_pct = CARD_ICON_PCT;
    s_icon_col = s_st.maneuver == NAV_OFF_ROUTE ? 0 : BOS_GO;
    draw_icon(layer, ax, oy + CARD_ICON_CY, s_st.maneuver);
    s_icon_col = 0;
    s_icon_pct = ICON_PCT;
    if (s_limit) card_limit(layer, cx + 58, oy + CARD_ICON_CY - 4, 27, s_limit);
    road_kind_t rk = road_kind(street_line());
    if (rk != RK_NONE) {
        int bx = ax - 92;
        draw_road_badge(layer, bx, oy + CARD_ICON_CY - 4, 24, rk);
        card_text(layer, road_kind_caption(rk), &lv_font_montserrat_14, rk == RK_BELOW ? BOS_WARN : BOS_SOFT,
                  bx - 50, oy + CARD_ICON_CY + 26, 100, LV_TEXT_ALIGN_CENTER);
    }

    // the distance: a big number and a small unit on one baseline; 0 is "not known yet"
    if (s_st.dist_m) {
        const lv_font_t *nf;
        const char *unit;
        unsigned m = s_st.dist_m;
        if (s_mph) {
            unsigned ft = m * 3281u / 1000u;
            if (ft < 1000) { snprintf(b, sizeof b, "%u", ft / 10 * 10); nf = BOS_FONT_HUGE; unit = "ft"; }
            else { unsigned t = m * 10u / 1609u; snprintf(b, sizeof b, "%u.%u", t / 10, t % 10); nf = BOS_FONT_BIG; unit = "mi"; }
        } else if (m < 1000) {
            snprintf(b, sizeof b, "%u", m >= 100 ? m / 10 * 10 : m);
            nf = BOS_FONT_HUGE;
            unit = "m";
        } else if (m < 10000) {
            snprintf(b, sizeof b, "%u.%u", m / 1000, m % 1000 / 100);
            nf = BOS_FONT_BIG;
            unit = "km";
        } else {
            snprintf(b, sizeof b, "%u", (m + 500) / 1000);
            nf = BOS_FONT_BIG;
            unit = "km";
        }
        const lv_font_t *uf = &lv_font_montserrat_28;
        int wn = text_w(b, nf), wu = text_w(unit, uf), gap = 8;
        int x0 = cx - (wn + gap + wu) / 2;
        card_text(layer, b, nf, BOS_TEXT, x0, oy + top_for_base(nf, CARD_NUM_BASE), wn + 4, LV_TEXT_ALIGN_LEFT);
        card_text(layer, unit, uf, BOS_MUTED, x0 + wn + gap, oy + top_for_base(uf, CARD_NUM_BASE), wu + 4, LV_TEXT_ALIGN_LEFT);
    }

    // "onto" street, on one line when it fits; else the street alone over two
    const char *st = street_line();
    if (st[0]) {
        const lv_font_t *sf = &lv_font_montserrat_20, *of = &lv_font_montserrat_16;
        bool onto = !is_instruction(st);
        int wo = onto ? text_w("onto", of) + 7 : 0, ws = text_w(st, sf);   // the gap by hand: a trailing space measures 0
        if (wo + ws <= CARD_STREET_W) {
            int x0 = cx - (wo + ws) / 2;
            if (onto) card_text(layer, "onto", of, BOS_MUTED, x0, oy + top_for_base(of, CARD_STREET_Y), wo, LV_TEXT_ALIGN_LEFT);
            card_text(layer, st, sf, BOS_TEXT, x0 + wo, oy + top_for_base(sf, CARD_STREET_Y), ws + 4, LV_TEXT_ALIGN_LEFT);
        } else {
            // two lines in the smaller size, so they fit between the number and the arrival
            const lv_font_t *f2 = &lv_font_montserrat_16;
            card_text(layer, st, f2, BOS_TEXT, cx - CARD_STREET_W / 2, oy + CARD_NUM_BASE + 6,
                      CARD_STREET_W, LV_TEXT_ALIGN_CENTER);
        }
    }

    // the arrival time in green, the distance left after it
    char eta[16] = "", left[24];
    uint32_t rm = s_st.remaining_m;
    if (s_mph) { unsigned t = rm * 10u / 1609u; snprintf(left, sizeof left, "%u.%u mi", t / 10, t % 10); }
    else if (rm >= 10000) snprintf(left, sizeof left, "%u km", (unsigned)((rm + 500) / 1000));
    else if (rm >= 1000) snprintf(left, sizeof left, "%u.%u km", (unsigned)(rm / 1000), (unsigned)(rm % 1000 / 100));
    else snprintf(left, sizeof left, "%u m", (unsigned)rm);
    const lv_font_t *ef = &lv_font_montserrat_20;
    if (now && s_st.remaining_s) fmt_clock(eta, sizeof eta, now + s_st.remaining_s);
    char rest[32];
    snprintf(rest, sizeof rest, eta[0] ? "  " LV_SYMBOL_BULLET "  %s" : "%s", left);
    int we = eta[0] ? text_w(eta, ef) : 0, wr = text_w(rest, ef);
    int x0 = cx - (we + wr) / 2;
    if (eta[0]) card_text(layer, eta, ef, BOS_GO, x0, oy + top_for_base(ef, CARD_ETA_Y), we + 4, LV_TEXT_ALIGN_LEFT);
    card_text(layer, rest, ef, BOS_MUTED, x0 + we, oy + top_for_base(ef, CARD_ETA_Y), wr + 4, LV_TEXT_ALIGN_LEFT);
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
    lv_obj_set_clickable(s_card, false);
    lv_obj_set_scrollable(s_card, false);
    lv_obj_add_event_cb(s_card, card_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    s_card_key = 0;
    lv_obj_set_hidden(s_card, true);

    layout();
    refresh();
}

void nav_view_destroy(void)
{
    s_banner = s_lbl_street = s_lbl_dist = s_lbl_left = NULL;
    s_top = s_top_street = s_top_eta = NULL;  // bike-os
    s_card = NULL;  // bike-os
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
