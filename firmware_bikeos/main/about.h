#pragma once
/* Settings > About (about.c): text for its four sections. GUI task only. */
#include <stddef.h>

void about_open(void);    /* the IMU and the chip temperature sensor on while About is shown */
void about_close(void);   /* and off again */

void about_board(char *out, size_t n);     /* model, chip, flash, PSRAM, display, Bluetooth */
void about_usage(char *out, size_t n);     /* RAM, PSRAM, app, rides storage, SD card */
void about_sensors(char *out, size_t n);   /* battery, RTC, IMU, chip temperature, touch, speaker */
void about_firmware(char *out, size_t n);  /* version, IDF, build, uptime, last start */
