#pragma once
// Turn edge: an orange arc on the left or right rim of the round screen that
// says which way the next turn goes, and blinks faster as it gets close.
//
//   > TURN_EDGE_SHOW_M  hidden
//   <= SHOW_M           steady
//   <= BLINK_M          blinks slowly (2 Hz)
//   <= FAST_M           blinks fast (4 Hz)
//
// Lives on LVGL's top layer, so it draws over every screen it is allowed on
// (map faces and music) without touching their code. GUI task only.
#include <stdbool.h>

#define TURN_EDGE_SHOW_M 400
#define TURN_EDGE_BLINK_M 200
#define TURN_EDGE_FAST_M 60

// Call once per GUI loop. `allowed` is false on screens it must not cover
// (boot, pairing, an incoming call).
void turn_edge_tick(bool allowed);
