#include "polycast5_macros.h"

#include <inttypes.h>
#include <string.h>

#include "sdkconfig.h"

#if !defined(CONFIG_ESP_WIFI_CSI_ENABLED)
// The CSI APIs link unconditionally on this chip, so without this guard a build with the option
// off would compile, run, and report zero frames, which reads as broken silicon rather than a
// misconfiguration. sdkconfig shadows sdkconfig.defaults, so delete sdkconfig to regenerate it.
#error "CSI sensing needs CONFIG_ESP_WIFI_CSI_ENABLED=y. Delete sdkconfig and rebuild."
#endif

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"

#include "wifi_csi.h"
#include "wifi_csi_ruview.h"
#include "wifi_ping.h"
#include "wifi_task.h"
#include "wifi_utils.h"

#define TAG "WIFI_CSI"

#define CSI_TASK_STACK (1024 * 4)
#define CSI_DRAIN_TIMEOUT_MS 20  // Bounded wait so a lost notify costs latency, not data
#define CSI_STOP_TIMEOUT_MS 200  // How long teardown waits for csi_task to exit
#define CSI_STATUS_PERIOD_US 100000 // Publish status to the LCD at 10 Hz

// The driver documents the first four bytes of every CSI payload as invalid. They are excluded
// everywhere, including the uniqueness hash: a varying garbage word over a frozen payload would
// otherwise report 100% unique, inverting the verdict of the check that exists to catch exactly
// that (ESP-IDF issue 18493).
#define CSI_SKIP_BYTES 4

// Stage A: written by the CSI callback from the Wi-Fi RX path. Deliberately NOT in PSRAM, a cache
// miss inside that path stalls the driver and drops all Wi-Fi traffic, not just CSI.
static wifi_csi_record_t s_stage[WIFI_CSI_STAGE_DEPTH];
static volatile uint32_t s_stage_head = 0; // Producer, callback only
static volatile uint32_t s_stage_tail = 0; // Consumer, csi_task only

// Stage B: analysis window, only ever touched by csi_task
POLYCAST5_USE_PSRAM_BSS static wifi_csi_record_t s_ring[WIFI_CSI_RING_DEPTH];
static uint32_t s_ring_head = 0;

static wifi_csi_stats_t s_stats;
static volatile bool s_csi_active = false;
static volatile bool s_stop_req = false;
static TaskHandle_t volatile s_csi_task = NULL;
static wifi_csi_cmd_t s_cmd;

// Radio holds, tracked separately so a half-finished start is still fully unwound
static bool s_lowlat_held = false;
static bool s_promisc_held = false;
static bool s_cb_held = false;
static bool s_sound_held = false;
static bool s_ruview_held = false;

// Teardown is reachable from wifi_task, espnow_task and the esp_event handler. Same idiom as
// wifi_utils_relay_lowlatency(): a portMUX around the test-and-set of the guard flag.
static portMUX_TYPE s_csi_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_in_teardown = false;

// Defined below, but session_start needs to know whether it actually owned the unwind
static bool wifi_csi_teardown_inner(void);

// Chosen once per session by a short survey, everything after must match or it is a mismatch
#define CSI_SURVEY_FRAMES 64   // Under a second at any usable capture rate
#define CSI_SURVEY_SLOTS 4     // Distinct (len, format) pairs tracked while surveying
#define CSI_SURVEY_MIN_COUNT 4 // Below this a format is a one-off, not a stream worth locking to

static uint16_t s_lock_len = 0;
static uint8_t s_lock_fmt = 0;
static struct {
    uint16_t len;
    uint16_t count;
    uint8_t fmt;
} s_survey[CSI_SURVEY_SLOTS];
static uint16_t s_survey_n = 0;

/**
 * @brief Integer square root, no FPU on this chip and no esp-dsp dependency
 */
static uint32_t csi_isqrt32(uint32_t v)
{
    uint32_t rem = 0;
    uint32_t root = 0;

    for (int i = 0; i < 16; ++i) {
        root <<= 1;
        rem = (rem << 2) | (v >> 30);
        v <<= 2;

        if (root < rem) {
            rem -= root | 1;
            root += 2;
        }
    }

    return root >> 1;
}

/**
 * @brief Convert one record's I/Q pairs into per-subcarrier amplitude
 *
 * Pairs are stored imaginary part first, real part second. The first two complex values are the
 * hardware-invalid word and are zeroed. Returns the subcarrier count written.
 */
static uint16_t csi_amplitudes(const wifi_csi_record_t *rec, uint8_t *out, uint16_t out_cap)
{
    uint16_t n = rec->len / 2;
    uint16_t first = CSI_SKIP_BYTES / 2;

    if (n > out_cap) {
        n = out_cap;
    }
    if (first > n) {
        first = n;
    }

    for (uint16_t k = 0; k < first; ++k) {
        out[k] = 0;
    }

    for (uint16_t k = first; k < n; ++k) {
        int32_t im = rec->iq[k * 2];
        int32_t re = rec->iq[k * 2 + 1];
        out[k] = (uint8_t)csi_isqrt32((uint32_t)(im * im + re * re));
    }

    return n;
}

#ifdef POLYCAST5_DEBUG_CSI

#define CSI_VAL_WINDOW 256 // Frames per validation report
#define CSI_VAL_SEEN 512   // Two slots per windowed frame so the table can never saturate
#define CSI_VAL_SRCS 4     // Previous frame retained per transmitter
#define CSI_VAL_RSSI_TOL 3 // dB of RSSI drift tolerated before a MAD sample is discarded

// Only ever touched by csi_task, so PSRAM costs no internal RAM
POLYCAST5_USE_PSRAM_BSS static uint32_t s_val_seen[CSI_VAL_SEEN];
POLYCAST5_USE_PSRAM_BSS static struct {
    uint8_t mac[6];
    int8_t rssi;
    uint16_t n_sc;
    bool valid;
    uint8_t amp[WIFI_CSI_MAX_SC];
} s_val_prev[CSI_VAL_SRCS];

static uint8_t s_val_prev_rr = 0;
static uint16_t s_val_frames = 0;
static uint16_t s_val_unique = 0;
static uint32_t s_val_diff_sum = 0; // Total absolute amplitude difference over the window
static uint32_t s_val_diff_cnt = 0; // Subcarriers compared, so the mean is taken exactly once
static uint32_t s_val_pairs = 0;    // Same-transmitter frame pairs contributing to the mean
static bool s_val_saturated = false;
static int64_t s_val_t0 = 0;

/**
 * @brief FNV-1a over the usable I/Q, used to count how many frames are actually distinct
 *
 * ESP-IDF issue 18493 reported the C5 returning a frozen buffer on 5 GHz: roughly two unique
 * patterns across twelve thousand frames. Everything downstream is meaningless if that happens.
 */
static uint32_t csi_hash_iq(const int8_t *buf, uint16_t len)
{
    uint32_t h = 2166136261u;

    for (uint16_t i = 0; i < len; ++i) {
        h ^= (uint8_t)buf[i];
        h *= 16777619u;
    }

    // 0 is the empty-slot sentinel, so fold a legitimate zero hash onto a neighbouring value
    return h ? h : 1u;
}

static void csi_validate_reset(void)
{
    memset(s_val_seen, 0, sizeof(s_val_seen));
    memset(s_val_prev, 0, sizeof(s_val_prev));
    s_val_prev_rr = 0;
    s_val_frames = 0;
    s_val_unique = 0;
    s_val_diff_sum = 0;
    s_val_diff_cnt = 0;
    s_val_pairs = 0;
    s_val_saturated = false;
    s_val_t0 = esp_timer_get_time();
}

/**
 * @brief Accumulate validation stats and log one line per window
 */
static void csi_validate_tick(const wifi_csi_record_t *rec, const uint8_t *amp, uint16_t n_sc)
{
    // Uniqueness over the usable payload only
    uint16_t hash_len = rec->len > CSI_SKIP_BYTES ? rec->len - CSI_SKIP_BYTES : 0;
    uint32_t h = csi_hash_iq(rec->iq + CSI_SKIP_BYTES, hash_len);
    uint32_t slot = h % CSI_VAL_SEEN;
    bool seen = false;
    bool stored = false;

    for (uint32_t probe = 0; probe < CSI_VAL_SEEN; ++probe) {
        uint32_t i = (slot + probe) % CSI_VAL_SEEN;

        if (s_val_seen[i] == h) {
            seen = true;
            break;
        }
        if (s_val_seen[i] == 0) {
            s_val_seen[i] = h;
            stored = true;
            break;
        }
    }

    // A full table cannot distinguish "new" from "not stored", so say so rather than scoring it
    if (!seen && !stored) {
        s_val_saturated = true;
    } else if (!seen) {
        s_val_unique++;
    }

    // Frame-to-frame amplitude change, the decisive test that the data tracks the physical
    // channel. Only compare frames from the SAME transmitter: in promiscuous mode consecutive
    // frames come from different radios in different places, and that difference would otherwise
    // swamp any motion. Also require a stable RSSI, so an AGC step is not read as movement.
    int prev_idx = -1;

    for (int i = 0; i < CSI_VAL_SRCS; ++i) {
        if (s_val_prev[i].valid && memcmp(s_val_prev[i].mac, rec->mac, 6) == 0) {
            prev_idx = i;
            break;
        }
    }

    if (prev_idx >= 0 && s_val_prev[prev_idx].n_sc == n_sc) {
        int rssi_delta = (int)rec->rssi - (int)s_val_prev[prev_idx].rssi;

        if (rssi_delta < 0) {
            rssi_delta = -rssi_delta;
        }

        if (rssi_delta <= CSI_VAL_RSSI_TOL) {
            const uint8_t *prev = s_val_prev[prev_idx].amp;

            // Accumulate raw differences and the comparison count separately: dividing per frame
            // and again per window would truncate twice and quantise the metric to uselessness
            for (uint16_t k = CSI_SKIP_BYTES / 2; k < n_sc; ++k) {
                int32_t d = (int32_t)amp[k] - (int32_t)prev[k];
                s_val_diff_sum += (uint32_t)(d < 0 ? -d : d);
                s_val_diff_cnt++;
            }

            s_val_pairs++;
        }
    }

    // Retain this frame as the reference for its transmitter
    if (prev_idx < 0) {
        prev_idx = s_val_prev_rr;
        s_val_prev_rr = (uint8_t)((s_val_prev_rr + 1) % CSI_VAL_SRCS);
        memcpy(s_val_prev[prev_idx].mac, rec->mac, 6);
    }

    s_val_prev[prev_idx].valid = true;
    s_val_prev[prev_idx].rssi = rec->rssi;
    s_val_prev[prev_idx].n_sc = n_sc;
    memcpy(s_val_prev[prev_idx].amp, amp, n_sc);

    s_val_frames++;

    if (s_val_frames < CSI_VAL_WINDOW) {
        return;
    }

    int64_t dt_us = esp_timer_get_time() - s_val_t0;
    uint32_t rate_x10 = dt_us > 0 ? (uint32_t)((int64_t)s_val_frames * 10000000 / dt_us) : 0;
    uint32_t uniq_pct = (uint32_t)s_val_unique * 100 / s_val_frames;

    // Kept in hundredths: the whole quiet-versus-disturbed span lives in the first few counts
    uint32_t mad_x100 = s_val_diff_cnt ? (s_val_diff_sum * 100) / s_val_diff_cnt : 0;

    ESP_LOGI(TAG, "CSI[%u] len=%u sc=%u fmt=%u rate=%"PRIu32".%"PRIu32"Hz uniq=%u/%u(%"PRIu32"%%)%s "
            "mad=%"PRIu32".%02"PRIu32"(%"PRIu32"p) rssi=%d drop=%"PRIu32" inval=%"PRIu32
            " badlen=%"PRIu32" mism=%"PRIu32,
            s_val_frames, rec->len, n_sc, rec->bb_format, rate_x10 / 10, rate_x10 % 10,
            s_val_unique, s_val_frames, uniq_pct, s_val_saturated ? " SAT!" : "",
            mad_x100 / 100, mad_x100 % 100, s_val_pairs, rec->rssi,
            s_stats.dropped, s_stats.invalid, s_stats.badlen, s_stats.mismatch);

    csi_validate_reset();
}

#endif // POLYCAST5_DEBUG_CSI

/**
 * @brief CSI receive callback
 *
 * Runs in the Wi-Fi RX path, same constraint as wifi_sniffer_raw_cb: do not block here (ever), or
 * the driver can stall. Gate and copy only. data->buf is deallocated the moment this returns.
 */
static void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *data)
{
    (void)ctx;

    if (!data || !data->buf) {
        return;
    }

    // The driver tells us when the channel estimate behind this frame is not usable
    if (!data->rx_ctrl.rx_channel_estimate_info_vld) {
        s_stats.invalid++;
        return;
    }

    uint16_t len = data->len;

    if (len < WIFI_CSI_MIN_BYTES || len > WIFI_CSI_MAX_BYTES) {
        s_stats.badlen++;
        return;
    }

    uint32_t head = s_stage_head;

    // Full: drop rather than overwrite, the consumer may be mid-read
    if ((uint32_t)(head - s_stage_tail) >= WIFI_CSI_STAGE_DEPTH) {
        s_stats.dropped++;
        return;
    }

    wifi_csi_record_t *r = &s_stage[head & (WIFI_CSI_STAGE_DEPTH - 1)];

    memcpy(r->iq, data->buf, len);
    r->len = len;
    r->seq = s_stats.captured;
    r->timestamp_us = data->rx_ctrl.timestamp;
    r->rssi = data->rx_ctrl.rssi;
    r->noise_floor = data->rx_ctrl.noise_floor;
    r->channel = data->rx_ctrl.channel;
    r->second = data->rx_ctrl.second & 0x0F; // Driver only populates the low nibble
    r->bb_format = data->rx_ctrl.cur_bb_format;
    r->rate = data->rx_ctrl.rate;
    r->first_word_invalid = data->first_word_invalid ? 1 : 0;
    memcpy(r->mac, data->mac, sizeof(r->mac));

    s_stats.captured++;

    // Publish the payload before the index so the consumer never sees a half-written slot
    __sync_synchronize();
    s_stage_head = head + 1;

    // Runs in the Wi-Fi task, not an ISR, so this is the plain notify. It never blocks.
    TaskHandle_t consumer = s_csi_task;

    if (consumer) {
        xTaskNotifyGive(consumer);
    }
}

/**
 * @brief Apply the CSI acquisition configuration for the band actually in use
 */
static esp_err_t wifi_csi_apply_config(uint32_t freq_mhz)
{
    // The C5 is an HE / MAC v3 part, so wifi_csi_config_t is the wifi_csi_acquire_config_t
    // bitfield struct. The legacy lltf_en / htltf_en / manu_scale fields do not exist here.
    wifi_csi_config_t cfg = {
        .enable = 1,
        .acquire_csi_legacy = 1,       // L-LTF: legacy OFDM, beacons, ACKs
        .acquire_csi_force_lltf = 0,   // Prefer HT/HE-LTF when a frame carries both
        .acquire_csi_ht20 = 1,         // Primary source on 2.4 GHz
        .acquire_csi_ht40 = 0,         // Four times the data, and HT40 is rare in 2.4 GHz
        .acquire_csi_vht = (freq_mhz >= 5000) ? 1 : 0, // The only band-specific field
        .acquire_csi_su = 1,           // HE20 SU, any 802.11ax AP on either band
        .acquire_csi_mu = 0,
        .acquire_csi_dcm = 0,
        .acquire_csi_beamformed = 0,
        .acquire_csi_he_stbc_mode = ESP_CSI_ACQUIRE_STBC_HELTF1,
        // Undocumented on HE parts beyond "value 0-8". Left at the IDF default. Do NOT assume it
        // pins the amplitude scale: the validation harness gates on RSSI stability instead.
        .val_scale_cfg = 0,
        .dump_ack_en = 1,              // ACKs are a second sample per round trip once associated
        .lltf_bit_mode = 0,            // 12-bit I/Q, the C5 gains ~24 dB of range over 8-bit
    };

    return esp_wifi_set_csi_config(&cfg);
}

/**
 * @brief Survey the frame formats on air, then lock to the richest one actually present
 *
 * Frames of different PHY formats interleave on a real network and differencing across them is
 * meaningless, so exactly one geometry has to be chosen. Taking whichever frame happens to arrive
 * first is a coin flip: a legacy ACK carries 53 subcarriers where an HT20 data frame carries 117,
 * and on an associated link with dump_ack_en both are common. Survey briefly, then prefer the
 * largest payload that is genuinely recurring rather than a one-off.
 *
 * @return true once a geometry is locked and the record may be consumed
 */
static bool wifi_csi_lock_geometry(const wifi_csi_record_t *rec)
{
    if (s_lock_len) {
        return true;
    }

    // Tally this (len, format) pair
    int slot = -1;

    for (int i = 0; i < CSI_SURVEY_SLOTS; ++i) {
        if (s_survey[i].count && s_survey[i].len == rec->len && s_survey[i].fmt == rec->bb_format) {
            slot = i;
            break;
        }
        if (!s_survey[i].count && slot < 0) {
            slot = i;
        }
    }

    if (slot >= 0) {
        s_survey[slot].len = rec->len;
        s_survey[slot].fmt = rec->bb_format;
        s_survey[slot].count++;
    }

    if (++s_survey_n < CSI_SURVEY_FRAMES) {
        return false;
    }

    // Prefer the largest payload, but only among formats carrying a real share of the traffic.
    // Richness is worthless if locking to it starves the stream: on a beacon-dominated channel
    // HT frames can be well under 1%, and choosing them would trade 90 Hz for under 1 Hz.
    uint16_t min_count = s_survey_n / 4;

    if (min_count < CSI_SURVEY_MIN_COUNT) {
        min_count = CSI_SURVEY_MIN_COUNT;
    }

    int best = -1;

    for (int i = 0; i < CSI_SURVEY_SLOTS; ++i) {
        if (s_survey[i].count < min_count) {
            continue;
        }
        if (best < 0 || s_survey[i].len > s_survey[best].len) {
            best = i;
        }
    }

    // Nothing cleared the share threshold: fall back to the most common format
    if (best < 0) {
        for (int i = 0; i < CSI_SURVEY_SLOTS; ++i) {
            if (s_survey[i].count && (best < 0 || s_survey[i].count > s_survey[best].count)) {
                best = i;
            }
        }
    }

    // Nothing recurred: fall back to whatever this frame is rather than surveying forever
    if (best < 0) {
        s_lock_len = rec->len;
        s_lock_fmt = rec->bb_format;
    } else {
        s_lock_len = s_survey[best].len;
        s_lock_fmt = s_survey[best].fmt;
    }

    s_stats.n_subcarriers = s_lock_len / 2;
    s_stats.bb_format = s_lock_fmt;

#ifdef POLYCAST5_DEBUG_CSI
    ESP_LOGI(TAG, "CSI locked len=%u fmt=%u; saw %u/%u %u/%u %u/%u %u/%u (len/count)",
            s_lock_len, s_lock_fmt,
            s_survey[0].len, s_survey[0].count, s_survey[1].len, s_survey[1].count,
            s_survey[2].len, s_survey[2].count, s_survey[3].len, s_survey[3].count);
#endif

    return true;
}

/**
 * @brief Map this chip's PHY format onto the value RuView expects in header byte 18
 *
 * The host locks each node onto one (subcarrier count, ppdu_type) pair and silently discards
 * frames that disagree, so this has to be derived consistently rather than guessed per frame.
 * Sending cur_bb_format raw would be misread: the host's enum is a small bucketed set, so a
 * legacy frame reporting format 1 would arrive tagged as HE-SU.
 */
static uint8_t csi_ppdu_type(uint8_t bb_format)
{
    switch (bb_format) {
        case RX_BB_FORMAT_11B:
        case RX_BB_FORMAT_11G:
        case RX_BB_FORMAT_HT:
        case RX_BB_FORMAT_VHT:
        case RX_BB_FORMAT_VHT_MU:
            return 0; // Legacy and HT/VHT all share the host's "not HE" bucket
        case RX_BB_FORMAT_HE_SU:
        case RX_BB_FORMAT_HE_ERSU:
            return 1;
        case RX_BB_FORMAT_HE_MU:
            return 2;
        case RX_BB_FORMAT_HE_TB:
            return 3;
        default:
            return 0xFF; // Host's explicit "unknown"
    }
}

/**
 * @brief Drain one staged record into the analysis ring and hand it to the consumers
 */
static void wifi_csi_consume(const wifi_csi_record_t *rec)
{
    static uint8_t amp[WIFI_CSI_MAX_SC];

    if (!wifi_csi_lock_geometry(rec)) {
        return;
    }

    if (rec->len != s_lock_len || rec->bb_format != s_lock_fmt) {
        s_stats.mismatch++;
        return;
    }

    uint16_t n_sc = csi_amplitudes(rec, amp, WIFI_CSI_MAX_SC);

    s_stats.rssi = rec->rssi;

#ifdef POLYCAST5_DEBUG_CSI
    csi_validate_tick(rec, amp, n_sc);
#else
    (void)n_sc;
#endif

    if (s_cmd.consumers & WIFI_CSI_CONSUMER_RUVIEW) {
        wifi_csi_ruview_send(rec, csi_ppdu_type(rec->bb_format));
    }

    // The on-device detector arrives with the local DSP milestone
}

static void csi_task(void *pv)
{
    (void)pv;

    int64_t last_status_us = esp_timer_get_time();
    int64_t rate_t0 = last_status_us;
    uint32_t rate_frames = 0;

#ifdef POLYCAST5_DEBUG_CSI
    int64_t last_seen_us = last_status_us;
#endif

    while (!s_stop_req) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CSI_DRAIN_TIMEOUT_MS));

        // Drain everything the callback staged since we last looked
        while (s_stage_tail != s_stage_head) {
            wifi_csi_record_t *src = &s_stage[s_stage_tail & (WIFI_CSI_STAGE_DEPTH - 1)];
            wifi_csi_record_t *dst = &s_ring[s_ring_head & (WIFI_CSI_RING_DEPTH - 1)];

            memcpy(dst, src, sizeof(*dst));

            // Release the slot before doing any real work with it
            __sync_synchronize();
            s_stage_tail++;
            s_ring_head++;
            rate_frames++;

            wifi_csi_consume(dst);
        }

        int64_t now = esp_timer_get_time();

        // Measured capture rate, the honest number to show on screen
        if (now - rate_t0 >= 1000000) {
            s_stats.frames_per_sec = (uint16_t)((int64_t)rate_frames * 1000000 / (now - rate_t0));
            rate_frames = 0;
            rate_t0 = now;
        }

#ifdef POLYCAST5_DEBUG_CSI
        // Never go silent: no frames at all is a result, and it must not look like a hung log
        if (s_stats.captured) {
            last_seen_us = now;
        } else if (now - last_seen_us >= 3000000) {
            ESP_LOGW(TAG, "CSI no frames in 3s: inval=%"PRIu32" badlen=%"PRIu32" mism=%"PRIu32,
                    s_stats.invalid, s_stats.badlen, s_stats.mismatch);
            last_seen_us = now;
        }
#endif

        if (now - last_status_us >= CSI_STATUS_PERIOD_US && xWifiCsiStatusQueue) {
            wifi_csi_status_t st = {0};

            st.state = s_stats.frames_per_sec ? WIFI_CSI_STATE_QUIET : WIFI_CSI_STATE_CONNECTING;
            st.stats = s_stats;

            xQueueOverwrite(xWifiCsiStatusQueue, &st);
            last_status_us = now;
        }
    }

    s_csi_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Release every radio hold this module may have taken
 *
 * Idempotent and independent of whether a session ever became active, so a start that fails
 * halfway still unwinds completely.
 */
static void wifi_csi_release_radio(void)
{
    if (s_cb_held) {
        (void)esp_wifi_set_csi(false);
        (void)esp_wifi_set_csi_rx_cb(NULL, NULL);
        s_cb_held = false;
    }

    if (s_sound_held) {
        wifi_ping_sound_stop();
        s_sound_held = false;
    }

    // The UDP socket is deliberately NOT closed here. csi_task sends on it, and closing while that
    // task is still alive leaves a window between its "is the socket open" check and its sendto()
    // where the descriptor could be closed and the number reused by another socket. Callers close
    // it after the task has been joined; the failure path below closes it directly, because there
    // no task exists yet.

    if (s_promisc_held) {
        (void)esp_wifi_set_promiscuous(false);
        s_promisc_held = false;
    }

    if (s_lowlat_held) {
        wifi_utils_relay_lowlatency(false);
        s_lowlat_held = false;
    }
}

esp_err_t wifi_csi_session_start(const wifi_csi_cmd_t *cmd)
{
    // Declared up front so no error path jumps over an initialisation on its way to the label
    esp_err_t err;
    uint32_t freq_mhz = 2412;
    uint8_t primary = 0;
    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;

    if (!cmd) {
        return ESP_ERR_INVALID_ARG;
    }

    // A teardown already in flight elsewhere returns immediately rather than waiting, so building
    // a new session on top of it would race that unwind over the same hold flags and leak every
    // one of them. Refuse instead; the caller retries on its next pass.
    if (!wifi_csi_teardown_inner()) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "Teardown in progress elsewhere, refusing to start");
#endif
        return ESP_ERR_INVALID_STATE;
    }

    // Never run two consumers against the single-producer/single-consumer stage ring: a stranded
    // task would double-advance the tail past the head and wedge the drain loop
    if (s_csi_task) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "Previous csi_task still running, refusing to start");
#endif
        return ESP_ERR_INVALID_STATE;
    }

    s_cmd = *cmd;

    // The radio has one tuner: drop any live sniff or deauth before claiming it
    (void)esp_wifi_set_promiscuous(false);
    (void)esp_wifi_set_promiscuous_rx_cb(NULL);
    xEventGroupSetBits(xWifiEventGroup, WIFI_STOP_DEAUTH_BIT);

    memset(&s_stats, 0, sizeof(s_stats));
    s_stage_head = s_stage_tail = 0;
    s_ring_head = 0;
    s_lock_len = 0;
    s_lock_fmt = 0;
    s_survey_n = 0;
    memset(s_survey, 0, sizeof(s_survey));
    s_stop_req = false;

    // Portals can leave the driver in AP mode, and promiscuous capture needs STA
    (void)esp_wifi_set_mode(WIFI_MODE_STA);

    err = esp_wifi_start();

    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
#endif
        return err;
    }

    // Both sources capture in promiscuous mode. On the associated path this is what lets us see
    // the ping replies and ACKs the AP sends us, which is the same arrangement RuView's own
    // reference firmware uses.
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };

    if (s_cmd.source == WIFI_CSI_SRC_PROMISC) {
        // Passive capture needs no credentials, so this is the fastest path to proving the silicon
        err = esp_wifi_set_channel(s_cmd.channel, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGE(TAG, "esp_wifi_set_channel failed: %s", esp_err_to_name(err));
#endif
            goto fail;
        }
    } else {
        // Associated: the caller is responsible for having brought the link up, because only it
        // knows which network to join and can show progress while that happens
        if (!(xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT)) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGE(TAG, "Associated capture requested with no connection");
#endif
            err = ESP_ERR_INVALID_STATE;
            goto fail;
        }
    }

    (void)esp_wifi_set_promiscuous_filter(&filter);

    // Holds that are PRIVATE to this module and idempotent to release are claimed BEFORE they are
    // acquired: teardown runs from the esp_event handler and can preempt at any instruction, so
    // claiming afterwards would leave a window where teardown sees no owner and the hold outlives
    // the session. Releasing one of these when it was never taken is harmless.
    //
    // The refcounted low-latency hold below is the exception and must be claimed AFTER, see there.
    s_promisc_held = true;

    err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_set_promiscuous failed: %s", esp_err_to_name(err));
#endif
        goto fail;
    }

    // Ask the driver which channel we actually ended up on rather than assuming: the associated
    // path lands wherever the AP is, and the band decides whether VHT capture is wanted
    if (esp_wifi_get_channel(&primary, &secondary) == ESP_OK && primary) {
        uint32_t f = wifi_utils_channel_to_freq(primary);

        if (f) {
            freq_mhz = f;
        }
    }

    // CSI only exists while the radio is receiving, so modem sleep has to go, and a DFS clock
    // downscale mid-session would jitter the sampling. The relay helper already refcounts exactly
    // that pair (PS off + CPU pinned) and restores whatever mode was active before.
    // Claimed AFTER the acquire, unlike every other hold here. This one is a REFCOUNT shared with
    // arp_spoof and ndp_spoof, so releasing it when we never took it does not no-op: it decrements
    // their count and hands their pinned clock and power-save override back underneath them. If a
    // teardown lands between these two lines the flag is still false, release skips, and the hold
    // we just took stays owned. Same ordering arp_spoof uses for the identical refcount.
    wifi_utils_relay_lowlatency(true);
    s_lowlat_held = true;

    s_cb_held = true;

    err = esp_wifi_set_csi_rx_cb(wifi_csi_rx_cb, NULL);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_set_csi_rx_cb failed: %s", esp_err_to_name(err));
#endif
        goto fail;
    }

    err = wifi_csi_apply_config(freq_mhz);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_set_csi_config failed: %s", esp_err_to_name(err));
#endif
        goto fail;
    }

    err = esp_wifi_set_csi(true);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_set_csi failed: %s", esp_err_to_name(err));
#endif
        goto fail;
    }

    // Uplink before the sounder, so the first frames the sounding traffic produces have somewhere
    // to go rather than being counted as drops
    if (s_cmd.consumers & WIFI_CSI_CONSUMER_RUVIEW) {
        s_ruview_held = true;

        err = wifi_csi_ruview_start(s_cmd.host_ip, s_cmd.host_port, s_cmd.node_id);
        if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGE(TAG, "wifi_csi_ruview_start failed: %s", esp_err_to_name(err));
#endif
            goto fail;
        }
    }

    // Ambient traffic alone is slow and comes from wherever, so on an associated link drive our
    // own steady stream off one fixed transmitter
    if (s_cmd.source == WIFI_CSI_SRC_ASSOC_PING) {
        uint16_t interval = s_cmd.sound_interval_ms ? s_cmd.sound_interval_ms : 20;

        // Claimed first: esp_ping_new_session spawns a task and makes blocking lwIP calls, so this
        // is the widest preemption window in the whole function. An unclaimed session here is an
        // infinite ping that nothing ever stops.
        s_sound_held = true;

        if (wifi_ping_sound_start(interval) != ESP_OK) {
            // Not fatal: ambient traffic still yields CSI, just slower and less uniform
#ifdef POLYCAST5_DEBUG
            ESP_LOGW(TAG, "Sounding ping unavailable, falling back to ambient traffic");
#endif
        }
    }

#ifdef POLYCAST5_DEBUG_CSI
    csi_validate_reset();
#endif

    s_csi_active = true;

    if (xTaskCreate(csi_task, "csi_task", CSI_TASK_STACK, NULL,
            POLYCAST5_PRIORITY_MEDIUM, (TaskHandle_t *)&s_csi_task) != pdPASS) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "Failed to create csi_task");
#endif
        s_csi_task = NULL;
        s_csi_active = false;
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "CSI session started: src=%u ch=%u freq=%"PRIu32" consumers=0x%02x",
            s_cmd.source, primary, freq_mhz, s_cmd.consumers);
#endif

    return ESP_OK;

fail:
    wifi_csi_release_radio();

    // Safe to close directly here: csi_task is only created once every step above has succeeded,
    // so on this path nothing else can be holding the socket
    if (s_ruview_held) {
        wifi_csi_ruview_stop();
        s_ruview_held = false;
    }

    return err;
}

void wifi_csi_session_stop(void)
{
    wifi_csi_teardown();
}

bool wifi_csi_is_active(void)
{
    return s_csi_active;
}

/**
 * @brief Unwind a session
 *
 * @return true if this call owned the unwind, false if another context was already inside one and
 *         this call did nothing. A caller that is about to rebuild state must not proceed on false.
 */
static bool wifi_csi_teardown_inner(void)
{
    bool busy;

    // Reachable from wifi_task, espnow_task and the esp_event handler, and the wait below is a
    // preemption point, so the holds must not be released twice
    taskENTER_CRITICAL(&s_csi_mux);
    busy = s_in_teardown;
    s_in_teardown = true;
    taskEXIT_CRITICAL(&s_csi_mux);

    if (busy) {
        return false;
    }

    bool was_active = s_csi_active || s_csi_task;
    bool exited = false;

    // Stop the flow of new frames and give back every hold, whether or not a session ever became
    // active: a start that failed halfway still owns some of these
    wifi_csi_release_radio();

    if (s_csi_task) {
        s_stop_req = true;

        // Bounded wait: a starved consumer must not wedge whoever is taking the radio down
        for (int waited = 0; waited < CSI_STOP_TIMEOUT_MS; waited += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));

            if (!s_csi_task) {
                exited = true;
                break;
            }
        }

        // Leave the request standing if it was not honoured, so the task still exits when it is
        // next scheduled. Clearing it here would strand an immortal second consumer.
        if (exited) {
            s_stop_req = false;
        } else {
#ifdef POLYCAST5_DEBUG
            ESP_LOGE(TAG, "csi_task did not exit within %d ms", CSI_STOP_TIMEOUT_MS);
#endif
        }
    } else {
        s_stop_req = false;
        exited = true; // Nothing was running, so the socket has no user
    }

    // Only once csi_task is provably gone is it safe to close the socket it was sending on.
    // Closing it while that task still lives could free the descriptor between its open-check and
    // its sendto(), and a number reused by another socket in that gap would be handed CSI frames.
    // On the timeout path the hold is deliberately left standing: session_start refuses to start
    // while s_csi_task is non-NULL, and the next teardown reclaims the socket once the task exits.
    if (exited && s_ruview_held) {
        wifi_csi_ruview_stop();
        s_ruview_held = false;
    }

    s_csi_active = false;

    taskENTER_CRITICAL(&s_csi_mux);
    s_in_teardown = false;
    taskEXIT_CRITICAL(&s_csi_mux);

#ifdef POLYCAST5_DEBUG
    // Every session start calls teardown first, so stay quiet when there was nothing to stop
    if (was_active) {
        ESP_LOGI(TAG, "CSI session stopped: captured=%"PRIu32" dropped=%"PRIu32" invalid=%"PRIu32
                " badlen=%"PRIu32" mismatch=%"PRIu32,
                s_stats.captured, s_stats.dropped, s_stats.invalid, s_stats.badlen, s_stats.mismatch);
    }
#else
    (void)was_active;
#endif

    return true;
}

void wifi_csi_teardown(void)
{
    (void)wifi_csi_teardown_inner();
}
