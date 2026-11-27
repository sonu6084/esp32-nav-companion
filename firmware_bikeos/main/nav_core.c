// Navigation core for bike-os.
//
// This is firmware_ws146/main/main.c split in two: everything that has to keep
// running while another bike-os app is on screen (BLE queues, route assembly,
// commands, map/nav/media/call state) lives here and is pumped by
// nav_core_poll() from the GUI task; the screens are built only while the
// Navigation app is open (nav_core_attach/detach). The code below is the
// original wherever it could stay the original - changes are marked "bike-os".
//
// Threading model (unchanged):
//   gui task   — sole owner of LVGL, touch I2C reads, NVS app writes
//   NimBLE     — callbacks only copy into queues
//   tile_ld    — SD reads into tile-cache slots handed to it
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "ble_service.h"
#include "board.h"
#include "call_view.h"
#include "boot_view.h"
#include "display.h"
#include "lvgl.h"
#include "map_view.h"
#include "media_art.h"
#include "media_view.h"
#include "nav_view.h"
#include "turn_edge.h"
#include "sdcard.h"
#include "settings.h"
#include "tiles.h"
#include "touch.h"
#include "ui_pair.h"

#include "nav_core.h"
#include "bos_faces.h"
#include "ride.h"
#include "app_manager.h"
#include "sys.h"

#define BRIGHTNESS_SAVE_MS 1500

static void note_activity(void);
static void enter_deep_sleep(void);

typedef enum { EV_LINK_UP, EV_LINK_DOWN, EV_CMD } ev_type_t;

typedef struct {
    uint8_t type;
    uint8_t code;
    uint8_t len;
    uint8_t args[7];
} app_ev_t;

static const char *TAG = "nav";


static QueueHandle_t s_ev_q;   // commands + link changes, in order
static QueueHandle_t s_pos_q;    // latest position only (overwrite)
static QueueHandle_t s_media_q;  // latest media state only (overwrite)
static QueueHandle_t s_text_q;   // latest track title/artist (overwrite)

// Nav STATE, TEXT and DEST arrive as separate writes, so merge them: a state
// that lands between two text messages must not be lost, the street name has
// to stay sticky, and the destination is latest-wins.
typedef struct {
    bool have_state;
    nav_state_t st;
    char street[NAV_TEXT_MAX + 1];
    bool have_street;
    bool have_dest;
    bool dest_present;
    int32_t dest_lat_e7, dest_lon_e7;
} nav_msg_t;
static QueueHandle_t s_nav_q;    // nav state + street + destination (overwrite)

// Route geometry chunks: ordered, unlike the overwrite-latest nav slot. The
// BLE callback only queues; the GUI task assembles into map_view.
typedef struct {
    uint8_t flags, seq, count;
    int32_t lat[NAV_ROUTE_CHUNK_MAX];
    int32_t lon[NAV_ROUTE_CHUNK_MAX];
} route_chunk_t;
#define ROUTE_Q_LEN 72   /* 2048 points / 31 per chunk = 67, and a little over */
static QueueHandle_t s_route_q;
static uint8_t s_route_seq;
static bool s_route_bad;

// Incoming call. One overwrite-latest slot: a ring that is immediately
// followed by an end must not leave a stale screen up.
typedef struct {
    bool ringing;
    char name[NAV_CALL_TEXT_MAX + 1];
    char number[NAV_CALL_TEXT_MAX + 1];
} call_msg_t;
static QueueHandle_t s_call_q;

typedef enum { SCREEN_NONE, SCREEN_PAIR, SCREEN_MAP, SCREEN_MEDIA, SCREEN_CALL } screen_t;
static screen_t s_screen = SCREEN_NONE;
// bike-os: the Navigation app's screen while it is open (NULL = another app is up).
static lv_obj_t *s_parent;
static bool s_want_focus;        // ask the app manager to open Navigation
static bool s_call_took_focus;   // a ring opened Navigation over another app
static bool s_give_back;         // ...and the call is over: return to that app
// Screen changes requested from inside LVGL events are applied by the GUI
// loop: rebuilding the screen deletes the object whose event is running.
static screen_t s_screen_req = SCREEN_NONE;
#define MEDIA_IDLE_BACK_MS 15000
static uint32_t s_media_touch_ms;  // last touch while the music controls were up
// Where to return after the call page closes (map or media page).
static screen_t s_screen_before_call = SCREEN_MAP;
static bool s_connected;

static uint8_t s_brightness_pending;
static uint32_t s_brightness_changed_ms;
static bool s_brightness_dirty;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---------------------------------------------------------- BLE (host task)

static void ble_on_position(const nav_position_t *p)
{
    xQueueOverwrite(s_pos_q, p);
}

static void ble_on_cmd(uint8_t code, const uint8_t *args, size_t len)
{
    app_ev_t ev = {.type = EV_CMD, .code = code};
    if (len > sizeof(ev.args)) len = sizeof(ev.args);
    memcpy(ev.args, args, len);
    ev.len = (uint8_t)len;
    xQueueSend(s_ev_q, &ev, 0);
}

static void ble_on_media_state(const nav_media_state_t *st)
{
    xQueueOverwrite(s_media_q, st);
}

typedef struct {
    char title[72];
    char artist[56];
} text_msg_t;

static void ble_on_media_text(const char *title, const char *artist)
{
    text_msg_t m;
    snprintf(m.title, sizeof(m.title), "%s", title);
    snprintf(m.artist, sizeof(m.artist), "%s", artist);
    xQueueOverwrite(s_text_q, &m);
}

static void ble_on_call_ring(const char *name, const char *number)
{
    call_msg_t m = {.ringing = true};
    snprintf(m.name, sizeof(m.name), "%s", name);
    snprintf(m.number, sizeof(m.number), "%s", number);
    xQueueOverwrite(s_call_q, &m);
}

static void ble_on_call_end(void)
{
    call_msg_t m = {.ringing = false};
    xQueueOverwrite(s_call_q, &m);
}

static void ble_on_nav_state(const nav_state_t *st, const char *street)
{
    static nav_msg_t m;
    if (st) {
        m.st = *st;
        m.have_state = true;
    }
    if (street) {
        snprintf(m.street, sizeof(m.street), "%s", street);
        m.have_street = true;
    }
    if (m.have_state || m.have_street) xQueueOverwrite(s_nav_q, &m);
}

static void ble_on_nav_dest(bool present, int32_t lat_e7, int32_t lon_e7)
{
    static nav_msg_t m;
    m.have_dest = true;
    m.dest_present = present;
    m.dest_lat_e7 = lat_e7;
    m.dest_lon_e7 = lon_e7;
    xQueueOverwrite(s_nav_q, &m);
}

// The limit is two small values, latest wins: no queue, the GUI loop picks
// them up. Written in the NimBLE host task, read in the GUI task.
static volatile bool s_limit_dirty, s_limit_mph;
static volatile uint16_t s_limit_val;

static volatile bool s_phone_dirty, s_phone_charging, s_phone_24h;
static volatile int s_phone_batt;
static volatile uint32_t s_phone_epoch;

static void ble_on_phone_info(int battery_pct, bool charging, bool clock_24h, uint32_t local_epoch_s)
{
    s_phone_24h = clock_24h;
    s_phone_batt = battery_pct;
    s_phone_charging = charging;
    s_phone_epoch = local_epoch_s;
    s_phone_dirty = true;
}

static void ble_on_nav_limit(bool mph, uint16_t limit)
{
    s_limit_mph = mph;
    s_limit_val = limit;
    s_limit_dirty = true;
}

// Nearby-street chunks: ordered like the route's. The parsing is map_view's.
typedef struct {
    uint8_t len;
    uint8_t data[NAV_ROADS_CHUNK_MAX];
} roads_chunk_t;
static QueueHandle_t s_roads_q;

static void ble_on_roads_chunk(const uint8_t *data, size_t len)
{
    if (len > NAV_ROADS_CHUNK_MAX) return;
    roads_chunk_t c = {.len = (uint8_t)len};
    memcpy(c.data, data, len);
    // A full queue drops the chunk; map_view sees the gap in seq and throws
    // the whole set away, so a partial set is never drawn.
    xQueueSend(s_roads_q, &c, 0);
}

// Congestion for the route: one message, latest wins. It is drained after the
// route chunks in the same pass, so it always lands on the route it came with.
static QueueHandle_t s_traffic_q;

static void ble_on_traffic(const uint8_t *data, size_t len)
{
    roads_chunk_t c = {.len = (uint8_t)len};
    memcpy(c.data, data, len);
    xQueueOverwrite(s_traffic_q, &c);
}

static void ble_on_route_chunk(uint8_t flags, uint8_t seq, uint8_t count,
                               const int32_t *lat_e7, const int32_t *lon_e7)
{
    if (count > NAV_ROUTE_CHUNK_MAX) return;
    route_chunk_t c = {.flags = flags, .seq = seq, .count = count};
    memcpy(c.lat, lat_e7, (size_t)count * sizeof(int32_t));
    memcpy(c.lon, lon_e7, (size_t)count * sizeof(int32_t));
    // Ordered: a full queue drops a chunk, the seq check below then discards
    // the whole route rather than drawing a gap. The phone re-sends on reroute.
    xQueueSend(s_route_q, &c, 0);
}

// bike-os: NAV_STOPS (waypoints), latest wins.
typedef struct {
    uint8_t len;
    uint8_t data[NAV_STOPS_HDR + NAV_STOPS_LABEL_MAX];
} stops_msg_t;
static QueueHandle_t s_stops_q;

static void ble_on_stops(const uint8_t *data, size_t len)
{
    stops_msg_t m = {.len = (uint8_t)(len > sizeof m.data ? sizeof m.data : len)};
    memcpy(m.data, data, m.len);
    xQueueOverwrite(s_stops_q, &m);
}

static void ble_on_link(bool up)
{
    app_ev_t ev = {.type = up ? EV_LINK_UP : EV_LINK_DOWN};
    xQueueSend(s_ev_q, &ev, 0);
}

// ---------------------------------------------------------- screens (GUI)

static void browse_cb(lv_event_t *e);
static void open_map(void);
static void return_from_call(void);

static void call_answer_cb(void)
{
#if CONFIG_NAV_DEBUG_BLE
    ESP_LOGW(TAG, "call: answer");
#endif
    nav_ble_call_cmd(NAV_CCMD_ANSWER);
    return_from_call();
}

static void call_reject_cb(void)
{
#if CONFIG_NAV_DEBUG_BLE
    ESP_LOGW(TAG, "call: reject");
#endif
    nav_ble_call_cmd(NAV_CCMD_REJECT);
    return_from_call();
}

static void call_dismiss_cb(void)
{
#if CONFIG_NAV_DEBUG_BLE
    ESP_LOGW(TAG, "call: dismiss");
#endif
    return_from_call();
}

static void show_screen(screen_t which)
{
    if (which == s_screen) return;
    if (!s_parent) {                 /* app closed: remember what to show when it opens */
        s_screen = SCREEN_NONE;
        return;
    }
    lv_obj_t *scr = s_parent;
    map_view_destroy();
    media_view_destroy();
    nav_view_destroy();
    call_view_destroy();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    if (which == SCREEN_PAIR) {
        ui_pair_create(scr, browse_cb);
    } else if (which == SCREEN_MEDIA) {
        s_media_touch_ms = now_ms();  // opening it counts as touching it
        media_view_create(scr, open_map);
    } else if (which == SCREEN_CALL) {
        call_view_create(scr, call_answer_cb, call_reject_cb, call_dismiss_cb);
    } else {
        map_view_create(scr);
        map_view_set_link(s_connected, settings_paired());
        nav_view_init();  // banner reappears if navigation is already live
    }
#if CONFIG_NAV_DEBUG_BLE
    ESP_LOGW(TAG, "screen -> %d (connected=%d paired=%d)", (int)which, s_connected,
             settings_paired());
#endif
    s_screen = which;
}

// Dismiss the call page and go back where the user was: the map, or the media
// page if the call interrupted it. Called from an LVGL click event, so the
// screen swap itself is deferred to the GUI loop.
static void return_from_call(void)
{
    s_screen_req = (s_screen_before_call == SCREEN_MEDIA) ? SCREEN_MEDIA : SCREEN_MAP;
    // bike-os: the ring opened Navigation over another app -> go back to it.
    if (s_call_took_focus) {
        s_call_took_focus = false;
        s_give_back = true;
    }
}

static void open_map(void)
{
    s_screen_req = SCREEN_MAP;
}

static void open_media(void)
{
    s_screen_req = SCREEN_MEDIA;
}

static void browse_cb(lv_event_t *e)
{
    (void)e;
    open_map();
}

static void apply_brightness(uint8_t pct)
{
    board_backlight_set(pct < NAV_BRIGHTNESS_MIN ? NAV_BRIGHTNESS_MIN : pct);
}

// remote: the phone asked (CMD 0x04) and removes its own bond, so ours goes
// too. Local (BOOT): keep the bond — the phone still holds it, and erasing
// ours makes Android loop on "pair again" popups. A fresh token is enough:
// the old QR scan no longer pairs, so the phone has to scan again.
static void unpair(bool remote)
{
    settings_set_paired(false);
    settings_new_token();
    if (remote) {
        nav_ble_forget_current();
    } else {
        nav_ble_disconnect();
    }
    map_view_set_link(false, false);
    s_screen = SCREEN_NONE;  // rebuild the QR with the new token
    if (s_parent) show_screen(SCREEN_PAIR);
}

static void media_changed(void)
{
    // The toast has a fixed width, so clip long titles instead of letting the
    // label run off the round screen.
    // The toast sits under the status pill, so keep it to one line: 20
    // characters, cut at a word boundary where there is one.
    const char *t = media_view_title();
    char buf[24];
    size_t n = 20;
    if (strlen(t) > n) {
        for (size_t i = n; i > 8; i--) {
            if (t[i] == ' ') {
                n = i;
                break;
            }
        }
        snprintf(buf, sizeof(buf), "%.*s...", (int)n, t);
    } else {
        snprintf(buf, sizeof(buf), "%s", t);
    }
    map_view_toast(buf);
}

static void local_reset(void)
{
    unpair(false);
}

// Theme change: cached tiles are dropped by tiles_set_dark() and re-read, so
// the repaint below is only for the placeholder colours.
// Minimal display on or off, from the phone, a long press on the map, or the
// stored setting at boot.
static void set_minimal(bool on)
{
    settings_set_minimal(on);
    map_view_set_minimal(on);
    nav_view_set_minimal(on);
    nav_ble_set_minimal(on);
}

static void toggle_minimal(void)
{
    bool on = !settings_minimal();
    set_minimal(on);
    map_view_toast(on ? "Minimal display" : "Map");
}

// Brightness from the device's own screen (the battery face). Returns the
// level now in use; a delta of 0 only asks.
static int local_brightness(int delta)
{
    int v = s_brightness_dirty ? s_brightness_pending : settings_brightness();
    if (!delta) return v;
    v += delta;
    if (v < NAV_BRIGHTNESS_MIN) v = NAV_BRIGHTNESS_MIN;
    if (v > 100) v = 100;
    apply_brightness((uint8_t)v);
    s_brightness_pending = (uint8_t)v;
    s_brightness_dirty = true;
    s_brightness_changed_ms = now_ms();
    return v;
}

static void set_theme(bool dark)
{
    tiles_set_dark(dark);
    settings_set_dark_map(dark);
    nav_ble_set_theme(dark);
    map_view_invalidate();
    ESP_LOGW(TAG, "%s map", dark ? "dark" : "light");
}

static bool valid_fix(const nav_position_t *p)
{
    if (p->lat_e7 == 0 && p->lon_e7 == 0) return false;  // "no fix yet"
    return p->lat_e7 >= -850000000 && p->lat_e7 <= 850000000 &&
           p->lon_e7 >= -1800000000 && p->lon_e7 <= 1800000000;
}

static void handle_cmd(const app_ev_t *ev)
{
    switch (ev->code) {
    case NAV_CMD_RECENTER:
        map_view_recenter();
        break;
    case NAV_CMD_SET_ZOOM:
        if (ev->len >= 1) map_view_set_zoom(ev->args[0]);
        break;
    case NAV_CMD_PING:
        break;
    case NAV_CMD_UNPAIR:
        unpair(true);
        break;
    case NAV_CMD_SET_BRIGHTNESS:
        if (ev->len >= 1) {
            uint8_t v = ev->args[0] > 100 ? 100 : ev->args[0];
            apply_brightness(v);
            // The app streams slider moves; persist once it settles.
            s_brightness_pending = v;
            s_brightness_dirty = true;
            s_brightness_changed_ms = now_ms();
        }
        break;
    case NAV_CMD_SET_THEME:
        if (ev->len >= 1) set_theme(ev->args[0] != 0);
        break;
    case NAV_CMD_SET_MINIMAL:
        if (ev->len >= 1) set_minimal(ev->args[0] != 0);
        break;
    case NAV_CMD_SLEEP:
        enter_deep_sleep();
        break;
    case NAV_CMD_PAIR:
        if (ev->len >= 6 && memcmp(ev->args, settings_token_str(), 6) == 0) {
            settings_set_paired(true);
            map_view_set_link(s_connected, true);
            if (s_parent) show_screen(SCREEN_MAP);
        } else {
            ESP_LOGW(TAG, "pair token mismatch");
        }
        break;
    default:
        break;
    }
}

// Assemble queued route chunks in order. Runs in the GUI task so map_view is
// touched from one task only.
static void process_route(void)
{
    route_chunk_t c;
    while (xQueueReceive(s_route_q, &c, 0) == pdTRUE) {
        if (c.flags & NAV_ROUTE_F_START) {
            s_route_seq = 0;
            s_route_bad = false;
            map_view_route_reset();
        }
        if (s_route_bad) continue;
        if (c.seq != s_route_seq) {
            s_route_bad = true;
            map_view_route_clear();
            continue;
        }
        s_route_seq++;
        if (c.count) map_view_route_add(c.lat, c.lon, c.count);
        if ((c.flags & NAV_ROUTE_F_END) && !s_route_bad) map_view_route_commit();
    }
}

static void process_events(void)
{
    app_ev_t ev;
    while (xQueueReceive(s_ev_q, &ev, 0) == pdTRUE) {
        // bike-os: the phone connecting or a command wakes the screen; the keep-alive
        // ping does not, or a connected phone would hold the screen on for ever.
        if (ev.type == EV_LINK_UP || (ev.type == EV_CMD && ev.code != NAV_CMD_PING)) note_activity();
        switch (ev.type) {
        case EV_LINK_UP:
            s_connected = true;
            map_view_set_link(true, settings_paired());
            media_view_set_link(true);
            break;
        case EV_LINK_DOWN:
            s_connected = false;
            map_view_set_link(false, settings_paired());
            media_view_set_link(false);
            // The phone is the navigation source: no link means no guidance
            // and no destination.
            nav_view_clear();
            map_view_set_dest(false, 0, 0);
            map_view_route_clear();
            bos_stops_clear();  // bike-os
            // A dropped link also ends any call UI: the phone can no longer
            // update it, and the page must not linger.
            call_view_clear();
            if (s_screen == SCREEN_CALL || s_screen_req == SCREEN_CALL) {
                return_from_call();
            }
            break;
        case EV_CMD:
            handle_cmd(&ev);
            break;
        }
    }
    nav_msg_t nm;
    if (xQueueReceive(s_nav_q, &nm, 0) == pdTRUE) {
        // bike-os: the phone sends STATE every second, drive or not. Only a drive
        // starting or its next turn changing wakes the screen (keep_awake holds it on
        // during a drive when "Keep screen on during navigation" is set).
        static bool nav_was_active;
        static uint8_t nav_last_maneuver;
        static uint16_t nav_last_dist;
        if (nm.have_state) {
            bool active = nm.st.flags & NAV_F_ACTIVE;
            bool new_turn = active && (nm.st.maneuver != nav_last_maneuver || nm.st.dist_m > nav_last_dist + 50);
            if ((active && !nav_was_active) || new_turn) note_activity();
            nav_was_active = active;
            nav_last_maneuver = nm.st.maneuver;
            nav_last_dist = nm.st.dist_m;
        }
        if (nm.have_street) nav_view_update(NULL, nm.street);
        if (nm.have_state) {
            nav_view_update(&nm.st, NULL);
            // Navigation stopped -> the destination pin goes with it. (No
            // point waiting for the next DEST with present=0; arrival may
            // never send one.)
            if (!(nm.st.flags & NAV_F_ACTIVE)) {
                map_view_set_dest(false, 0, 0);
                map_view_route_clear();
                bos_stops_clear();  // bike-os
            }
        }
        if (nm.have_dest) map_view_set_dest(nm.dest_present, nm.dest_lat_e7, nm.dest_lon_e7);
    }
    if (s_phone_dirty) {
        s_phone_dirty = false;
        map_view_set_phone(s_phone_batt, s_phone_charging, s_phone_24h, s_phone_epoch);
    }
    if (s_limit_dirty) {
        s_limit_dirty = false;
        nav_view_set_limit(s_limit_mph, s_limit_val);
        map_view_invalidate();
    }
    process_route();
    stops_msg_t sm;  // bike-os
    if (xQueueReceive(s_stops_q, &sm, 0) == pdTRUE) {
        bos_stops_set(sm.data, sm.len);
        map_view_invalidate();
    }
    roads_chunk_t rc;
    while (xQueueReceive(s_roads_q, &rc, 0) == pdTRUE) map_view_roads_chunk(rc.data, rc.len);
    if (xQueueReceive(s_traffic_q, &rc, 0) == pdTRUE) map_view_traffic(rc.data, rc.len);
    text_msg_t tm;
    if (xQueueReceive(s_text_q, &tm, 0) == pdTRUE) media_view_set_text(tm.title, tm.artist);
    nav_media_state_t ms;
    if (xQueueReceive(s_media_q, &ms, 0) == pdTRUE) {
        // bike-os: a new track or play/pause wakes the screen; the position the phone
        // sends every second does not.
        static uint8_t media_last_flags = 0xFF;
        static uint16_t media_last_art = 0xFFFF;
        if ((ms.flags & NAV_MEDIA_F_PLAYING) != (media_last_flags & NAV_MEDIA_F_PLAYING) ||
            (ms.art_id != media_last_art && media_last_art != 0xFFFF)) note_activity();
        media_last_flags = ms.flags;
        media_last_art = ms.art_id;
        media_view_set_state(&ms);
    }
    nav_position_t p;
    if (xQueueReceive(s_pos_q, &p, 0) == pdTRUE && valid_fix(&p)) {
        // bike-os: the recorder first; it also measures speed from position
        // change, which the speedometer uses when the phone sends none.
        ride_on_fix(p.lat_e7, p.lon_e7, p.speed_cms, p.fix != 0, now_ms());
        map_view_set_fix(p.lat_e7, p.lon_e7, ride_speed_cms(), p.cog_cdeg);
    }
    call_msg_t cm;
    if (xQueueReceive(s_call_q, &cm, 0) == pdTRUE) {
        note_activity();
        if (cm.ringing) {
#if CONFIG_NAV_DEBUG_BLE
            ESP_LOGW(TAG, "call ring: name='%s' number='%s' screen=%d", cm.name, cm.number,
                     (int)s_screen);
#endif
            // Take over the map or media page; never interrupt pairing. A
            // ring that arrives after the link dropped is stale: ignore it.
            // bike-os: a ring takes over from any app, not only the map/media
            // pages; never interrupt pairing.
            if (s_connected && settings_paired() && s_screen != SCREEN_PAIR) {
                call_view_set_caller(cm.name, cm.number);
                if (!s_parent) {
                    s_screen_before_call = SCREEN_MAP;
                    s_call_took_focus = true;
                    s_want_focus = true;
                } else if (s_screen != SCREEN_CALL) {
                    s_screen_before_call = s_screen;
                }
                s_screen_req = SCREEN_CALL;
            }
        } else {
#if CONFIG_NAV_DEBUG_BLE
            ESP_LOGW(TAG, "call end (screen=%d)", (int)s_screen);
#endif
            if (s_screen == SCREEN_CALL || s_screen_req == SCREEN_CALL) {
                return_from_call();
            }
        }
    }
    if (s_brightness_dirty && now_ms() - s_brightness_changed_ms > BRIGHTNESS_SAVE_MS) {
        s_brightness_dirty = false;
        settings_set_brightness(s_brightness_pending);
    }
}

#if CONFIG_NAV_DEBUG_DEMO_WALK
// Bench aid: feeds simulated fixes (1 Hz, ~25 km/h circle around the
// Gateway of India) through the same path as BLE positions.
static void demo_walk(void)
{
    static uint32_t last;
    static float ang;
    uint32_t t = now_ms();
    if (t - last < 1000) return;
    last = t;
    ang += 0.02f;
    nav_position_t p = {
        .lat_e7 = (int32_t)((18.9219 + 0.012 * sinf(ang)) * 1e7),
        .lon_e7 = (int32_t)((72.8347 + 0.012 * cosf(ang)) * 1e7),
        .speed_cms = 700,
        .cog_cdeg = (uint16_t)(fmodf(360.0f - ang * 57.2958f, 360.0f) * 100),
        .fix = 1,
    };
    xQueueOverwrite(s_pos_q, &p);
}
#endif


// ---------------------------------------------------------- power (bike-os)
// Screen-off timeout, the PWR key and deep sleep belong to bike-os now (sys.c,
// app_manager.c). What stays: activity from the phone wakes the screen, and the
// map's "tap again to sleep" plays firmware_ws146's sleep animation first.
static bool s_sleep_pending;
static uint32_t s_sleep_at;

static void note_activity(void)
{
    am_screen_wake();
}

static void enter_deep_sleep(void)
{
    if (s_sleep_pending) return;
    s_sleep_pending = true;
    s_sleep_at = now_ms() + BOOT_VIEW_SLEEP_MS;
    // The start-up animation run backwards, over whatever is showing.
    boot_view_play_sleep(lv_screen_active(), settings_device_name());
    ESP_LOGW(TAG, "sleeping");
}

static void sleep_tick(void)
{
    if (s_sleep_pending && (int32_t)(now_ms() - s_sleep_at) >= 0) {
        s_sleep_pending = false;
        map_view_flush_state();
        if (s_brightness_dirty) settings_set_brightness(s_brightness_pending);
        sys_deep_sleep();
    }
}

// bike-os: the phone's clock keeps the RTC right (other apps read the RTC).
static void sync_rtc_from_phone(uint32_t local_epoch)
{
    time_t t = (time_t)local_epoch;           // already local time
    struct tm tm;
    gmtime_r(&t, &tm);
    if (tm.tm_year + 1900 < 2024) return;
    datetime_t now;
    sys_get_time(&now);
    long a = (long)now.hour * 3600 + now.minute * 60 + now.second;
    long b = (long)tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    bool same_day = now.year == tm.tm_year + 1900 && now.month == tm.tm_mon + 1 && now.day == tm.tm_mday;
    if (same_day && labs(a - b) <= 30) return;
    datetime_t d = { .year = tm.tm_year + 1900, .month = tm.tm_mon + 1, .day = tm.tm_mday,
                     .hour = tm.tm_hour, .minute = tm.tm_min, .second = tm.tm_sec };
    sys_set_time(&d);
    ESP_LOGW(TAG, "RTC synced from phone");
}

// ---------------------------------------------------------- public API

void nav_core_init(void)
{
    tiles_init();
    // Stored preference wins over the build-time default.
    set_theme(settings_dark_map());
    map_view_init_state();
    // No map card: start on the minimal display, which draws the streets the
    // phone streams over BLE. A long press still switches if wanted.
    set_minimal(settings_minimal() || !sdcard_ok());
    map_view_set_mode_cb(toggle_minimal);
    map_view_set_brightness_cb(local_brightness);
    map_view_set_reset_cb(local_reset);
    map_view_set_media_cb(open_media);
    map_view_set_sleep_cb(enter_deep_sleep);
    // Tapping the nav banner re-centres, same as the recentre button (which is
    // hidden while the banner is up).
    nav_view_set_click_cb(map_view_recenter);
    media_view_set_change_cb(media_changed);
    media_art_init();

    s_ev_q = xQueueCreate(16, sizeof(app_ev_t));
    s_pos_q = xQueueCreate(1, sizeof(nav_position_t));
    s_media_q = xQueueCreate(1, sizeof(nav_media_state_t));
    s_text_q = xQueueCreate(1, sizeof(text_msg_t));
    s_nav_q = xQueueCreate(1, sizeof(nav_msg_t));
    s_call_q = xQueueCreate(1, sizeof(call_msg_t));
    // bike-os fix: the whole line can arrive in one burst (2048 points = 67
    // chunks). With 8 slots a burst overflowed, a chunk was dropped, and the
    // sequence check then threw the whole route away. ~17 KB, in PSRAM.
#ifdef ESP_PLATFORM
    s_route_q = xQueueCreateWithCaps(ROUTE_Q_LEN, sizeof(route_chunk_t), MALLOC_CAP_SPIRAM);
    if (!s_route_q) s_route_q = xQueueCreate(ROUTE_Q_LEN, sizeof(route_chunk_t));
#else
    s_route_q = xQueueCreate(ROUTE_Q_LEN, sizeof(route_chunk_t));
#endif
    s_roads_q = xQueueCreate(16, sizeof(roads_chunk_t));
    s_traffic_q = xQueueCreate(1, sizeof(roads_chunk_t));
    s_stops_q = xQueueCreate(1, sizeof(stops_msg_t));

    static const nav_ble_cbs_t cbs = {
        .on_position = ble_on_position,
        .on_cmd = ble_on_cmd,
        .on_link = ble_on_link,
        .on_media_state = ble_on_media_state,
        .on_media_art = media_art_feed,
        .on_media_text = ble_on_media_text,
        .on_nav_state = ble_on_nav_state,
        .on_nav_dest = ble_on_nav_dest,
        .on_nav_limit = ble_on_nav_limit,
        .on_phone_info = ble_on_phone_info,
        .on_traffic = ble_on_traffic,
        .on_roads_chunk = ble_on_roads_chunk,
        .on_route_chunk = ble_on_route_chunk,
        .on_call_ring = ble_on_call_ring,
        .on_call_end = ble_on_call_end,
        .on_stops = ble_on_stops,  // bike-os
    };
    nav_ble_start(&cbs, settings_device_name());
}

void nav_core_poll(void)
{
    static bool was_active;
    static uint32_t last_fps_pub;
    uint32_t t = now_ms();

    // Never time the screen out while moving.
    if (map_view_is_moving()) lv_display_trigger_activity(NULL);
#if CONFIG_NAV_DEBUG_DEMO_WALK
    demo_walk();
#endif
    if (s_phone_dirty) sync_rtc_from_phone(s_phone_epoch);   // before process_events clears it
    process_events();
    sleep_tick();
    ride_tick(now_ms(), nav_view_active());  // bike-os: auto start/stop; now, not the loop's t: the fixes above were stamped after it
    map_view_tick();
    media_view_tick();
    // Orange rim on the side of the next turn; only over the map/media pages.
    turn_edge_tick(s_parent && (s_screen == SCREEN_MAP || s_screen == SCREEN_MEDIA));
    // During a drive the music controls are a detour: left untouched for
    // MEDIA_IDLE_BACK_MS they give the screen back to the navigation.
    if (s_parent && s_screen == SCREEN_MEDIA && s_screen_req == SCREEN_NONE && nav_view_active() &&
        t - s_media_touch_ms >= MEDIA_IDLE_BACK_MS) {
        s_screen_req = SCREEN_MAP;
    }
    if (touch_get()->n) s_media_touch_ms = t;

    // bike-os: a route starting pulls Navigation to the front.
    bool active = nav_view_active();
    if (active && !was_active && !s_parent) s_want_focus = true;
    was_active = active;

    if (s_screen_req != SCREEN_NONE) {
        screen_t req = s_screen_req;
        if (s_parent) {
            s_screen_req = SCREEN_NONE;
            show_screen(req);
        }
    }
    if (t - last_fps_pub > 1000) {
        last_fps_pub = t;
        nav_ble_set_fps((uint8_t)(display_fps() > 255 ? 255 : display_fps()));
    }
}

void nav_core_attach(lv_obj_t *scr, bool media)
{
    s_parent = scr;
    s_screen = SCREEN_NONE;
    screen_t want = s_screen_req != SCREEN_NONE ? s_screen_req : (media ? SCREEN_MEDIA : SCREEN_MAP);
    s_screen_req = SCREEN_NONE;
    if (!settings_paired() && want != SCREEN_CALL) want = SCREEN_PAIR;
    show_screen(want);
}

void nav_core_detach(void)
{
    map_view_destroy();
    media_view_destroy();
    nav_view_destroy();
    call_view_destroy();
    turn_edge_tick(false);
    if (s_screen == SCREEN_CALL) s_screen_req = SCREEN_CALL;   // still ringing: come back to it
    s_screen = SCREEN_NONE;
    s_parent = NULL;
    s_call_took_focus = false;
}

bool nav_core_take_focus_request(void)
{
    bool w = s_want_focus;
    s_want_focus = false;
    return w;
}

bool nav_core_take_give_back(void)
{
    bool g = s_give_back;
    s_give_back = false;
    return g;
}

bool nav_core_connected(void) { return s_connected; }
bool nav_core_on_media(void) { return s_screen == SCREEN_MEDIA; }
const char *nav_core_media_title(void) { return media_view_title(); }
bool nav_core_navigating(void) { return nav_view_active(); }
bool nav_core_moving(void) { return map_view_is_moving(); }
int  nav_core_dist_m(void) { return nav_view_dist_m(); }

void nav_core_key(bool long_press)
{
    if (!s_parent) return;
    if (long_press) {
        if (s_screen == SCREEN_MAP) toggle_minimal();
        return;
    }
    if (s_screen == SCREEN_MEDIA) s_screen_req = SCREEN_MAP;
    else if (s_screen == SCREEN_MAP) map_view_recenter();
}

void nav_core_forget_phone(void)
{
    unpair(false);
}
