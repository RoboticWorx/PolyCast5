/* Host tests for the Screen Mirror quality controller and its ack round-trip ring
 * (components/mirror/src/mirror_quality.c).
 *
 * The ring and the controller are checked directly first. Then a simulated link drives the real
 * controller in 1 ms steps: the device's TCP send buffer (a full one blocks the send, as lwIP's
 * does), the relay's queue to the viewer, the viewer's acks, and mirror_task's pacing gate, ack
 * timeout, periodic keyframe and window. Only the network and the content are made up. */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mirror_quality.h"

static int failures = 0;

static void ok(const char *what) { printf("  ok  %s\n", what); }
static void bad(const char *what) { printf("  !! %s\n", what); failures++; }

static void expect(bool cond, const char *what)
{
    if (cond) ok(what); else bad(what);
}

static void expect_eq(long got, long want, const char *what)
{
    if (got == want) {
        ok(what);
    } else {
        printf("  !! %s: got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

/* ---- window builders ---------------------------------------------------------------- */

/* A busy window: n frames sent and n acks back, every one at rtt_ms */
static mirror_window_t busy(int n, uint32_t rtt_ms)
{
    mirror_window_t w;

    memset(&w, 0, sizeof(w));
    w.frames = (uint16_t)n;
    w.bytes = 20000;
    for (int i = 0; i < n; i++) {
        mirror_window_rtt(&w, rtt_ms * 1000);
    }
    return w;
}

/* Nothing needed sending and nothing is owed */
static mirror_window_t idle(void)
{
    mirror_window_t w;

    memset(&w, 0, sizeof(w));
    return w;
}

static uint8_t feed(mirror_quality_ctl_t *q, mirror_window_t w)
{
    return mirror_quality_window(q, &w, 0);
}

/* A viewer attaches and 5 clear windows at base_ms teach the controller its path. Then the
 * level is set outright, with no skip, hold or streak pending */
static void start(mirror_quality_ctl_t *q, uint8_t level, uint32_t base_ms)
{
    mirror_quality_init(q, 0);
    mirror_quality_viewer(q, true);
    for (int i = 0; i < 5; i++) {
        feed(q, busy(10, base_ms));
    }
    q->level = level;
    q->good = 0;
    q->tested = false;
}

/* ---- RTT ring ----------------------------------------------------------------------- */

static void test_ring(void)
{
    mirror_rtt_t r;
    uint32_t us = 0;

    printf("=== RTT ring ===\n");

    mirror_rtt_reset(&r);
    mirror_rtt_sent(&r, 7, 1000);
    mirror_rtt_acked(&r, 7, 81000);
    expect(mirror_rtt_take(&r, &us) && us == 80000, "a stamped seq's ack gives its round trip");
    expect(!mirror_rtt_take(&r, &us), "once");

    mirror_rtt_sent(&r, 8, 0xFFFFFF00u);
    mirror_rtt_acked(&r, 8, 0x00000100u);
    expect(mirror_rtt_take(&r, &us) && us == 0x200, "across the 2^32 us wrap");

    mirror_rtt_acked(&r, 9, 5000);
    expect(!mirror_rtt_take(&r, &us), "an ack with no stamp gives nothing");

    mirror_rtt_sent(&r, 10, 9000);
    mirror_rtt_acked(&r, 10, 8000);
    expect(!mirror_rtt_take(&r, &us), "an ack handled before its stamp landed gives nothing");

    /* 11 and 27 share a slot: the later stamp owns it */
    mirror_rtt_reset(&r);
    mirror_rtt_sent(&r, 11, 0);
    mirror_rtt_sent(&r, 27, 100);
    mirror_rtt_acked(&r, 11, 500);
    expect(!mirror_rtt_take(&r, &us), "an ack older than the ring pairs with nothing");
    mirror_rtt_acked(&r, 27, 600);
    expect(mirror_rtt_take(&r, &us) && us == 500, "the slot's own seq still pairs");

    /* A frame whose last message never went out has no stamp; the next ack passes it */
    mirror_rtt_reset(&r);
    mirror_rtt_sent(&r, 20, 0);
    mirror_rtt_sent(&r, 21, 10);
    mirror_rtt_sent(&r, 22, 20);
    mirror_rtt_acked(&r, 22, 300);
    expect(mirror_rtt_take(&r, &us) && us == 280, "a later ack gives its own round trip");
    expect(!mirror_rtt_take(&r, &us), "and nothing for the stamps it passed");
    mirror_rtt_acked(&r, 21, 400);
    expect(!mirror_rtt_take(&r, &us), "a passed stamp is gone, not paired late");
    expect_eq((long)mirror_rtt_pending_us(&r, 22, 1000), 0, "passed stamps are not pending");

    mirror_rtt_reset(&r);
    mirror_rtt_sent(&r, 30, 1000);
    mirror_rtt_sent(&r, 31, 5000);
    expect_eq((long)mirror_rtt_pending_us(&r, 29, 9000), 8000, "pending is the oldest unacked stamp");
    mirror_rtt_acked(&r, 30, 7000);
    expect_eq((long)mirror_rtt_pending_us(&r, 30, 9000), 4000, "an acked one stops pending");
    expect_eq((long)mirror_rtt_pending_us(&r, 31, 9000), 0, "a stamp at or before acked is not pending");
    expect(mirror_rtt_take(&r, &us) && us == 6000, "the ack still counts");
    mirror_rtt_acked(&r, 30, 9500);
    expect(!mirror_rtt_take(&r, &us), "a repeated ack gives nothing");

    mirror_rtt_sent(&r, 40, 0xFFFFF000u);
    expect_eq((long)mirror_rtt_pending_us(&r, 39, 0x1000u), 0x2000, "pending across the wrap");

    mirror_rtt_reset(&r);
    mirror_rtt_acked(&r, 40, 0x2000u);
    expect(!mirror_rtt_take(&r, &us), "a reset drops the stamps, as a resync must");
    expect_eq((long)mirror_rtt_pending_us(&r, 39, 0x3000u), 0, "and nothing is pending");
}

/* ---- window statistic --------------------------------------------------------------- */

static void test_median(void)
{
    mirror_quality_ctl_t q;
    mirror_window_t w;

    printf("=== window statistic ===\n");

    mirror_quality_init(&q, 0);
    w = busy(2, 60);
    mirror_window_rtt(&w, 900000);
    feed(&q, w);
    expect_eq(q.rtt_ms, 60, "median of 60, 60, 900 is 60");

    mirror_quality_init(&q, 0);
    w = busy(1, 60);
    mirror_window_rtt(&w, 900000);
    feed(&q, w);
    expect_eq(q.rtt_ms, 60, "an even count takes the lower middle");

    mirror_quality_init(&q, 0);
    w = busy(0, 0);
    for (int i = 0; i < 30; i++) {
        mirror_window_rtt(&w, (i < 12 ? 900u : 70u) * 1000u);
    }
    feed(&q, w);
    expect_eq(q.rtt_ms, 70, "past MIRROR_QC_SAMPLES the latest samples count");
    expect_eq(q.base_ms, 70, "the baseline takes the window's second fastest");

    mirror_quality_init(&q, 0);
    w = idle();
    w.frames = 1;
    w.waiting = true;
    w.pending_ms = 1200;
    feed(&q, w);
    expect_eq(q.rtt_ms, 1200, "with no ack back, the pending age stands in");
}

/* ---- controller, directed ----------------------------------------------------------- */

static void test_skip_and_hold(void)
{
    mirror_quality_ctl_t q;
    static const uint8_t want[12] = { 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 2 };
    uint8_t got[12];
    uint8_t bad_in_hold = 0;

    printf("=== (e) the keyframe window after a change is skipped ===\n");

    mirror_quality_init(&q, 3);
    mirror_quality_viewer(&q, true);

    for (int i = 0; i < 8; i++) {
        got[i] = feed(&q, busy(10, 60));
    }

    /* Straight after the step up to 1: a burst that would step down on its own */
    for (int i = 8; i < 12; i++) {
        got[i] = feed(&q, busy(10, 2000));
        if (i < 11) {
            bad_in_hold |= q.bad;
        }
    }

    expect(memcmp(got, want, sizeof(want)) == 0,
           "attach skips 1 then 3 clear windows step up; again after the step up; the burst waits out skip + hold");
    expect_eq(bad_in_hold, 0, "the skipped and held windows count no congestion");
    expect(q.why != NULL && strcmp(q.why, "queueing") == 0, "and the step down says queueing");
    expect_eq(q.good_needed, 6, "a step down 4 windows after a step up is a failed probe");
}

static void test_idle_climb(void)
{
    mirror_quality_ctl_t q;
    int reached = -1;

    printf("=== idle windows climb ===\n");

    mirror_quality_init(&q, 3);
    mirror_quality_viewer(&q, true);
    for (int i = 1; i <= 30; i++) {
        if (feed(&q, idle()) == 0 && reached < 0) {
            reached = i;
        }
    }
    expect_eq(reached, 12, "a still screen climbs 3 -> 0 in 3 x (skip + 3 windows)");

    mirror_quality_init(&q, 1);
    mirror_quality_viewer(&q, true);
    for (int i = 0; i < 20; i++) {
        mirror_window_t w = idle();

        w.frames = 1;
        w.waiting = true;
        w.pending_ms = 30;
        feed(&q, w);
    }
    expect_eq(q.level, 1, "frames with no ack back yet are not clear");

    /* Level 3 held under load, then a still screen climbs to 0 and the load comes back */
    const char *why = NULL;
    int changes = 0;

    start(&q, 3, 60);
    for (int i = 0; i < 12 && q.level > 0; i++) {
        feed(&q, idle());
        why = q.why;
    }
    expect_eq(q.level, 0, "a still screen climbs from the level load held");
    expect(why != NULL && strcmp(why, "screen still") == 0, "saying screen still");
    for (int i = 0; i < 1 + MIRROR_QC_HOLD_WINDOWS + MIRROR_QC_DOWN_WINDOWS; i++) {
        const uint8_t was = q.level;

        feed(&q, busy(2, 400));
        changes += (q.level != was);
    }
    expect_eq(q.level, 3, "load that congests the climb goes straight back to the level it held");
    expect_eq(changes, 1, "in one change, one keyframe");
    expect_eq(q.good_needed, MIRROR_QC_GOOD_MIN, "a climb on still windows is no probe, so nothing backs off");

    /* A real probe 3 -> 2 first, then still windows climb on inside its probe period */
    start(&q, 3, 60);
    for (int i = 0; i < 1 + MIRROR_QC_GOOD_MIN && q.level == 3; i++) {
        feed(&q, busy(10, 60));
    }
    for (int i = 0; i < 8 && q.level > 0; i++) {
        feed(&q, idle());
    }
    for (int i = 0; i < 1 + MIRROR_QC_HOLD_WINDOWS + MIRROR_QC_DOWN_WINDOWS && q.level < 2; i++) {
        feed(&q, busy(2, 400));
    }
    expect_eq(q.level, 2, "load returns to the level the probe reached");
    expect_eq(q.good_needed, MIRROR_QC_GOOD_MIN, "and the still climbs above it fail that probe no more");
}

static void test_no_evidence(void)
{
    mirror_quality_ctl_t q;
    mirror_window_t owed = idle();

    printf("=== a window with only an ack still owed is no evidence ===\n");

    owed.frames = 1;
    owed.waiting = true;
    owed.pending_ms = 120; /* Under the path's round trip: not overdue */

    /* Sparse presses on a 250 ms path: a press late in a window has no ack back by its end */
    start(&q, 2, 250);
    q.good_needed = MIRROR_QC_GOOD_MAX;
    int up_at = -1;

    for (int i = 0; i < 2 * MIRROR_QC_GOOD_MAX + 10 && up_at < 0; i++) {
        feed(&q, (i % 2) ? owed : busy(1, 255));
        if (q.level < 2) {
            up_at = i;
        }
    }
    expect(up_at > 0 && up_at <= 2 * MIRROR_QC_GOOD_MAX, "the clear streak survives them and the climb comes");

    start(&q, 0, 60);
    feed(&q, busy(4, 300));
    feed(&q, owed);
    feed(&q, busy(4, 300));
    expect_eq(q.level, 1, "congested, owed, congested is two in a row: bursty content steps down");
}

static void test_neutral(void)
{
    mirror_quality_ctl_t q;

    printf("=== neutral windows hold ===\n");

    start(&q, 1, 60);
    feed(&q, busy(10, 60));
    feed(&q, busy(10, 60));
    feed(&q, busy(10, 160));
    feed(&q, busy(10, 60));
    feed(&q, busy(10, 60));
    expect_eq(q.level, 1, "100 ms of queueing resets the clear streak");
    feed(&q, busy(10, 60));
    expect_eq(q.level, 0, "three clear windows in a row step up");

    start(&q, 0, 60);
    for (int i = 0; i < 30; i++) {
        feed(&q, busy(10, i % 3 == 0 ? 280 : 60));
    }
    expect_eq(q.level, 0, "congested windows that never come two in a row do not step down");
}

static void test_blocked_resync(void)
{
    mirror_quality_ctl_t q;
    mirror_window_t w;

    printf("=== (f) a blocked send or an ack timeout is congestion ===\n");

    start(&q, 0, 60);
    w = busy(10, 60);
    w.longest_send_ms = MIRROR_QC_BLOCKED_MS - 1;
    for (int i = 0; i < 5; i++) {
        feed(&q, w);
    }
    expect_eq(q.level, 0, "a 499 ms send is preemption, not backpressure");

    w.longest_send_ms = MIRROR_QC_BLOCKED_MS;
    feed(&q, w);
    expect_eq(q.level, 0, "one blocked window holds");
    feed(&q, w);
    expect_eq(q.level, 1, "two step down");
    expect(q.why != NULL && strcmp(q.why, "blocked send") == 0, "saying blocked send");

    start(&q, 0, 60);
    w = busy(10, 60);
    w.resync = true;
    feed(&q, w);
    feed(&q, busy(10, 60));
    expect_eq(q.level, 0, "one ack timeout then a clear window holds");
    feed(&q, w);
    feed(&q, w);
    expect_eq(q.level, 1, "two ack timeouts in a row step down");
    expect(q.why != NULL && strcmp(q.why, "ack timeout") == 0, "saying ack timeout");

    /* No ack back at all: the oldest one owed stands in for the round trip */
    mirror_window_t silent = idle();

    silent.frames = 1;
    silent.waiting = true;
    silent.pending_ms = 1400;

    start(&q, 0, 60);
    for (int i = 0; i < 3; i++) {
        feed(&q, silent);
    }
    expect_eq(q.level, 0, "a run of windows with no ack back is one strike");
    feed(&q, busy(1, 1300));
    expect_eq(q.level, 1, "a late ack after it is the second");
    expect(q.why != NULL && strcmp(q.why, "queueing") == 0, "saying queueing");

    start(&q, 0, 60);
    feed(&q, busy(1, 1300));
    feed(&q, silent);
    expect_eq(q.level, 1, "a late ack then a silent window step down");
    expect(q.why != NULL && strcmp(q.why, "acks overdue") == 0, "saying acks overdue");

    start(&q, 0, 60);
    feed(&q, silent);
    feed(&q, silent);
    w = idle();
    w.frames = 1;
    w.resync = true;
    feed(&q, w);
    expect_eq(q.level, 1, "a silence that reaches the ack timeout steps down");

    start(&q, 0, 60);
    for (int i = 0; i < 5; i++) {
        feed(&q, silent);
        w = busy(12, 66);
        mirror_window_rtt(&w, 1400000);
        mirror_window_rtt(&w, 1350000);
        feed(&q, w);
    }
    expect_eq(q.level, 0, "stalls that end in clear acks never step down");
}

static void test_byte_cap(void)
{
    mirror_quality_ctl_t q;
    mirror_window_t w;

    printf("=== byte cap ===\n");

    start(&q, 0, 60);
    w = busy(20, 60);
    w.bytes = MIRROR_QC_CAP_BYTES - 1;
    for (int i = 0; i < 20; i++) {
        feed(&q, w);
    }
    expect_eq(q.level, 0, "511 KB/s on a clear link is not a reason");

    w.bytes = MIRROR_QC_CAP_BYTES;
    feed(&q, w);
    feed(&q, w);
    expect_eq(q.level, 0, "two windows over the cap hold");
    feed(&q, w);
    expect_eq(q.level, 1, "three step down");
    expect(q.why != NULL && strcmp(q.why, "byte cap") == 0, "saying byte cap");

    start(&q, 1, 60);
    w.bytes = MIRROR_QC_CAP_BYTES;
    for (int i = 0; i < 2; i++) {
        feed(&q, w);
    }
    w.bytes = 0;
    feed(&q, w);
    w.bytes = MIRROR_QC_CAP_BYTES;
    for (int i = 0; i < 2; i++) {
        feed(&q, w);
    }
    expect_eq(q.level, 1, "a window over the cap is not clear");
}

static void test_pinned(void)
{
    mirror_quality_ctl_t q;
    mirror_window_t w;
    bool held = true;

    printf("=== (g) the viewer's pin wins ===\n");

    start(&q, 0, 60);
    w = busy(10, 60);
    mirror_quality_window(&q, &w, 4);
    expect_eq(q.level, 3, "pin 4 is level 3 at once");
    expect(q.why != NULL && strcmp(q.why, "pinned by viewer") == 0, "reported as pinned");
    for (int i = 0; i < 20; i++) {
        held &= (mirror_quality_window(&q, &w, 4) == 3);
    }
    expect(held, "clear windows do not move a pin");

    /* Fewer than the baseline's horizon, or 1.5 s would become the path */
    w = busy(10, 1500);
    held = true;
    mirror_quality_window(&q, &w, 1);
    for (int i = 0; i < 5; i++) {
        held &= (mirror_quality_window(&q, &w, 1) == 0);
    }
    expect(held, "congested windows do not move a pin");

    held = true;
    for (int i = 0; i < 4; i++) {
        held &= (mirror_quality_window(&q, &w, 0) == 0);
    }
    expect(held, "unpinning skips the keyframe window and waits out the hold");
    mirror_quality_window(&q, &w, 0);
    expect_eq(q.level, 1, "then automatic control resumes (a severe window steps down alone)");
}

static void test_backoff(void)
{
    mirror_quality_ctl_t q;
    static const uint8_t want[6] = { 6, 12, 24, 48, 60, 60 };
    uint8_t got[6] = { 0 };
    int fails = 0, ups = 0, up_at[8] = { 0 }, worst = 0;

    printf("=== (d) failed probes back off, and reset once one holds ===\n");

    /* Level 2 is clear, levels 0 and 1 queue 190 ms */
    start(&q, 0, 60);
    for (int i = 0; i < 1000 && fails < 6; i++) {
        const uint8_t was = q.level;
        const uint8_t now = feed(&q, busy(10, was <= 1 ? 250 : 60));

        if (now < was && ups < 8) {
            up_at[ups++] = i;
        }
        if (now > was && ups > 0) {
            got[fails++] = q.good_needed;
        }
        if (now > worst) {
            worst = now;
        }
    }

    expect(memcmp(got, want, sizeof(want)) == 0, "good_needed doubles 3 -> 6 -> 12 -> 24 -> 48 -> 60, capped");
    expect(up_at[5] - up_at[4] >= 60, "the capped probe waits at least 60 windows");
    expect_eq(worst, 2, "no step past the clear level");

    /* The link improves: level 1 and 0 are clear too */
    int reset_at = -1, first_up = -1, last_up = -1, top = -1;
    for (int i = 0; i < 200; i++) {
        const uint8_t was = q.level;

        feed(&q, busy(10, 60));
        if (q.level < was) {
            first_up = (first_up < 0) ? i : first_up;
            last_up = i;
        }
        if (q.level == 0 && top < 0) {
            top = i;
        }
        if (q.good_needed == MIRROR_QC_GOOD_MIN && reset_at < 0) {
            reset_at = i;
        }
    }
    printf("      first step up after %d windows, level 0 after %d, backoff reset after %d\n",
           first_up, top, reset_at);
    expect(first_up > 0 && first_up <= MIRROR_QC_GOOD_MAX + 2, "the capped wait ends in a step up");
    expect(top > 0 && top - first_up <= MIRROR_QC_PROBE_WINDOWS + 2,
           "one that outlives its probe lets the next climb at once, not after the backoff again");
    expect_eq(reset_at - last_up, MIRROR_QC_STABLE_WINDOWS, "good_needed resets to 3 once the last step up has held 60 windows");
    expect_eq(q.level, 0, "and the climb finishes");
}

static void test_viewer(void)
{
    mirror_quality_ctl_t q;

    printf("=== viewer approval and resume ===\n");

    start(&q, 0, 60);
    mirror_quality_viewer(&q, false);
    feed(&q, busy(10, 2000));
    expect_eq(q.level, 0, "a resume's keyframe window is skipped");
    expect_eq(q.base_ms, 60, "and the same viewer's baseline stands");

    mirror_quality_viewer(&q, true);
    feed(&q, busy(10, 900));
    expect_eq(q.base_ms, 0, "a newly approved viewer's keyframe burst does not seed its path");
    feed(&q, busy(10, 250));
    expect_eq(q.base_ms, 250, "the window after it does: the path is learned afresh");

    start(&q, 0, 60);
    feed(&q, busy(10, 2500));
    for (int i = 0; i < 40; i++) {
        feed(&q, busy(10, 2500));
    }
    expect_eq(q.base_ms, MIRROR_QC_BASE_MAX_MS, "a baseline never exceeds any real path");
    expect_eq(q.level, MIRROR_QC_WORST, "so a queue that never drains is still one");

    /* Viewer A's probes into level 2 failed until the backoff capped */
    int at0 = -1;

    start(&q, 3, 60);
    q.good_needed = MIRROR_QC_GOOD_MAX;
    mirror_quality_viewer(&q, false);
    expect_eq(q.good_needed, MIRROR_QC_GOOD_MAX, "a resume keeps the backoff: same path");
    mirror_quality_viewer(&q, true);
    expect_eq(q.good_needed, MIRROR_QC_GOOD_MIN, "a new viewer drops it with the baseline");
    for (int i = 1; i <= 30 && at0 < 0; i++) {
        if (feed(&q, busy(10, 50)) == 0) {
            at0 = i;
        }
    }
    expect(at0 > 0 && at0 <= 3 * (1 + MIRROR_QC_GOOD_MIN) + 1, "so its clear path climbs 3 -> 0 at once");
}

static void test_baseline_holds(void)
{
    mirror_quality_ctl_t q;
    mirror_window_t w;
    int changes = 0;

    printf("=== the baseline stands through still screens and standing queues ===\n");

    start(&q, 0, 60);
    for (int i = 0; i < 60; i++) {
        feed(&q, idle());
    }
    expect_eq(q.base_ms, 60, "a minute of still screen measures nothing and forgets nothing");
    feed(&q, busy(3, 700));
    feed(&q, busy(3, 700));
    expect_eq(q.level, 1, "so load after it that queues 640 ms is still congestion");

    /* Level 2 holds 100 ms of queue: neutral, so it holds, and must go on holding */
    start(&q, 2, 60);
    for (int i = 0; i < 120; i++) {
        const uint8_t was = q.level;

        feed(&q, busy(10, 160));
        changes += (q.level != was);
    }
    expect_eq(q.base_ms, 60, "two minutes of a standing queue never become the path");
    expect_eq(changes, 0, "so the level holding it never climbs into more");

    /* One ack of ten read straight after a late stamp: lcd_task preempted past the round trip */
    start(&q, 0, 170);
    changes = 0;
    w = busy(9, 170);
    mirror_window_rtt(&w, 500);
    feed(&q, w);
    for (int i = 0; i < 40; i++) {
        const uint8_t was = q.level;

        feed(&q, busy(10, 170));
        changes += (q.level != was);
    }
    expect_eq(q.base_ms, 170, "a 0 ms ack among nine is not the path: the window's second fastest is");
    expect_eq(changes, 0, "and a 170 ms path is not mistaken for a queue");

    /* The same, alone in a sparse window */
    start(&q, 0, 170);
    changes = 0;
    w = idle();
    w.frames = 1;
    mirror_window_rtt(&w, 500);
    feed(&q, w);
    for (int i = 0; i < 40; i++) {
        const uint8_t was = q.level;

        feed(&q, busy(1, 170));
        changes += (q.level != was);
    }
    expect_eq(q.base_ms, 170, "alone in its window: the bucket's second fastest window is the path");
    expect_eq(changes, 0, "nothing steps down");
}

static void test_route_change(void)
{
    mirror_quality_ctl_t q;
    int risen = -1, back = -1, changes = 0;

    printf("=== (h) the baseline follows a route change ===\n");

    start(&q, 0, 50);
    for (int i = 0; i < 35; i++) {
        feed(&q, busy(10, 50));
    }
    expect_eq(q.base_ms, 50, "baseline 50 ms");

    for (int i = 1; i <= 120; i++) {
        const uint8_t was = q.level;

        feed(&q, busy(10, 300));
        changes += (q.level != was);
        if (risen < 0 && q.base_ms >= 300) {
            risen = i;
        }
        if (risen > 0 && back < 0 && q.level == 0) {
            back = i;
        }
    }

    printf("      baseline 300 after %d windows, back at level 0 after %d, %d changes\n",
           risen, back, changes);
    expect(risen > 0 && risen <= 31, "a sustained 300 ms path becomes the baseline within ~30 windows");
    expect(back > 0 && back <= 60, "and the stream climbs back to level 0");
    expect(q.good_needed == MIRROR_QC_GOOD_MIN, "the climb is not mistaken for failed probes");
}

static void test_spikes(void)
{
    mirror_quality_ctl_t q;
    mirror_window_t w;
    int changes = 0, worst = 0;

    printf("=== (i) a single spike does not cascade ===\n");

    start(&q, 0, 60);
    w = busy(8, 60);
    mirror_window_rtt(&w, 900000);
    mirror_window_rtt(&w, 950000);
    feed(&q, w);
    for (int i = 0; i < 10; i++) {
        feed(&q, busy(10, 60));
    }
    expect_eq(q.level, 0, "two stalled acks among eight clear ones: nothing");

    start(&q, 0, 60);
    for (int i = 0; i < 5; i++) {
        feed(&q, busy(2, 60));
    }
    w = busy(0, 0);
    w.frames = 2;
    mirror_window_rtt(&w, 900000);
    mirror_window_rtt(&w, 950000);
    feed(&q, w);
    feed(&q, busy(2, 60));
    expect_eq(q.level, 0, "a sparse window of two stalled acks: nothing, one stall delays both frames in flight");

    start(&q, 0, 60);
    w = busy(4, 60);
    mirror_window_rtt(&w, 900000);
    feed(&q, w);
    w = busy(4, 60);
    mirror_window_rtt(&w, 950000);
    feed(&q, w);
    expect_eq(q.level, 0, "a stall straddling two windows: nothing");

    /* Seen in the simulator: a stall ending late in a window leaves its two frames and one more */
    start(&q, 0, 60);
    w = busy(1, 66);
    mirror_window_rtt(&w, 1527000);
    mirror_window_rtt(&w, 1526000);
    feed(&q, w);
    expect_eq(q.rtt_ms, 66, "1527, 1526, 66: the two frames one stall held are trimmed");
    expect_eq(q.level, 0, "so it is neither severe nor congested");

    start(&q, 0, 60);
    feed(&q, busy(3, 1500));
    expect_eq(q.level, 1, "three late acks are more than one stall: severe");

    start(&q, 0, 60);
    feed(&q, busy(10, 900));
    for (int i = 0; i < 20; i++) {
        const uint8_t was = q.level;

        feed(&q, busy(10, 60));
        changes += (q.level != was);
        if (q.level > worst) {
            worst = q.level;
        }
    }
    expect_eq(q.level, 0, "a whole second of 840 ms queueing then clear: back at 0");
    expect_eq(worst, 1, "one step down at most");
    expect_eq(changes, 1, "and one step back");
}

/* ---- simulated link ----------------------------------------------------------------- */

#define SIM_SNDBUF 23040 /* CONFIG_LWIP_TCP_SND_BUF_DEFAULT */
#define SIM_MSG_MAX 3890 /* MIRROR_MSG_MAX_BYTES */
#define SIM_MIN_FRAME_MS 50
#define SIM_POLL_MS 20
#define SIM_ACK_TIMEOUT_MS 3000
#define SIM_KEYFRAME_MS 60000
#define SIM_INFLIGHT 2
#define SIM_Q 256
#define SIM_MAX_WINDOWS 1024

typedef struct {
    const char *name;
    uint32_t up_Bps; /* Device uplink, behind the TCP send buffer */
    uint32_t down_Bps; /* Relay to viewer; the relay queues without limit */
    uint32_t rtt_ms; /* Base round trip, plus 0..9 ms of viewer jitter */
    uint32_t cpu_ms; /* Preemption and TLS inside every send call */
    uint32_t lead_ms; /* Menu first: lead_bytes every lead_every_ms, 0 = still */
    uint32_t lead_every_ms;
    uint32_t lead_bytes;
    uint32_t still_ms; /* Then a still screen */
    uint32_t frame[4]; /* Then, per level: one frame of changing content, 0 = still */
    uint32_t busy_ms, idle_ms; /* Content for busy_ms then still for idle_ms, repeating; 0 = always */
    uint32_t key[4]; /* Keyframe per level */
    uint32_t stall_every_ms; /* Wi-Fi hiccups: nothing moves either way for stall_ms */
    uint32_t stall_ms;
    uint32_t pre_every, pre_ms; /* 1 in pre_every last messages: lcd_task preempts for pre_ms
                                 * after the socket write, before the stamp */
    uint32_t change_ms, change_down_Bps; /* The relay->viewer rate from then on, 0 = never */
    uint32_t minutes;
    uint8_t start_level;
    uint8_t good_needed; /* Backoff left by earlier failed probes, 0 = none */
    bool old; /* Drive the old send-time/bytes controller instead, for comparison */
} sim_cfg_t;

typedef struct {
    int windows;
    int changes, ups, downs, resyncs;
    int at[4];
    int max_level;
    int last_change;
    int up_win[64];
    uint32_t stall_sum;
    uint32_t rtt_sum;
    uint8_t trace[SIM_MAX_WINDOWS];
    uint16_t rtt[SIM_MAX_WINDOWS];
    bool busy[SIM_MAX_WINDOWS]; /* Content was changing when the window closed */
} sim_res_t;

typedef struct {
    uint32_t size, left;
    uint16_t seq;
    bool last;
} sim_msg_t;

typedef struct {
    sim_msg_t m[SIM_Q];
    int head, n;
    uint32_t bytes;
} fifo_t;

typedef struct {
    uint16_t seq;
    uint32_t due;
} sim_ack_t;

static const sim_cfg_t *C;
static sim_res_t *R;
static fifo_t upq, relayq;
static sim_ack_t acks[SIM_Q];
static int ack_head, ack_n;
static uint32_t last_due, rng;

/* mirror_task's state */
static mirror_rtt_t ring;
static mirror_quality_ctl_t qc;
static mirror_window_t win;
static uint16_t seq, acked, seen_ack, frame_seq;
static uint32_t last_ack_t, last_frame_t, last_key_t, window_t, wake_t, next_content_t;
static bool want_key, dirty, dirty_menu, in_frame, in_call, enq;
static uint8_t level, old_good;
static int msg_i, msg_n;
static uint32_t frame_bytes, call_t, ret_t, msg_size;

/* esp_timer's low 32 bits, set to wrap two minutes in */
static uint32_t us(uint32_t t)
{
    return 0xFFFFFFFFu - 120000000u + t * 1000u;
}

static void fifo_push(fifo_t *f, sim_msg_t m)
{
    if (f->n == SIM_Q) {
        bad("sim queue overflow");
        return;
    }
    f->m[(f->head + f->n) % SIM_Q] = m;
    f->n++;
    f->bytes += m.left;
}

/* Drain up to budget bytes from the head; returns the message it finished, if any */
static bool fifo_drain(fifo_t *f, uint32_t *budget, sim_msg_t *done)
{
    if (f->n == 0 || *budget == 0) {
        return false;
    }

    sim_msg_t *h = &f->m[f->head];
    const uint32_t take = (h->left < *budget) ? h->left : *budget;

    h->left -= take;
    f->bytes -= take;
    *budget -= take;

    if (h->left > 0) {
        return false;
    }

    *done = *h;
    f->head = (f->head + 1) % SIM_Q;
    f->n--;
    return true;
}

static bool stalled(uint32_t t)
{
    return C->stall_every_ms && t % C->stall_every_ms >= C->stall_every_ms - C->stall_ms;
}

/* Past the menu and the still screen, whether the changing content is on */
static bool busy_at(uint32_t t)
{
    const uint32_t from = C->lead_ms + C->still_ms;

    if (t < from || C->frame[level] == 0) {
        return false;
    }
    return C->busy_ms == 0 || (t - from) % (C->busy_ms + C->idle_ms) < C->busy_ms;
}

static void link_step(uint32_t t)
{
    uint32_t budget = stalled(t) ? 0 : C->up_Bps / 1000;
    sim_msg_t m;

    while (budget > 0 && upq.n > 0) {
        if (fifo_drain(&upq, &budget, &m)) {
            m.left = m.size;
            fifo_push(&relayq, m);
        }
    }

    const uint32_t down = (C->change_ms && t >= C->change_ms) ? C->change_down_Bps : C->down_Bps;

    budget = stalled(t) ? 0 : down / 1000;
    while (budget > 0 && relayq.n > 0) {
        if (fifo_drain(&relayq, &budget, &m) && m.last) {
            /* The viewer acks a frame's last message; TCP keeps the acks in order */
            rng = rng * 1103515245u + 12345u;
            uint32_t due = t + C->rtt_ms + (rng >> 16) % 10;

            if (due < last_due) {
                due = last_due;
            }
            last_due = due;
            acks[(ack_head + ack_n) % SIM_Q] = (sim_ack_t){ m.seq, due };
            ack_n++;
        }
    }
}

/* handle_input runs only while mirror_task is outside a send call, which holds the client lock */
static void acks_step(uint32_t t)
{
    while (ack_n > 0 && acks[ack_head].due <= t && !in_call && !stalled(t)) {
        const uint16_t a = acks[ack_head].seq;

        ack_head = (ack_head + 1) % SIM_Q;
        ack_n--;

        if ((int16_t)(a - acked) > 0) {
            acked = a;
            mirror_rtt_acked(&ring, a, us(t));
        }
    }
}

static void collect(void)
{
    uint32_t v;

    while (mirror_rtt_take(&ring, &v)) {
        mirror_window_rtt(&win, v);
    }
}

/* The controller this replaces: total send time and bytes */
static uint8_t old_adjust(uint32_t bytes, uint32_t stall_ms, uint8_t *good, uint8_t lvl)
{
    if (stall_ms > 150 || bytes > 140u * 1024u) {
        if (lvl < 3) {
            lvl++;
            *good = 0;
        }
    } else if (bytes < 60u * 1024u && stall_ms == 0) {
        if (++*good >= 3 && lvl > 0) {
            lvl--;
            *good = 0;
        }
    } else {
        *good = 0;
    }
    return lvl;
}

static void window_check(uint32_t t)
{
    if (t - window_t < 1000) {
        return;
    }

    collect();
    win.waiting = (seq != acked);
    win.pending_ms = mirror_rtt_pending_us(&ring, acked, us(t)) / 1000;

    const uint8_t next = C->old ? old_adjust(win.bytes, win.stall_ms, &old_good, level) :
                                  mirror_quality_window(&qc, &win, 0);

    if (R->windows < SIM_MAX_WINDOWS) {
        R->trace[R->windows] = next;
        R->rtt[R->windows] = qc.rtt_ms;
        R->busy[R->windows] = busy_at(t);
    }
    R->at[next]++;
    R->stall_sum += win.stall_ms;
    R->rtt_sum += qc.rtt_ms;
    if (next != level) {
        R->changes++;
        R->last_change = R->windows;
        if (next < level) {
            if (R->ups < 64) {
                R->up_win[R->ups] = R->windows;
            }
            R->ups++;
        } else {
            R->downs++;
        }
        level = next;
        want_key = true;
    }
    if (next > R->max_level) {
        R->max_level = next;
    }
    R->windows++;

    memset(&win, 0, sizeof(win));
    window_t = t;
}

static void call_start(uint32_t t)
{
    const uint32_t rem = frame_bytes - (uint32_t)msg_i * SIM_MSG_MAX;

    msg_size = (rem < SIM_MSG_MAX) ? rem : SIM_MSG_MAX;
    in_call = true;
    enq = false;
    call_t = t;
}

static void call_step(uint32_t t)
{
    /* The message reaches the socket halfway through the call's CPU time, once the send
     * buffer has room for it; the call returns after the rest */
    if (!enq && t >= call_t + C->cpu_ms / 2 && upq.bytes + msg_size <= SIM_SNDBUF) {
        fifo_push(&upq, (sim_msg_t){ msg_size, msg_size, frame_seq, msg_i == msg_n - 1 });
        enq = true;
        ret_t = t + (C->cpu_ms - C->cpu_ms / 2);

        if (C->pre_every && msg_i == msg_n - 1) {
            rng = rng * 1103515245u + 12345u;
            if ((rng >> 16) % C->pre_every == 0) {
                ret_t += C->pre_ms;
            }
        }
    }

    if (!enq || t < ret_t) {
        return;
    }

    const uint32_t ms = t - call_t;

    win.stall_ms += ms;
    if (ms > win.longest_send_ms) {
        win.longest_send_ms = ms;
    }
    in_call = false;

    if (msg_i == msg_n - 1) {
        mirror_rtt_sent(&ring, frame_seq, us(t));
    }

    if (++msg_i == msg_n) {
        seq = frame_seq;
        in_frame = false;
        wake_t = t + 1;
        window_check(t);
    }
}

static void iteration(uint32_t t)
{
    collect();

    if (acked != seen_ack) {
        seen_ack = acked;
        last_ack_t = t;
    }

    bool behind = (uint16_t)(seq - acked) >= SIM_INFLIGHT;

    if (behind && t - last_ack_t > SIM_ACK_TIMEOUT_MS) {
        acked = seq;
        mirror_rtt_reset(&ring);
        seen_ack = seq;
        last_ack_t = t;
        want_key = true;
        win.resync = true;
        behind = false;
        R->resyncs++;
    }

    if (!behind && t - last_frame_t >= SIM_MIN_FRAME_MS) {
        const bool key = want_key || t - last_key_t > SIM_KEYFRAME_MS;
        const uint32_t content = dirty_menu ? C->lead_bytes : C->frame[level];
        const uint32_t bytes = key ? C->key[level] : dirty ? content : 0;

        if (key) {
            want_key = false;
            last_key_t = t;
        }

        if (bytes > 0) {
            dirty = false;
            if (seq == acked) {
                last_ack_t = t;
            }
            frame_bytes = bytes;
            msg_n = (int)((bytes + SIM_MSG_MAX - 1) / SIM_MSG_MAX);
            msg_i = 0;
            frame_seq = (uint16_t)(seq + 1);
            in_frame = true;
            last_frame_t = t;
            win.frames++;
            win.bytes += bytes;
            call_start(t);
            return; /* The window closes after the frame, as the firmware's does */
        }
    }

    wake_t = t + SIM_POLL_MS;
    window_check(t);
}

static void content_step(uint32_t t)
{
    if (t < C->lead_ms) {
        if (C->lead_every_ms && t >= next_content_t) {
            dirty = dirty_menu = true;
            next_content_t = t + C->lead_every_ms;
        }
    } else if (busy_at(t)) {
        dirty = true;
        dirty_menu = false;
    }
}

static void sim_run(const sim_cfg_t *cfg, sim_res_t *res)
{
    C = cfg;
    R = res;
    memset(res, 0, sizeof(*res));
    memset(&upq, 0, sizeof(upq));
    memset(&relayq, 0, sizeof(relayq));
    memset(&win, 0, sizeof(win));
    ack_head = ack_n = 0;
    last_due = 0;
    rng = 12345;

    seq = acked = seen_ack = frame_seq = 0;
    last_ack_t = last_frame_t = last_key_t = window_t = wake_t = next_content_t = 0;
    want_key = true; /* The session's first frame is a keyframe */
    dirty = dirty_menu = in_frame = in_call = enq = false;
    level = cfg->start_level;
    old_good = 0;

    mirror_rtt_reset(&ring);
    mirror_quality_init(&qc, level);
    mirror_quality_viewer(&qc, true);
    if (cfg->good_needed) {
        qc.good_needed = cfg->good_needed;
    }

    for (uint32_t t = 0; t < cfg->minutes * 60000u; t++) {
        content_step(t);
        link_step(t);

        if (!in_call && in_frame) {
            call_start(t);
        } else if (!in_call && !in_frame && t >= wake_t) {
            iteration(t);
        }
        if (in_call) {
            call_step(t);
        }

        acks_step(t);
    }

    printf("      %s%s: %d windows, %d changes (%d up, %d down), %d resyncs, level windows "
           "%d/%d/%d/%d, mean rtt %u ms, mean stall %u ms/s\n",
           cfg->name, cfg->old ? " [old controller]" : "", res->windows, res->changes, res->ups,
           res->downs, res->resyncs, res->at[0], res->at[1], res->at[2], res->at[3],
           res->windows ? res->rtt_sum / (uint32_t)res->windows : 0,
           res->windows ? res->stall_sum / (uint32_t)res->windows : 0);
}

/* First window at or after from where the level is lvl, -1 if never */
static int first_at(const sim_res_t *r, int from, uint8_t lvl)
{
    for (int i = from; i < r->windows && i < SIM_MAX_WINDOWS; i++) {
        if (r->trace[i] == lvl) {
            return i;
        }
    }
    return -1;
}

/* Smallest gap between step ups whose window is at or after from; 9999 if fewer than two */
static int min_up_gap(const sim_res_t *r, int from)
{
    int gap = 9999;

    for (int i = 1; i < r->ups && i < 64; i++) {
        if (r->up_win[i - 1] >= from && r->up_win[i] - r->up_win[i - 1] < gap) {
            gap = r->up_win[i] - r->up_win[i - 1];
        }
    }
    return gap;
}

static sim_res_t res, res_old;

static void test_sim_still(void)
{
    printf("=== (a) a still screen climbs to level 0 and stays ===\n");

    const sim_cfg_t fast = {
        .name = "still, 1 MB/s", .up_Bps = 1000000, .down_Bps = 1000000, .rtt_ms = 50,
        .cpu_ms = 2, .key = { 40000, 36000, 20000, 8000 }, .minutes = 10, .start_level = 3,
    };

    sim_run(&fast, &res);
    expect(first_at(&res, 0, 0) >= 0 && first_at(&res, 0, 0) <= 20, "at level 0 within 20 s");
    expect(res.last_change <= 20, "and never leaves it");

    const sim_cfg_t slow = {
        .name = "still, 60 KB/s", .up_Bps = 60000, .down_Bps = 1000000, .rtt_ms = 50,
        .cpu_ms = 2, .key = { 40000, 36000, 20000, 8000 }, .minutes = 10, .start_level = 3,
    };

    sim_run(&slow, &res);
    expect(first_at(&res, 0, 0) >= 0 && first_at(&res, 0, 0) <= 30,
           "a slow link too: a still screen's keyframes are its only load");
    expect(res.last_change <= 30, "and it stays");
}

static void test_sim_cpu(void)
{
    printf("=== (b) busy content on a fast link with long CPU-bound sends stays at level 0 ===\n");

    /* The log's case: lcd_task preempting every send, ~30 ms each, nothing queued */
    sim_cfg_t cfg = {
        .name = "busy, 2 MB/s, 30 ms/send", .up_Bps = 2000000, .down_Bps = 2000000,
        .rtt_ms = 60, .cpu_ms = 30, .frame = { 24000, 22000, 12000, 5000 },
        .key = { 40000, 36000, 20000, 8000 }, .minutes = 10, .start_level = 0,
    };

    sim_run(&cfg, &res);
    expect(res.stall_sum / (uint32_t)res.windows > 150, "send calls take > 150 ms a second (the old trigger)");
    expect_eq(res.changes, 0, "no level change in 10 minutes");
    expect_eq(res.at[0], res.windows, "every window at level 0");

    cfg.old = true;
    sim_run(&cfg, &res_old);
    expect(res_old.max_level >= 2, "the old controller drops on the same trace (the regression)");
}

/* Mean window round trip from window from on */
static uint32_t mean_rtt(const sim_res_t *r, int from)
{
    uint32_t sum = 0;
    int n = 0;

    for (int i = from; i < r->windows && i < SIM_MAX_WINDOWS; i++) {
        sum += r->rtt[i];
        n++;
    }
    return n ? sum / (uint32_t)n : 0;
}

static void check_settles(const sim_cfg_t *cfg, uint8_t floor)
{
    const int lead = (int)((cfg->lead_ms + cfg->still_ms) / 1000);

    sim_run(cfg, &res);

    const int settled = first_at(&res, lead, floor);
    int after = 0;

    for (int i = lead + 60; i < res.windows && i < SIM_MAX_WINDOWS; i++) {
        after += (res.trace[i] == floor);
    }

    expect(settled >= 0 && settled <= lead + 30, "reaches its level within 30 s of busy content");
    expect_eq(res.max_level, floor, "never steps past it: no flapping below");
    expect(res.changes <= 30, "at most 30 changes in 10 minutes");
    expect(min_up_gap(&res, 300) >= 55, "probes in the last 5 minutes at least 55 s apart");
    expect(after * 10 >= (res.windows - lead - 60) * 9, "at its level >= 90% of the time after the first minute");
    expect_eq(res.resyncs, 0, "no ack timeouts");
}

static void test_sim_limited(void)
{
    printf("=== (c) a bandwidth-limited link steps down and settles ===\n");

    /* The session always opens on the pairing page, then menus: small updates first */
    const sim_cfg_t up = {
        .name = "uplink 100 KB/s", .up_Bps = 100000, .down_Bps = 2000000, .rtt_ms = 50,
        .cpu_ms = 2, .lead_ms = 15000, .lead_every_ms = 700, .lead_bytes = 2000,
        .frame = { 20000, 18000, 4000, 1500 }, .key = { 40000, 36000, 16000, 6000 },
        .minutes = 10, .start_level = 0,
    };

    check_settles(&up, 2);

    /* At 150 KB/s level 1 queues within a few ms of MIRROR_QC_QUEUE_HI_MS, so how long its
     * probes last is noise, for the old controller too; 140 KB/s is clear of that edge */
    const sim_cfg_t down = {
        .name = "relay->viewer 140 KB/s", .up_Bps = 1000000, .down_Bps = 140000, .rtt_ms = 50,
        .cpu_ms = 2, .lead_ms = 15000, .lead_every_ms = 700, .lead_bytes = 2000,
        .frame = { 24000, 20000, 5000, 2000 }, .key = { 40000, 36000, 16000, 6000 },
        .minutes = 10, .start_level = 0,
    };

    check_settles(&down, 2);

    const sim_cfg_t slow = {
        .name = "uplink 60 KB/s", .up_Bps = 60000, .down_Bps = 2000000, .rtt_ms = 50,
        .cpu_ms = 2, .lead_ms = 15000, .lead_every_ms = 700, .lead_bytes = 2000,
        .frame = { 20000, 18000, 8000, 2000 }, .key = { 40000, 36000, 16000, 6000 },
        .minutes = 10, .start_level = 0,
    };

    check_settles(&slow, 3);

    /* Every level queues seconds here, more than any path's round trip */
    sim_cfg_t crawl = slow;
    int floor = 0;

    crawl.name = "uplink 10 KB/s";
    crawl.up_Bps = 10000;
    sim_run(&crawl, &res);
    for (int i = 120; i < res.windows && i < SIM_MAX_WINDOWS; i++) {
        floor += (res.trace[i] == 3);
    }
    expect(floor * 10 >= (res.windows - 120) * 8, "10 KB/s: at level 3 >= 80% of the time after 2 minutes");
    expect(res.changes <= 30, "with at most 30 changes");

    sim_cfg_t old = up;

    old.old = true;
    sim_run(&old, &res_old);

    /* Level 2's own frames keep a queue standing: it must not be learned as the path */
    const sim_cfg_t sat = {
        .name = "uplink 100 KB/s, level 2 saturated", .up_Bps = 100000, .down_Bps = 2000000,
        .rtt_ms = 50, .cpu_ms = 2, .lead_ms = 15000, .lead_every_ms = 700, .lead_bytes = 2000,
        .frame = { 24000, 22000, 12000, 5000 }, .key = { 40000, 36000, 20000, 8000 },
        .minutes = 10, .start_level = 0,
    };

    int held = 0, top = 0;

    sim_run(&sat, &res);
    for (int i = 75; i < res.windows && i < SIM_MAX_WINDOWS; i++) {
        held += (res.trace[i] >= 2);
        top += (res.trace[i] == 0);
    }
    expect(held * 10 >= (res.windows - 75) * 9, "a standing queue at level 2: level 2 or lower >= 90% of the time");
    expect_eq(top, 0, "never climbing to level 0 into more of it");

    /* A still screen measures nothing; the path it forgot would be relearned under load */
    const sim_cfg_t gap = {
        .name = "relay->viewer 60 KB/s, 40 s still screen first", .up_Bps = 1000000,
        .down_Bps = 60000, .rtt_ms = 50, .cpu_ms = 2, .lead_ms = 15000, .lead_every_ms = 700,
        .lead_bytes = 2000, .still_ms = 40000, .frame = { 24000, 22000, 12000, 5000 },
        .key = { 40000, 36000, 20000, 8000 }, .minutes = 10, .start_level = 0,
    };

    check_settles(&gap, 3);
    expect(mean_rtt(&res, 115) < 50 + MIRROR_QC_QUEUE_HI_MS, "and its round trip stays within the path plus 150 ms");
}

static void test_sim_content(void)
{
    printf("=== content that comes and goes ===\n");

    /* 20 s of changing content, 20 s still, repeating */
    const sim_cfg_t alt = {
        .name = "relay->viewer 60 KB/s, 20 s busy / 20 s still", .up_Bps = 1000000,
        .down_Bps = 60000, .rtt_ms = 50, .cpu_ms = 2, .lead_ms = 15000, .lead_every_ms = 700,
        .lead_bytes = 2000, .frame = { 24000, 22000, 12000, 5000 }, .busy_ms = 20000,
        .idle_ms = 20000, .key = { 40000, 36000, 20000, 8000 }, .minutes = 10, .start_level = 0,
    };
    int phases = 0, sharp = 0, busy = 0, low = 0;

    sim_run(&alt, &res);
    for (int i = 180; i < res.windows && i < SIM_MAX_WINDOWS; i++) {
        if (res.busy[i]) {
            busy++;
            low += (res.trace[i] == 3);
        }
        if (res.busy[i] && !res.busy[i - 1]) {
            phases++;
            sharp += (res.trace[i - 1] == 0);
        }
    }
    printf("      still phases ending at level 0: %d of %d; busy windows at level 3: %d of %d\n",
           sharp, phases, low, busy);
    expect(phases > 0 && sharp == phases, "every still screen climbs back to level 0");
    expect_eq(qc.good_needed, MIRROR_QC_GOOD_MIN, "those climbs fail no probes, so nothing backs off");
    expect(low * 10 >= busy * 9, "busy content returns to level 3 in one change, not a walk down");

    /* Sparse presses on a clear 250 ms path, level 2 after earlier failed probes */
    const sim_cfg_t press = {
        .name = "presses every 700 ms, 250 ms path, backoff 60", .up_Bps = 2000000,
        .down_Bps = 2000000, .rtt_ms = 250, .cpu_ms = 5, .lead_ms = 600000, .lead_every_ms = 700,
        .lead_bytes = 3000, .key = { 40000, 36000, 20000, 8000 }, .minutes = 10,
        .start_level = 2, .good_needed = MIRROR_QC_GOOD_MAX,
    };

    sim_run(&press, &res);
    printf("      level 0 after %d windows\n", first_at(&res, 0, 0));
    expect(first_at(&res, 0, 0) >= 0 && first_at(&res, 0, 0) <= MIRROR_QC_GOOD_MAX + 30,
           "windows with an ack still owed do not reset the climb: level 0 after one backoff wait");
    expect_eq(res.downs, 0, "and nothing steps down");
}

static void test_sim_paths(void)
{
    printf("=== preemption, long paths and a link that recovers ===\n");

    /* lcd_task preempts 200 ms between the last socket write and its stamp: that ack reads ~0 */
    const sim_cfg_t pre = {
        .name = "busy, 2 MB/s, 170 ms path, 1 in 200 stamps 200 ms late", .up_Bps = 2000000,
        .down_Bps = 2000000, .rtt_ms = 170, .cpu_ms = 30, .frame = { 24000, 22000, 12000, 5000 },
        .key = { 40000, 36000, 20000, 8000 }, .pre_every = 200, .pre_ms = 200, .minutes = 10,
        .start_level = 0,
    };

    sim_run(&pre, &res);
    expect_eq(res.changes, 0, "an ack cut short is not the path: no change");

    const sim_cfg_t far = {
        .name = "busy, 2 MB/s, 700 ms path, from level 2", .up_Bps = 2000000, .down_Bps = 2000000,
        .rtt_ms = 700, .cpu_ms = 10, .frame = { 24000, 22000, 12000, 5000 },
        .key = { 40000, 36000, 20000, 8000 }, .minutes = 10, .start_level = 2,
    };

    sim_run(&far, &res);
    expect(first_at(&res, 0, 0) >= 0 && res.at[0] * 10 >= res.windows * 9,
           "a path past the baseline cap still climbs to level 0 and stays");

    /* Five minutes limited, probes backed off to the cap, then the link clears */
    const sim_cfg_t rise = {
        .name = "relay->viewer 30 KB/s, 2 MB/s from 300 s", .up_Bps = 2000000,
        .down_Bps = 30000, .change_ms = 300000, .change_down_Bps = 2000000, .rtt_ms = 50,
        .cpu_ms = 2, .lead_ms = 15000, .lead_every_ms = 700, .lead_bytes = 2000,
        .frame = { 24000, 22000, 12000, 5000 }, .key = { 40000, 36000, 20000, 8000 },
        .minutes = 10, .start_level = 0,
    };

    sim_run(&rise, &res);

    const int back = first_at(&res, 300, 0);

    printf("      back at level 0 %d windows after the link cleared\n", back < 0 ? -1 : back - 300);
    expect(back >= 0 && back - 300 <= MIRROR_QC_GOOD_MAX + 2 * MIRROR_QC_PROBE_WINDOWS,
           "one backoff wait, not one per level");
}

static void test_sim_stalls(void)
{
    printf("=== (i) Wi-Fi stalls and far viewers on a fast link ===\n");

    sim_cfg_t cfg = {
        .name = "busy, 2 MB/s, 500 ms stall every 6 s", .up_Bps = 2000000, .down_Bps = 2000000,
        .rtt_ms = 60, .cpu_ms = 10, .frame = { 24000, 22000, 12000, 5000 },
        .key = { 40000, 36000, 20000, 8000 }, .stall_every_ms = 6000, .stall_ms = 500,
        .minutes = 10, .start_level = 0,
    };

    sim_run(&cfg, &res);
    expect_eq(res.changes, 0, "500 ms stalls: no change");

    cfg.name = "busy, 2 MB/s, 1.5 s stall every 10 s";
    cfg.stall_every_ms = 10000;
    cfg.stall_ms = 1500;
    sim_run(&cfg, &res);
    expect_eq(res.changes, 0, "1.5 s stalls: no change");

    cfg.name = "busy, 2 MB/s, 2.5 s stall every 15 s";
    cfg.stall_every_ms = 15000;
    cfg.stall_ms = 2500;
    sim_run(&cfg, &res);
    expect(res.max_level <= 1 && res.changes <= 16, "2.5 s outages: never past level 1, at most 16 changes");

    cfg.stall_every_ms = 0;
    cfg.stall_ms = 0;
    cfg.name = "busy, 2 MB/s, 400 ms path";
    cfg.rtt_ms = 400;
    sim_run(&cfg, &res);
    expect_eq(res.changes, 0, "a 400 ms path is not a queue");
}

int main(void)
{
    test_ring();
    test_median();
    test_skip_and_hold();
    test_idle_climb();
    test_no_evidence();
    test_neutral();
    test_blocked_resync();
    test_byte_cap();
    test_pinned();
    test_backoff();
    test_viewer();
    test_route_change();
    test_spikes();
    test_baseline_holds();
    test_sim_still();
    test_sim_cpu();
    test_sim_limited();
    test_sim_content();
    test_sim_paths();
    test_sim_stalls();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
