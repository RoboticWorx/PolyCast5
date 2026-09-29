#include <stdbool.h>
#include <string.h>

#include "gpio_remote.h"

#define F_HELD 0x01 // Driven low
#define F_OPEN 0x02 // The held press has had no UP yet
#define F_GAP 0x04 // Forced released until gap_ms past start_ms
#define F_SEEN 0x08 // apply() has returned the current press or gap at least once
#define F_PEND_OPEN 0x10 // The queue holds a DOWN of this pin that has had no UP yet

#define Q_PIN 0x07
#define Q_OPEN 0x80 // Queue entry of a DOWN with no UP yet: armed as an open press

// Elapsed ms, wrap-safe. A timestamp ahead of now counts as no time at all
static uint32_t since(uint32_t now_ms, uint32_t then_ms)
{
    const int32_t d = (int32_t)(now_ms - then_ms);
    return (d > 0) ? (uint32_t)d : 0;
}

static void press(gpio_remote_t *r, uint8_t pin, bool open, uint32_t span_ms, uint32_t now_ms)
{
    gpio_remote_pin_t *p = &r->pin[pin];

    p->flags = (uint8_t)((p->flags & F_PEND_OPEN) | F_HELD | (open ? F_OPEN : 0));
    p->start_ms = now_ms;
    p->mark_ms = now_ms;
    p->span_ms = span_ms;
    r->active |= (uint8_t)(1u << pin);
}

// End the press and hold the pin released for the gap
static void release(gpio_remote_pin_t *p, uint32_t now_ms)
{
    p->flags = (uint8_t)((p->flags & F_PEND_OPEN) | F_GAP);
    p->start_ms = now_ms;
}

// A press of pin must wait: that pin is held or in its gap, or another pin's click is still held
// or its release not yet returned. Only an open hold lets presses of other pins overlap it
static bool blocked(const gpio_remote_t *r, uint8_t pin, uint32_t now_ms)
{
    for (uint8_t i = 0; i < 8; i++) {
        const gpio_remote_pin_t *p = &r->pin[i];

        if (i == pin) {
            if ((p->flags & F_HELD) != 0 || ((p->flags & F_GAP) != 0 &&
                    ((p->flags & F_SEEN) == 0 || since(now_ms, p->start_ms) < r->gap_ms))) {
                return true;
            }
        } else if ((p->flags & (F_HELD | F_OPEN)) == F_HELD ||
                (p->flags & (F_GAP | F_SEEN)) == F_GAP) {
            return true;
        }
    }

    return false;
}

static bool enqueue(gpio_remote_t *r, uint8_t entry)
{
    if (r->queued >= r->queue_max || r->queued >= sizeof(r->queue)) {
        return false;
    }

    r->queue[r->queued++] = entry;
    return true;
}

void gpio_remote_down(gpio_remote_t *r, uint8_t pin, uint32_t hold_ms, uint32_t now_ms)
{
    gpio_remote_pin_t *p = &r->pin[pin];

    if ((p->flags & (F_HELD | F_OPEN)) == (F_HELD | F_OPEN)) {
        // Re-assert only ever extends a live hold
        const uint32_t used = since(now_ms, p->mark_ms);

        if (used >= p->span_ms || hold_ms > p->span_ms - used) {
            p->mark_ms = now_ms;
            p->span_ms = hold_ms;
        }
    } else if ((p->flags & F_PEND_OPEN) != 0) {
        // Re-assert of a queued DOWN, which is armed with hold_max_ms anyway
    } else if (r->queued > 0 || blocked(r, pin, now_ms)) {
        // A DOWN after an UP never merges into that press, nor overtakes an earlier click
        if (enqueue(r, (uint8_t)(pin | Q_OPEN))) {
            p->flags |= F_PEND_OPEN;
        }
    } else {
        press(r, pin, true, hold_ms, now_ms);
    }
}

void gpio_remote_up(gpio_remote_t *r, uint8_t pin, uint32_t floor_ms, uint32_t now_ms)
{
    gpio_remote_pin_t *p = &r->pin[pin];

    if ((p->flags & F_PEND_OPEN) != 0) {
        // The queued DOWN becomes a queued click, keeping its place
        p->flags &= (uint8_t)~F_PEND_OPEN;

        for (uint8_t i = 0; i < r->queued; i++) {
            if (r->queue[i] == (uint8_t)(pin | Q_OPEN)) {
                r->queue[i] = pin;
                break;
            }
        }
    } else if ((p->flags & (F_HELD | F_OPEN)) == (F_HELD | F_OPEN)) {
        // Owe the 20ms poll a minimum hold, or a click faster than one cycle never lands
        p->flags &= (uint8_t)~F_OPEN;
        p->mark_ms = p->start_ms;
        p->span_ms = floor_ms;

        if ((p->flags & F_SEEN) != 0 && since(now_ms, p->start_ms) >= floor_ms) {
            release(p, now_ms);
        }
    }
}

void gpio_remote_tap(gpio_remote_t *r, uint8_t pin, uint32_t hold_ms, uint32_t now_ms)
{
    if (r->queued > 0 || blocked(r, pin, now_ms)) {
        (void)enqueue(r, pin);
    } else {
        press(r, pin, false, hold_ms, now_ms);
    }
}

uint8_t gpio_remote_apply(gpio_remote_t *r, uint8_t inputs, uint32_t now_ms)
{
    // Arm the oldest queued press once nothing ahead of it is held or unreturned. Before the pins
    // advance, so the release ahead of it has already been returned by an earlier call
    if (r->queued > 0 && !blocked(r, r->queue[0] & Q_PIN, now_ms)) {
        const uint8_t entry = r->queue[0];
        const uint8_t pin = entry & Q_PIN;

        r->queued--;
        memmove(&r->queue[0], &r->queue[1], r->queued);

        if ((entry & Q_OPEN) != 0) {
            r->pin[pin].flags &= (uint8_t)~F_PEND_OPEN;
            press(r, pin, true, r->hold_max_ms, now_ms);
        } else {
            press(r, pin, false, r->click_ms, now_ms);
        }
    }

    for (uint8_t pin = 0; pin < 8; pin++) {
        const uint8_t bit = (uint8_t)(1u << pin);
        gpio_remote_pin_t *p = &r->pin[pin];

        if ((r->active & bit) == 0) {
            continue;
        }

        if ((p->flags & (F_HELD | F_SEEN)) == (F_HELD | F_SEEN) &&
                since(now_ms, p->mark_ms) >= p->span_ms) {
            release(p, now_ms);
        }

        if ((p->flags & F_GAP) != 0) {
            if ((p->flags & F_SEEN) == 0) {
                // The gap runs from its first released sample, so the next poll is released too
                p->flags |= F_SEEN;
                p->start_ms = now_ms;
            } else if (since(now_ms, p->start_ms) >= r->gap_ms) {
                p->flags &= F_PEND_OPEN;
                r->active &= (uint8_t)~bit;
            }
            continue; // Released
        }

        p->flags |= F_SEEN;
        inputs &= (uint8_t)~bit; // Active low, so a press clears the bit
    }

    return inputs;
}

void gpio_remote_clear(gpio_remote_t *r)
{
    memset(r->pin, 0, sizeof(r->pin));
    r->queued = 0;
    r->active = 0;
}
