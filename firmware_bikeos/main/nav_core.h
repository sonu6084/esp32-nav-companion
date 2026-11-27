#pragma once
/*
 * Navigation core (firmware_ws146's main.c, split for bike-os).
 * GUI task only, except where noted.
 */
#include <stdbool.h>
#include "lvgl.h"

void nav_core_init(void);              /* after display_init(); starts BLE */
void nav_core_poll(void);              /* every GUI loop, whatever app is open */

void nav_core_attach(lv_obj_t *scr, bool media);  /* app opened: map (or media page) / pair / call */
void nav_core_detach(void);            /* ...closed: views torn down, state kept */

bool nav_core_take_focus_request(void);/* true once: route started / call ringing */
bool nav_core_take_give_back(void);    /* true once: call ended that had taken focus */

bool nav_core_connected(void);
bool nav_core_on_media(void);   /* the music page is showing */
const char *nav_core_media_title(void);   /* "" when nothing is playing */
bool nav_core_navigating(void);
bool nav_core_moving(void);
int  nav_core_dist_m(void);            /* to the next maneuver, -1 when not navigating */

void nav_core_key(bool long_press);    /* BOOT in the app: tap = recentre / back to map, hold = minimal */
void nav_core_forget_phone(void);      /* Settings > Forget phone (keeps the bond, new token) */
