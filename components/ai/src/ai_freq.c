/**
 * Fundamental-frequency detector for the Tools > Frequency Meter page.
 *
 * Built for the Voron "sonic" belt-tensioning method -- the user plucks a belt, the T5848
 * mic hears it ring, and this publishes the fundamental in Hz -- and extended with wider
 * ranges for strings (see ai_freq_mode_t and s_mode_cfg below).
 *
 * Signal chain (all fixed-point -- the C5 has no FPU, so float only appears in the
 * one-time table build):
 *
 *   48kHz I2S (left slot, 24-bit) -> DC block -> CIC decimator /R -> decimated ring
 *     -> 1024-pt window -> block-float -> Hann
 *     -> int32 radix-2 FFT -> |X|^2 -> noise floor -> peak -> octave fix -> parabolic
 *
 * R and the analysed band come from the selected mode, trading bin resolution against how
 * high the band reaches. Hop and settle are derived in milliseconds, so every mode runs the
 * same ~5.9 analyses per second rather than the widest one costing 4x the CPU.
 *
 * Runs entirely on ai_task, which is the single owner of the I2S channel. Buffers are
 * allocated for the session and freed on exit; ai_task's stack is only 4KB, so nothing
 * large may live on it.
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "polycast5_macros.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "ai_voice.h"
#include "ai_freq.h"

#define TAG "AI_FREQ"

// Extra per-second serial dump while tuning the detector on real hardware
#define AI_FREQ_DEBUG 1

/* ------------------------------------------------------------------------------------ */
/* Parameters                                                                             */
/* ------------------------------------------------------------------------------------ */

#define AI_FREQ_FS_HZ  48000 // Native mic rate
#define AI_FREQ_CIC_N_MAX 6  // Largest CIC order any mode uses (sizes the state arrays)

#define AI_FREQ_FFT_N  1024 // Transform size, the same in every mode
#define AI_FREQ_RING_N 2048 // Power of two, > FFT_N

// Analysis is paced in time, not in samples, so every mode runs the same number of FFTs per
// second regardless of its decimated rate -- otherwise PIANO would do 4x the work of BELT
#define AI_FREQ_HOP_MS    170
#define AI_FREQ_SETTLE_MS 300 // ~3.5 DC-blocker time constants

/* One row per ai_freq_mode_t. The decimation factor sets the rate the FFT sees, so it fixes
 * both the bin width (dec_hz / 1024) and the top of the band (well under dec_hz / 2).
 *
 * k_lo/k_hi are bin indices into the transform. The alias figures below are the worst case,
 * at the top edge of the band, for the order-4 CIC:
 *
 *   mode    R   N   dec_hz   bin Hz   window   band            worst alias rejection
 *   BELT    32   4    1500    1.465    683ms    39.6-300.3 Hz   -50 dB
 *   GUITAR  16   4    3000    2.930    341ms    61.5-600.6 Hz   -50 dB
 *   PIANO    8   6    6000    5.859    171ms    82.0-1998.0 Hz  -51 dB
 *
 * PIANO carries a higher CIC order because its band top sits at a third of its decimated
 * rate rather than a fifth, so the fold-back band lands where an order-4 response is still
 * only ~-34 dB. Order 6 costs two more adds per input sample and ~3 dB of extra passband
 * droop at the very top of the band.
 *
 * KNOWN LIMIT, common to every mode: a LOUD tone above the band folds its harmonics back in
 * and is reported as a confident in-band reading (C7 2093 Hz reads as 1814 Hz in PIANO). No
 * filter order fixes this -- against a quiet noise floor even order 12 leaves the alias well
 * above the confidence gate -- it is what decimation does. The UI shows the active band so
 * the user can tell whether what they are measuring belongs in it.
 */
typedef struct {
    uint16_t cic_r;     // Decimation factor (power of two)
    uint8_t  cic_n;     // CIC order (<= AI_FREQ_CIC_N_MAX)
    uint8_t  cic_shift; // cic_n * log2(cic_r): undoes the CIC's (R*M)^N gain
    uint16_t k_lo;      // Lowest analysed bin
    uint16_t k_hi;      // Highest analysed bin (k_hi + 1 must stay < FFT_N/2)
    uint16_t dec_hz;    // Decimated sample rate
    const char *name;
} ai_freq_mode_cfg_t;

static const ai_freq_mode_cfg_t s_mode_cfg[AI_FREQ_MODE_COUNT] = {
    [AI_FREQ_MODE_BELT]   = { 32, 4, 20,  27, 205, 1500, "Belt"   },
    [AI_FREQ_MODE_GUITAR] = { 16, 4, 16,  21, 205, 3000, "Guitar" },
    [AI_FREQ_MODE_PIANO]  = {  8, 6, 18,  14, 341, 6000, "Piano"  },
};

// 480 stereo frames = exactly 10.0ms = exactly 15 decimated samples, so a read never
// straddles a partial decimated sample
#define AI_FREQ_FRAMES_PER_READ 480
#define AI_FREQ_READ_BYTES      (AI_FREQ_FRAMES_PER_READ * 2 * sizeof(int32_t)) // 3840
#define AI_FREQ_READ_TIMEOUT_MS 200
#define AI_FREQ_READ_FAIL_MAX   10 // ~2s of no data before giving up on the mic

// Consecutive dropped windows before the UI is told. Below this it just rebuilds quietly.
#define AI_FREQ_OVF_STREAK_WARN 3

// One-pole DC blocker: corner = fs / (2*pi*2^shift) ~= 1.9Hz, well below the 40Hz band
#define AI_FREQ_DC_SHIFT 12

// Discard while the mic wakes (T5848 emits valid data ~6ms after SCK starts)
#define AI_FREQ_WARMUP_READS 4 // 4 x 10ms

// Discard while the DC blocker settles (time constant 2^12/48000 = 85ms, so ~3.5 tau)
/* (settle time is derived per mode from AI_FREQ_SETTLE_MS) */

// Onset detection: a pluck must beat the tracked background AND an absolute floor
#define AI_FREQ_ONSET_MULT    8
#define AI_FREQ_ONSET_ABS_MIN 1000000ULL

// Above this background a realistic pluck can no longer clear ONSET_MULT * background,
// so say so instead of sitting there silently showing nothing. Worth recalibrating
// against a real machine: it is the level at which the tool stops being usable.
#define AI_FREQ_NOISY_BG (AI_FREQ_ONSET_ABS_MIN * 2048ULL)

// A pluck decays in 100-300ms, shorter than one window, so instead of guessing when to
// look we analyse every hop after onset and keep the most confident frame of the burst
#define AI_FREQ_BURST_HOPS 7 // ~1.2s

// Peak must clear the noise floor by this power ratio (64 = 18dB in amplitude)
#define AI_FREQ_CONF_MIN 64

// A belt plucked off-centre often rings louder on its 2nd harmonic, so a subharmonic
// candidate is promoted to fundamental -- but only if it is loud relative to the peak AND
// well clear of the noise floor. A relative-only test lets a noise bump at f/2 steal the
// answer and report 55Hz for a 110Hz belt.
// NOTE: both thresholds are POWER ratios, because they are compared against |X|^2
#define AI_FREQ_SUB_REL_SHIFT 4  // >= peak/16 power  = -12dB amplitude
#define AI_FREQ_SUB_ABS_MULT  16 // >= 16x floor power = +12dB amplitude

/* ------------------------------------------------------------------------------------ */
/* State                                                                                  */
/* ------------------------------------------------------------------------------------ */

/* Every buffer below is allocated when the page opens and freed when it closes, so a feature
 * that is used occasionally costs nothing the rest of the time. As statics they were ~18KB of
 * permanent .bss.
 *
 * Placement is per buffer rather than all-or-nothing. The C5 has a 32KB L1 cache shared with
 * everything else running from PSRAM -- LVGL's heap included -- so a buffer only survives in
 * cache if it is small and touched in a predictable order:
 *
 *   internal preferred: walked with a stride that doubles each FFT stage (s_re/s_im), hit on
 *                       every one of the 5120 butterflies (twiddles), or walked 48000x/s
 *                       (the I2S landing buffer)
 *   PSRAM:              streamed start to finish once per hop (ring, window) or touched only
 *                       a few hundred times (magnitudes)
 *
 * If internal RAM is short the internal-preferred ones fall back to PSRAM and the analysis
 * just gets slower; the overrun counter is what surfaces it.
 */
static int32_t  *s_i2s_words;  // 3840 B, internal preferred
static int32_t  *s_ring;       // 8192 B, PSRAM
static int32_t  *s_re;         // 4096 B, internal preferred
static int32_t  *s_im;         // 4096 B, internal preferred
static int16_t  *s_hann;       // 2048 B, PSRAM
static int16_t  *s_tw_re;      // 1024 B, internal preferred
static int16_t  *s_tw_im;      // 1024 B, internal preferred
static uint32_t *s_mag2;       // 2048 B, PSRAM

static bool s_tables_built = false;

// Active mode, unpacked into plain scalars so the per-sample path never dereferences the
// config table. Re-seeded by ai_freq_apply_mode().
static uint32_t s_cic_r;
static uint32_t s_cic_n;
static uint32_t s_cic_shift;
static int      s_k_lo;
static int      s_k_hi;
static uint32_t s_dec_hz;
static uint32_t s_hop_dec;
static uint32_t s_settle_dec;
static uint8_t  s_mode_active = AI_FREQ_MODE_BELT;

static int64_t s_dc_acc;

// Unsigned so the integrators wrap with DEFINED behaviour. A CIC is only exact because
// the comb stages undo the integrators modulo the register width; doing this in int64_t
// would be signed-overflow UB the moment a long session accumulated enough drift.
static uint64_t s_cic_int[AI_FREQ_CIC_N_MAX];
static uint64_t s_cic_comb[AI_FREQ_CIC_N_MAX];
static uint32_t s_cic_phase;

static uint32_t s_ring_wr;
static uint32_t s_ring_fill;
static uint32_t s_settle_left;

static uint64_t s_hop_sumsq;
static uint32_t s_hop_count;
static uint32_t s_dec_since_hop;

static int s_mag2_sh;

static uint32_t s_dbg_fft_us;

/* ------------------------------------------------------------------------------------ */
/* Buffer lifetime                                                                        */
/* ------------------------------------------------------------------------------------ */

// prefer_internal buffers fall back to PSRAM rather than failing: a slower analysis that
// reports overruns beats a page that refuses to open because internal RAM was fragmented.
static void *ai_freq_alloc(size_t bytes, bool prefer_internal)
{
    void *p = NULL;

    if (prefer_internal) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!p) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    return p;
}

static void ai_freq_free_buffers(void)
{
    free(s_i2s_words); s_i2s_words = NULL;
    free(s_ring);      s_ring      = NULL;
    free(s_re);        s_re        = NULL;
    free(s_im);        s_im        = NULL;
    free(s_hann);      s_hann      = NULL;
    free(s_tw_re);     s_tw_re     = NULL;
    free(s_tw_im);     s_tw_im     = NULL;
    free(s_mag2);      s_mag2      = NULL;

    // The tables live in the buffers just freed, so they must be rebuilt next session
    s_tables_built = false;
}

static esp_err_t ai_freq_alloc_buffers(void)
{
    s_i2s_words = ai_freq_alloc(AI_FREQ_READ_BYTES, true);
    s_re        = ai_freq_alloc(AI_FREQ_FFT_N * sizeof(int32_t), true);
    s_im        = ai_freq_alloc(AI_FREQ_FFT_N * sizeof(int32_t), true);
    s_tw_re     = ai_freq_alloc((AI_FREQ_FFT_N / 2) * sizeof(int16_t), true);
    s_tw_im     = ai_freq_alloc((AI_FREQ_FFT_N / 2) * sizeof(int16_t), true);

    s_ring      = ai_freq_alloc(AI_FREQ_RING_N * sizeof(int32_t), false);
    s_hann      = ai_freq_alloc(AI_FREQ_FFT_N * sizeof(int16_t), false);
    s_mag2      = ai_freq_alloc((AI_FREQ_FFT_N / 2) * sizeof(uint32_t), false);

    if (!s_i2s_words || !s_re || !s_im || !s_tw_re || !s_tw_im || !s_ring || !s_hann || !s_mag2) {
        ESP_LOGE(TAG, "ai_freq_alloc_buffers: out of memory");
        ai_freq_free_buffers();
        return ESP_ERR_NO_MEM;
    }

    // s_mag2 is read at k_lo-1 .. k_hi+1 before every bin in that span has been written on
    // the very first frame, and the ring is scanned before it is full
    memset(s_mag2, 0, (AI_FREQ_FFT_N / 2) * sizeof(uint32_t));
    memset(s_ring, 0, AI_FREQ_RING_N * sizeof(int32_t));

    return ESP_OK;
}

/* ------------------------------------------------------------------------------------ */
/* Mode configuration                                                                     */
/* ------------------------------------------------------------------------------------ */

static inline const ai_freq_mode_cfg_t *ai_freq_cfg(uint8_t mode)
{
    if (mode >= AI_FREQ_MODE_COUNT) {
        mode = AI_FREQ_MODE_BELT;
    }

    return &s_mode_cfg[mode];
}

const char *ai_freq_mode_name(uint8_t mode)
{
    return ai_freq_cfg(mode)->name;
}

uint16_t ai_freq_mode_lo_hz(uint8_t mode)
{
    const ai_freq_mode_cfg_t *c = ai_freq_cfg(mode);

    return (uint16_t)(((uint32_t)c->k_lo * c->dec_hz) / AI_FREQ_FFT_N);
}

uint16_t ai_freq_mode_hi_hz(uint8_t mode)
{
    const ai_freq_mode_cfg_t *c = ai_freq_cfg(mode);

    return (uint16_t)(((uint32_t)c->k_hi * c->dec_hz) / AI_FREQ_FFT_N);
}

// Unpacks a mode into the scalars the analysis path uses. The caller must re-seed the DSP
// afterwards: the decimator state and everything already in the ring belong to the old rate.
static void ai_freq_apply_mode(uint8_t mode)
{
    const ai_freq_mode_cfg_t *c = ai_freq_cfg(mode);

    s_cic_r      = c->cic_r;
    s_cic_n      = c->cic_n;
    s_cic_shift  = c->cic_shift;
    s_k_lo       = c->k_lo;
    s_k_hi       = c->k_hi;
    s_dec_hz     = c->dec_hz;
    s_hop_dec    = (c->dec_hz * AI_FREQ_HOP_MS) / 1000U;
    s_settle_dec = (c->dec_hz * AI_FREQ_SETTLE_MS) / 1000U;

    s_mode_active = (mode < AI_FREQ_MODE_COUNT) ? mode : AI_FREQ_MODE_BELT;
}

/* ------------------------------------------------------------------------------------ */
/* Fixed-point helpers                                                                    */
/* ------------------------------------------------------------------------------------ */

// Bit-by-bit integer square root, same shape as fp_isqrt() in lcd_anim_fluid.c but 64-bit
// (that one is static to its translation unit and a different width)
static uint32_t isqrt64(uint64_t v)
{
    uint64_t rem = 0;
    uint64_t root = 0;

    for (int i = 0; i < 32; i++) {
        root <<= 1;
        rem = (rem << 2) | (v >> 62);
        v <<= 2;

        if (root < rem) {
            rem -= ++root;
            ++root;
        }
    }

    return (uint32_t)(root >> 1);
}

// One-pole DC blocker at the full 48kHz rate. Shift-only: no multiply, no FPU.
// MUST run before the CIC -- the integrators are only well-behaved on a zero-mean signal.
static inline int32_t dc_block(int32_t x)
{
    s_dc_acc += (int64_t)x - (s_dc_acc >> AI_FREQ_DC_SHIFT);

    return x - (int32_t)(s_dc_acc >> AI_FREQ_DC_SHIFT);
}

// Pushes one 48kHz sample through the CIC; emits one decimated sample every CIC_R inputs
// Returns true when a decimated sample was produced
static inline bool cic_push(int32_t x)
{
    uint64_t acc = (uint64_t)(int64_t)x;

    // Integrator cascade, runs at 48kHz
    for (uint32_t i = 0; i < s_cic_n; i++) {
        s_cic_int[i] += acc;
        acc = s_cic_int[i];
    }

    if (++s_cic_phase < s_cic_r) {
        return false;
    }
    s_cic_phase = 0;

    // Comb cascade, runs at 1500Hz (differential delay M = 1)
    uint64_t v = acc;
    for (uint32_t i = 0; i < s_cic_n; i++) {
        uint64_t d = v - s_cic_comb[i];
        s_cic_comb[i] = v;
        v = d;
    }

    // The DC blocker can output up to ~2^24 and the largest (R*M)^N any mode uses is 2^20
    // (BELT) -- PIANO's order 6 over R=8 is 2^18 -- so the true CIC output stays under 2^44,
    // far inside int64, and reinterpreting the wrapped unsigned result as signed is exact
    int32_t out = (int32_t)(((int64_t)v) >> s_cic_shift);

    // Hold back the first samples so the DC blocker has settled before any of them can
    // reach an analysis window
    if (s_settle_left) {
        s_settle_left--;
        return true;
    }

    s_ring[s_ring_wr] = out;
    s_ring_wr = (s_ring_wr + 1) & (AI_FREQ_RING_N - 1);
    if (s_ring_fill < AI_FREQ_RING_N) {
        s_ring_fill++;
    }

    s_hop_sumsq += (uint64_t)((int64_t)out * out);
    s_hop_count++;

    return true;
}

/* ------------------------------------------------------------------------------------ */
/* Tables                                                                                 */
/* ------------------------------------------------------------------------------------ */

// Built once. Float is fine here: this is a cold path, matching the house style where
// lcd_anim_fluid.c uses sqrtf/powf at init and stays integer-only per frame.
static void ai_freq_build_tables(void)
{
    if (s_tables_built) {
        return;
    }

    for (int i = 0; i < AI_FREQ_FFT_N; i++) {
        float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(AI_FREQ_FFT_N - 1)));
        s_hann[i] = (int16_t)(w * 32767.0f + 0.5f);
    }

    for (int i = 0; i < AI_FREQ_FFT_N / 2; i++) {
        float a = -2.0f * (float)M_PI * (float)i / (float)AI_FREQ_FFT_N;
        s_tw_re[i] = (int16_t)(cosf(a) * 32767.0f);
        s_tw_im[i] = (int16_t)(sinf(a) * 32767.0f);
    }

    s_tables_built = true;
}

/* ------------------------------------------------------------------------------------ */
/* Transform                                                                              */
/* ------------------------------------------------------------------------------------ */

// Copies the newest FFT_N ring samples into s_re[], block-float normalised so the window
// peak lands near 2^29, then applies the Hann window. Normalising per frame is free
// accuracy: magnitudes are only ever compared within one frame.
static void ai_freq_load_window(void)
{
    uint32_t start = (s_ring_wr - AI_FREQ_FFT_N) & (AI_FREQ_RING_N - 1);

    uint32_t pk = 1;
    for (int i = 0; i < AI_FREQ_FFT_N; i++) {
        int32_t v = s_ring[(start + i) & (AI_FREQ_RING_N - 1)];
        uint32_t a = (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
        if (a > pk) {
            pk = a;
        }
    }

    // Leave 3 bits of headroom so the butterfly sums cannot overflow int32
    int sh = (int)__builtin_clz(pk) - 3;
    if (sh < 0) {
        sh = 0;
    }

    for (int i = 0; i < AI_FREQ_FFT_N; i++) {
        // Shift through unsigned: left-shifting a negative signed value is undefined
        int32_t v = (int32_t)((uint32_t)s_ring[(start + i) & (AI_FREQ_RING_N - 1)] << sh);
        s_re[i] = (int32_t)(((int64_t)v * s_hann[i]) >> 15);
        s_im[i] = 0;
    }
}

// In-place radix-2 DIT FFT. int32 data with Q15 twiddles: RV32 has no 16-bit SIMD, so
// Q15 data would buy no speed at all, only half the memory -- and it would cost ~6 bits
// of SNR that the peak-to-floor confidence metric actually needs.
static void ai_freq_fft(void)
{
    const int n = AI_FREQ_FFT_N;

    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;

        if (i < j) {
            int32_t t;
            t = s_re[i]; s_re[i] = s_re[j]; s_re[j] = t;
            t = s_im[i]; s_im[i] = s_im[j]; s_im[j] = t;
        }
    }

    // 10 stages, each scaled by 1/2, so total transform gain is exactly 1/n and overflow
    // is impossible no matter what the input looks like
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1;
        int step = n / len;

        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                int32_t wr = s_tw_re[k * step];
                int32_t wi = s_tw_im[k * step];

                int32_t ur = s_re[i + k];
                int32_t ui = s_im[i + k];
                int32_t xr = s_re[i + k + half];
                int32_t xi = s_im[i + k + half];

                int32_t vr = (int32_t)(((int64_t)xr * wr - (int64_t)xi * wi) >> 15);
                int32_t vi = (int32_t)(((int64_t)xr * wi + (int64_t)xi * wr) >> 15);

                s_re[i + k]        = (ur + vr) >> 1;
                s_im[i + k]        = (ui + vi) >> 1;
                s_re[i + k + half] = (ur - vr) >> 1;
                s_im[i + k + half] = (ui - vi) >> 1;
            }
        }
    }
}

// Scaled |X|^2 across the band (+/-1 bin of slack for interpolation and the octave check).
// Storing squared magnitude keeps this to zero square roots: it is monotonic in |X|, so
// argmax and the floor histogram behave identically.
static void ai_freq_mag2(void)
{
    uint32_t bound = 1;
    for (int k = s_k_lo - 1; k <= s_k_hi + 1; k++) {
        int32_t re = s_re[k];
        int32_t im = s_im[k];
        uint32_t a = (uint32_t)((re < 0 ? -(int64_t)re : (int64_t)re) +
                                (im < 0 ? -(int64_t)im : (int64_t)im));
        if (a > bound) {
            bound = a;
        }
    }

    // bound < 2^(32-lead), so bound^2 < 2^(64-2*lead); shift that down into 2^31
    int lead = (int)__builtin_clz(bound);
    s_mag2_sh = 33 - 2 * lead;
    if (s_mag2_sh < 0) {
        s_mag2_sh = 0;
    }

    for (int k = s_k_lo - 1; k <= s_k_hi + 1; k++) {
        uint64_t p = (uint64_t)((int64_t)s_re[k] * s_re[k] + (int64_t)s_im[k] * s_im[k]);
        s_mag2[k] = (uint32_t)(p >> s_mag2_sh);
    }
}

/* ------------------------------------------------------------------------------------ */
/* Peak picking                                                                           */
/* ------------------------------------------------------------------------------------ */

// Median of the in-band bins, quantised to a power of two, via a log2 histogram.
// No sort and no divide, and unlike a mean it is not dragged upward by the peak itself.
static uint32_t ai_freq_floor_est(void)
{
    uint16_t hist[32] = {0};
    int n = 0;

    for (int k = s_k_lo; k <= s_k_hi; k++) {
        uint32_t v = s_mag2[k];
        int b = (v == 0) ? 0 : (31 - (int)__builtin_clz(v));
        hist[b]++;
        n++;
    }

    int target = n / 2;
    int run = 0;
    for (int b = 0; b < 32; b++) {
        run += hist[b];
        if (run >= target) {
            return (b == 0) ? 1U : (1U << b);
        }
    }

    return 1U;
}

// The Voron instruction is "take the lowest peak". Check f/3 before f/2 so the lowest
// qualifying subharmonic wins.
static int ai_freq_octave_fix(int kmax, uint32_t m2max, uint32_t floor_p)
{
    for (int div = 3; div >= 2; div--) {
        int ks = (kmax + div / 2) / div;
        if (ks < s_k_lo + 1) {
            continue;
        }

        // Widen by a bin either way: bin quantisation plus real belt inharmonicity
        int kk = ks;
        for (int d = -1; d <= 1; d++) {
            if (s_mag2[ks + d] > s_mag2[kk]) {
                kk = ks + d;
            }
        }

        // floor_p is an exact power of two and can reach 2^30, so the 16x must be evaluated
        // in 64-bit: in 32-bit it wraps to exactly 0 and the guard inverts from "block" to
        // "admit everything" precisely on the noisiest frames it exists to reject
        bool loud_enough = s_mag2[kk] >= (m2max >> AI_FREQ_SUB_REL_SHIFT);
        bool above_floor = (uint64_t)s_mag2[kk] >= (uint64_t)floor_p * AI_FREQ_SUB_ABS_MULT;
        bool is_local_max = (s_mag2[kk] > s_mag2[kk - 1]) && (s_mag2[kk] > s_mag2[kk + 1]);

        if (loud_enough && above_floor && is_local_max) {
            return kk;
        }
    }

    return kmax;
}

// Sub-bin refinement. Parabolic fit on LINEAR magnitude, not |X|^2 -- fitting the squared
// values roughly doubles the interpolation bias. Only three square roots are needed, so
// the per-bin sqrt that would otherwise cost as much as the whole FFT is avoided.
// The fit is scale-invariant, so the mag2 scaling never has to be undone.
static uint32_t ai_freq_interp_mhz(int kh)
{
    int64_t a = (int64_t)isqrt64((uint64_t)s_mag2[kh - 1]);
    int64_t b = (int64_t)isqrt64((uint64_t)s_mag2[kh]);
    int64_t c = (int64_t)isqrt64((uint64_t)s_mag2[kh + 1]);

    int64_t num = a - c;
    int64_t den = a - 2 * b + c; // Negative at a real peak

    int32_t d_q16 = 0;
    if (den != 0) {
        // Multiply rather than shift: num is negative about half the time, and left-shifting
        // a negative signed value is not something to rely on
        d_q16 = (int32_t)((num * 32768) / den); // 0.5 * num/den, in Q16
        if (d_q16 > 32768) {
            d_q16 = 32768;
        }
        if (d_q16 < -32768) {
            d_q16 = -32768;
        }
    }

    // f = (k + delta) * dec_hz / FFT_N. kq maxes at ~2.2e7 and dec_hz at 6000, so the
    // product stays far inside int64.
    int64_t kq = ((int64_t)kh << 16) + d_q16;
    int64_t f_q16 = (kq * (int64_t)s_dec_hz) / AI_FREQ_FFT_N;

    return (uint32_t)((f_q16 * 1000) >> 16);
}

/* ------------------------------------------------------------------------------------ */
/* Display mapping                                                                        */
/* ------------------------------------------------------------------------------------ */

// Power ratio to a 0..100 bar, in half-log2 steps (~1.5dB amplitude each), full scale at
// 16 doublings (~48dB)
static uint8_t bar_height(uint32_t m, uint32_t floor_p)
{
    if (floor_p == 0) {
        floor_p = 1;
    }
    if (m <= floor_p) {
        return 0;
    }

    uint32_t ratio = m / floor_p;
    if (ratio < 2) {
        return 0;
    }

    int lg = 31 - (int)__builtin_clz(ratio);
    int q = (lg << 1) | (int)((ratio >> (lg - 1)) & 1U);

    int h = q * 100 / 32;
    if (h > 100) {
        h = 100;
    }

    return (uint8_t)h;
}

// Fills the spectrum strip and reports which column holds the fundamental
static void ai_freq_fill_bars(ai_freq_result_t *r, uint32_t floor_p, int kpeak)
{
    const int span = s_k_hi - s_k_lo + 1;

    r->peak_bar = AI_FREQ_BAR_NONE;

    for (int i = 0; i < AI_FREQ_BARS; i++) {
        int k0 = s_k_lo + (i * span) / AI_FREQ_BARS;
        int k1 = s_k_lo + ((i + 1) * span) / AI_FREQ_BARS;
        if (k1 <= k0) {
            k1 = k0 + 1;
        }

        uint32_t m = 0;
        for (int k = k0; k < k1 && k <= s_k_hi; k++) {
            if (s_mag2[k] > m) {
                m = s_mag2[k];
            }
        }

        r->bars[i] = bar_height(m, floor_p);

        if (kpeak >= k0 && kpeak < k1) {
            r->peak_bar = (uint8_t)i;
        }
    }
}

// Input level as a 0..100 log scale, so the user can tell whether they are close enough
static uint8_t level_from_rms(uint32_t rms)
{
    if (rms < 256) {
        return 0;
    }

    int lg = 31 - (int)__builtin_clz(rms);
    int q = (lg << 1) | (int)((rms >> (lg - 1)) & 1U);

    const int base = 8 << 1;  // ~ -90dBFS
    const int full = 23 << 1; // full scale

    int h = (q - base) * 100 / (full - base);
    if (h < 0) {
        h = 0;
    }
    if (h > 100) {
        h = 100;
    }

    return (uint8_t)h;
}

/* ------------------------------------------------------------------------------------ */
/* Capture loop                                                                           */
/* ------------------------------------------------------------------------------------ */

static void ai_freq_reset_dsp(void)
{
    s_dc_acc = 0;
    memset(s_cic_int, 0, sizeof(s_cic_int));
    memset(s_cic_comb, 0, sizeof(s_cic_comb));
    s_cic_phase = 0;

    s_ring_wr = 0;
    s_ring_fill = 0;
    s_settle_left = s_settle_dec;

    s_hop_sumsq = 0;
    s_hop_count = 0;
    s_dec_since_hop = 0;
}

static void ai_freq_publish(const ai_freq_result_t *r)
{
    if (xAiFreqResultQueue) {
        xQueueOverwrite(xAiFreqResultQueue, r);
    }
}

esp_err_t ai_freq_run(volatile bool *keep_running)
{
    // Validate args
    if (!keep_running) {
        return ESP_ERR_INVALID_ARG;
    }

    // Buffers live only for this session, so nothing is held while the page is closed
    esp_err_t aerr = ai_freq_alloc_buffers();
    if (aerr != ESP_OK) {
        ai_freq_result_t fail;
        memset(&fail, 0, sizeof(fail));
        fail.state = AI_FREQ_ST_ERROR;
        fail.peak_bar = AI_FREQ_BAR_NONE;
        fail.seq = 1;
        ai_freq_publish(&fail);
        return aerr;
    }

    ai_freq_apply_mode(ai_freq_mode);
    ai_freq_build_tables();

    // ai_task normally sits at MEDIUM, below lcd_task's HIGH, on a single core. That means it
    // only runs during lcd_task's 10ms vTaskDelay -- roughly 10ms of CPU per LVGL frame, which
    // is not enough to keep 48kHz drained while also running an FFT, so the DMA ring overruns
    // continuously. Matching lcd_task's priority makes the two time-slice instead. Restored on
    // every exit path below.
    UBaseType_t prev_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, POLYCAST5_PRIORITY_HIGH);

    ai_freq_result_t res;
    memset(&res, 0, sizeof(res));
    res.state = AI_FREQ_ST_WARMUP;
    res.peak_bar = AI_FREQ_BAR_NONE;
    res.mode = s_mode_active;

    // Must not be 0: the page tracks "something changed" by comparing seq against its own
    // stored value, which also starts at 0, so a seq-0 frame is indistinguishable from no
    // frame at all and gets dropped. This is the only frame carrying the active range before
    // the first completed hop, so dropping it left the band label showing the previous mode.
    res.seq = 1;
    ai_freq_publish(&res);

    // The deep ring matters: ai_task is lower priority than lcd_task on a single core, so
    // one long LVGL flush can stall this reader. Fall back if internal RAM is tight --
    // Wi-Fi/BLE may already have taken it.
    // Both entries give a DMA buffer of 480 * 8 = 3840 bytes, exactly one read request, so a
    // read always consumes whole buffers. A shallower ring (e.g. 4 x 200 -> 1600 bytes) is
    // deliberately NOT offered: i2s_channel_read would then abandon the unread tail of a
    // buffer without raising an overrun, silently splicing the stream in a way the continuity
    // check cannot see. Failing outright beats reporting a quietly wrong frequency.
    static const struct { int desc; int frames; } dma_cfgs[] = {
        { 8, 480 }, // 80ms - preferred
        { 4, 480 }, // 40ms - fallback when internal RAM is tight
    };

    // ai_voice_init_ex() returns early when the channel already exists, so a mic left up by
    // a just-finished dictation would silently keep the shallow STT ring. Drop it first so
    // the sizing below actually takes effect.
    ai_voice_deinit();

    esp_err_t err = ESP_ERR_NO_MEM;
    for (int i = 0; i < (int)(sizeof(dma_cfgs) / sizeof(dma_cfgs[0])) && err != ESP_OK; i++) {
        err = ai_voice_init_ex(dma_cfgs[i].desc, dma_cfgs[i].frames);
#ifdef POLYCAST5_DEBUG
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Freq meter DMA ring: %d x %d frames (%d ms)",
                    dma_cfgs[i].desc, dma_cfgs[i].frames,
                    (dma_cfgs[i].desc * dma_cfgs[i].frames * 1000) / AI_FREQ_FS_HZ);
        }
#endif
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_freq_run: mic init failed: %s", esp_err_to_name(err));

        // init_ex already released the pin holds before it failed, and ai_voice_deinit()
        // no-ops when the channel was never created, so re-park the clock pins here or the
        // T5848 keeps drawing current
        ai_voice_force_sleep_pins_low();

        res.state = AI_FREQ_ST_ERROR;
        res.seq++;
        ai_freq_publish(&res);

        ai_freq_free_buffers();
        vTaskPrioritySet(NULL, prev_prio);
        return err;
    }

    ai_freq_reset_dsp();

    uint32_t warmup_reads = AI_FREQ_WARMUP_READS;
    uint32_t read_fail_streak = 0;
    uint32_t ovf_streak = 0;
    uint32_t ovf_seen = ai_voice_get_ovf_count();

    int64_t bg = 0;          // Tracked background energy
    bool bg_primed = false;  // Seeded from the first hop, not from zero
    int burst_left = 0;      // Hops remaining in the current measurement burst
    uint32_t best_conf = 0;  // Best frame of the burst
    uint32_t best_mhz = 0;
    uint32_t held_mhz = 0;   // Last confident reading, latched for the UI

#if AI_FREQ_DEBUG
    int64_t last_log_us = esp_timer_get_time();
    uint32_t dbg_floor = 0;
    int dbg_kpeak = 0;
#endif

    while (*keep_running) {
        // Range changed from the UI. The decimator state and everything already in the ring
        // belong to the old rate, so re-seed rather than tearing the microphone down.
        if (ai_freq_mode != s_mode_active && ai_freq_mode < AI_FREQ_MODE_COUNT) {
            ai_freq_apply_mode(ai_freq_mode);
            ai_freq_reset_dsp();
            bg_primed = false;
            burst_left = 0;
            ovf_streak = 0;
            held_mhz = 0;

            res.mode = s_mode_active;
            res.f_mhz = 0;
            res.peak_bar = AI_FREQ_BAR_NONE;
            memset(res.bars, 0, sizeof(res.bars));
            res.state = AI_FREQ_ST_RESEED;
            res.seq++;
            ai_freq_publish(&res);
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Range -> %s (%u-%u Hz, %u Hz bins)", ai_freq_mode_name(s_mode_active),
                    (unsigned)ai_freq_mode_lo_hz(s_mode_active),
                    (unsigned)ai_freq_mode_hi_hz(s_mode_active),
                    (unsigned)(s_dec_hz / AI_FREQ_FFT_N));
#endif
        }

        size_t got = 0;
        err = ai_voice_read_raw(s_i2s_words, AI_FREQ_READ_BYTES, &got, AI_FREQ_READ_TIMEOUT_MS);

        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "ai_freq_run: read failed: %s", esp_err_to_name(err));
        }

        if (got == 0) {
            // No data at all. At 48kHz a 10ms request should always fill well inside the
            // 200ms timeout, so a run of these means the mic stopped clocking.
            if (++read_fail_streak >= AI_FREQ_READ_FAIL_MAX) {
                ESP_LOGE(TAG, "ai_freq_run: mic delivered no data, aborting");
                res.state = AI_FREQ_ST_ERROR;
                res.seq++;
                ai_freq_publish(&res);
                break;
            }
            continue;
        }
        read_fail_streak = 0;

        // Let the mic wake up (T5848 needs ~6ms of clock) before trusting any samples
        if (warmup_reads) {
            warmup_reads--;
            ai_freq_reset_dsp();
            continue;
        }

        // Should be impossible: the request size is frame-aligned and the driver copies in
        // frame-aligned chunks. Guarded anyway because the failure would be silent and
        // permanent -- one odd word desyncs the stereo phase and every later read would take
        // the mic's dead right slot instead of the left one.
        if ((got % (2U * sizeof(int32_t))) != 0U) {
            ESP_LOGE(TAG, "ai_freq_run: unaligned read (%u bytes)", (unsigned)got);
            ai_freq_reset_dsp();
            continue;
        }

        // A partial read (ESP_ERR_TIMEOUT with got > 0) is still valid, frame-aligned audio and
        // is processed normally. Dropping it would splice a gap into the decimator that the
        // overrun counter cannot see, leaving the next window quietly wrong instead of rejected.
        size_t frames = (got / sizeof(int32_t)) / 2;
        for (size_t f = 0; f < frames; f++) {
            // 24-bit payload sits in bits [31:8]; the T5848 drives the left slot
            int32_t s24 = s_i2s_words[f * 2 + 0] >> 8;
            if (cic_push(dc_block(s24))) {
                s_dec_since_hop++;
            }
        }

        if (s_dec_since_hop < s_hop_dec || s_ring_fill < AI_FREQ_FFT_N) {
            continue;
        }
        s_dec_since_hop = 0;

        // Hop energy and level
        uint64_t e = s_hop_count ? (s_hop_sumsq / s_hop_count) : 0;
        uint32_t rms = isqrt64(e);
        s_hop_sumsq = 0;
        s_hop_count = 0;

        res.level_pct = level_from_rms(rms);
        res.ovf_count = ai_voice_get_ovf_count();

        // A window that spans dropped samples is discontinuous; throw it away and rebuild
        if (res.ovf_count != ovf_seen) {
            ovf_seen = res.ovf_count;
            s_ring_fill = 0;
            burst_left = 0;
            ovf_streak++;

            // One dropped window is normal (a long LVGL flush is enough) and just costs a
            // rebuild, so stay on "listening" for it. Only call it out once it is persistent,
            // which is the case that actually stops the meter working. Logging is rate-limited
            // to the transition: an ESP_LOGW every hop blocks on the UART long enough to cause
            // the very overruns it is reporting.
            if (ovf_streak >= AI_FREQ_OVF_STREAK_WARN) {
                res.state = AI_FREQ_ST_OVERRUN;
                if (ovf_streak == AI_FREQ_OVF_STREAK_WARN) {
                    ESP_LOGW(TAG, "I2S overruns persisting (%u total): analysis cannot keep up",
                            (unsigned)res.ovf_count);
                }
            } else {
                res.state = AI_FREQ_ST_LISTENING;
            }

            res.seq++;
            ai_freq_publish(&res);
            continue;
        }
        ovf_streak = 0;

        // Analyse every hop, not just after an onset. That is what makes "best frame of
        // the burst" work: the first frame holds the broadband pluck click and simply
        // loses the confidence contest to a later frame of clean ringing. It also keeps
        // the spectrum strip live so the user can aim the device.
        int64_t t0 = esp_timer_get_time();
        ai_freq_load_window();
        ai_freq_fft();
        ai_freq_mag2();
        s_dbg_fft_us = (uint32_t)(esp_timer_get_time() - t0);

        uint32_t floor_p = ai_freq_floor_est();

        int kmax = s_k_lo;
        uint32_t m2max = 0;
        for (int k = s_k_lo; k <= s_k_hi; k++) {
            if (s_mag2[k] > m2max) {
                m2max = s_mag2[k];
                kmax = k;
            }
        }

        int kh = ai_freq_octave_fix(kmax, m2max, floor_p);
        uint32_t conf = (uint32_t)(s_mag2[kh] / (floor_p ? floor_p : 1));
        if (conf > 9999) {
            conf = 9999;
        }

        ai_freq_fill_bars(&res, floor_p, kh);
        res.conf = (uint16_t)conf;

#if AI_FREQ_DEBUG
        dbg_floor = floor_p;
        dbg_kpeak = kh;
#endif

        // Onset: beat the tracked background and an absolute floor
        bool onset = ((int64_t)e > bg * AI_FREQ_ONSET_MULT) && (e > AI_FREQ_ONSET_ABS_MIN);

        if (burst_left == 0) {
            // Seed the background from the first hop rather than from zero, or the very
            // first analysis always "onsets" and measures the room instead of a pluck
            if (!bg_primed) {
                bg = (int64_t)e;
                bg_primed = true;
                onset = false;
            }

            // Idle: keep tracking the room
            bg += ((int64_t)e - bg) >> 5;

            if (onset) {
                burst_left = AI_FREQ_BURST_HOPS;
                best_conf = 0;
                best_mhz = 0;
                res.state = AI_FREQ_ST_MEASURING;
            } else if ((uint64_t)bg > AI_FREQ_NOISY_BG) {
                // A steady in-band interferer (fans, idling steppers) raises the onset bar
                // out of reach. Tell the user rather than looking broken.
                res.state = AI_FREQ_ST_NOISY;
            } else {
                res.state = AI_FREQ_ST_LISTENING;
            }
        }

        if (burst_left > 0) {
            // Background is frozen during a burst so the pluck cannot raise its own bar
            if (conf > best_conf) {
                best_conf = conf;
                best_mhz = ai_freq_interp_mhz(kh);
            }

            burst_left--;
            res.state = AI_FREQ_ST_MEASURING;

            if (burst_left == 0) {
                if (best_conf >= AI_FREQ_CONF_MIN && best_mhz) {
                    held_mhz = best_mhz;
                    res.results++;
                    res.state = AI_FREQ_ST_RESULT;
                } else {
                    res.state = AI_FREQ_ST_LISTENING;
                }
            }
        }

        // Publish the latched value on EVERY hop, not just the RESULT one. The LCD polls
        // at 200ms while this publishes every 171ms, so a single-hop RESULT would
        // regularly be overwritten before the page ever saw it. res.results is what tells
        // the page a reading is new.
        res.f_mhz = held_mhz;
        res.mode = s_mode_active;
        res.seq++;
        ai_freq_publish(&res);

#if AI_FREQ_DEBUG
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_log_us >= 1000000) {
            last_log_us = now_us;
            ESP_LOGI(TAG,
                    "st=%u f=%lu.%03lu conf=%u floor=%lu k=%d lvl=%u ovf=%lu fft=%luus fill=%lu",
                    (unsigned)res.state,
                    (unsigned long)(held_mhz / 1000), (unsigned long)(held_mhz % 1000),
                    (unsigned)res.conf, (unsigned long)dbg_floor, dbg_kpeak,
                    (unsigned)res.level_pct, (unsigned long)res.ovf_count,
                    (unsigned long)s_dbg_fft_us, (unsigned long)s_ring_fill);
        }
#endif
    }

    // Single exit for every path that got the mic up, so it is always handed back
    esp_err_t derr = ai_voice_deinit();
    if (derr != ESP_OK) {
        ESP_LOGE(TAG, "ai_freq_run: ai_voice_deinit failed: %s", esp_err_to_name(derr));
    }

    // Give the ~26KB of working buffers back; the page is closing
    ai_freq_free_buffers();

    // Hand ai_task back to its normal priority so the AI/network paths behave as before
    vTaskPrioritySet(NULL, prev_prio);

    return ESP_OK;
}
