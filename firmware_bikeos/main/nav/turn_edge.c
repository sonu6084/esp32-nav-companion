#include "turn_edge.h"
#include "board.h"
#include "ble_service.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "nav_view.h"

// Orange like a bike's indicator. A wide, faint arc behind a narrow, bright
// one reads as a glow on the black background.
#define EDGE_COLOR 0xFF7A00
#define EDGE_SPAN_DEG 70           // length of the arc along the rim
#define EDGE_CORE_W UI_SC(16)      // bright arc thickness
#define EDGE_GLOW_W UI_SC(34)      // glow thickness
#define EDGE_INSET UI_SC(3)        // gap from the glass edge
#define BLINK_SLOW_MS 250          // half-period: 2 Hz
#define BLINK_FAST_MS 125          // half-period: 4 Hz

typedef enum { SIDE_NONE, SIDE_LEFT, SIDE_RIGHT } side_t;

static lv_obj_t *s_glow, *s_core;
static side_t s_side = SIDE_NONE;
static bool s_lit;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Which rim a maneuver lights. U-turns and roundabouts are left out: their
// direction depends on the country's traffic side and the exit, which the
// phone does not send.
static side_t side_of(uint8_t m)
{
    switch (m) {
    case NAV_SLIGHT_LEFT:
    case NAV_LEFT:
    case NAV_SHARP_LEFT:
    case NAV_KEEP_LEFT:
        return SIDE_LEFT;
    case NAV_SLIGHT_RIGHT:
    case NAV_RIGHT:
    case NAV_SHARP_RIGHT:
    case NAV_KEEP_RIGHT:
        return SIDE_RIGHT;
    default:
        return SIDE_NONE;
    }
}

static lv_obj_t *make_arc(int width, lv_opa_t opa)
{
    int d = BOARD_LCD_W - 2 * EDGE_INSET;
    lv_obj_t *a = lv_arc_create(lv_layer_top());
    lv_obj_set_size(a, d, d);
    lv_obj_center(a);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(a, false);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);  // no track
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(EDGE_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(a, opa, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(a, 0, EDGE_SPAN_DEG);
    lv_arc_set_angles(a, 0, EDGE_SPAN_DEG);
    lv_obj_set_hidden(a, true);
    return a;
}

static void ensure_objs(void)
{
    if (s_core) return;
    s_glow = make_arc(EDGE_GLOW_W, LV_OPA_30);
    s_core = make_arc(EDGE_CORE_W, LV_OPA_COVER);
}

static void set_side(side_t side)
{
    if (side == s_side) return;
    s_side = side;
    if (side == SIDE_NONE) return;
    // LVGL arc angles: 0 deg = 3 o'clock, clockwise. Centre the span on 180
    // (9 o'clock) for left, on 0 (3 o'clock) for right.
    int start = (side == SIDE_LEFT ? 180 : 0) - EDGE_SPAN_DEG / 2;
    if (start < 0) start += 360;
    lv_arc_set_rotation(s_glow, start);
    lv_arc_set_rotation(s_core, start);
}

static void set_lit(bool lit)
{
    if (lit == s_lit) return;
    s_lit = lit;
    lv_obj_set_hidden(s_glow, !lit);
    lv_obj_set_hidden(s_core, !lit);
}

void turn_edge_tick(bool allowed)
{
    ensure_objs();
    side_t side = allowed ? side_of(nav_view_maneuver()) : SIDE_NONE;
    int d = nav_view_dist_m();
    if (d < 0 || d > TURN_EDGE_SHOW_M) side = SIDE_NONE;
    set_side(side);
    if (side == SIDE_NONE) {
        set_lit(false);
        return;
    }
    if (d > TURN_EDGE_BLINK_M) {
        set_lit(true);  // steady: a turn is coming, no hurry yet
        return;
    }
    uint32_t half = d <= TURN_EDGE_FAST_M ? BLINK_FAST_MS : BLINK_SLOW_MS;
    set_lit((now_ms() / half) % 2 == 0);
}
