#pragma once
/*
 * Home screen: the resting face after the boot intro. Big time, date, phone
 * link and battery, a seconds ring round the rim. A tap anywhere opens the
 * menu; while a route is running a "Navigating" pill opens Navigation.
 * Owned by app_manager (built once, kept for the life of the firmware).
 */
#include "lvgl.h"

lv_obj_t *home_create(void (*on_tap)(void), void (*on_nav)(void));
void home_refresh(void);   /* cheap; call a few times a second while it shows */
