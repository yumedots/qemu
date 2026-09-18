/*
 * The frame a D-Bus client's surface is written with, and where the read of it lands.
 *
 * Copyright (c) 2026
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#include "qemu/osdep.h"
#include "ui/dbus-surface.h"

#define W 4
#define H 3
#define STRIDE (W * 4 + 8)

typedef struct Read {
    uint8_t *pixels;
    uint8_t *landed;
} Read;

static void read_pixels(void *opaque, uint8_t *into)
{
    Read *read = opaque;

    read->landed = into;
    memcpy(into, read->pixels, W * H * 4);
}

/* the surface the client draws, held apart from the frame the read comes back with */
static uint8_t surface[STRIDE * H];

static void test_the_read_does_not_land_in_the_surface_the_client_draws(void)
{
    uint8_t frame[W * H * 4];
    uint8_t scratch[W * H * 4];
    Read read = { .pixels = frame };
    int i;

    for (i = 0; i < W * H; i++) {
        frame[i * 4] = 0x11;
        frame[i * 4 + 1] = 0x22;
        frame[i * 4 + 2] = 0x33;
        frame[i * 4 + 3] = 0xff;
    }

    dbus_gl_read_into_surface(surface, STRIDE, scratch, W, H, false, read_pixels,
                              &read);

    g_assert_true(read.landed == scratch);
    g_assert_true(read.landed != surface);
}

/* every row of the surface is the frame with the channels the way round it holds them */
static void test_a_row_of_the_surface_is_already_the_right_way_round(void)
{
    uint8_t frame[W * H * 4];
    uint8_t scratch[W * H * 4];
    Read read = { .pixels = frame };
    int line, pixel;

    for (line = 0; line < H; line++) {
        for (pixel = 0; pixel < W; pixel++) {
            int at = (line * W + pixel) * 4 + line;

            frame[(line * W + pixel) * 4] = at;
            frame[(line * W + pixel) * 4 + 1] = 0x22;
            frame[(line * W + pixel) * 4 + 2] = at + 0x40;
            frame[(line * W + pixel) * 4 + 3] = 0xff;
        }
    }
    memset(surface, 0xaa, sizeof(surface));

    dbus_gl_read_into_surface(surface, STRIDE, scratch, W, H, false, read_pixels,
                              &read);

    for (line = 0; line < H; line++) {
        for (pixel = 0; pixel < W; pixel++) {
            uint8_t *held = surface + line * STRIDE + pixel * 4;

            g_assert_cmphex(held[0], ==, frame[(line * W + pixel) * 4 + 2]);
            g_assert_cmphex(held[1], ==, frame[(line * W + pixel) * 4 + 1]);
            g_assert_cmphex(held[2], ==, frame[(line * W + pixel) * 4]);
            g_assert_cmphex(held[3], ==, 0xff);
        }
        /* a surface whose rows are wider than the read keeps what follows the row */
        for (pixel = W * 4; pixel < STRIDE; pixel++) {
            g_assert_cmphex(*(surface + line * STRIDE + pixel), ==, 0xaa);
        }
    }
}

/* a read whose first row is the top one is turned over on the way in */
static void test_a_bottom_up_read_is_flipped_on_the_way_in(void)
{
    uint8_t frame[W * H * 4];
    uint8_t scratch[W * H * 4];
    Read read = { .pixels = frame };
    int line, pixel;

    for (line = 0; line < H; line++) {
        for (pixel = 0; pixel < W; pixel++) {
            frame[(line * W + pixel) * 4] = line + 1;
            frame[(line * W + pixel) * 4 + 3] = 0xff;
        }
    }

    dbus_gl_read_into_surface(surface, STRIDE, scratch, W, H, true, read_pixels,
                              &read);

    for (line = 0; line < H; line++) {
        uint8_t *held = surface + line * STRIDE;

        g_assert_cmphex(held[2], ==, H - line);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/dbus-surface/read-lands-in-our-buffer",
                    test_the_read_does_not_land_in_the_surface_the_client_draws);
    g_test_add_func("/dbus-surface/row-is-the-right-way-round",
                    test_a_row_of_the_surface_is_already_the_right_way_round);
    g_test_add_func("/dbus-surface/bottom-up-read-is-flipped",
                    test_a_bottom_up_read_is_flipped_on_the_way_in);
    return g_test_run();
}
