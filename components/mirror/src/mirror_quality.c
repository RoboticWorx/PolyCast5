#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "mirror_quality.h"

#define RTT_EMPTY 0
#define RTT_SENT  1
#define RTT_ACKED 2

#define NO_PROBE UINT16_MAX
#define NO_LEVEL UINT8_MAX
#define BASE_EMPTY UINT16_MAX

_Static_assert((MIRROR_RTT_SLOTS & (MIRROR_RTT_SLOTS - 1)) == 0, "Slots index by seq bits");
_Static_assert(MIRROR_QC_GOOD_MAX <= UINT8_MAX / 2, "good_needed doubles in a uint8_t");

void mirror_rtt_reset(mirror_rtt_t *r)
{
    memset(r, 0, sizeof(*r));
}

void mirror_rtt_sent(mirror_rtt_t *r, uint16_t seq, uint32_t now_us)
{
    mirror_rtt_slot_t *s = &r->slot[seq & (MIRROR_RTT_SLOTS - 1)];

    s->seq = seq;
    s->sent_us = now_us;
    s->ack_us = 0;
    s->state = RTT_SENT;
}

void mirror_rtt_acked(mirror_rtt_t *r, uint16_t seq, uint32_t now_us)
{
    for (int i = 0; i < MIRROR_RTT_SLOTS; i++) {
        mirror_rtt_slot_t *s = &r->slot[i];

        if (s->state != RTT_SENT) {
            continue;
        }

        if (s->seq == seq) {
            s->ack_us = now_us;
            s->state = RTT_ACKED;
        } else if ((int16_t)(seq - s->seq) > 0) {
            s->state = RTT_EMPTY;
        }
    }
}

bool mirror_rtt_take(mirror_rtt_t *r, uint32_t *rtt_us)
{
    for (int i = 0; i < MIRROR_RTT_SLOTS; i++) {
        mirror_rtt_slot_t *s = &r->slot[i];

        if (s->state != RTT_ACKED) {
            continue;
        }

        s->state = RTT_EMPTY;

        const uint32_t d = s->ack_us - s->sent_us;

        // The ack was handled before its stamp landed: no round trip to measure
        if ((int32_t)d < 0) {
            continue;
        }

        *rtt_us = d;
        return true;
    }

    return false;
}

uint32_t mirror_rtt_pending_us(const mirror_rtt_t *r, uint16_t acked, uint32_t now_us)
{
    uint32_t oldest = 0;

    for (int i = 0; i < MIRROR_RTT_SLOTS; i++) {
        const mirror_rtt_slot_t *s = &r->slot[i];

        if (s->state != RTT_SENT || (int16_t)(s->seq - acked) <= 0) {
            continue;
        }

        const uint32_t age = now_us - s->sent_us;

        if ((int32_t)age > 0 && age > oldest) {
            oldest = age;
        }
    }

    return oldest;
}

void mirror_window_rtt(mirror_window_t *w, uint32_t rtt_us)
{
    const uint32_t ms = rtt_us / 1000;

    if (w->samples == UINT16_MAX) {
        return;
    }

    w->rtt_ms[w->samples % MIRROR_QC_SAMPLES] = (ms > UINT16_MAX) ? UINT16_MAX : (uint16_t)ms;
    w->samples++;
}

// Lower median once the MIRROR_QC_INFLIGHT slowest are dropped. Not the mean or the maximum:
// preemption and the websocket lock delay single acks, and one Wi-Fi stall delays only the
// frames in flight, while a standing queue lifts every sample. Nor the minimum: preemption
// between the socket write and the stamp shortens single samples. The lower middle of an even
// count leans towards holding
static uint16_t window_median(const mirror_window_t *w)
{
    uint16_t v[MIRROR_QC_SAMPLES];
    const int n = (w->samples < MIRROR_QC_SAMPLES) ? w->samples : MIRROR_QC_SAMPLES;
    const int kept = (n > MIRROR_QC_INFLIGHT) ? n - MIRROR_QC_INFLIGHT : n;

    for (int i = 0; i < n; i++) {
        const uint16_t x = w->rtt_ms[i];
        int j = i;

        while (j > 0 && v[j - 1] > x) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }

    return v[(kept - 1) / 2];
}

// The window's second fastest ack, or its fastest below three: the baseline's sample. Near the
// minimum, where the path shows, but one ack cut short by preemption cannot set it
static uint16_t window_low(const mirror_window_t *w)
{
    const int n = (w->samples < MIRROR_QC_SAMPLES) ? w->samples : MIRROR_QC_SAMPLES;
    uint16_t lo1 = UINT16_MAX;
    uint16_t lo2 = UINT16_MAX;

    for (int i = 0; i < n; i++) {
        const uint16_t x = w->rtt_ms[i];

        if (x < lo1) {
            lo2 = lo1;
            lo1 = x;
        } else if (x < lo2) {
            lo2 = x;
        }
    }

    return (n >= 3) ? lo2 : lo1;
}

static uint32_t queue_hi(uint32_t base)
{
    return (base > MIRROR_QC_QUEUE_HI_MS) ? base : MIRROR_QC_QUEUE_HI_MS;
}

static uint32_t queue_lo(uint32_t base)
{
    return (base / 2 > MIRROR_QC_QUEUE_LO_MS) ? base / 2 : MIRROR_QC_QUEUE_LO_MS;
}

static void base_bucket_clear(mirror_quality_ctl_t *q, int i)
{
    q->base_min[i] = BASE_EMPTY;
    q->base_next[i] = BASE_EMPTY;
}

static void base_clear(mirror_quality_ctl_t *q)
{
    for (int i = 0; i < MIRROR_QC_BASE_BUCKETS; i++) {
        base_bucket_clear(q, i);
    }

    q->base_cur = 0;
    q->base_age = 0;
}

static bool base_empty(const mirror_quality_ctl_t *q)
{
    for (int i = 0; i < MIRROR_QC_BASE_BUCKETS; i++) {
        if (q->base_min[i] != BASE_EMPTY) {
            return false;
        }
    }

    return true;
}

// The lowest bucket's second fastest window, so one window cut short cannot set the path.
// A bucket of one window counts only while no bucket has two. 0 until a sample arrives
static uint32_t base_get(const mirror_quality_ctl_t *q)
{
    uint16_t one = BASE_EMPTY;
    uint16_t two = BASE_EMPTY;

    for (int i = 0; i < MIRROR_QC_BASE_BUCKETS; i++) {
        if (q->base_min[i] < one) {
            one = q->base_min[i];
        }

        if (q->base_next[i] < two) {
            two = q->base_next[i];
        }
    }

    const uint16_t b = (two != BASE_EMPTY) ? two : one;

    if (b == BASE_EMPTY) {
        return 0;
    }

    return (b > MIRROR_QC_BASE_MAX_MS) ? MIRROR_QC_BASE_MAX_MS : b;
}

// Only a window near the baseline refreshes it, so no level's standing queue becomes the path.
// A keyframe burst never seeds it. A run of congestion at the worst level retires it: stepping
// down left the delay standing, so the delay is the path's own
static void base_feed(mirror_quality_ctl_t *q, uint16_t rtt, uint16_t low)
{
    const uint32_t base = base_get(q);

    if (q->level == MIRROR_QC_WORST && q->bad >= MIRROR_QC_RELEARN_WINDOWS &&
            rtt > base + queue_hi(base)) {
        base_clear(q);
    } else if (base_empty(q)) {
        if (q->skip > 0) {
            return;
        }
    } else if (rtt >= base + MIRROR_QC_QUEUE_LO_MS) {
        return;
    }

    uint16_t *const lo1 = &q->base_min[q->base_cur];
    uint16_t *const lo2 = &q->base_next[q->base_cur];

    if (low < *lo1) {
        *lo2 = *lo1;
        *lo1 = low;
    } else if (low < *lo2) {
        *lo2 = low;
    }
}

// Close the current bucket after MIRROR_QC_BASE_WINDOWS, dropping the oldest. An empty one
// waits: a still screen or a queued level measures nothing, and the last path stands
static void base_tick(mirror_quality_ctl_t *q)
{
    if (q->base_min[q->base_cur] == BASE_EMPTY || ++q->base_age < MIRROR_QC_BASE_WINDOWS) {
        return;
    }

    q->base_age = 0;
    q->base_cur = (uint8_t)((q->base_cur + 1) % MIRROR_QC_BASE_BUCKETS);
    base_bucket_clear(q, q->base_cur);
}

static void streaks_clear(mirror_quality_ctl_t *q)
{
    q->bad = 0;
    q->good = 0;
    q->tested = false;
    q->over_cap = 0;
    q->silent = false;
}

// The next window carries the keyframe burst, and the old level's backlog still drains
static void settle(mirror_quality_ctl_t *q)
{
    streaks_clear(q);
    q->skip = 1;
    q->hold = MIRROR_QC_HOLD_WINDOWS;
}

static void step_down(mirror_quality_ctl_t *q, const char *why)
{
    if (q->since_up <= MIRROR_QC_PROBE_WINDOWS) {
        const unsigned n = (unsigned)q->good_needed * 2u;

        q->good_needed = (uint8_t)((n > MIRROR_QC_GOOD_MAX) ? MIRROR_QC_GOOD_MAX : n);
    }

    // Load that congests a climb made on still windows goes straight back to the level it last
    // held: one keyframe, not one per level
    const uint8_t next = (uint8_t)(q->level + 1);

    q->level = (q->held_level != NO_LEVEL && q->held_level > next) ? q->held_level : next;
    q->held_level = NO_LEVEL;
    q->since_up = NO_PROBE;
    q->passed = false;
    q->why = why;
    settle(q);
}

// A climb on still windows alone tested nothing, so a step down after it fails no probe
static void step_up(mirror_quality_ctl_t *q)
{
    if (q->tested) {
        q->since_up = 0;
        q->held_level = NO_LEVEL;
    } else {
        q->since_up = NO_PROBE;

        if (q->held_level == NO_LEVEL) {
            q->held_level = q->level;
        }
    }

    q->passed = false;
    q->level--;
    q->why = q->tested ? "link clear" : "screen still";
    settle(q);
}

uint8_t mirror_quality_up_after(const mirror_quality_ctl_t *q)
{
    return q->passed ? MIRROR_QC_GOOD_MIN : q->good_needed;
}

void mirror_quality_init(mirror_quality_ctl_t *q, uint8_t level)
{
    memset(q, 0, sizeof(*q));
    q->level = (level > MIRROR_QC_WORST) ? MIRROR_QC_WORST : level;
    q->good_needed = MIRROR_QC_GOOD_MIN;
    q->held_level = NO_LEVEL;
    q->since_up = NO_PROBE;
    base_clear(q);
}

void mirror_quality_viewer(mirror_quality_ctl_t *q, bool new_path)
{
    if (new_path) {
        base_clear(q);
        q->good_needed = MIRROR_QC_GOOD_MIN; // The backoff was the old path's
        q->held_level = NO_LEVEL;
    }

    q->since_up = NO_PROBE;
    q->passed = false;
    settle(q);
}

uint8_t mirror_quality_window(mirror_quality_ctl_t *q, const mirror_window_t *w, uint8_t pinned)
{
    q->why = NULL;

    // Timers count every window, skipped or pinned alike. A step up that outlived its probe
    // lets the next one climb at once; one that held stops the backing off
    if (q->since_up != NO_PROBE) {
        q->passed |= (++q->since_up > MIRROR_QC_PROBE_WINDOWS);

        if (q->since_up >= MIRROR_QC_STABLE_WINDOWS) {
            q->good_needed = MIRROR_QC_GOOD_MIN;
            q->since_up = NO_PROBE;
        }
    }

    const bool held = (q->hold > 0);

    if (held) {
        q->hold--;
    }

    // A skipped window feeds the baseline too: a burst only runs high
    uint16_t rtt = 0;

    if (w->samples > 0) {
        rtt = window_median(w);
        base_feed(q, rtt, window_low(w));
    } else {
        // No ack came back: the oldest one owed is a floor on the round trip
        rtt = (w->pending_ms > UINT16_MAX) ? UINT16_MAX : (uint16_t)w->pending_ms;
    }

    const uint32_t base = base_get(q);

    base_tick(q);

    q->rtt_ms = rtt;
    q->base_ms = (uint16_t)base;
    q->queue_ms = (uint16_t)((rtt > base) ? rtt - base : 0);

    if (pinned != 0) {
        const uint8_t want = (uint8_t)(pinned - 1);

        if (want <= MIRROR_QC_WORST && want != q->level) {
            q->level = want;
            q->why = "pinned by viewer";
        }

        q->pinned = pinned;
        q->held_level = NO_LEVEL;
        streaks_clear(q);
        return q->level;
    }

    if (q->pinned != 0) {
        // Back to automatic. The unpin forced a keyframe inside this window
        q->pinned = 0;
        settle(q);
        return q->level;
    }

    if (q->skip > 0) {
        q->skip--;
        return q->level;
    }

    if (w->bytes < MIRROR_QC_CAP_BYTES) {
        q->over_cap = 0;
    } else if (q->over_cap < UINT8_MAX) {
        q->over_cap++;
    }

    const uint32_t hi = queue_hi(base);
    const uint32_t lo = queue_lo(base);
    const bool blocked = (w->longest_send_ms >= MIRROR_QC_BLOCKED_MS);
    const bool queued = (q->queue_ms > hi);
    const bool capped = (q->over_cap >= MIRROR_QC_CAP_WINDOWS);
    const bool idle = (w->frames == 0 && w->samples == 0 && !w->waiting);
    const bool silent = (queued && w->samples == 0 && !blocked && !w->resync && !capped);
    const bool was_silent = q->silent;

    q->silent = silent;

    if (queued || blocked || w->resync || capped) {
        const char *why = w->resync ? "ack timeout" : blocked ? "blocked send" :
                silent ? "acks overdue" : queued ? "queueing" : "byte cap";
        const bool severe = (w->samples >= MIRROR_QC_SEVERE_SAMPLES && q->queue_ms > 2 * hi);

        q->good = 0;
        q->tested = false;

        if (held) {
            return q->level; // The change is still draining; judge the level it made
        }

        // A run of windows with no ack back is one silence, one strike: a Wi-Fi stall ends
        // in clear acks, a slow link keeps answering late, and a dead one times out
        if (!(silent && was_silent) && q->bad < UINT8_MAX) {
            q->bad++;
        }

        if ((q->bad >= MIRROR_QC_DOWN_WINDOWS || severe || capped) && q->level < MIRROR_QC_WORST) {
            step_down(q, why);
        }
    } else if (q->over_cap == 0 && ((w->samples > 0 && q->queue_ms < lo) || idle)) {
        q->bad = 0;
        q->tested |= (w->samples > 0);

        if (q->good < UINT8_MAX) {
            q->good++;
        }

        if (q->level > 0 && q->good >= mirror_quality_up_after(q)) {
            step_up(q);
        }
    } else if (w->samples == 0 && q->over_cap == 0) {
        // No round trip came back and none is overdue: no evidence either way, so both
        // streaks stand
    } else {
        // Some queue, or a byte-cap window: hold
        q->bad = 0;
        q->good = 0;
        q->tested = false;
    }

    return q->level;
}
