/* Host tests for the Screen Mirror button scheduler (components/gpio/src/gpio_remote.c).
 *
 * Time is simulated in 1 ms steps with gpio_task's 20 ms poll, or on gpio_utils.c's 10 ms tick
 * clock, and every sample goes through a copy of gpio_task's edge detector, so each case checks
 * the presses the real button state machine would report rather than the scheduler's
 * internals. */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "gpio_remote.h"

#define PIN 3
#define PIN_B 5
#define PIN_SELECT 3 /* polycast5_gpios.h */
#define PIN_UP 1
#define PIN_RIGHT 2
#define PIN_DOWN 6
#define LONG_MS 500 /* gpio_task.c LONG_PRESS_THRESHOLD_MS */

/* The firmware's values, from gpio_utils.h (which the host cannot include) */
#define TAP_MS 80
#define MIN_MS 70
#define MAX_MS 1200
#define GAP_MS 40
#define QUEUE_MAX 8

static gpio_remote_t R;
static uint32_t now;
static uint32_t poll_ms = 20;
static uint32_t phase; /* Polls land where (now - phase) % poll_ms == 0 */
static int failures = 0;

typedef struct {
    int pin;
    int level; /* Last sampled level: 1 = released */
    int presses, shorts, longs;
    bool long_fired;
    uint32_t press_at;
    uint32_t last_len; /* Duration of the last completed press, in ms */
    int run; /* Samples at the current level */
    int min_down, min_up; /* Fewest samples in one press, and between two presses */
    uint8_t other; /* The other pins' levels, OR'd over every sample */
} trace_t;

/* Every pin's presses in the order gpio_task gives their short press: at release, walking its
 * buttons[] in SELECT, HOME, UP, DOWN, LEFT, RIGHT order within one poll */
typedef struct {
    uint8_t level;
    int polls;
    int pressed_at[8];
    int n;
    int pin[32];
    int down_at[32], up_at[32]; /* Poll index of each press's two edges, in release order */
} order_t;

static const int gpio_task_order[6] = { 3, 5, 1, 6, 4, 2 };
static order_t *seq; /* Sampled on every poll when set */

static void ok(const char *what) { printf("  ok  %s\n", what); }
static void bad(const char *what) { printf("  !! %s\n", what); failures++; }

static void expect(bool cond, const char *what)
{
    if (cond) ok(what); else bad(what);
}

static void expect_eq(int got, int want, const char *what)
{
    if (got == want) {
        ok(what);
    } else {
        printf("  !! %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

static void reset(uint32_t start)
{
    memset(&R, 0, sizeof(R));
    seq = NULL;
    R.queue_max = QUEUE_MAX;
    R.click_ms = TAP_MS;
    R.hold_max_ms = MAX_MS;
    R.gap_ms = GAP_MS;
    now = start;
    phase = start;
    poll_ms = 20;
}

static void trace_init(trace_t *t, int pin)
{
    memset(t, 0, sizeof(*t));
    t->pin = pin;
    t->level = 1;
    t->min_down = 1 << 30;
    t->min_up = 1 << 30;
}

/* gpio_task's per-button logic, reduced to what the tests count */
static void sample(trace_t *t, uint8_t in)
{
    const int level = (in >> t->pin) & 1;

    if (level == 0 && t->level == 1) {
        if (t->presses > 0 && t->run < t->min_up) t->min_up = t->run;
        t->presses++;
        t->press_at = now;
        t->long_fired = false;
        t->run = 0;
    } else if (level == 1 && t->level == 0) {
        if (t->run < t->min_down) t->min_down = t->run;
        if (!t->long_fired) t->shorts++;
        t->last_len = now - t->press_at;
        t->run = 0;
    }

    if (level == 0 && !t->long_fired && now - t->press_at >= LONG_MS) {
        t->long_fired = true;
        t->longs++;
    }

    t->run++;
    t->level = level;
}

static void order_init(order_t *o)
{
    memset(o, 0, sizeof(*o));
    o->level = 0xFF;
    seq = o;
}

static void order_sample(order_t *o, uint8_t in)
{
    for (int k = 0; k < 6; k++) {
        const int pin = gpio_task_order[k];
        const int level = (in >> pin) & 1;
        const int was = (o->level >> pin) & 1;

        if (level == 0 && was == 1) {
            o->pressed_at[pin] = o->polls;
        } else if (level == 1 && was == 0 && o->n < 32) {
            o->pin[o->n] = pin;
            o->down_at[o->n] = o->pressed_at[pin];
            o->up_at[o->n] = o->polls;
            o->n++;
        }
    }
    o->level = in;
    o->polls++;
}

/* The presses ended in exactly this pin order, each starting on a poll after the one before
 * it was seen released */
static void expect_order(const order_t *o, const int *pins, int n, const char *what)
{
    bool good = (o->n == n);

    for (int k = 0; good && k < n; k++) {
        good = (o->pin[k] == pins[k]) && (k == 0 || o->down_at[k] > o->up_at[k - 1]);
    }
    if (good) {
        ok(what);
        return;
    }

    printf("  !! %s: got", what);
    for (int k = 0; k < o->n; k++) {
        printf(" %d[%d..%d]", o->pin[k], o->down_at[k], o->up_at[k]);
    }
    printf("\n");
    failures++;
}

static void poll(trace_t *a, trace_t *b)
{
    const uint8_t in = gpio_remote_apply(&R, 0xFF, now);

    if (a) {
        sample(a, in);
        a->other |= (uint8_t)~(in | (1u << a->pin) | (b ? (1u << b->pin) : 0));
    }
    if (b) sample(b, in);
    if (seq) order_sample(seq, in);
}

/* Advance the clock, polling on the poll grid. b may be NULL */
static void advance(trace_t *a, trace_t *b, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        if ((now - phase) % poll_ms == 0) {
            poll(a, b);
        }
        now++;
    }
}

/* gpio_utils.c's clock: tick * 10 ms, with gpio_task polling every 2 ticks. Returns right after
 * the last tick's poll, so the next event lands at that poll's own timestamp, as when the
 * websocket task runs in the same tick */
static void ticks(trace_t *a, trace_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        now += 10;
        if ((now - phase) % poll_ms == 0) {
            poll(a, b);
        }
    }
}

static void down(int pin) { gpio_remote_down(&R, (uint8_t)pin, MAX_MS, now); }
static void up(int pin) { gpio_remote_up(&R, (uint8_t)pin, MIN_MS, now); }
static void tap(int pin) { gpio_remote_tap(&R, (uint8_t)pin, TAP_MS, now); }

static void bunched_clicks(uint32_t start, const char *label)
{
    trace_t t;
    char what[128];

    reset(start);
    trace_init(&t, PIN);

    /* A TCP stall releases five DOWN/UP pairs in one burst */
    for (int i = 0; i < 5; i++) {
        down(PIN);
        up(PIN);
    }
    advance(&t, NULL, 2000);

    snprintf(what, sizeof(what), "%s: 5 bunched DOWN/UP pairs -> 5 presses", label);
    expect_eq(t.presses, 5, what);
    snprintf(what, sizeof(what), "%s: all 5 are short presses", label);
    expect_eq(t.shorts, 5, what);
    snprintf(what, sizeof(what), "%s: every press holds >= 3 polls", label);
    expect(t.min_down >= 3, what);
    snprintf(what, sizeof(what), "%s: >= 2 released polls between presses", label);
    expect(t.min_up >= 2, what);
    snprintf(what, sizeof(what), "%s: idle afterwards", label);
    expect(R.active == 0 && t.level == 1, what);
}

int main(void)
{
    trace_t t, u;

    printf("=== single tap ===\n");
    reset(1000);
    trace_init(&t, PIN);
    tap(PIN);
    advance(&t, NULL, 400);
    expect_eq(t.presses, 1, "one press");
    expect_eq(t.shorts, 1, "one short press");
    expect(t.min_down >= 3, "held for >= 3 polls");
    expect(R.active == 0, "active mask clears once the gap is served");

    printf("=== bunched clicks never merge ===\n");
    bunched_clicks(1000, "mid-range clock");

    reset(1000);
    trace_init(&t, PIN);
    for (int i = 0; i < 5; i++) tap(PIN);
    advance(&t, NULL, 2000);
    expect_eq(t.presses, 5, "5 bunched taps -> 5 presses");
    expect(t.min_up >= 2, ">= 2 released polls between taps");

    printf("=== DOWN after UP inside the minimum hold ===\n");
    reset(1000);
    trace_init(&t, PIN);
    down(PIN);
    advance(&t, NULL, 10);
    up(PIN);
    advance(&t, NULL, 10);
    down(PIN);
    advance(&t, NULL, 10);
    up(PIN);
    advance(&t, NULL, 600);
    expect_eq(t.presses, 2, "two presses, not one extended press");

    printf("=== click shorter than a poll ===\n");
    reset(1003); /* Off the poll grid */
    phase = 1000;
    trace_init(&t, PIN);
    down(PIN);
    advance(&t, NULL, 3);
    up(PIN);
    advance(&t, NULL, 400);
    expect_eq(t.presses, 1, "lands as one press");
    expect(t.min_down >= 3, "the minimum hold covers >= 3 polls");

    printf("=== re-asserted DOWN extends the hold ===\n");
    reset(1000);
    trace_init(&t, PIN);
    down(PIN);
    for (int k = 0; k < 7; k++) {
        advance(&t, NULL, 400);
        down(PIN);
    }
    advance(&t, NULL, 400);
    up(PIN);
    advance(&t, NULL, 500);
    expect_eq(t.presses, 1, "one continuous press across 7 re-asserts");
    expect_eq(t.longs, 1, "one long press");
    expect(t.last_len >= 3200 && t.last_len <= 3280, "released on the UP, not the watchdog");

    reset(1000);
    trace_init(&t, PIN);
    down(PIN);
    advance(&t, NULL, 2500);
    expect_eq(t.presses, 1, "unrefreshed DOWN: one press");
    expect(t.last_len >= MAX_MS && t.last_len <= MAX_MS + 40, "watchdog releases at HOLD_MAX");
    expect(R.active == 0, "idle after the watchdog");

    printf("=== tap during a hold is queued ===\n");
    reset(1000);
    trace_init(&t, PIN);
    down(PIN);
    advance(&t, NULL, 100);
    tap(PIN);
    advance(&t, NULL, 300);
    down(PIN);
    advance(&t, NULL, 300);
    up(PIN);
    advance(&t, NULL, 600);
    expect_eq(t.presses, 2, "the hold, then the queued tap");
    expect_eq(t.longs, 1, "the hold is a long press");
    expect_eq(t.shorts, 1, "the tap is a short press after it");
    expect(t.min_up >= 2, ">= 2 released polls between them");

    printf("=== a queued DOWN becomes an open press ===\n");
    reset(1000);
    trace_init(&t, PIN);
    down(PIN);
    up(PIN);
    down(PIN); /* Arrives while the first press is still owed its minimum hold */
    for (int k = 0; k < 3; k++) {
        advance(&t, NULL, 400);
        down(PIN);
    }
    advance(&t, NULL, 300);
    up(PIN);
    advance(&t, NULL, 400);
    expect_eq(t.presses, 2, "two presses");
    expect_eq(t.shorts, 1, "the first is a click");
    expect_eq(t.longs, 1, "the queued one is held to its UP");

    printf("=== duplicate UPs are ignored ===\n");
    reset(1000);
    trace_init(&t, PIN);
    up(PIN);
    expect(R.active == 0, "UP on an idle pin does nothing");
    down(PIN);
    advance(&t, NULL, 100);
    up(PIN);
    up(PIN);
    advance(&t, NULL, 300);
    down(PIN);
    advance(&t, NULL, 30);
    up(PIN);
    up(PIN);
    advance(&t, NULL, 400);
    expect_eq(t.presses, 2, "two presses");
    expect_eq(t.shorts, 2, "both short");
    expect(R.active == 0, "nothing left queued");

    printf("=== queue cap ===\n");
    reset(1000);
    trace_init(&t, PIN);
    for (int i = 0; i < 20; i++) tap(PIN);
    expect(R.queued == QUEUE_MAX, "the queue saturates at the cap");
    for (int i = 0; i < 20; i++) {
        down(PIN);
        up(PIN);
    }
    expect(R.queued == QUEUE_MAX, "DOWN/UP pairs respect the cap too");
    advance(&t, NULL, 4000);
    expect_eq(t.presses, 1 + QUEUE_MAX, "the live press plus the cap");
    expect(R.active == 0, "drains to idle");

    printf("=== clear() drops everything ===\n");
    reset(1000);
    trace_init(&t, PIN);
    down(PIN_B);
    for (int i = 0; i < 6; i++) tap(PIN);
    advance(&t, NULL, 50);
    gpio_remote_clear(&R);
    expect(R.active == 0, "active mask cleared");
    expect(gpio_remote_apply(&R, 0xFF, now) == 0xFF, "every pin reads released");
    expect(R.queue_max == QUEUE_MAX && R.gap_ms == GAP_MS, "configuration kept");
    advance(&t, NULL, 1500);
    expect_eq(t.presses, 1, "no queued tap survives");
    tap(PIN);
    advance(&t, NULL, 300);
    expect_eq(t.presses, 2, "a fresh tap after clear() still works");

    printf("=== late polls still see both edges ===\n");
    reset(1000);
    trace_init(&t, PIN);
    poll_ms = 150; /* gpio_task stalled behind the I2C bus */
    for (int i = 0; i < 4; i++) tap(PIN);
    advance(&t, NULL, 3000);
    expect_eq(t.presses, 4, "4 taps -> 4 presses at a 150 ms poll");
    expect(t.min_down >= 1 && t.min_up >= 1, "each press and each gap sampled at least once");

    reset(1000);
    trace_init(&t, PIN);
    poll_ms = 150;
    advance(&t, NULL, 1); /* Just past a poll: the tap's whole hold lapses before the next */
    tap(PIN);
    advance(&t, NULL, 600);
    expect_eq(t.presses, 1, "a tap outlived by the poll interval is still sampled");

    reset(1000);
    trace_init(&t, PIN);
    poll_ms = 150;
    down(PIN);
    advance(&t, NULL, 100);
    up(PIN); /* Seen and past its floor: released at once, gap starts here */
    tap(PIN); /* Queued behind the gap, which lapses before the next poll */
    advance(&t, NULL, 900);
    expect_eq(t.presses, 2, "a gap outlived by the poll interval is still sampled");

    printf("=== a stray UP does not cut a tap short ===\n");
    reset(1000);
    trace_init(&t, PIN);
    tap(PIN);
    up(PIN);
    advance(&t, NULL, 400);
    expect_eq(t.presses, 1, "one press");
    expect_eq((int)t.last_len, TAP_MS, "held for the full tap");

    printf("=== pins never drive each other ===\n");
    reset(1000);
    trace_init(&t, PIN);
    trace_init(&u, PIN_B);
    tap(PIN);
    tap(PIN_B);
    tap(PIN_B);
    advance(&t, &u, 600);
    expect_eq(t.presses, 1, "pin A: one press");
    expect_eq(u.presses, 2, "pin B: its own two presses, after pin A's");
    expect(t.other == 0, "no other pin is ever driven");
    expect(gpio_remote_apply(&R, 0x5A, now) == 0x5A, "idle overlay passes the raw byte through");

    reset(1000);
    tap(PIN);
    expect((gpio_remote_apply(&R, 0xFF & (uint8_t)~(1u << 0), now) & 0x01) == 0,
           "a physical press on another pin is kept");

    printf("=== bunched clicks on different pins keep their order ===\n");
    {
        static const int nav[3] = { PIN_DOWN, PIN_DOWN, PIN_SELECT };
        static const int held[2] = { PIN_DOWN, PIN_SELECT };
        static const int kept[3] = { PIN_DOWN, PIN_SELECT, PIN_DOWN };
        order_t o;

        /* A TCP stall releases navigate-then-select in one burst */
        reset(1000);
        order_init(&o);
        tap(PIN_DOWN);
        tap(PIN_DOWN);
        tap(PIN_SELECT);
        advance(NULL, NULL, 1000);
        expect_order(&o, nav, 3, "TAP DOWN, DOWN, SELECT -> DOWN, DOWN, SELECT, one per poll");
        expect(R.active == 0 && R.queued == 0, "idle afterwards");

        reset(1000);
        order_init(&o);
        down(PIN_DOWN);
        up(PIN_DOWN);
        down(PIN_DOWN);
        up(PIN_DOWN);
        down(PIN_SELECT);
        up(PIN_SELECT);
        advance(NULL, NULL, 1000);
        expect_order(&o, nav, 3, "the same as DOWN/UP pairs");

        /* A hold on an idle pin still waits for the click ahead of it */
        reset(1000);
        order_init(&o);
        tap(PIN_DOWN);
        down(PIN_SELECT);
        for (int k = 0; k < 2; k++) {
            advance(NULL, NULL, 400);
            down(PIN_SELECT);
        }
        up(PIN_SELECT);
        advance(NULL, NULL, 400);
        expect_order(&o, held, 2, "a queued hold starts after the click ahead of it");
        expect(o.up_at[1] - o.down_at[1] >= LONG_MS / 20, "and is held to its UP");

        /* An UP turns a queued DOWN into a click in its own place */
        reset(1000);
        order_init(&o);
        tap(PIN_DOWN);
        down(PIN_SELECT);
        up(PIN_SELECT);
        tap(PIN_DOWN);
        advance(NULL, NULL, 1000);
        expect_order(&o, kept, 3, "TAP DOWN, DOWN/UP SELECT, TAP DOWN keep their order");
    }

    printf("=== an open hold never delays another pin ===\n");
    {
        order_t o;
        int at;

        reset(1000);
        order_init(&o);
        down(PIN_RIGHT);
        advance(NULL, NULL, 100);
        at = o.polls;
        tap(PIN_UP); /* A game: holding RIGHT, tapping UP */
        advance(NULL, NULL, 300);
        down(PIN_RIGHT);
        advance(NULL, NULL, 100);
        up(PIN_RIGHT);
        advance(NULL, NULL, 400);
        expect(o.n == 2 && o.pin[0] == PIN_UP && o.pin[1] == PIN_RIGHT,
               "UP's tap ends inside RIGHT's hold");
        expect(o.n == 2 && o.down_at[0] == at, "and is pressed on the very next poll");
    }

    printf("=== gpio_utils clock: 10 ms ticks, events right after a poll ===\n");
    reset(1000);
    trace_init(&t, PIN);
    poll(&t, NULL);
    down(PIN);
    ticks(&t, NULL, 30);
    up(PIN); /* Seen and past its floor: released at once, at the poll's own timestamp */
    tap(PIN);
    ticks(&t, NULL, 40);
    expect_eq(t.presses, 2, "hold, then UP + TAP bunched: two presses");
    expect(t.min_up >= 2, "the gap still covers >= 2 released polls");

    reset(1000);
    trace_init(&t, PIN);
    poll(&t, NULL);
    down(PIN);
    up(PIN);
    ticks(&t, NULL, 30);
    expect_eq(t.presses, 1, "DOWN/UP: one press");
    expect(t.min_down >= 3, "the minimum hold covers >= 3 polls");

    reset(1000);
    trace_init(&t, PIN);
    poll(&t, NULL);
    for (int i = 0; i < 5; i++) {
        down(PIN);
        up(PIN);
    }
    ticks(&t, NULL, 200);
    expect_eq(t.presses, 5, "5 bunched DOWN/UP pairs -> 5 presses");
    expect(t.min_down >= 3, "every press holds >= 3 polls");
    expect(t.min_up >= 2, ">= 2 released polls between presses");

    {
        static const int nav[3] = { PIN_DOWN, PIN_DOWN, PIN_SELECT };
        order_t o;

        reset(1000);
        order_init(&o);
        poll(NULL, NULL);
        tap(PIN_DOWN);
        tap(PIN_DOWN);
        tap(PIN_SELECT);
        ticks(NULL, NULL, 100);
        expect_order(&o, nav, 3, "TAP DOWN, DOWN, SELECT keep their order");
    }

    printf("=== clock wrap ===\n");
    bunched_clicks(0xFFFFFFFFu - 150, "across 2^32");

    reset(0xFFFFFFFFu - 1000);
    trace_init(&t, PIN);
    down(PIN);
    for (int k = 0; k < 5; k++) {
        advance(&t, NULL, 400);
        down(PIN);
    }
    advance(&t, NULL, 200);
    up(PIN);
    advance(&t, NULL, 500);
    expect_eq(t.presses, 1, "a hold spanning the wrap stays one press");
    expect(t.last_len >= 2200 && t.last_len <= 2280, "and ends on its UP");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
