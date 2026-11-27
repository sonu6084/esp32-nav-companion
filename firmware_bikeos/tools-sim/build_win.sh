#!/bin/sh
# Windows (MSYS2 MinGW gcc) build of a host simulator, for screenshots of the UI:
#   PATH=/c/msys64/mingw64/bin:$PATH sh tools-sim/build_win.sh sim6
# LVGL comes from managed_components/ (run idf.py build once first). No sanitizers:
# MinGW has none. Screens land in tools-sim/out/*.ppm.
set -e
cd "$(dirname "$0")"
SIM=${1:-sim6}
R=..; B=$R/main; N=$B/nav; L=$R/managed_components/lvgl__lvgl
FL="-O1 -g -w -DLV_CONF_INCLUDE_SIMPLE -I. -Istub -I$L -I$L/include -I$B -I$N -I$B/apps -I$B/fonts"
mkdir -p obj_win out out/rides
if [ ! -f liblv_win.a ]; then
    for f in $(find $L/src -name "*.c"); do gcc $FL -c $f -o obj_win/$(echo $f | md5sum | cut -c1-12).o; done
    ar rcs liblv_win.a obj_win/*.o
fi
gcc $FL $SIM.c platform.c win_compat.c $B/ride.c $B/bos_faces.c $B/nav_core.c $B/app_manager.c $B/home.c $B/ui_theme.c $B/ui_settime.c $B/alert.c $B/cfg.c \
    $B/apps/*.c $B/fonts/*.c \
    $N/map_view.c $N/nav_view.c $N/media_view.c $N/call_view.c $N/turn_edge.c $N/ui_pair.c $N/ui_common.c \
    $N/boot_view.c $N/settings.c liblv_win.a -o $SIM.exe -lm
./$SIM.exe
