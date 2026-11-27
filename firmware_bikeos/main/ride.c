#include "ride.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "sys.h"

#define MOVING_CMS     83        /* 3 km/h */
#define POINT_STEP_M   25.0
#define JUMP_MAX_M     500.0     /* a fix this far from the last one is a glitch / dropout */

static char s_dir[32];
static ride_state_t s_state;
static ride_summary_t s_cur, s_last;
static int32_t *s_lat, *s_lon;
static int32_t s_prev_lat, s_prev_lon, s_pt_lat, s_pt_lon;
static bool s_have_prev;
static uint32_t s_t0_ms, s_prev_ms, s_still_since, s_fast_since, s_saved_at;
static bool s_was_nav;
static uint32_t s_moving_ms;
static uint32_t s_nav_seen_ms;
static uint32_t s_dist_cm;              /* this ride, cm (summary keeps whole metres) */

static ride_summary_t s_index[RIDE_MAX_SAVED];
static int s_n;                          /* saved rides, s_index[0] newest */
static uint32_t s_next_id = 1;

/* ------------------------------ helpers ------------------------------ */
static double dist_m(int32_t la1, int32_t lo1, int32_t la2, int32_t lo2)
{
    double a1 = la1 / 1e7 * M_PI / 180.0, a2 = la2 / 1e7 * M_PI / 180.0;
    double dl = (lo2 - lo1) / 1e7 * M_PI / 180.0;
    double x = dl * cos((a1 + a2) / 2), y = a2 - a1;
    return sqrt(x * x + y * y) * 6371000.0;
}

static uint32_t epoch_now(void)
{
    datetime_t t;
    sys_get_time(&t);
    if (t.year < 2024 || t.month < 1 || t.month > 12) return 0;
    /* days since 1970 (civil from days, Howard Hinnant) */
    int y = t.year, m = t.month, d = t.day;
    y -= m <= 2;
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    return (uint32_t)(days * 86400 + t.hour * 3600 + t.minute * 60 + t.second);
}

static void path(char *out, size_t n, uint32_t id) { snprintf(out, n, "%s/r%04u.trk", s_dir, (unsigned)(id % 10000)); }

static bool s_storage_ok;
bool ride_storage_ok(void) { return s_storage_ok; }

static bool save_index(void)
{
    char p[48];
    snprintf(p, sizeof p, "%s/index.bin", s_dir);
    FILE *f = fopen(p, "wb");
    if (!f) return false;
    uint32_t hdr[2] = {0x52494445u /* "RIDE" */, (uint32_t)s_n};
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(s_index, sizeof s_index[0], s_n, f);
    fwrite(&s_next_id, sizeof s_next_id, 1, f);
    return fclose(f) == 0;
}

static void load_index(void)
{
    char p[48];
    snprintf(p, sizeof p, "%s/index.bin", s_dir);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    uint32_t hdr[2];
    if (fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == 0x52494445u && hdr[1] <= RIDE_MAX_SAVED) {
        s_n = (int)fread(s_index, sizeof s_index[0], hdr[1], f);
        if (fread(&s_next_id, sizeof s_next_id, 1, f) != 1) s_next_id = s_n ? s_index[0].id + 1 : 1;
        if (s_n) s_last = s_index[0];
    }
    fclose(f);
}

/* ------------------------------ public ------------------------------ */
void ride_init(const char *dir)
{
    snprintf(s_dir, sizeof s_dir, "%s", dir);
    s_lat = heap_caps_malloc(RIDE_MAX_POINTS * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    s_lon = heap_caps_malloc(RIDE_MAX_POINTS * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    load_index();
    /* can we write there? (no "rides" partition = app flashed without the new table) */
    char p[48];
    snprintf(p, sizeof p, "%s/.probe", s_dir);
    FILE *f = fopen(p, "wb");
    s_storage_ok = f && fputc('x', f) != EOF;
    if (f) fclose(f);
    remove(p);
}

ride_state_t ride_state(void) { return s_state; }
const ride_summary_t *ride_current(void) { return s_state == RIDE_RECORDING ? &s_cur : &s_last; }
bool ride_just_saved(uint32_t now_ms) { return s_saved_at && now_ms - s_saved_at < 10000; }
int ride_count(void) { return s_n; }

bool ride_get(int i, ride_summary_t *out)
{
    if (i < 0 || i >= s_n) return false;
    *out = s_index[i];
    return true;
}

void ride_start(void)
{
    if (s_state == RIDE_RECORDING) return;
    memset(&s_cur, 0, sizeof s_cur);
    s_cur.id = s_next_id;
    s_cur.start_epoch = epoch_now();
    s_dist_cm = 0;
    s_state = RIDE_RECORDING;
    s_have_prev = false;
    s_t0_ms = s_prev_ms = 0;
    s_moving_ms = 0;
    s_still_since = 0;
    s_saved_at = 0;
}

ride_result_t ride_stop(bool save)
{
    if (s_state != RIDE_RECORDING) return RIDE_RES_NONE;
    s_state = RIDE_IDLE;
    if (!save) return RIDE_RES_DISCARDED;
    if (s_cur.distance_m < RIDE_MIN_SAVE_M) return RIDE_RES_TOO_SHORT;   /* nothing worth keeping */
    s_next_id++;
    char p[48];
    path(p, sizeof p, s_cur.id);
    bool ok = false;
    FILE *f = fopen(p, "wb");
    if (f) {
        ok = true;
        for (int i = 0; i < s_cur.points; i++) {
            int32_t pt[2] = {s_lat[i], s_lon[i]};
            ok &= fwrite(pt, sizeof pt, 1, f) == 1;
        }
        ok &= fclose(f) == 0;
    }
    if (s_n == RIDE_MAX_SAVED) {                          /* drop the oldest */
        path(p, sizeof p, s_index[s_n - 1].id);
        remove(p);
        s_n--;
    }
    memmove(&s_index[1], &s_index[0], (size_t)s_n * sizeof s_index[0]);
    s_index[0] = s_cur;
    s_n++;
    s_last = s_cur;
    ok &= save_index();
    s_saved_at = s_prev_ms ? s_prev_ms : 1;
    s_storage_ok = ok;
    return ok ? RIDE_RES_SAVED : RIDE_RES_NO_STORAGE;   /* kept in the list until a restart */
}

bool ride_delete(int i)
{
    if (i < 0 || i >= s_n) return false;
    char p[48];
    path(p, sizeof p, s_index[i].id);
    remove(p);
    memmove(&s_index[i], &s_index[i + 1], (size_t)(s_n - i - 1) * sizeof s_index[0]);
    s_n--;
    s_last = s_n ? s_index[0] : (ride_summary_t){0};
    save_index();
    return true;
}

int ride_trace(int i, int32_t *lat, int32_t *lon, int max)
{
    if (i < 0 || i >= s_n) return 0;
    char p[48];
    path(p, sizeof p, s_index[i].id);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    int n = 0;
    int32_t pt[2];
    while (n < max && fread(pt, sizeof pt, 1, f) == 1) { lat[n] = pt[0]; lon[n] = pt[1]; n++; }
    fclose(f);
    return n;
}

/* Motion is measured two ways and either counts: the phone's speed, and how far
   the position itself moves. Android often leaves speed out of a fix (hasSpeed()
   false -> the phone sends 0), and then the old recorder saw a parked bike all
   ride long: no distance, no auto-start, and an auto-stop that threw the ride
   away. Position moves are taken in HOP_M steps from an anchor so GPS drift
   while parked does not add up to distance. */
#define HOP_M          15.0
#define HOP_MAX_MS     60000     /* a hop slower than this is drift, not riding */
#define GLITCH_MPS     70.0      /* 250 km/h between two fixes: a bad fix */
#define DERIVED_FRESH  10000     /* a hop counts as "moving now" for this long */
#define DERIVED_MOVING_CMS 139   /* 5 km/h: below this a hop is creep / drift */

static int32_t s_a_lat, s_a_lon;         /* anchor: where the last hop ended */
static uint32_t s_a_ms, s_hop_ms;
static bool s_have_anchor;
static uint16_t s_derived_cms, s_eff_cms;

uint16_t ride_speed_cms(void) { return s_eff_cms; }
static uint16_t s_last_phone_cms;
static uint32_t s_last_fix_ms;
bool ride_moving_now(void)
{
    return s_last_phone_cms >= MOVING_CMS ||
           (s_hop_ms && s_last_fix_ms - s_hop_ms < 4000 && s_derived_cms >= DERIVED_MOVING_CMS);
}

void ride_on_fix(int32_t lat, int32_t lon, uint16_t cms, uint32_t now)
{
    uint32_t hop_cm = 0;               /* distance this fix adds, cm */
    s_last_phone_cms = cms;
    s_last_fix_ms = now;
    if (!s_have_anchor) {
        s_a_lat = lat; s_a_lon = lon; s_a_ms = now; s_have_anchor = true;
    } else {
        double hop = dist_m(s_a_lat, s_a_lon, lat, lon);
        uint32_t dt = now - s_a_ms;
        if (hop >= HOP_M && dt > 0) {
            double mps = hop / (dt / 1000.0);
            if (mps > GLITCH_MPS && dt < 30000) {
                /* jumped: start again from here, count nothing */
            } else if (hop < JUMP_MAX_M || dt < HOP_MAX_MS) {
                hop_cm = (uint32_t)lround(hop * 100.0);
                if (dt <= HOP_MAX_MS) { s_derived_cms = (uint16_t)fmin(mps * 100.0, 65000.0); s_hop_ms = now; }
            }
            s_a_lat = lat; s_a_lon = lon; s_a_ms = now;
        }
    }
    bool fresh = s_hop_ms && now - s_hop_ms < DERIVED_FRESH;
    uint16_t derived = fresh ? s_derived_cms : 0;
    s_eff_cms = cms > derived ? cms : derived;
    bool moving = cms >= MOVING_CMS || derived >= DERIVED_MOVING_CMS;

    /* auto-start bookkeeping runs even while idle */
    if (s_eff_cms >= RIDE_AUTO_START_KMH * 1000 / 36) { if (!s_fast_since) s_fast_since = now; }
    else s_fast_since = 0;

    if (s_state != RIDE_RECORDING) return;
    if (!s_t0_ms) s_t0_ms = now;
    s_dist_cm += hop_cm;
    s_cur.distance_m = s_dist_cm / 100;
    if (s_have_prev && moving) {
        uint32_t gap = now - s_prev_ms;
        if (gap < 5000) {                                  /* a normal gap, not a link dropout */
            s_moving_ms += gap;
            s_cur.moving_s = s_moving_ms / 1000;
        }
    }
    if (s_eff_cms > s_cur.max_cms) s_cur.max_cms = s_eff_cms;
    s_cur.elapsed_s = (now - s_t0_ms) / 1000;
    if (moving) s_still_since = 0;
    else if (!s_still_since) s_still_since = now;

    /* the trace: first fix, then a point every ~25 m */
    if (s_lat && s_cur.points < RIDE_MAX_POINTS &&
        (s_cur.points == 0 || dist_m(s_pt_lat, s_pt_lon, lat, lon) >= POINT_STEP_M)) {
        s_lat[s_cur.points] = lat;
        s_lon[s_cur.points] = lon;
        s_cur.points++;
        s_pt_lat = lat;
        s_pt_lon = lon;
    }
    s_prev_lat = lat;
    s_prev_lon = lon;
    s_prev_ms = now;
    s_have_prev = true;
}

void ride_tick(uint32_t now, bool navigating)
{
    if (navigating) s_nav_seen_ms = now;    /* a route update gap is not the route ending */
    if (s_state == RIDE_IDLE) {
        bool nav_started = navigating && !s_was_nav;
        bool riding = s_fast_since && now - s_fast_since >= RIDE_AUTO_START_S * 1000u;
        if (nav_started || riding) ride_start();
    } else {
        if (s_t0_ms) s_cur.elapsed_s = (now - s_t0_ms) / 1000;
        bool stale = s_prev_ms && now - s_prev_ms > RIDE_AUTO_STOP_S * 1000u;   /* no fixes at all */
        /* parked for 10 min ends a ride, but never while a route is running
           (traffic, a long light, a fuel stop on the way) */
        bool nav_recent = navigating || (s_nav_seen_ms && now - s_nav_seen_ms < 60000u);
        bool still = !nav_recent && s_still_since && now - s_still_since > RIDE_AUTO_STOP_S * 1000u;
        if (stale || still) ride_stop(true);
    }
    s_was_nav = navigating;
}
