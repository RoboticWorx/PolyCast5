#ifndef GPIO_REMOTE_H
#define GPIO_REMOTE_H

#include <stdint.h>

// Remote (Screen Mirror) button scheduler for the eight port-0 pins. Pure logic with the time
// passed in, so the host tests drive it directly; gpio_utils.c owns the lock and the clock.
// Pin numbers are trusted: the caller has already rejected anything above 7

typedef struct {
    uint32_t start_ms; // Press start, or gap start
    uint32_t mark_ms; // The press expires span_ms after this
    uint32_t span_ms;
    uint8_t flags;
} gpio_remote_pin_t;

typedef struct {
    volatile uint8_t active; // Bit N = pin N pressed or in its gap. Read unlocked as a fast path
    uint8_t queue_max; // Presses queued across all pins, at most 8; more are dropped
    uint16_t click_ms; // Hold of a queued click
    uint16_t hold_max_ms; // Watchdog of a queued press that has had no UP yet
    uint16_t gap_ms; // Forced release between two presses of one pin
    uint8_t queued; // Only ever nonzero with active nonzero, so the fast path cannot strand it
    uint8_t queue[8]; // Pins in arrival order; the top bit marks a DOWN that has had no UP yet
    gpio_remote_pin_t pin[8];
} gpio_remote_t;

/**
 * @brief DOWN: extends the pin's open press, otherwise queues as gpio_remote_tap() does or
 *        presses with a hold_ms watchdog
 */
void gpio_remote_down(gpio_remote_t *r, uint8_t pin, uint32_t hold_ms, uint32_t now_ms);

/**
 * @brief UP: ends the open press once it has held floor_ms, or turns a queued DOWN into a
 *        queued click. Anything else is a duplicate and is ignored
 */
void gpio_remote_up(gpio_remote_t *r, uint8_t pin, uint32_t floor_ms, uint32_t now_ms);

/**
 * @brief One complete click of hold_ms. Queued in arrival order, across all pins, while anything
 *        is queued, the pin is pressed or in its gap, or another pin's click is still held or its
 *        release not yet returned. Another pin's open hold never delays it
 */
void gpio_remote_tap(gpio_remote_t *r, uint8_t pin, uint32_t hold_ms, uint32_t now_ms);

/**
 * @brief Advance every active pin and drive the pressed ones low in inputs
 *
 *        A press ends only after a call has returned it pressed, and a gap only after one
 *        has returned it released, so a late poll still sees both edges of every press.
 *        The oldest queued press starts only after the release ahead of it was returned, so
 *        queued clicks reach the poll one at a time and in order.
 *
 * @return inputs with the remotely pressed pins cleared (active low)
 */
uint8_t gpio_remote_apply(gpio_remote_t *r, uint8_t inputs, uint32_t now_ms);

/**
 * @brief Release every pin and drop the queue. The configuration is kept
 */
void gpio_remote_clear(gpio_remote_t *r);

#endif // GPIO_REMOTE_H
