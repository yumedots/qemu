/*
 * Cocoa pinch geometry, independent of AppKit for deterministic testing.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_COCOA_PINCH_H
#define QEMU_COCOA_PINCH_H

#include <stdbool.h>
#include <math.h>

typedef struct CocoaPinch {
    bool active;
    double scale;
} CocoaPinch;

enum {
    COCOA_PINCH_CENTER = 16384,
    COCOA_PINCH_RADIUS = 125,
    COCOA_PINCH_AXIS_MAX = 32767,
};

static inline void cocoa_pinch_begin(CocoaPinch *pinch)
{
    pinch->active = true;
    pinch->scale = 1.0;
}

static inline bool cocoa_pinch_end(CocoaPinch *pinch)
{
    bool active = pinch->active;
    pinch->active = false;
    pinch->scale = 1.0;
    return active;
}

/* NSEvent.magnification is the relative change since the previous event. */
static inline bool cocoa_pinch_update(CocoaPinch *pinch, double delta)
{
    if (!pinch->active || !isfinite(delta) || delta <= -1.0) {
        return false;
    }
    double scale = pinch->scale * (1.0 + delta);
    pinch->scale = fmin(3.5, fmax(0.125, scale));
    return true;
}

static inline int cocoa_pinch_x(const CocoaPinch *pinch, int slot)
{
    int radius = (int)lround(COCOA_PINCH_RADIUS * pinch->scale);
    return COCOA_PINCH_CENTER + (slot == 0 ? -radius : radius);
}

typedef struct CocoaPinchFrame {
    bool down;
    int x[2];
} CocoaPinchFrame;

typedef void (*CocoaPinchEmit)(void *opaque, const CocoaPinchFrame *frame);

enum {
    COCOA_PINCH_BEGIN = 1,
    COCOA_PINCH_END = 2,
    COCOA_PINCH_CANCEL = 4,
};

static inline void cocoa_pinch_emit(const CocoaPinch *pinch,
                                    CocoaPinchEmit emit, void *opaque)
{
    CocoaPinchFrame frame = {
        .down = pinch->active,
        .x = { cocoa_pinch_x(pinch, 0), cocoa_pinch_x(pinch, 1) },
    };
    emit(opaque, &frame);
}

static inline void cocoa_pinch_cancel(CocoaPinch *pinch,
                                      CocoaPinchEmit emit, void *opaque)
{
    if (cocoa_pinch_end(pinch)) {
        cocoa_pinch_emit(pinch, emit, opaque);
    }
}

/* Input is dropped while QEMU is paused. Clear stale guest slots on resume,
 * even if focus loss already cleared the local state during the pause. */
static inline void cocoa_pinch_vm_state(CocoaPinch *pinch, bool running,
                                        CocoaPinchEmit emit, void *opaque)
{
    cocoa_pinch_end(pinch);
    if (running) {
        cocoa_pinch_emit(pinch, emit, opaque);
    }
}

/* Only a new begin can re-establish contacts after cancellation/focus loss. */
static inline bool cocoa_pinch_event(CocoaPinch *pinch, unsigned phase,
                                     bool eligible, double delta,
                                     CocoaPinchEmit emit, void *opaque)
{
    if (!eligible) {
        cocoa_pinch_cancel(pinch, emit, opaque);
        return false;
    }
    if (phase & COCOA_PINCH_CANCEL) {
        cocoa_pinch_cancel(pinch, emit, opaque);
        return true;
    }
    if (!isfinite(delta) || delta <= -1.0) {
        cocoa_pinch_cancel(pinch, emit, opaque);
        return true;
    }
    if (phase & COCOA_PINCH_BEGIN) {
        cocoa_pinch_cancel(pinch, emit, opaque);
        cocoa_pinch_begin(pinch);
        cocoa_pinch_emit(pinch, emit, opaque);
    }
    if (pinch->active && delta != 0.0) {
        cocoa_pinch_update(pinch, delta);
        cocoa_pinch_emit(pinch, emit, opaque);
    }
    if (phase & COCOA_PINCH_END) {
        cocoa_pinch_cancel(pinch, emit, opaque);
    }
    return true;
}

#endif
