#include "bos_intro.h"
#include <string.h>

#define INTRO_MAX_MS 8000   /* a broken or very long GIF can never hold the boot */
#define INTRO_HOLD_MS 600   /* READY comes as the last frame is drawn: let it be seen */

static lv_obj_t *s_root, *s_gif;
static lv_timer_t *s_guard;
static lv_image_dsc_t s_dsc;
static void (*s_done)(void);

static void finish(void)
{
    void (*cb)(void) = s_done;
    s_done = NULL;               /* once only: READY, a tap and the guard can all race */
    if (s_guard) { lv_timer_delete(s_guard); s_guard = NULL; }
    if (s_gif) lv_gif_pause(s_gif);
    if (cb) cb();
}

static void ready_cb(lv_event_t *e)
{
    (void)e;
    if (s_guard) { lv_timer_set_period(s_guard, INTRO_HOLD_MS); lv_timer_reset(s_guard); }
    else finish();
}
static void tap_cb(lv_event_t *e) { (void)e; finish(); }
static void guard_cb(lv_timer_t *t) { (void)t; finish(); }

void bos_intro_play(lv_obj_t *scr, void (*done)(void))
{
    s_done = done;
    s_root = lv_obj_create(scr);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_set_clickable(s_root, true);
    lv_obj_add_event_cb(s_root, tap_cb, LV_EVENT_CLICKED, NULL);

    size_t n = 0;
    const uint8_t *p = bos_intro_gif(&n);
    memset(&s_dsc, 0, sizeof s_dsc);
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.data = p;
    s_dsc.data_size = (uint32_t)n;

    s_gif = lv_gif_create(s_root);
    lv_obj_set_clickable(s_gif, false);                 /* taps go to the root: skip */
    lv_gif_set_color_format(s_gif, LV_COLOR_FORMAT_RGB565);  /* half the RAM of ARGB */
    lv_obj_add_event_cb(s_gif, ready_cb, LV_EVENT_READY, NULL);
    if (n) lv_gif_set_src(s_gif, &s_dsc);
    lv_obj_center(s_gif);

    /* not decodable: go straight on (from a timer, so the caller's state is set up) */
    s_guard = lv_timer_create(guard_cb, (n && lv_gif_is_loaded(s_gif)) ? INTRO_MAX_MS : 1, NULL);
    if (lv_gif_is_loaded(s_gif)) lv_gif_set_loop_count(s_gif, 1);
}

void bos_intro_destroy(void)
{
    s_done = NULL;
    if (s_guard) { lv_timer_delete(s_guard); s_guard = NULL; }
    if (s_root) { lv_obj_delete(s_root); s_root = NULL; s_gif = NULL; }
}
