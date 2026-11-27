#pragma once
/*
 * Full-screen alert over any app (lv_layer_top): timer done, alarm ringing.
 * LVGL thread only. One alert at a time; a new one replaces the old.
 *
 *   right button / BOOT tap / PWR tap  -> dismiss   (cb(ALERT_DISMISS))
 *   left button  / BOOT hold           -> secondary (cb(ALERT_SECONDARY)), e.g. Snooze, +1 min
 */
#include <stdbool.h>
#include "lvgl.h"
#include "audio.h"

typedef enum { ALERT_DISMISS, ALERT_SECONDARY } alert_action_t;
typedef void (*alert_cb_t)(alert_action_t a);

void alert_show(const char *title, const char *big, uint32_t color, snd_t sound,
                const char *secondary_label, alert_cb_t cb);
bool alert_active(void);
void alert_close(alert_action_t a);   /* runs the callback, stops sound, removes the overlay */
