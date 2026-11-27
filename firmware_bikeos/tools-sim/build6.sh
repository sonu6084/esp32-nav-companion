#!/bin/sh
# Host simulator for bike-os v0.5 (LVGL 9.6). Runs the real firmware_ws146 view
# code + bike-os apps with a FreeRTOS/NVS/board shim (platform.c, stub/).
# Needs gcc and a cJSON source dir (ESP-IDF's components/json/cJSON works).
set -e
cd "$(dirname "$0")"
R=..; B=$R/main; N=$B/nav; L=$R/components/lvgl; J=${CJSON:-$IDF_PATH/components/json/cJSON}
FL="-O1 -g -w -fsanitize=address,undefined -DLV_CONF_INCLUDE_SIMPLE -I. -Istub -I$L -I$L/include -I$B -I$N -I$B/apps -I$B/fonts -I$B/icons -I$J"
mkdir -p obj out
[ -f liblv.a ] || { for f in $(find $L/src -name "*.c"); do gcc $FL -c $f -o obj/$(echo $f | md5sum | cut -c1-12).o; done; ar rcs liblv.a obj/*.o; }
gcc $FL sim6.c platform.c $B/ride.c $B/bos_faces.c $B/nav_core.c $B/app_manager.c $B/home.c $B/ui_theme.c $B/ui_settime.c $B/alert.c $B/cfg.c \
    $B/weather_store.c $J/cJSON.c $B/apps/*.c $B/fonts/*.c $B/icons/*.c \
    $N/map_view.c $N/nav_view.c $N/media_view.c $N/call_view.c $N/turn_edge.c $N/ui_pair.c $N/ui_common.c \
    $N/boot_view.c $N/settings.c liblv.a -o sim6 -lm
ASAN_OPTIONS=detect_leaks=0 mkdir -p out/rides; ./sim6
