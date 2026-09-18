/*
 * Putting a frame read back from GL into the surface a D-Bus client handed over.
 *
 * GL ES has no GL_BGRA to read into and no row length, so the channels come back in
 * the other order for the BGRA surface they are going into and the rows have to be
 * packed by hand.  The client draws that surface the whole time it is being written,
 * so the read does not land in it: it lands in a buffer of ours, is turned the right
 * way round there, and only then is it copied into the surface a row at a time.  A
 * window at the size whose rows needed no padding showed one frame in eight with red
 * and blue exchanged while the read went straight into the surface and was swapped
 * where it lay, because the rows the swap had not reached yet were still the read's
 * own order.
 */
#ifndef DBUS_SURFACE_H
#define DBUS_SURFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void dbus_gl_swap_red_blue(uint8_t *pixels, int width)
{
    int i;

    for (i = 0; i < width; i++) {
        uint8_t red = pixels[i * 4];

        pixels[i * 4] = pixels[i * 4 + 2];
        pixels[i * 4 + 2] = red;
    }
}

/*
 * Rows come back bottom up, so a read that belongs to a texture saying its first row
 * is the top one is handed over the other way round.
 */
static inline void dbus_gl_flip_rows(uint8_t *base, int x, int y, int w, int h,
                                     size_t stride)
{
    uint8_t *tmp = g_malloc((size_t)w * 4);
    int line;

    for (line = 0; line < h / 2; line++) {
        uint8_t *top = base + (size_t)(y + line) * stride + (size_t)x * 4;
        uint8_t *bottom = base + (size_t)(y + h - 1 - line) * stride +
                          (size_t)x * 4;

        memcpy(tmp, top, (size_t)w * 4);
        memcpy(top, bottom, (size_t)w * 4);
        memcpy(bottom, tmp, (size_t)w * 4);
    }

    g_free(tmp);
}

/*
 * `read` is the framebuffer read of a w x h rect, which must put its pixels in the
 * buffer it is given and nowhere else.
 */
static inline void dbus_gl_read_into_surface(uint8_t *dst, size_t dst_stride,
                                             uint8_t *scratch, int w, int h,
                                             bool flip,
                                             void (*read)(void *opaque,
                                                          uint8_t *into),
                                             void *opaque)
{
    int line;

    read(opaque, scratch);
    for (line = 0; line < h; line++) {
        dbus_gl_swap_red_blue(scratch + (size_t)line * w * 4, w);
    }
    if (flip) {
        dbus_gl_flip_rows(scratch, 0, 0, w, h, (size_t)w * 4);
    }
    for (line = 0; line < h; line++) {
        memcpy(dst + (size_t)line * dst_stride,
               scratch + (size_t)line * w * 4, (size_t)w * 4);
    }
}

#endif
