#pragma once
/*
 * Ride recorder ("journey tracking"). GUI task only.
 *
 * Fed with every phone GPS fix by nav_core, whatever app is on screen.
 * Starts by itself when a route starts or after RIDE_AUTO_START_S of riding
 * above RIDE_AUTO_START_KMH, or by hand (ride face / Rides app). Ends by hand,
 * or after RIDE_AUTO_STOP_S standing still, and is then saved to flash:
 * a summary in /rides/index.bin and the trace (a point every ~25 m) in
 * /rides/rNNNN.trk. The newest RIDE_MAX_SAVED rides are kept.
 */
#include <stdbool.h>
#include <stdint.h>

#define RIDE_MAX_SAVED      30
#define RIDE_MAX_POINTS     4000
#define RIDE_AUTO_START_KMH 10
#define RIDE_AUTO_START_S   15
#define RIDE_AUTO_STOP_S    600   /* parked this long ends a ride (not while navigating) */
#define RIDE_MIN_SAVE_M     50

typedef enum { RIDE_IDLE, RIDE_RECORDING } ride_state_t;
typedef enum {
    RIDE_RES_NONE,        /* was not recording */
    RIDE_RES_SAVED,
    RIDE_RES_TOO_SHORT,   /* under RIDE_MIN_SAVE_M: dropped */
    RIDE_RES_DISCARDED,   /* stop(false) */
    RIDE_RES_NO_STORAGE,  /* could not write /rides: listed until a restart only */
} ride_result_t;

typedef struct {
    uint32_t id;
    uint32_t start_epoch;     /* local time, 0 if unknown */
    uint32_t distance_m;
    uint32_t elapsed_s;       /* start to end */
    uint32_t moving_s;        /* while above 3 km/h */
    uint16_t max_cms;         /* top speed, cm/s */
    uint16_t points;
} ride_summary_t;   /* stored as-is in index.bin: do not change the layout */

void ride_init(const char *dir);   /* "/rides" on the device, a folder on the host */
/* good: the phone's accuracy is within 50 m (its fix flag). Only good fixes measure
   distance, motion and the trace; every fix counts as the phone being there. */
void ride_on_fix(int32_t lat_e7, int32_t lon_e7, uint16_t speed_cms, bool good, uint32_t now_ms);
void ride_tick(uint32_t now_ms, bool navigating);

void ride_start(void);
ride_result_t ride_stop(bool save);
bool ride_storage_ok(void);                 /* false: the "rides" partition is not writable */
uint16_t ride_speed_cms(void);
uint32_t ride_fix_count(void);              /* fixes seen since boot (diagnostics) */
uint16_t ride_phone_cms(void);              /* the speed the phone sent with the last fix */
bool ride_moving_now(void);                 /* moving at the last fix (for the compass heading) */              /* phone speed, or from position change when the phone sends none */
ride_state_t ride_state(void);
const ride_summary_t *ride_current(void);   /* live figures while recording, else the last ride */
bool ride_just_saved(uint32_t now_ms);      /* true for 10 s after a save */

int  ride_count(void);                                   /* saved rides, newest first */
bool ride_get(int i, ride_summary_t *out);
int  ride_trace(int i, int32_t *lat_e7, int32_t *lon_e7, int max);   /* points read */
bool ride_delete(int i);
