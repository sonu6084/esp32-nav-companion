/*
 * Navigation — your firmware_ws146 navigation system as a bike-os app.
 *
 * The screens are firmware_ws146's own (map with SD tiles or the minimal BLE
 * street display, turn banner, orange turn edge, media controls, incoming call,
 * QR pairing). nav_core keeps BLE running and the state current while other
 * apps are open; this file only opens and closes the screens.
 *
 *  Opens by itself when the phone starts a route or a call rings; after a call
 *  that interrupted another app, that app comes back.
 *  Keys: BOOT tap = recentre (map) / back to the map (media)
 *        BOOT hold = map <-> minimal display     PWR tap = menu
 *  No swipe-back here: the map pans with a drag.
 */
#include <stdio.h>
#include "app_manager.h"
#include "nav_core.h"
#include "settings.h"
#include "cfg.h"

#define COLOR 0x2ECC71

static void nv_create(lv_obj_t *scr, const app_t *self) { nav_core_attach(scr, false); }
static void nv_destroy(void) { nav_core_detach(); }

static bool nv_on_key(app_key_t k)
{
    nav_core_key(k == APP_KEY_SECONDARY);
    return true;
}

static void nv_bg_tick(void)
{
    if (nav_core_take_focus_request() && am_current() != am_find("Navigation"))
        am_open(am_find("Navigation"));
    if (nav_core_take_give_back() && am_current() == am_find("Navigation"))
        am_back();
}

static bool nv_keep_awake(void)
{
    return cfg_keep_on_nav() && (nav_core_navigating() || nav_core_moving());
}

static const char *nv_status(void)
{
    static char buf[32];
    if (!settings_paired()) return "Not paired";
    if (!nav_core_connected()) return "Phone not connected";
    int d = nav_core_dist_m();
    if (!nav_core_navigating() || d < 0) return LV_SYMBOL_BLUETOOTH " Connected";
    if (d < 1000) snprintf(buf, sizeof buf, "Turn in %d m", (d + 5) / 10 * 10);
    else snprintf(buf, sizeof buf, "Turn in %d.%d km", d / 1000, (d % 1000) / 100);
    return buf;
}

const app_t app_nav = {
    .name       = "Navigation",
    .symbol     = LV_SYMBOL_GPS,
    .color      = COLOR,
    .create     = nv_create,
    .destroy    = nv_destroy,
    .on_key     = nv_on_key,
    .status     = nv_status,
    .bg_tick    = nv_bg_tick,
    .keep_awake = nv_keep_awake,
    .instant    = true,      /* nav_view uses lv_screen_active(): load before building */
    .no_swipe_back = true,   /* the map pans with a drag */
    .ready      = true,
};

/* ------------------------------------------------------------------------- */
/* Music: firmware_ws146's media page (phone's player over BLE), as its own   */
/* menu entry. Same nav core, opened on the media page.                       */
/* ------------------------------------------------------------------------- */
static void mu_create(lv_obj_t *scr, const app_t *self) { nav_core_attach(scr, true); }

static const char *mu_status(void)
{
    if (!nav_core_connected()) return NULL;
    const char *t = nav_core_media_title();
    return (t && t[0]) ? t : NULL;
}

const app_t app_music = {
    .name       = "Music",
    .symbol     = LV_SYMBOL_AUDIO,
    .color      = 0xF06292,
    .create     = mu_create,
    .destroy    = nv_destroy,
    .on_key     = nv_on_key,
    .status     = mu_status,
    .instant    = true,
    .no_swipe_back = true,   /* the media page has its own swipe */
    .ready      = true,
};
