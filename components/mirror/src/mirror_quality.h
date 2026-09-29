#ifndef MIRROR_QUALITY_H
#define MIRROR_QUALITY_H

#include <stdint.h>
#include <stdbool.h>

// Screen Mirror quality controller and ack round-trip ring. Pure logic with the time passed
// in, so the host tests drive it directly; mirror_task.c owns the locks and the clock.
// Levels are plain numbers, 0 (exact) to MIRROR_QC_WORST, as mirror_quality_t counts them

#define MIRROR_QC_WORST 3 // MIRROR_Q_444_HALF; mirror_task.c asserts it
#define MIRROR_QC_INFLIGHT 2 // MIRROR_MAX_INFLIGHT: the most frames one stall can delay

// Link thresholds. The baseline is the path's own round trip, so a slow path is not
// mistaken for a queue
#define MIRROR_QC_QUEUE_HI_MS 150 // Queueing above this, or above the baseline, is congestion
#define MIRROR_QC_QUEUE_LO_MS 50 // Queueing below this, or below half the baseline, is clear
#define MIRROR_QC_BLOCKED_MS 500 // One send held this long is TCP backpressure, not preemption
#define MIRROR_QC_BASE_MAX_MS 500 // No relay path is slower: a larger minimum is a queue. It
                                  // caps a seed taken under load; paths to 1.5x still read clear

// Decision timing, in 1 s windows
#define MIRROR_QC_DOWN_WINDOWS 2 // Congested windows in a row that step down
#define MIRROR_QC_SEVERE_SAMPLES (MIRROR_QC_INFLIGHT + 1) // One window steps down alone only
                                   // when more acks ran late than one stall can delay
#define MIRROR_QC_HOLD_WINDOWS 3 // No step down this soon after a change, while it drains
#define MIRROR_QC_GOOD_MIN 3 // Clear windows a step up takes...
#define MIRROR_QC_GOOD_MAX 60 // ...doubled by every failed probe, up to this
#define MIRROR_QC_PROBE_WINDOWS 15 // A step down this soon after a step up failed the probe; a
                                   // step up that outlives it lets the next climb at GOOD_MIN
#define MIRROR_QC_STABLE_WINDOWS 60 // A step up that held this long resets the backoff

// Relay volume ceiling. The relay bills per message and a message holds ~3.9 KB, so a link
// that could carry more still stops here
#define MIRROR_QC_CAP_BYTES (512u * 1024u)
#define MIRROR_QC_CAP_WINDOWS 3

// Baseline: the fastest RTT over the last 20 to 30 windows that were near it. A still screen
// or a queued level leaves it standing; a run of congestion at the worst level relearns it
#define MIRROR_QC_BASE_BUCKETS 3
#define MIRROR_QC_BASE_WINDOWS 10
#define MIRROR_QC_RELEARN_WINDOWS 10 // That run, in congested windows in a row

#define MIRROR_QC_SAMPLES 24 // RTT samples kept per window; 20 fps is the most there can be

#define MIRROR_RTT_SLOTS 16 // Power of two, far past the frames the ack window lets out

// One frame's round trip: stamped when its acked message has gone to the socket, answered
// when the viewer's ack arrives
typedef struct {
    uint32_t sent_us; // Low 32 bits of esp_timer: differences hold for 71 minutes
    uint32_t ack_us;
    uint16_t seq;
    uint8_t state;
} mirror_rtt_slot_t;

typedef struct {
    mirror_rtt_slot_t slot[MIRROR_RTT_SLOTS];
} mirror_rtt_t;

// What one window saw. Zeroed at the start of each
typedef struct {
    uint32_t bytes;
    uint32_t stall_ms; // Time inside send calls. Logged only: preemption and TLS count too
    uint32_t longest_send_ms;
    uint32_t pending_ms; // Age of the oldest stamped frame still unacked at the window's end
    uint16_t frames; // Sequence numbers sent
    uint16_t samples; // RTT samples seen; rtt_ms keeps the latest MIRROR_QC_SAMPLES
    uint16_t rtt_ms[MIRROR_QC_SAMPLES];
    bool waiting; // Something sent is still unacked at the window's end
    bool resync; // The ack timeout fired
} mirror_window_t;

typedef struct {
    uint8_t level;
    uint8_t pinned; // Last pin seen, 0 = automatic
    uint8_t bad; // Congested windows in a row, outside a hold
    uint8_t good; // Clear windows in a row
    uint8_t good_needed; // Clear windows a step up takes, backed off by failed probes
    bool passed; // The last step up outlived its probe: the next takes MIRROR_QC_GOOD_MIN
    uint8_t held_level; // The level acked load last held before still windows climbed, UINT8_MAX
                        // when none: congestion returns there
    uint8_t skip; // Windows to ignore whole: they carry a keyframe burst
    uint8_t hold; // Windows before a step down is allowed again
    uint8_t over_cap; // Windows over the byte cap in a row
    bool tested; // The clear streak holds acked frames, not only still windows: its step up
                 // is a probe
    bool silent; // The last window was congested only by an overdue ack
    uint16_t since_up; // Windows since the step up being judged, UINT16_MAX when none is
    uint8_t base_cur;
    uint8_t base_age;
    uint16_t base_min[MIRROR_QC_BASE_BUCKETS]; // Fastest window per bucket, UINT16_MAX if empty
    uint16_t base_next[MIRROR_QC_BASE_BUCKETS]; // Second fastest, UINT16_MAX until there is one

    // The last window's figures, for the change log
    uint16_t rtt_ms; // Median RTT, or the pending age when no ack came back
    uint16_t base_ms; // 0 until a sample arrives, at most MIRROR_QC_BASE_MAX_MS
    uint16_t queue_ms;
    const char *why; // Set only by the window that changed the level
} mirror_quality_ctl_t;

/**
 * @brief Forget every stamp, after a resync or a viewer change
 */
void mirror_rtt_reset(mirror_rtt_t *r);

/**
 * @brief Stamp a sequence number whose acked message has just gone to the socket
 */
void mirror_rtt_sent(mirror_rtt_t *r, uint16_t seq, uint32_t now_us);

/**
 * @brief A forward ack arrived. Pairs it with its stamp, and drops the stamps it passed:
 *        acks arrive in order, so theirs will never come
 */
void mirror_rtt_acked(mirror_rtt_t *r, uint16_t seq, uint32_t now_us);

/**
 * @brief Take one completed round trip, clearing its slot
 *
 * @return false when none is waiting
 */
bool mirror_rtt_take(mirror_rtt_t *r, uint32_t *rtt_us);

/**
 * @brief Age of the oldest stamp after acked still waiting for its ack, 0 when none is
 */
uint32_t mirror_rtt_pending_us(const mirror_rtt_t *r, uint16_t acked, uint32_t now_us);

/**
 * @brief Add one round trip to the window
 */
void mirror_window_rtt(mirror_window_t *w, uint32_t rtt_us);

/**
 * @brief Start a session at a level
 */
void mirror_quality_init(mirror_quality_ctl_t *q, uint8_t level);

/**
 * @brief A viewer was approved or resumed. Its first window carries the keyframe
 *
 * @param [in] new_path A newly approved viewer: forget the baseline and the probe backoff.
 *                      A resume keeps both
 */
void mirror_quality_viewer(mirror_quality_ctl_t *q, bool new_path);

/**
 * @brief Clear windows in a row the next step up takes
 */
uint8_t mirror_quality_up_after(const mirror_quality_ctl_t *q);

/**
 * @brief Judge one closed window
 *
 * @param [in] pinned The viewer's pin, level + 1, or 0 for automatic
 *
 * @return The level to encode at; q->why says why when it changed
 */
uint8_t mirror_quality_window(mirror_quality_ctl_t *q, const mirror_window_t *w, uint8_t pinned);

#endif // MIRROR_QUALITY_H
