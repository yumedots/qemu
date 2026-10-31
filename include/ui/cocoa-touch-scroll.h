/*
 * Cocoa precise scrolling as two touchpad contacts, independent of AppKit
 * for deterministic testing.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_COCOA_TOUCH_SCROLL_H
#define QEMU_COCOA_TOUCH_SCROLL_H

#include <stdbool.h>
#include <math.h>

/*
 * Positions use the pinch touchpad's 0-32767 axes, 10 units/mm. Contacts sit
 * 12.4 mm apart and stay 12.5% inside every edge, clear of libinput's palm
 * zones (8 mm at the sides, 5% at the top) and thumb zone (the bottom 15%).
 */
enum {
    COCOA_TOUCH_SCROLL_AXIS_MAX = 32767,
    COCOA_TOUCH_SCROLL_CENTER = 16384,
    COCOA_TOUCH_SCROLL_SPREAD = 62,
    COCOA_TOUCH_SCROLL_MIN = 4096 + COCOA_TOUCH_SCROLL_SPREAD,
    COCOA_TOUCH_SCROLL_MAX = COCOA_TOUCH_SCROLL_AXIS_MAX - 4096 -
                             COCOA_TOUCH_SCROLL_SPREAD,
};

/*
 * 0.12 mm per point for fast scrolls. The guest's touchpad scroll factor
 * tunes the speed, as on real hardware.
 */
#define COCOA_TOUCH_SCROLL_UNITS_PER_POINT 1.2

/*
 * libinput starts a two-finger scroll once both touches have moved 1.5 mm,
 * and discards that movement. A first move of 1.6 mm in the scroll's
 * direction, in a frame of its own, starts it, so no scrolling is lost.
 */
#define COCOA_TOUCH_SCROLL_PREROLL 16.0

typedef struct CocoaTouchScroll {
    bool active;
    /* The current AppKit gesture reached the guest as finger scrolling. */
    bool owns_momentum;
    /* libinput has started scrolling with the current contacts. */
    bool started;
    unsigned gesture;
    double x;
    double y;
} CocoaTouchScroll;

typedef struct CocoaTouchScrollFrame {
    bool down;
    int x[2];
    int y;
    int id[2];
} CocoaTouchScrollFrame;

typedef void (*CocoaTouchScrollEmit)(void *opaque,
                                     const CocoaTouchScrollFrame *frame);

enum {
    COCOA_TOUCH_SCROLL_BEGIN = 1,
    COCOA_TOUCH_SCROLL_END = 2,
    COCOA_TOUCH_SCROLL_CANCEL = 4,
    COCOA_TOUCH_SCROLL_MOMENTUM = 8,
    COCOA_TOUCH_SCROLL_MOMENTUM_END = 16,
};

/*
 * Every landing gets fresh tracking IDs, never the pinch bridge's 0 and 1.
 * A release dropped by a full guest queue then reads as a new touch rather
 * than as the old contacts jumping.
 */
static inline int cocoa_touch_scroll_id(const CocoaTouchScroll *scroll,
                                        int slot)
{
    return 2 + 2 * (int)(scroll->gesture % 32767) + slot;
}

static inline void cocoa_touch_scroll_emit(const CocoaTouchScroll *scroll,
                                           CocoaTouchScrollEmit emit,
                                           void *opaque)
{
    int x = (int)lround(scroll->x);
    CocoaTouchScrollFrame frame = {
        .down = scroll->active,
        .x = { x - COCOA_TOUCH_SCROLL_SPREAD, x + COCOA_TOUCH_SCROLL_SPREAD },
        .y = (int)lround(scroll->y),
        .id = { cocoa_touch_scroll_id(scroll, 0),
                cocoa_touch_scroll_id(scroll, 1) },
    };
    emit(opaque, &frame);
}

static inline void cocoa_touch_scroll_cancel(CocoaTouchScroll *scroll,
                                             CocoaTouchScrollEmit emit,
                                             void *opaque)
{
    if (scroll->active) {
        scroll->active = false;
        cocoa_touch_scroll_emit(scroll, emit, opaque);
    }
}

static inline void cocoa_touch_scroll_begin(CocoaTouchScroll *scroll,
                                            double x, double y,
                                            CocoaTouchScrollEmit emit,
                                            void *opaque)
{
    scroll->active = true;
    scroll->owns_momentum = true;
    scroll->started = false;
    scroll->gesture++;
    scroll->x = x;
    scroll->y = y;
    cocoa_touch_scroll_emit(scroll, emit, opaque);
}

static inline bool cocoa_touch_scroll_inside(double value)
{
    return value >= COCOA_TOUCH_SCROLL_MIN && value <= COCOA_TOUCH_SCROLL_MAX;
}

/*
 * Slow and normal scrolls move 0.35 times as far as fast ones. The gain rises
 * smoothly from 4 to 40 points per event, so flicks keep their reach.
 */
static inline double cocoa_touch_scroll_gain(double points)
{
    double t = fmin(1.0, fmax(0.0, (points - 4.0) / 36.0));

    return 0.35 + 0.65 * t * t * (3.0 - 2.0 * t);
}

/* A move that reaches an edge lifts and lands again at the opposite edge. */
static inline void cocoa_touch_scroll_place(CocoaTouchScroll *scroll,
                                            double x, double y,
                                            CocoaTouchScrollEmit emit,
                                            void *opaque)
{
    if (cocoa_touch_scroll_inside(x) && cocoa_touch_scroll_inside(y)) {
        scroll->x = x;
        scroll->y = y;
        cocoa_touch_scroll_emit(scroll, emit, opaque);
        return;
    }
    if (!cocoa_touch_scroll_inside(x)) {
        x = x < COCOA_TOUCH_SCROLL_MIN ? COCOA_TOUCH_SCROLL_MAX :
                                         COCOA_TOUCH_SCROLL_MIN;
    }
    if (!cocoa_touch_scroll_inside(y)) {
        y = y < COCOA_TOUCH_SCROLL_MIN ? COCOA_TOUCH_SCROLL_MAX :
                                         COCOA_TOUCH_SCROLL_MIN;
    }
    cocoa_touch_scroll_cancel(scroll, emit, opaque);
    cocoa_touch_scroll_begin(scroll, x, y, emit, opaque);
}

/*
 * Contacts move as libinput's default, non-natural finger scrolling expects,
 * so the guest scrolls in the direction the wheel path used.
 */
static inline void cocoa_touch_scroll_move(CocoaTouchScroll *scroll,
                                           double dx, double dy,
                                           CocoaTouchScrollEmit emit,
                                           void *opaque)
{
    double points = hypot(dx, dy);
    double units = COCOA_TOUCH_SCROLL_UNITS_PER_POINT *
                   cocoa_touch_scroll_gain(points);

    if (!scroll->started) {
        scroll->started = true;
        cocoa_touch_scroll_place(scroll,
                                 scroll->x - dx / points * COCOA_TOUCH_SCROLL_PREROLL,
                                 scroll->y - dy / points * COCOA_TOUCH_SCROLL_PREROLL,
                                 emit, opaque);
    }
    cocoa_touch_scroll_place(scroll, scroll->x - dx * units,
                             scroll->y - dy * units, emit, opaque);
}

/*
 * Returns false when the wheel path should handle the event: outside the
 * focused guest view, invalid deltas, or a gesture that began elsewhere.
 * Momentum after finger scrolling is dropped: the guest starts its own
 * kinetic scrolling when the fingers lift, as on real hardware.
 */
static inline bool cocoa_touch_scroll_event(CocoaTouchScroll *scroll,
                                            unsigned phase, bool eligible,
                                            double dx, double dy,
                                            CocoaTouchScrollEmit emit,
                                            void *opaque)
{
    if (phase & COCOA_TOUCH_SCROLL_MOMENTUM) {
        bool owned = scroll->owns_momentum;

        if (phase & COCOA_TOUCH_SCROLL_MOMENTUM_END) {
            scroll->owns_momentum = false;
        }
        return owned;
    }
    if (phase & COCOA_TOUCH_SCROLL_BEGIN) {
        scroll->owns_momentum = false;
    }
    if (!eligible || !isfinite(dx) || !isfinite(dy)) {
        cocoa_touch_scroll_cancel(scroll, emit, opaque);
        return false;
    }
    if (phase & COCOA_TOUCH_SCROLL_CANCEL) {
        cocoa_touch_scroll_cancel(scroll, emit, opaque);
        return true;
    }
    if (phase & COCOA_TOUCH_SCROLL_BEGIN) {
        cocoa_touch_scroll_cancel(scroll, emit, opaque);
        cocoa_touch_scroll_begin(scroll, COCOA_TOUCH_SCROLL_CENTER,
                                 COCOA_TOUCH_SCROLL_CENTER, emit, opaque);
    }
    if (!scroll->active) {
        return false;
    }
    if (dx != 0.0 || dy != 0.0) {
        cocoa_touch_scroll_move(scroll, dx, dy, emit, opaque);
    }
    if (phase & COCOA_TOUCH_SCROLL_END) {
        cocoa_touch_scroll_cancel(scroll, emit, opaque);
    }
    return true;
}

#endif
