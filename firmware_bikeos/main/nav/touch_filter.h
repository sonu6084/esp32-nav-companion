#pragma once
// bike-os fix: the SPD2010 is polled (every 10 ms), not read on its INT edge.
// A poll that lands between two of its reports finds no new packet and the
// driver returns 0 points although the finger is still down. Passed straight
// to LVGL that is a release: a swipe is chopped into short presses, none of
// which travels far enough to change face, and a hold never reaches 900 ms.
//
// The filter keeps the last real sample through short gaps and only reports
// the lift once nothing has been seen for TOUCH_RELEASE_US. Pure C, no ESP-IDF:
// the host simulator runs the same code.
#include <stdint.h>
#include "touch.h"

#define TOUCH_RELEASE_US 60000

typedef struct {
    touch_sample_t out;     // what consumers see
    int64_t last_seen_us;   // last poll that had a finger
} touch_filter_t;

static inline void touch_filter_feed(touch_filter_t *f, const touch_sample_t *raw, int64_t now_us)
{
    if (raw->n) {
        f->out = *raw;
        f->last_seen_us = now_us;
    } else if (f->out.n && now_us - f->last_seen_us >= TOUCH_RELEASE_US) {
        f->out.n = 0;
    }
}
