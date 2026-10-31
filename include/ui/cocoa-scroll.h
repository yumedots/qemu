/*
 * Cocoa precise-scroll conversion, independent of AppKit for testing.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_COCOA_SCROLL_H
#define QEMU_COCOA_SCROLL_H

#include <limits.h>
#include <math.h>

typedef struct CocoaScroll {
    double residual_x;
    double residual_y;
} CocoaScroll;

typedef struct CocoaScrollFrame {
    int x;
    int y;
} CocoaScrollFrame;

/*
 * AppKit reports precise deltas in points.  Keep one Linux high-resolution
 * wheel unit per point: this avoids inventing acceleration on top of macOS's
 * device acceleration, while retaining sub-point deltas between events.
 */
static inline int cocoa_scroll_axis(double delta, double *residual)
{
    double value;
    int whole;

    if (!isfinite(delta)) {
        *residual = 0.0;
        return 0;
    }

    value = delta + *residual;
    if (value >= INT_MAX) {
        *residual = 0.0;
        return INT_MAX;
    }
    if (value <= INT_MIN) {
        *residual = 0.0;
        return INT_MIN;
    }

    whole = (int)trunc(value);
    *residual = value - whole;
    return whole;
}

static inline CocoaScrollFrame cocoa_scroll_update(CocoaScroll *scroll,
                                                    double x, double y)
{
    CocoaScrollFrame frame = {
        .x = cocoa_scroll_axis(x, &scroll->residual_x),
        .y = cocoa_scroll_axis(y, &scroll->residual_y),
    };
    return frame;
}

static inline void cocoa_scroll_cancel(CocoaScroll *scroll)
{
    scroll->residual_x = 0.0;
    scroll->residual_y = 0.0;
}

#endif
