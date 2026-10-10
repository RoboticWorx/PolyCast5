#include "ir_exp_task.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "nvs.h"

#include "TCA9535.h" // i2c_bus_handle
#include "gpio_task.h" // xI2CBusMutex
#include "polycast5_macros.h" // POLYCAST5_PRIORITY_MEDIUM, POLYCAST5_DEBUG

#define TAG "IR_EXP"

#define BUS_WAIT_MS         1000 // Bounded: a wedged bus must not hang this task forever
#define LOOP_TICK_MS         10  // One FreeRTOS tick at CONFIG_FREERTOS_HZ=100
#define HEARTBEAT_MS         500 // Publish even without a new frame, so the side panel stays live

// How long a sensor may go without a reading before its last one is withdrawn; each clock starts at that sensor's
// FIRST good reading. Several times the natural cadence: expiring early flickers, expiring late lies.
#define IMAGER_STALE_MS      2000
#define TOF_STALE_MS         2000
#define MEDICAL_STALE_MS     5000

// Longer where a sensor answered the probe but never produced a reading: ~205 ms to first data, ~1 s of spot-sensor pairing
#define FIRST_READING_MS     10000
#define MEDICAL_POLL_MS      100 // The spot sensor only produces a pair about once a second

#define IMAGER_STABILIZE_MS  180000 // Absolute accuracy needs this long after a cold wake
#define PARK_ACK_WAIT_MS     2000  // Bounded wait for the pre-sleep park acknowledgement
#define PARK_EXIT_WAIT_MS    1000  // After an unanswered park, how long to wait for the producer to tear down
#define PARK_EXIT_POLL_MS      20  // Two ticks at CONFIG_FREERTOS_HZ=100
#define CREATE_EXIT_WAIT_MS  2500  // Wait for a previous teardown: the three chained BUS_WAIT_MS takes in
                                   // session_stop() plus a half-seated module's per-transfer I2C timeouts
#define CREATE_EXIT_POLL_MS    20  // Two ticks at CONFIG_FREERTOS_HZ=100

#define SCL_HZ_FAST 400000
#define SCL_HZ_SLOW 100000

// Centre 2x2 of the 32x24 grid - averaging four pixels halves the crosshair noise
#define CENTRE_A ((MLX90642_ROWS / 2 - 1) * MLX90642_COLS + (MLX90642_COLS / 2 - 1))
#define CENTRE_B (CENTRE_A + 1)
#define CENTRE_C (CENTRE_A + MLX90642_COLS)
#define CENTRE_D (CENTRE_C + 1)

#define TOF_AVG_N       3   // The 100 ms timing budget does most of the smoothing now

// Drop the averaging history as soon as the reading really moves, or the display trails the target - which reads as the range running backwards
#define TOF_RESET_MM    40

// Re-run the rangefinder self-calibration once ambient has moved this far (1.3 mm/C)
#define TOF_RECAL_DELTA_C100 300

// Room temperature for the skin-to-core conversion. The spot sensor's die is the only usable thermometer, and only
// until the array (~92 mW) and rangefinder (~66 mW) warm the module: latch it early, then let it follow only downward
// unless it has been below the window this visit (a cold device warming up).
#define ROOM_LATCH_SAMPLES 3    // Median of the first three fresh samples, ~2.5 s after open
#define ROOM_CARRY_US      (10LL * 60 * 1000000) // A re-open inside this finds the module still warm: keep the old floor
#define ROOM_SELFHEAT_C100 0    // BENCH: latched die minus a reference thermometer beside a settled handset (~50 expected)
#define ROOM_DIE_MIN_C100  -4000 // Die readings outside -40..85 C are torn or post-POR reads, never a room
#define ROOM_DIE_MAX_C100  8500

// Body temperature from forehead skin. Only shown inside the distance the spot sensor's 50 deg view fills with forehead.
#define BODY_DIST_MIN_MM    10   // Omron's 1-3 cm guidance; also the rangefinder's trusted floor
#define BODY_DIST_MAX_MM    40   // Field-of-view loss is ~0.1 C of skin here, 0.15-0.35 C by 50 mm
#define BODY_TOF_FRESH_MS   500  // ~Two 200 ms ranging budgets; an older range cannot gate a skin sample
// Per-design skin offset onto the table's skin scale. 140 is from published MLX9061x forehead data at 2-3 cm (Sensors
// 2021, PMC8198193), ~1.3-1.5 C under the table's 34.75 C anchor - not this module. BENCH: refine against a forehead thermometer.
#define BODY_SKIN_TRIM_C100 140
#define BODY_SKIN_MIN_C100  3000 // Table floor; below it is usually aim, distance or hair
#define BODY_SKIN_MAX_C100  4000
#define BODY_SKIN_WIN       3    // Median of three fresh samples, restarted whenever the distance gate drops

QueueHandle_t xIrExpStatusQueue = NULL;
QueueHandle_t xIrExpCtrlQueue = NULL;
QueueHandle_t xIrExpDoneQueue = NULL;
QueueHandle_t xIrExpTofDetailQueue = NULL;

// Double-buffered frames in PSRAM, allocated on START and freed on STOP
static int16_t *s_frame[2] = { NULL, NULL };
static uint8_t s_write_idx = 0;

// Per-pixel exponential average: ~0.21 C RMS of noise at 8 Hz, and alpha = 0.5 is one add and one shift for ~42% of it
static int16_t *s_ema = NULL;
static bool s_ema_seeded = false;

static uint32_t s_seq = 0;

// Staged here rather than on the stack: 196 bytes on top of ST's ~3.4 kB histogram post-processing peak
static ir_exp_tof_detail_t s_detail;

// When the array was last woken; absolute accuracy needs up to 180 s of thermal settling after that
static TickType_t s_imager_wake_tick = 0;

// Last session's room floor, kept across page visits. esp_timer, not the tick: the tick stops in light sleep.
static int16_t s_room_floor_c100 = INT16_MIN;
static int64_t s_room_floor_us = 0;

const int16_t *ir_exp_get_frame(uint8_t idx)
{
    return (idx < 2) ? s_frame[idx] : NULL;
}

// Bring the array up, preferring 400 kHz: a frame read is ~35 ms there, ~139 ms at 100 kHz - past the 8 Hz frame period, so
// the slow path also slows the sensor. The rate is set BOTH ways: it lives in sensor EEPROM. Returns the clock, 0 if absent.
static uint32_t negotiate_imager(void)
{
    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) != pdTRUE) {
        return 0;
    }

    uint32_t scl = 0;
    const esp_err_t fast = mlx90642_init(SCL_HZ_FAST);
    if (fast == ESP_OK) {
        scl = SCL_HZ_FAST;

#ifdef POLYCAST5_DEBUG
        // Diagnostic only: does NOT demote the clock. init() verifies ~36 clock edges where every frame is ~13,800, so
        // the frame read is the transfer worth measuring - but a demotion writes the 100 kHz code into sensor EEPROM.
        if (s_frame[0] != NULL) {
            const esp_err_t probe = mlx90642_read_frame(s_frame[0], NULL);
            ESP_LOGI(TAG, "400 kHz frame probe: %s", esp_err_to_name(probe));
        }
#endif
    } else if (fast == ESP_ERR_NOT_SUPPORTED) {
        // A configuration refusal, not signal integrity. The retry below would read the same EEPROM word and fail
        // identically, and its warning would send someone after the pull-ups instead of the line init() just logged.
        mlx90642_deinit();
    } else {
        // Most likely signal integrity - the module's 10k pull-ups are weak for fast mode. Fall back rather than run unreliably.
        ESP_LOGW(TAG, "Imager did not verify at 400 kHz, falling back to 100 kHz");
        mlx90642_deinit();
        if (mlx90642_init(SCL_HZ_SLOW) == ESP_OK) {
            scl = SCL_HZ_SLOW;
        }
    }

    if (scl != 0) {
        // 2 Hz at 100 kHz, 8 Hz otherwise. A ~139 ms frame read is one indivisible transfer, so at 4 Hz it would
        // hold xI2CBusMutex 56% of the time on a bus gpio_task needs every 20 ms; 2 Hz is 28%, parity with fast.
        const uint8_t want = (scl >= SCL_HZ_FAST) ? MLX90642_REFRESH_8HZ : MLX90642_REFRESH_2HZ;
        const esp_err_t r = mlx90642_set_refresh(want);
        if (r != ESP_OK) {
            // Not fatal: the frame period follows whatever rate the sensor reports, so this costs frame rate, not correctness
            ESP_LOGW(TAG, "Could not set refresh code %u (%s); running at %u ms frames",
                    want, esp_err_to_name(r), mlx90642_frame_period_ms());
        }
        s_imager_wake_tick = xTaskGetTickCount();
    }

    xSemaphoreGive(xI2CBusMutex);
    return scl;
}

// Free the streaming buffers (the frames are only needed while the page is open)
static void free_buffers(void)
{
    heap_caps_free(s_frame[0]);
    heap_caps_free(s_frame[1]);
    heap_caps_free(s_ema);
    s_frame[0] = s_frame[1] = NULL;
    s_ema = NULL;
    s_ema_seeded = false;
}

static bool alloc_buffers(void)
{
    const size_t bytes = MLX90642_PIXELS * sizeof(int16_t);
    if (s_frame[0] == NULL) {
        s_frame[0] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_frame[1] == NULL) {
        s_frame[1] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_ema == NULL) {
        s_ema = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    if (s_frame[0] == NULL || s_frame[1] == NULL || s_ema == NULL) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM frame buffers");
        free_buffers();
        return false;
    }

    // heap_caps_malloc hands back whatever the last owner left, so anything reaching the page before the first good frame is black
    memset(s_frame[0], 0, bytes);
    memset(s_frame[1], 0, bytes);
    memset(s_ema, 0, bytes);

    s_ema_seeded = false;
    return true;
}

// Read, filter and measure one frame into the buffer that is not currently published.
// Returns false if the array had nothing usable yet. Caller must hold xI2CBusMutex.
static bool process_frame(ir_exp_status_t *st)
{
    const uint8_t widx = (uint8_t)(s_write_idx ^ 1u);
    int16_t *dst = s_frame[widx];
    int16_t die = 0;

    if (mlx90642_read_frame(dst, &die) != ESP_OK) {
        return false;
    }

    // Count the sentinels rather than bailing on the first: -273.16 C on EVERY pixel is warmup and must never reach the
    // auto-range, but a few are unresolved pixels, or ones permanently defective below firmware 1.18.0 - and dropping
    // those frames would freeze the image for the life of the session.
    int bad = 0;
    for (int i = 0; i < MLX90642_PIXELS; ++i) {
        if (dst[i] == MLX90642_INVALID_RAW) {
            ++bad;
        }
    }
    if (bad >= MLX90642_PIXELS / 4) {
        return false; // Warming up, or too little of the frame resolved to be worth showing
    }

    // Which crosshair pixels resolved THIS frame; after the substitution below they are indistinguishable from real readings
    uint8_t centre_ok = 0;
    if (dst[CENTRE_A] != MLX90642_INVALID_RAW) centre_ok |= 1u;
    if (dst[CENTRE_B] != MLX90642_INVALID_RAW) centre_ok |= 2u;
    if (dst[CENTRE_C] != MLX90642_INVALID_RAW) centre_ok |= 4u;
    if (dst[CENTRE_D] != MLX90642_INVALID_RAW) centre_ok |= 8u;

    // Substitute before anything else reads dst: the EMA, min/max and centre average all need a plausible number. The
    // stand-in is this frame's mean of the valid pixels - history would pin a never-resolved pixel to the first scene
    // for the session. The count test above leaves 3/4 valid, so the divisor is never zero.
    if (bad > 0) {
        int32_t sum = 0;
        for (int i = 0; i < MLX90642_PIXELS; ++i) {
            if (dst[i] != MLX90642_INVALID_RAW) {
                sum += dst[i];
            }
        }
        const int16_t fill = (int16_t)(sum / (MLX90642_PIXELS - bad));
        for (int i = 0; i < MLX90642_PIXELS; ++i) {
            if (dst[i] == MLX90642_INVALID_RAW) {
                dst[i] = fill;
            }
        }
    }

    // Seed on the first good frame so the average does not ramp up from zero
    if (!s_ema_seeded) {
        memcpy(s_ema, dst, MLX90642_PIXELS * sizeof(int16_t));
        s_ema_seeded = true;
    }

    int32_t mn = INT16_MAX;
    int32_t mx = INT16_MIN;
    for (int i = 0; i < MLX90642_PIXELS; ++i) {
        const int32_t f = ((int32_t)s_ema[i] + (int32_t)dst[i]) >> 1; // alpha = 0.5
        s_ema[i] = (int16_t)f;
        dst[i] = (int16_t)f;
        if (f < mn) mn = f;
        if (f > mx) mx = f;
    }

    st->buf_idx = widx;
    st->min_c50 = (int16_t)mn;
    st->max_c50 = (int16_t)mx;
    st->t_die_c100 = die;
    // Only the pixels that genuinely resolved: a substituted one would fold the scene mean into the number being pointed at
    {
        int32_t csum = 0;
        int cn = 0;
        if (centre_ok & 1u) { csum += dst[CENTRE_A]; ++cn; }
        if (centre_ok & 2u) { csum += dst[CENTRE_B]; ++cn; }
        if (centre_ok & 4u) { csum += dst[CENTRE_C]; ++cn; }
        if (centre_ok & 8u) { csum += dst[CENTRE_D]; ++cn; }
        if (cn == 0) {
            csum = (int32_t)dst[CENTRE_A] + dst[CENTRE_B] + dst[CENTRE_C] + dst[CENTRE_D];
            cn = 4;
        }
        st->center_c50 = (int16_t)(csum / cn);
    }

    s_write_idx = widx; // Published; the next frame goes to the other buffer
    return true;
}

static uint32_t s_scl_hz = 0;
// The task exists only while the page is open; NULL is the authoritative "no session" test ir_exp_task_park() branches on
static TaskHandle_t s_task = NULL;

static int32_t s_tof_hist[TOF_AVG_N];
static uint8_t s_tof_n = 0;
static uint8_t s_tof_head = 0;

static void tof_reset(void)
{
    s_tof_n = 0;
    s_tof_head = 0;
}

// Average the last few valid ranges, dropping the history on a large step so the reading snaps when the user re-aims
static int32_t tof_push(int32_t mm)
{
    if (s_tof_n > 0) {
        const int32_t last = s_tof_hist[(s_tof_head + TOF_AVG_N - 1) % TOF_AVG_N];
        int32_t delta = mm - last;
        if (delta < 0) delta = -delta;
        if (delta > TOF_RESET_MM) {
            tof_reset();
        }
    }

    s_tof_hist[s_tof_head] = mm;
    s_tof_head = (uint8_t)((s_tof_head + 1) % TOF_AVG_N);
    if (s_tof_n < TOF_AVG_N) {
        s_tof_n++;
    }

    int32_t sum = 0;
    for (uint8_t i = 0; i < s_tof_n; ++i) {
        sum += s_tof_hist[i];
    }
    return sum / s_tof_n;
}

// Oral minus forehead-centre skin, 0.01 C, from Microlife's clinical lookup (US 2005/0094705 A1 FIG. 6, digitised to
// ~0.05 C). Rows are skin 30..38 C; the patent draws 38-40 as one bundle, so the last row stands for all three.
// Columns are ambient 4..40 C in 2 C steps. Its ambient slope is ~-0.1 C/C, a quarter of Exergen's temporal-artery
// formula, which matters because the only ambient on this module is a die its neighbours warm.
#define CORE_SKIN_MIN_C100 3000
#define CORE_SKIN_ROWS     9
#define CORE_AMB_MIN_C100  400
#define CORE_AMB_STEP_C100 200
#define CORE_AMB_COLS      19

static const int16_t s_core_offset[CORE_SKIN_ROWS][CORE_AMB_COLS] = {
    { 676, 644, 616, 591, 568, 542, 523, 507, 494, 481, 466, 457, 449, 443, 438, 434, 432, 434, 440 }, // 30 C
    { 639, 605, 571, 538, 515, 490, 470, 453, 438, 424, 411, 399, 390, 382, 374, 365, 364, 364, 378 }, // 31 C
    { 594, 555, 524, 495, 466, 440, 417, 395, 378, 361, 349, 338, 327, 317, 312, 307, 300, 305, 316 }, // 32 C
    { 543, 512, 480, 448, 424, 398, 376, 354, 332, 314, 299, 283, 268, 257, 253, 247, 246, 250, 259 }, // 33 C
    { 496, 465, 438, 408, 381, 351, 325, 302, 280, 261, 243, 227, 213, 203, 197, 195, 194, 200, 205 }, // 34 C
    { 458, 432, 407, 376, 349, 317, 288, 260, 234, 213, 192, 173, 159, 150, 148, 145, 149, 154, 159 }, // 35 C
    { 432, 402, 375, 348, 317, 283, 256, 221, 196, 173, 144, 125, 109, 100,  96,  92,  97, 104, 114 }, // 36 C
    { 403, 387, 361, 335, 301, 275, 245, 221, 189, 160, 130,  98,  67,  56,  51,  50,  51,  55,  68 }, // 37 C
    { 385, 359, 335, 304, 278, 244, 215, 185, 155, 125, 102,  86,  55,  41,  28,  20,  17,  26,  28 }, // 38-40 C
};

// Skin plus the bilinearly interpolated offset. Skin above 38 C takes the 38-40 C row unchanged, as the patent draws
// it; the other clamps are for safety only - callers gate LO/HI/room first.
static int16_t core_from_skin(int16_t skin_c100, int16_t amb_c100)
{
    int32_t f = skin_c100 - CORE_SKIN_MIN_C100;
    if (f < 0) f = 0;
    if (f > (CORE_SKIN_ROWS - 1) * 100) f = (CORE_SKIN_ROWS - 1) * 100;
    int32_t a = amb_c100 - CORE_AMB_MIN_C100;
    if (a < 0) a = 0;
    if (a > (CORE_AMB_COLS - 1) * CORE_AMB_STEP_C100) a = (CORE_AMB_COLS - 1) * CORE_AMB_STEP_C100;

    int32_t row = f / 100;
    int32_t col = a / CORE_AMB_STEP_C100;
    if (row > CORE_SKIN_ROWS - 2) row = CORE_SKIN_ROWS - 2; // Top edge interpolates the last cell at weight 1
    if (col > CORE_AMB_COLS - 2) col = CORE_AMB_COLS - 2;
    const int32_t fr = f - row * 100;                // 0..100
    const int32_t fc = a - col * CORE_AMB_STEP_C100; // 0..200

    // One rounded division: two truncating passes bias every result up, as the offsets fall along both axes
    const int16_t *r0 = s_core_offset[row];
    const int16_t *r1 = s_core_offset[row + 1];
    const int32_t num = (r0[col] * (CORE_AMB_STEP_C100 - fc) + r0[col + 1] * fc) * (100 - fr)
                      + (r1[col] * (CORE_AMB_STEP_C100 - fc) + r1[col + 1] * fc) * fr;

    return (int16_t)(skin_c100 + (num + CORE_AMB_STEP_C100 * 50) / (CORE_AMB_STEP_C100 * 100));
}

static int32_t median3(int32_t a, int32_t b, int32_t c)
{
    if (a > b) { const int32_t t = a; a = b; b = t; }
    if (b > c) { b = c; }
    return (a > b) ? a : b;
}

// Per-session body estimate, fed one fresh spot-sensor sample at a time
typedef struct {
    int16_t room_win[ROOM_LATCH_SAMPLES]; // Most recent plausible die samples
    uint8_t room_n;         // Samples in room_win, up to ROOM_LATCH_SAMPLES
    uint8_t room_i;
    int16_t room_floor;     // Lower of the carry and every running median since the latch (the latest median once
                            // room_cold), 0.01 C, or INT16_MIN until latched
    bool room_cold;         // Fell below the window this visit: the device was acclimating, so from then on the floor
                            // tracks the running median both ways and is never carried
    int16_t room_carry;     // Last session's floor if still inside ROOM_CARRY_US, else INT16_MIN
    int32_t skin_win[BODY_SKIN_WIN];
    uint8_t skin_n;         // Samples in skin_win since the distance gate last held
    uint8_t skin_i;
} body_est_t;

static void body_reset(body_est_t *b)
{
    memset(b, 0, sizeof(*b));
    b->room_floor = INT16_MIN;
    b->room_carry = (s_room_floor_c100 != INT16_MIN &&
            esp_timer_get_time() - s_room_floor_us < ROOM_CARRY_US) ? s_room_floor_c100 : INT16_MIN;
}

// Fold in one fresh pair and set room_c100, body_c100 and body_state. have_range: the rangefinder has a current answer;
// dist_ok: every range since the last sample was inside the gate.
static void body_update(body_est_t *b, ir_exp_status_t *st, float t_obj, float t_amb, bool have_range, bool dist_ok)
{
    // Room: median of the first three samples, before the neighbours' heat arrives; after that only a fall in the
    // running median is believed, so one torn read cannot drag the floor down for this visit and the next. Once it has
    // been below the window it follows the median both ways: that rise is a cold device warming up, not self-heating.
    const float die = t_amb * 100.0f;
    if (die >= (float)ROOM_DIE_MIN_C100 && die <= (float)ROOM_DIE_MAX_C100) { // Also rejects NaN
        b->room_win[b->room_i] = (int16_t)lrintf(die);
        b->room_i = (uint8_t)((b->room_i + 1) % ROOM_LATCH_SAMPLES);
        if (b->room_n < ROOM_LATCH_SAMPLES) {
            b->room_n++;
        }
        if (b->room_n == ROOM_LATCH_SAMPLES) {
            const int16_t m = (int16_t)median3(b->room_win[0], b->room_win[1], b->room_win[2]);
            if (b->room_floor == INT16_MIN) {
                b->room_floor = (b->room_carry != INT16_MIN && b->room_carry < m) ? b->room_carry : m;
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Room latched at %d (median %d, carry %d), 0.01 C",
                        (int)b->room_floor, (int)m, (int)b->room_carry);
#endif
            } else if (b->room_cold || m < b->room_floor) {
                b->room_floor = m;
            }
        }
    }
    if (b->room_floor != INT16_MIN) {
        if ((int32_t)b->room_floor - ROOM_SELFHEAT_C100 < IR_EXP_ROOM_MIN_C100) {
            b->room_cold = true;
        }
        // A cold visit's floor is never carried: as the lower value it would win the next latch and flag that visit
        // cold too, so its floor would follow self-heating up
        s_room_floor_c100 = b->room_cold ? INT16_MIN : b->room_floor;
        s_room_floor_us = esp_timer_get_time(); // Refreshed every sample, so the carry window opens at session end
    }
    const int32_t room = (b->room_floor != INT16_MIN) ? (int32_t)b->room_floor - ROOM_SELFHEAT_C100 : INT16_MIN;
    st->room_c100 = (int16_t)room;

    // Skin: only samples taken inside the distance gate count, so a reading never mixes in one taken from too far
    if (!have_range) {
        b->skin_n = 0;
        st->body_state = IR_EXP_BODY_NONE; // No distance to gate on: say nothing rather than hint
        return;
    }
    if (!dist_ok) {
        b->skin_n = 0;
        st->body_state = IR_EXP_BODY_FAR;
        return;
    }
    if (!(t_obj > -100.0f && t_obj < 200.0f)) { // Also rejects NaN; past this lrintf saturates and the trim would overflow
        b->skin_n = 0;
        st->body_state = IR_EXP_BODY_NONE;
        return;
    }
    b->skin_win[b->skin_i] = (int32_t)lrintf(t_obj * 100.0f) + BODY_SKIN_TRIM_C100;
    b->skin_i = (uint8_t)((b->skin_i + 1) % BODY_SKIN_WIN);
    if (b->skin_n < BODY_SKIN_WIN) {
        b->skin_n++;
    }

    if (room == INT16_MIN || b->skin_n < BODY_SKIN_WIN) {
        st->body_state = IR_EXP_BODY_NONE;
        return;
    }
    const int32_t skin = median3(b->skin_win[0], b->skin_win[1], b->skin_win[2]);

    // Never clamp an input and still show a number
    if (room < IR_EXP_ROOM_MIN_C100 || room > IR_EXP_ROOM_MAX_C100) {
        st->body_state = IR_EXP_BODY_ROOM;
    } else if (skin < BODY_SKIN_MIN_C100) {
        st->body_state = IR_EXP_BODY_LO;
    } else if (skin > BODY_SKIN_MAX_C100) {
        st->body_state = IR_EXP_BODY_HI;
    } else {
        st->body_c100 = core_from_skin((int16_t)skin, (int16_t)room);
        st->body_state = IR_EXP_BODY_OK;
    }
}

static bool imager_park(void); // Defined below; session_stop() parks on the way out
void ir_exp_cold_park_all(void); // Also below; the retry path for a refused teardown

static void session_start(ir_exp_done_t *done)
{
    memset(done, 0, sizeof(*done));
    done->cmd = IR_EXP_CMD_START;

    // Every early return below can leave the array awake: mlx90642_init() sends the wake byte before it verifies anything
    if (!alloc_buffers()) {
        // Said explicitly rather than inferred from done->imager: this runs BEFORE the probe, so imager reads false on a cold entry
        done->no_mem = true;
        done->imager = mlx90642_is_present();
        return;
    }

    // Present with no recorded clock means a teardown gave up partway. mlx90642_init() short-circuits on an existing
    // handle, so without dropping it the reported speed is not the one in use and the companions get 0 Hz and refuse.
    if (mlx90642_is_present() && s_scl_hz == 0) {
        if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
            mlx90642_deinit();
            xSemaphoreGive(xI2CBusMutex);
        }
    }

    // A handle here means this session already probed the array, so it is at the negotiated clock already
    if (!mlx90642_is_present()) {
        s_scl_hz = negotiate_imager();
    }
    done->scl_hz = s_scl_hz;
    done->imager = mlx90642_is_present();

    if (!done->imager) { // No array means no page
        free_buffers();
        return;
    }

    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
        (void)mlx90642_wake(); // Harmless if it never slept
        done->medical = (mlx90632_init(s_scl_hz) == ESP_OK);
        done->tof = (vl53l4cx_init(s_scl_hz) == ESP_OK);
        xSemaphoreGive(xI2CBusMutex);
    }

    if (done->tof) {
        // Nothing to restore: the part runs on its own NVM crosstalk calibration.
        if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
            const esp_err_t r = vl53l4cx_start();
            xSemaphoreGive(xI2CBusMutex);
            if (r != ESP_OK) {
                ESP_LOGE(TAG, "Rangefinder failed to start: %s", esp_err_to_name(r));
            }
        }
    }

    tof_reset();

    // Drop the previous visit's descriptor: the status queue is length 1 and written with xQueueOverwrite, so with
    // s_seq restarting at 0 the page would accept a stale frame and follow its buf_idx into a fresh, unfilled buffer.
    if (xIrExpStatusQueue != NULL) {
        xQueueReset(xIrExpStatusQueue);
    }

    // Same for the detail snapshot, peeked with no sequence number to vet it: the TOF flag goes up as soon as the rangefinder answers
    if (xIrExpTofDetailQueue != NULL) {
        xQueueReset(xIrExpTofDetailQueue);
    }
    s_seq = 0;

    done->ok = true;

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Session up: imager=%d tof=%d medical=%d at %lu Hz",
            done->imager, done->tof, done->medical, (unsigned long)s_scl_hz);
#endif
}

// Stop the rangefinder and the spot sensor, then release their handles. Returns false if either could not be parked -
// busy bus or refusal - which the caller must retry through ir_exp_cold_park_all(); they may still draw ~21 mA.
static bool sensors_park(void)
{
    if (!vl53l4cx_is_present() && !mlx90632_is_present()) {
        return true; // Already down
    }

    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) != pdTRUE) {
        return false; // Nothing tried
    }

    // The handle is released even on a refusal: vl53l4cx_init() short-circuits on a live one, so a reseated module would skip DataInit
    bool refused = false;

    if (vl53l4cx_is_present()) {
        if (vl53l4cx_stop() != ESP_OK) {
            ESP_LOGW(TAG, "Rangefinder refused the stop; releasing it anyway");
            refused = true;
        }
        vl53l4cx_deinit();
    }
    if (mlx90632_is_present()) {
        if (mlx90632_set_continuous(false) != ESP_OK) {
            ESP_LOGW(TAG, "Spot sensor refused the stop; releasing it anyway");
            refused = true;
        }
        mlx90632_deinit();
    }

    xSemaphoreGive(xI2CBusMutex);
    return !refused;
}

static void session_stop(void)
{
    const bool sensors_down = sensors_park();

    free_buffers();

    // Parked here rather than on a timer: nothing is left running to service a deferred shutdown, and the tick stops in light sleep
    const bool imager_down = imager_park();

    // Anything refused, or untried on a busy bus, gets one more attempt through fresh handles, or the parts keep drawing up to ~49 mA all sleep
    if (!sensors_down || !imager_down) {
        ESP_LOGW(TAG, "Teardown incomplete (sensors=%d array=%d); retrying cold",
                (int)sensors_down, (int)imager_down);
        ir_exp_cold_park_all();
    }
}

// Drop the array to ~2 uA. Returns false if the bus was busy and nothing was done.
// Used by session teardown and by the synchronous pre-sleep park.
static bool imager_park(void)
{
    if (!mlx90642_is_present()) {
        s_scl_hz = 0;
        return true; // Already down
    }

    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) != pdTRUE) {
        return false;
    }

    // The result is reported, not discarded: a refused sleep leaves the array at ~28 mA, so the caller must retry cold
    const esp_err_t r = mlx90642_sleep();
    mlx90642_deinit();
    xSemaphoreGive(xI2CBusMutex);

    if (r != ESP_OK) {
        ESP_LOGE(TAG, "Array refused the sleep command: %s", esp_err_to_name(r));
    }

    // Cleared even on a refusal: the handle is gone either way, and the other two sensors refuse a 0 Hz clock.
    s_scl_hz = 0;
    return r == ESP_OK;
}

void ir_exp_cold_park_all(void)
{
    if (i2c_bus_handle == NULL) {
        return;
    }

    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Bus busy; could not quiesce the expansion module");
        return;
    }

    // 100 kHz: nothing here needs speed, and the negotiated rate is unknown on every path that reaches this
    const esp_err_t a = mlx90642_park_cold(SCL_HZ_SLOW);
    const esp_err_t m = mlx90632_park_cold(SCL_HZ_SLOW);

    // The rangefinder has nothing to park at a cold boot (SW standby, microamps); this is for a teardown it refused,
    // which drops the handle and leaves it ranging at 19-21 mA. The write NACKs harmlessly when idle or absent.
    const esp_err_t t = vl53l4cx_park_cold(SCL_HZ_SLOW);
    (void)a; // All three are only read by the debug log below
    (void)m;
    (void)t;

    xSemaphoreGive(xI2CBusMutex);

#ifdef POLYCAST5_DEBUG
    if (a == ESP_OK || m == ESP_OK || t == ESP_OK) {
        ESP_LOGI(TAG, "Cold-parked the expansion module (array=%s spot=%s range=%s)",
                esp_err_to_name(a), esp_err_to_name(m), esp_err_to_name(t));
    }
#endif
}

static void ir_exp_task(void *arg)
{
    (void)arg;

    // The queues exist before this task does, so a START queued right after ir_exp_task_create() cannot race this point
    bool running = false;
    bool teardown = false;
    ir_exp_status_t st;
    memset(&st, 0, sizeof(st));
    st.dist_mm = VL53L4CX_NO_TARGET;
    st.medical_c100 = IR_EXP_MEDICAL_NONE;
    st.room_c100 = INT16_MIN;
    body_est_t body;
    body_reset(&body);

    TickType_t next_frame = 0;
    TickType_t last_publish = 0;
    TickType_t next_medical = 0;
#ifdef POLYCAST5_DEBUG
    TickType_t dist_diag_next = 0;
#endif
    int16_t amb_c100 = INT16_MIN; // Latest ambient from the spot sensor
    int16_t recal_ref_c100 = INT16_MIN; // Ambient when the ToF last self-calibrated
    // Last good reading per sensor; 0 until one arrives. Nothing here has a presence pin and is_present() only tests the
    // handle, so a module pulled mid-session shows up only as reads that stop succeeding.
    TickType_t last_frame_tick = 0;
    TickType_t last_tof_tick = 0;
    TickType_t last_med_tick = 0;
    TickType_t session_tick = 0; // When the current session started, for the above
    // The BODY gate reads unaveraged ranges: the 3-deep display average lags a pull-back by up to ~25 mm
    int32_t body_raw_mm = VL53L4CX_NO_TARGET; // Latest range
    bool body_gate_broken = false;            // A range outside the gate since the last spot sample

    while (1) {
        // Between sessions this task exists only long enough to be told to stop, so the idle block is unbounded.
        const TickType_t wait = running ? pdMS_TO_TICKS(LOOP_TICK_MS) : portMAX_DELAY;

        ir_exp_cmd_t cmd;
        if (xQueueReceive(xIrExpCtrlQueue, &cmd, wait) == pdTRUE) {
            ir_exp_done_t done;
            memset(&done, 0, sizeof(done));
            done.cmd = cmd.type;

            switch (cmd.type) {
            case IR_EXP_CMD_START:
                if (!running) {
                    session_start(&done);
                    running = done.ok;
                    
                    if (running) {
                        next_frame = xTaskGetTickCount();
                        last_publish = next_frame;
                        amb_c100 = INT16_MIN;
                        recal_ref_c100 = INT16_MIN;
                        last_frame_tick = 0;
                        last_tof_tick = 0;
                        last_med_tick = 0;
                        session_tick = next_frame;
                        memset(&st, 0, sizeof(st));
                        st.dist_mm = VL53L4CX_NO_TARGET;
                        st.medical_c100 = IR_EXP_MEDICAL_NONE;
                        st.room_c100 = INT16_MIN;
                        body_reset(&body); // Picks up the last visit's room floor if it is recent enough
                        body_raw_mm = VL53L4CX_NO_TARGET;
                        body_gate_broken = false;
                        // WARMING starts SET, cleared only by the first good frame: IMAGER alone would claim a live image
                        st.flags = (uint8_t)(IR_EXP_FLAG_WARMING |
                                             (done.imager ? IR_EXP_FLAG_IMAGER : 0) |
                                             (done.tof ? IR_EXP_FLAG_TOF : 0) |
                                             (done.medical ? IR_EXP_FLAG_MEDICAL : 0));
                    }
                } else { // Already streaming; report the live state so the page never blocks
                    done.imager = mlx90642_is_present();
                    done.tof = vl53l4cx_is_present();
                    done.medical = mlx90632_is_present();
                    done.scl_hz = s_scl_hz;
                    done.ok = true;
                }
                xQueueSend(xIrExpDoneQueue, &done, 0);
                break;

            case IR_EXP_CMD_STOP:
                if (running) {
                    session_stop();
                    running = false;
                } else {
                    // A failed start can still leave the array awake: the wake byte goes out before verification
                    ir_exp_cold_park_all();
                }
                done.ok = true;

                // Retired BEFORE the ack: the page runs the instant it reads it, and a re-entry must not see a producer on its way out
                s_task = NULL;

                // Only acknowledged once every sensor is parked - the page blocks on this
                xQueueSend(xIrExpDoneQueue, &done, 0);
                teardown = true; // Leaving the page frees the 8 kB stack
                break;

            case IR_EXP_CMD_PARK:
                // About to sleep, which stops the tick a deferred teardown would be measured in: park the array now
                if (running) {
                    session_stop(); // Also parks the ToF and the spot sensor
                    running = false;
                } else {
                    ir_exp_cold_park_all(); // Same failed-start case as STOP above
                }

                // Nothing left to park: session_stop() retried what it could not, the no-session branch cold-parks.
                // A second park pair here hits the "already down" early returns and reports success for a refusal.
                done.ok = true;
                s_task = NULL; // Before the ack, for the reason given under STOP
                xQueueSend(xIrExpDoneQueue, &done, 0);
                teardown = true; // The page is gone; do not hold the stack through sleep
                break;

            default:
                break;
            }

            if (teardown) {
                break; // Frees itself below
            }
        }

        if (!running) {
            continue;
        }

        const TickType_t now = xTaskGetTickCount();

        // From the rate the sensor reports, not the bus clock: the two disagree when a rate change was refused
        const TickType_t frame_period = pdMS_TO_TICKS(mlx90642_frame_period_ms());
        bool published = false;

        /* =============== Thermal frame =============== */
        if ((int32_t)(now - next_frame) >= 0) {
            if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
                bool ready = false;
                if (mlx90642_frame_ready(&ready) == ESP_OK && ready) {
                    if (process_frame(&st)) {
                        st.flags &= (uint8_t)~IR_EXP_FLAG_WARMING;
                        st.flags |= IR_EXP_FLAG_IMAGER; // Re-arm if a fault withdrew it
                        last_frame_tick = now;
                        next_frame = now + frame_period;
                        published = true;
                    } else {
                        st.flags |= IR_EXP_FLAG_WARMING;
                    }
                }
                xSemaphoreGive(xI2CBusMutex);
            }
        }

        /* =============== Rangefinder =============== */
        if (vl53l4cx_is_present()) {
            if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
                int32_t mm = VL53L4CX_NO_TARGET;
                const esp_err_t r = vl53l4cx_read_mm(&mm);
                // Taken inside the same bus window, while the driver's result block still holds this frame. A copy
                // out of driver RAM, so it costs the bus nothing and no other task is inside ST's unlocked state.
                const bool have_detail = (r == ESP_OK)
                        && (vl53l4cx_get_detail(&s_detail) == ESP_OK);
                xSemaphoreGive(xI2CBusMutex);
                if (have_detail && xIrExpTofDetailQueue != NULL) {
                    xQueueOverwrite(xIrExpTofDetailQueue, &s_detail);
                }
                if (r == ESP_OK) {
                    // Answered, target or not - that is what makes the reading current.
                    last_tof_tick = now;
                    st.flags |= IR_EXP_FLAG_TOF; // Re-arm if a fault withdrew it
                    if (mm != VL53L4CX_NO_TARGET) {
                        st.dist_mm = tof_push(mm);
#ifdef POLYCAST5_DEBUG
                        // Sensor reading beside the averaged value the panel shows: a display fault reads apart from a sensor fault
                        if ((int32_t)(xTaskGetTickCount() - dist_diag_next) >= 0) {
                            dist_diag_next = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
                            ESP_LOGI(TAG, "range: sensor %d mm -> panel %d mm",
                                    (int)mm, (int)st.dist_mm);
                        }
#endif
                    } else { // Failed the validity gate: show nothing rather than a guess
                        tof_reset();
                        st.dist_mm = VL53L4CX_NO_TARGET;
                    }

                    // The next spot sample integrated across this range, so one out of the gate disqualifies it
                    body_raw_mm = mm;
                    if (mm < BODY_DIST_MIN_MM || mm > BODY_DIST_MAX_MM) { // NO_TARGET is negative
                        body_gate_broken = true;
                    }
                }
            }
        }

        /* =============== Medical spot temperature =============== */
        // Throttled: the part only publishes a new pair about once a second, so polling every loop tick is bus chatter
        if (mlx90632_is_present() && (int32_t)(now - next_medical) >= 0) {
            next_medical = now + pdMS_TO_TICKS(MEDICAL_POLL_MS);
            if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
                float t_obj = 0.0f;
                float t_amb = 0.0f;
                const esp_err_t r = mlx90632_read_object(&t_obj, &t_amb);
                xSemaphoreGive(xI2CBusMutex);
                if (r == ESP_OK) {
                    st.medical_c100 = (int16_t)(t_obj * 100.0f);
                    amb_c100 = (int16_t)(t_amb * 100.0f); // Live, for the ToF recalibration - not the room estimate
                    last_med_tick = now;
                    st.flags |= IR_EXP_FLAG_MEDICAL; // Re-arm if a fault withdrew it

                    // A range is held through a stalled ranger, so it must also be recent; with none, BODY says nothing
                    // rather than hinting at a distance it cannot see. Every range since the last sample must be in the gate.
                    const bool have_range = (st.flags & IR_EXP_FLAG_TOF) && last_tof_tick != 0 &&
                            (int32_t)(now - last_tof_tick) <= (int32_t)pdMS_TO_TICKS(BODY_TOF_FRESH_MS);
                    const bool dist_ok = have_range && !body_gate_broken &&
                            body_raw_mm >= BODY_DIST_MIN_MM && body_raw_mm <= BODY_DIST_MAX_MM;
                    body_gate_broken = false; // Consumed by this sample
                    body_update(&body, &st, t_obj, t_amb, have_range, dist_ok);
                }
            }
        }

        /* =============== Cancel range drift as ambient moves =============== */
        if (amb_c100 != INT16_MIN && vl53l4cx_is_present()) {
            if (recal_ref_c100 == INT16_MIN) {
                recal_ref_c100 = amb_c100;
            } else {
                int32_t d = (int32_t)amb_c100 - recal_ref_c100;
                if (d < 0) d = -d;
                if (d > TOF_RECAL_DELTA_C100) {
                    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
                        (void)vl53l4cx_recalibrate();
                        xSemaphoreGive(xI2CBusMutex);
                    }
                    recal_ref_c100 = amb_c100;
                    tof_reset();
                }
            }
        }

        /* =============== Withdraw readings whose sensor has gone quiet =============== */
        // Clearing the flag is what reaches the user: the page renders "--" and holds the last image rather than showing
        // it as live. Each sensor runs on its own clock once it has answered, on the session clock until then - a zero
        // clock never expires, so a module unplugged between visits would sit in WARMING UP forever.
        const bool imager_gone = (last_frame_tick != 0)
                ? (int32_t)(now - last_frame_tick) >= (int32_t)pdMS_TO_TICKS(IMAGER_STALE_MS)
                : (int32_t)(now - session_tick) >= (int32_t)pdMS_TO_TICKS(FIRST_READING_MS);
        const bool tof_gone = (last_tof_tick != 0)
                ? (int32_t)(now - last_tof_tick) >= (int32_t)pdMS_TO_TICKS(TOF_STALE_MS)
                : (int32_t)(now - session_tick) >= (int32_t)pdMS_TO_TICKS(FIRST_READING_MS);
        const bool med_gone = (last_med_tick != 0)
                ? (int32_t)(now - last_med_tick) >= (int32_t)pdMS_TO_TICKS(MEDICAL_STALE_MS)
                : (int32_t)(now - session_tick) >= (int32_t)pdMS_TO_TICKS(FIRST_READING_MS);

        // Gated on the flag still being up, which also makes each test idempotent once withdrawn.
        if ((st.flags & IR_EXP_FLAG_IMAGER) && imager_gone) {
            st.flags &= (uint8_t)~IR_EXP_FLAG_IMAGER;
        }
        if ((st.flags & IR_EXP_FLAG_TOF) && tof_gone) {
            st.flags &= (uint8_t)~IR_EXP_FLAG_TOF;
            st.dist_mm = VL53L4CX_NO_TARGET;
            tof_reset(); // The first range after a stall must not be averaged with ranges from before it
            body_raw_mm = VL53L4CX_NO_TARGET;
        }
        if ((st.flags & IR_EXP_FLAG_MEDICAL) && med_gone) {
            st.flags &= (uint8_t)~IR_EXP_FLAG_MEDICAL;
            st.medical_c100 = IR_EXP_MEDICAL_NONE;
            st.body_state = IR_EXP_BODY_NONE;
            body.skin_n = 0; // A resumed sensor must not be medianed against samples from before the gap
        }

        // FAULT tracks the ARRAY alone, as a pure function of its flag so it cannot latch on: a held image looks
        // exactly like a live one, while a quiet rangefinder or spot sensor already blanks its own value to "--".
        if ((st.flags & IR_EXP_FLAG_IMAGER) == 0) {
            st.flags |= IR_EXP_FLAG_FAULT;
        } else {
            st.flags &= (uint8_t)~IR_EXP_FLAG_FAULT;
        }

        /* =============== Publish =============== */
        if (s_imager_wake_tick != 0 &&
                (int32_t)(now - s_imager_wake_tick) < (int32_t)pdMS_TO_TICKS(IMAGER_STABILIZE_MS)) {
            st.flags |= IR_EXP_FLAG_STABILIZING;
        } else {
            st.flags &= (uint8_t)~IR_EXP_FLAG_STABILIZING;
        }

        // Every new frame, plus a slow heartbeat so range and spot still refresh while the array is warming up
        if (published || (int32_t)(now - last_publish) >= (int32_t)pdMS_TO_TICKS(HEARTBEAT_MS)) {
            st.seq = ++s_seq;
            xQueueOverwrite(xIrExpStatusQueue, &st);
            last_publish = now;
        }
    }

    // Reached only after an acknowledged STOP or PARK: sensors parked, bus mutex free, s_task already retired. The
    // queues stay alive so the page and the sleep path can always send; nothing below may touch state a re-entry owns.
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Session over, freeing the task (high water %u B)",
            (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    vTaskDelete(NULL);
}

// Called on every entry to the page, not once at boot: most units have no expansion module, and 8 kB is a lot to reserve
bool ir_exp_task_create(void)
{
    // Created here, not in the task, so a START queued the instant this returns cannot race the task's first statement.
    //
    // Each gated on its OWN handle. Gating all four on the status queue skipped the whole block on every later visit
    // once that one existed, so a queue that failed on the first visit could never be retried: the composite check
    // below kept failing and the page reported "did not respond" for the rest of the boot, with the queues that did
    // come up stranded.
    if (xIrExpStatusQueue == NULL) {
        xIrExpStatusQueue = xQueueCreate(1, sizeof(ir_exp_status_t));
    }
    if (xIrExpCtrlQueue == NULL) {
        xIrExpCtrlQueue = xQueueCreate(4, sizeof(ir_exp_cmd_t));
    }
    if (xIrExpDoneQueue == NULL) {
        xIrExpDoneQueue = xQueueCreate(2, sizeof(ir_exp_done_t));
    }
    // Separate from the status queue: 196 bytes against a couple of dozen, peeked into a stack local every frame
    if (xIrExpTofDetailQueue == NULL) {
        xIrExpTofDetailQueue = xQueueCreate(1, sizeof(ir_exp_tof_detail_t));
    }

    if (xIrExpStatusQueue == NULL || xIrExpCtrlQueue == NULL ||
            xIrExpDoneQueue == NULL || xIrExpTofDetailQueue == NULL) {
        ESP_LOGE(TAG, "Failed to create queues");
        return false;
    }

    // A task still up here is NOT one to reuse: every page exit stops the producer and waits for the ack, so this is
    // a teardown whose ack the page gave up on, and it will dequeue its STOP and delete itself, never the START.
    for (int i = 0; s_task != NULL && i < CREATE_EXIT_WAIT_MS / CREATE_EXIT_POLL_MS; ++i) {
        vTaskDelay(pdMS_TO_TICKS(CREATE_EXIT_POLL_MS));
    }
    if (s_task != NULL) {
        // Still there: the producer is wedged, likely on a bus a half-seated module is NACKing. Report it rather than queue a START nothing reads
        ESP_LOGE(TAG, "Previous session has not finished tearing down");
        return false;
    }

    // Both queues are emptied unconditionally before every spawn: a stale ack would satisfy this visit's START
    // handshake, and a leftover STOP would be the first thing the new task dequeues, tearing it back down.
    xQueueReset(xIrExpDoneQueue);
    xQueueReset(xIrExpCtrlQueue);

    // 8 kB: ST's driver runs histogram post-processing on THIS stack inside vl53l4cx_read_mm(), measured peak ~3.4 kB
    if (xTaskCreate(ir_exp_task, "ir_exp_task", 1024 * 8, NULL,
            POLYCAST5_PRIORITY_MEDIUM, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start ir_exp_task");
        s_task = NULL;
        return false;
    }

    return true;
}

bool ir_exp_task_is_running(void)
{
    return s_task != NULL;
}

// The park went unanswered. The caller is about to hold xI2CBusMutex for the whole sleep, so returning quietly would block
// the producer on its next take and leave the array measuring at ~28 mA all sleep. Likeliest cause is a bring-up inside
// ST's DataInit: give it a bounded chance to exit, then cold-park regardless.
static void park_timed_out(void)
{
    ESP_LOGE(TAG, "Expansion did not acknowledge the pre-sleep park");

    for (int i = 0; i < PARK_EXIT_WAIT_MS / PARK_EXIT_POLL_MS; ++i) {
        if (s_task == NULL) {
            break; // It finished and tore itself down; its own teardown parked the parts
        }
        vTaskDelay(pdMS_TO_TICKS(PARK_EXIT_POLL_MS));
    }

    if (s_task != NULL) {
        ESP_LOGE(TAG, "Producer still up; cold-parking the module from under it");
    }

    // Cheap and idempotent; a producer wedged holding the bus makes this give up after BUS_WAIT_MS and log
    ir_exp_cold_park_all();
}

void ir_exp_task_park(void)
{
    // No task means no session owns a handle, which is NOT the same as nothing being powered: a module merely plugged
    // in has both Melexis parts running from power-on, about 29 mA, with every is_present() reading false.
    if (s_task == NULL || xIrExpCtrlQueue == NULL || xIrExpDoneQueue == NULL) {
        ir_exp_cold_park_all();
        return;
    }

    // A live task with no handles is no proof either: a bring-up that failed partway leaves the array measuring at ~28 mA
    if (!mlx90642_is_present() && !vl53l4cx_is_present() && !mlx90632_is_present()) {
        ir_exp_cold_park_all();
        return;
    }

    ir_exp_cmd_t cmd = { .type = IR_EXP_CMD_PARK, .arg = 0 };
    if (xQueueSend(xIrExpCtrlQueue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        // A full command queue means a producer that is not draining it; the caller then sleeps holding the bus with the module measuring
        ESP_LOGE(TAG, "Could not queue the pre-sleep park; parking cold instead");
        park_timed_out();
        return;
    }

    // Wait for OUR acknowledgement: a late ack from a caller that gave up would let the bus mutex go mid-teardown
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PARK_ACK_WAIT_MS);
    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - deadline) >= 0) {
            park_timed_out();
            return;
        }

        ir_exp_done_t done;
        if (xQueueReceive(xIrExpDoneQueue, &done, deadline - now) != pdTRUE) {
            park_timed_out();
            return;
        }
        if (done.cmd == IR_EXP_CMD_PARK) {
            return;
        }
    }
}
