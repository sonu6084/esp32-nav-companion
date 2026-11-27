#pragma once
/* Set time & date overlay (rollers + ✓/✗), shared by Clock and Settings. LVGL thread. */
#include <stdbool.h>
#include "lvgl.h"

void ui_settime_open(lv_obj_t *parent, void (*on_close)(bool saved));
void ui_settime_close(bool save);
bool ui_settime_active(void);
