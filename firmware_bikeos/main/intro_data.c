/* main/assets/intro.gif, linked in by EMBED_FILES (CMakeLists.txt). */
#include <stddef.h>
#include <stdint.h>
#include "bos_intro.h"

extern const uint8_t intro_gif_start[] asm("_binary_intro_gif_start");
extern const uint8_t intro_gif_end[] asm("_binary_intro_gif_end");

const uint8_t *bos_intro_gif(size_t *len)
{
    *len = (size_t)(intro_gif_end - intro_gif_start);
    return intro_gif_start;
}
