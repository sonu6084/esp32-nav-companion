/* MinGW has no gmtime_r: the Windows simulator build only (build_win.sh). */
#include <time.h>
struct tm *gmtime_r(const time_t *t, struct tm *out) { return gmtime_s(out, t) == 0 ? out : NULL; }
