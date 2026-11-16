#pragma once
#include <stdint.h>
#include <string.h>

/* Snapshot only the state that can affect the draw. Inactive lights are never
   serialized, compared or read by the shader; leave their storage untouched. */
static inline void r_tnl_copy(int32_t* to, const int32_t* from) {
    uint32_t lights = (uint32_t)from[25];
    if (lights > 8) { memcpy(to, from, 338 * 4); return; }
    memcpy(to, from, 99 * 4);
    memcpy(to + 99, from + 99, lights * 80);
    memcpy(to + 259, from + 259, 79 * 4);
}

/* State arrays are aligned int32 words. libc memcmp walks these byte by byte
   in the wasm build; comparing words preserves exact float/NaN bit patterns. */
static inline int r_words_equal(const int32_t* a, const int32_t* b, uint32_t count) {
    while (count >= 4) {
        if ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3])) return 0;
        a += 4; b += 4; count -= 4;
    }
    while (count--) if (*a++ != *b++) return 0;
    return 1;
}

/* Only the first light-count entries are sent to the renderer or read by the
   shader. Unused light slots are not part of the effective draw state. */
static inline int r_tnl_material_equal(const int32_t* a, const int32_t* b) {
    return (uint32_t)a[25] <= 8 && r_words_equal(a + 21, b + 21, 5) &&
        r_words_equal(a + 81, b + 81, 15) && r_words_equal(a + 99, b + 99, 20u * (uint32_t)a[25]);
}

static inline int r_tnl_batch_equal(const int32_t* a, const int32_t* b) {
    return a[13] == b[13] && (uint32_t)a[25] <= 8 && r_words_equal(a + 15, b + 15, 12) &&
        r_words_equal(a + 43, b + 43, 56) && r_words_equal(a + 259, b + 259, 78) &&
        r_words_equal(a + 99, b + 99, 20u * (uint32_t)a[25]);
}
