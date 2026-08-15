#include "claw_link.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "TCA9535.h" // i2c_bus_handle, I2C_MASTER_FREQ_HZ
#include "polycast5_macros.h" // POLYCAST5_DEBUG, POLYCAST5_USE_PSRAM_BSS
#include "gpio_task.h" // xI2CBusMutex

#define TAG "CLAW_LINK"

#define CLAW_LINK_BUS_WAIT_MS 1000 // How long to wait for the shared bus mutex
#define CLAW_LINK_TXN_TIMEOUT_MS 150 // Per-transaction I2C timeout
#define CLAW_LINK_PROBE_TIMEOUT_MS 50 // Address probe timeout
#define CLAW_LINK_CHUNK_GAP_MS 2 // Breathing room between chunks so gpio_task can poll

// Per-device handle on the shared bus
static i2c_master_dev_handle_t s_dev = NULL;

// Last command shipped to the expansion, kept so the UI can echo it back
POLYCAST5_USE_PSRAM_BSS static char s_last_command[CLAW_LINK_PAYLOAD_MAX + 1] = {0};

// Scratch for encoding credential payloads
POLYCAST5_USE_PSRAM_BSS static uint8_t s_payload_buf[CLAW_LINK_PAYLOAD_MAX];

// Register the expansion on the shared bus
static esp_err_t claw_link_ensure_dev(void)
{
    // Already registered by an earlier transaction
    if (s_dev != NULL) {
        return ESP_OK;
    }

    // The shared bus must already exist
    if (i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Same addressing and clock rate as every other device on this bus
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CLAW_LINK_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    // Registering doesn't touch the wire, so this succeeds with nothing plugged in
    esp_err_t err = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        s_dev = NULL; // Keep the "not registered" guard accurate
    }

    return err;
}

// One master -> slave write, bus locked only for the transaction itself
static esp_err_t claw_link_write(const uint8_t *buf, size_t len)
{
    // Register the expansion on first use
    esp_err_t err = claw_link_ensure_dev();
    if (err != ESP_OK) {
        return err;
    }

    // Wait for then lock I2C bus
    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(CLAW_LINK_BUS_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "claw_link_write: I2C bus busy - timed out");
        return ESP_ERR_TIMEOUT;
    }

    // On the wire: START, addr+W, payload, STOP
    err = i2c_master_transmit(s_dev, buf, len, CLAW_LINK_TXN_TIMEOUT_MS);
    xSemaphoreGive(xI2CBusMutex); // Release I2C bus

    return err;
}

// Request one page of the status frame and read it back.
// The bus is deliberately released across the turnaround delay: the expansion builds
// its reply from a task, not from the receive ISR, so it needs a scheduling window.
static esp_err_t claw_link_read_frame(uint8_t page, uint8_t *frame)
{
    // Select which page of the expansion's display text we want next
    uint8_t req[2] = { CLAW_LINK_OP_STATUS, page };

    esp_err_t err = claw_link_write(req, sizeof(req));
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(CLAW_LINK_TURNAROUND_MS)); // Let the expansion stage its reply

    // Re-take the bus for the read half; never held across the delay above
    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(CLAW_LINK_BUS_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "claw_link_read_frame: I2C bus busy - timed out");
        return ESP_ERR_TIMEOUT;
    }

    // Always exactly one frame. The expansion stretches SCL until it has one ready
    err = i2c_master_receive(s_dev, frame, CLAW_LINK_FRAME_LEN, CLAW_LINK_TXN_TIMEOUT_MS);
    xSemaphoreGive(xI2CBusMutex); // Release I2C bus

    if (err != ESP_OK) {
        return err;
    }

    // Reject anything that isn't a frame we understand rather than showing the user garbage
    // An absent expansion reads back as all 0xFF, which fails right here
    if (frame[CLAW_LINK_OFF_MAGIC] != CLAW_LINK_MAGIC) {
        ESP_LOGE(TAG, "Bad frame magic 0x%02X (expected 0x%02X)", frame[CLAW_LINK_OFF_MAGIC], CLAW_LINK_MAGIC);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Catches an expansion flashed with mismatched firmware
    if (frame[CLAW_LINK_OFF_VERSION] != CLAW_LINK_VERSION) {
        ESP_LOGE(TAG, "Unsupported protocol version %u (expected %u)",
                frame[CLAW_LINK_OFF_VERSION], CLAW_LINK_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }

    // A bogus length would run the callers' memcpy past the end of the frame
    if (frame[CLAW_LINK_OFF_TEXT_LEN] > CLAW_LINK_FRAME_TEXT_MAX) {
        ESP_LOGE(TAG, "Frame text length %u exceeds %u", frame[CLAW_LINK_OFF_TEXT_LEN], CLAW_LINK_FRAME_TEXT_MAX);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

// Ship one assembled payload as BEGIN / CHUNK... / END
static esp_err_t claw_link_send_payload(uint8_t kind, const uint8_t *data, size_t len)
{
    // Staged on the stack: the payload itself may live in PSRAM
    uint8_t buf[2 + CLAW_LINK_CHUNK_MAX];

    // Start a new payload of this kind. Also discards anything the expansion had
    // half-assembled from an interrupted earlier attempt
    buf[0] = CLAW_LINK_OP_BEGIN;
    buf[1] = kind;
    esp_err_t err = claw_link_write(buf, 2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BEGIN failed: %s", esp_err_to_name(err));
        return err;
    }

    // Body, CLAW_LINK_CHUNK_MAX bytes at a time
    for (size_t off = 0; off < len; off += CLAW_LINK_CHUNK_MAX) {
        // Final chunk is usually short
        size_t n = len - off;
        if (n > CLAW_LINK_CHUNK_MAX) {
            n = CLAW_LINK_CHUNK_MAX;
        }

        // [opcode][length][bytes...] so the receiver can frame it without guessing
        buf[0] = CLAW_LINK_OP_CHUNK;
        buf[1] = (uint8_t)n;
        memcpy(&buf[2], data + off, n);

        err = claw_link_write(buf, 2 + n);
        if (err != ESP_OK) {
            // Abandon the payload; the expansion drops the partial on the next BEGIN
            ESP_LOGE(TAG, "CHUNK at offset %u failed: %s", (unsigned)off, esp_err_to_name(err));
            return err;
        }

        vTaskDelay(pdMS_TO_TICKS(CLAW_LINK_CHUNK_GAP_MS)); // Don't monopolize the bus
    }

    // Hand it over for processing
    buf[0] = CLAW_LINK_OP_END;
    err = claw_link_write(buf, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "END failed: %s", esp_err_to_name(err));
    }

    return err;
}

// Append a length-prefixed string to a payload buffer. Strings longer than 255 bytes
// don't fit the single-byte prefix and are truncated.
static size_t claw_link_put_str(uint8_t *dst, size_t cap, size_t off, const char *s)
{
    // A NULL field encodes as zero-length rather than being skipped, so the
    // receiver's field order still lines up
    size_t len = (s != NULL) ? strlen(s) : 0;

    // Clamp to what the single-byte prefix can describe
    if (len > 255) {
        len = 255;
    }

    // Leave the payload unchanged if the field would overflow the buffer
    if (off + 1 + len > cap) {
        return off;
    }

    // Length prefix, then the bytes
    dst[off++] = (uint8_t)len;
    if (len > 0) {
        memcpy(dst + off, s, len);
        off += len;
    }

    return off;
}

esp_err_t claw_link_probe(void)
{
    // The shared bus must already exist
    if (i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Wait for then lock I2C bus
    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(CLAW_LINK_BUS_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "claw_link_probe: I2C bus busy - timed out");
        return ESP_ERR_TIMEOUT;
    }

    // Address-only transaction: ESP_OK means something ACKed at the expansion's address
    esp_err_t err = i2c_master_probe(i2c_bus_handle, CLAW_LINK_I2C_ADDR, CLAW_LINK_PROBE_TIMEOUT_MS);
    xSemaphoreGive(xI2CBusMutex); // Release I2C bus

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Probe 0x%02X: %s", CLAW_LINK_I2C_ADDR, esp_err_to_name(err));
#endif

    return err;
}

esp_err_t claw_link_send_wifi(const char *ssid, const char *password)
{
    // An open network has no password, but a nameless one is meaningless
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t off = 0;

    // Field order has to match what the expansion unpacks: [ssid][password]
    off = claw_link_put_str(s_payload_buf, sizeof(s_payload_buf), off, ssid);
    off = claw_link_put_str(s_payload_buf, sizeof(s_payload_buf), off, password);

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Sending Wi-Fi credentials for SSID '%s'", ssid);
#endif

    return claw_link_send_payload(CLAW_LINK_KIND_WIFI, s_payload_buf, off);
}

esp_err_t claw_link_send_llm(const char *api_key, const char *model, const char *base_url)
{
    // Without a key the expansion's agent can't think, so don't bother sending
    if (api_key == NULL || api_key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t off = 0;

    // Field order has to match what the expansion unpacks: [key][model][url]
    off = claw_link_put_str(s_payload_buf, sizeof(s_payload_buf), off, api_key);
    off = claw_link_put_str(s_payload_buf, sizeof(s_payload_buf), off, model);
    off = claw_link_put_str(s_payload_buf, sizeof(s_payload_buf), off, base_url);

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Sending LLM config: model='%s' url='%s'", model ? model : "", base_url ? base_url : "");
#endif

    return claw_link_send_payload(CLAW_LINK_KIND_LLM, s_payload_buf, off);
}

esp_err_t claw_link_send_command(const char *text)
{
    // Nothing worth handing over
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    // Keep a copy for the UI to echo, truncated to what the expansion accepts
    strlcpy(s_last_command, text, sizeof(s_last_command));

    // The copy above is also what gets transmitted, so measure it rather than the input
    size_t len = strlen(s_last_command);
    if (len > CLAW_LINK_PAYLOAD_MAX) {
        len = CLAW_LINK_PAYLOAD_MAX;
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Sending command (%u bytes): %s", (unsigned)len, s_last_command);
#endif

    // Raw UTF-8, no length prefix: the payload is the whole command
    return claw_link_send_payload(CLAW_LINK_KIND_COMMAND, (const uint8_t *)s_last_command, len);
}

esp_err_t claw_link_poll(claw_link_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Page 0 carries the status header plus the first slice of display text
    uint8_t frame[CLAW_LINK_FRAME_LEN] = {0};
    esp_err_t err = claw_link_read_frame(0, frame);
    if (err != ESP_OK) {
        return err;
    }

    // Only written on success, so a failed poll leaves the caller's last snapshot intact
    memset(out, 0, sizeof(*out));
    out->state = frame[CLAW_LINK_OFF_STATE];
    out->flags = frame[CLAW_LINK_OFF_FLAGS];
    out->seq = frame[CLAW_LINK_OFF_SEQ];
    out->total_len = (uint16_t)frame[CLAW_LINK_OFF_TOTAL_LO] |
            ((uint16_t)frame[CLAW_LINK_OFF_TOTAL_HI] << 8);

    // Bounded by claw_link_read_frame, so this can't overrun out->text
    uint8_t text_len = frame[CLAW_LINK_OFF_TEXT_LEN];
    memcpy(out->text, &frame[CLAW_LINK_OFF_TEXT], text_len);
    out->text[text_len] = '\0'; // The wire format carries no terminator

    return ESP_OK;
}

esp_err_t claw_link_read_result(char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out[0] = '\0'; // Valid (empty) result even if the first read fails

    uint8_t frame[CLAW_LINK_FRAME_LEN] = {0};
    size_t written = 0;

    // Page 0 also tells us how much text there is in total
    esp_err_t err = claw_link_read_frame(0, frame);
    if (err != ESP_OK) {
        return err;
    }

    // Clamp to the protocol cap so a corrupt length can't spin the loop below
    uint16_t total = (uint16_t)frame[CLAW_LINK_OFF_TOTAL_LO] |
            ((uint16_t)frame[CLAW_LINK_OFF_TOTAL_HI] << 8);
    if (total > CLAW_LINK_TEXT_MAX) {
        total = CLAW_LINK_TEXT_MAX;
    }

    // Walk the pages until the expansion's whole display text is assembled
    uint8_t page = 0;
    while (written < total) {
        // Page 0 is already in hand from the length probe above
        if (page > 0) {
            err = claw_link_read_frame(page, frame);
            if (err != ESP_OK) {
                break; // Keep whatever pages did arrive
            }
        }

        uint8_t text_len = frame[CLAW_LINK_OFF_TEXT_LEN];
        if (text_len == 0) {
            break; // Expansion ran out of text early
        }

        // Stop at the caller's buffer, leaving room for the terminator
        size_t room = out_sz - 1 - written;
        size_t n = (text_len < room) ? text_len : room;
        if (n == 0) {
            break;
        }

        memcpy(out + written, &frame[CLAW_LINK_OFF_TEXT], n);
        written += n;

        if (n < text_len) {
            break; // Caller's buffer is full
        }

        page++;
    }

    out[written] = '\0';

    // A mid-transfer failure still leaves usable text, so only surface the error
    // when nothing at all came back
    return (written > 0) ? ESP_OK : err;
}

esp_err_t claw_link_abort(void)
{
    // Single opcode, no arguments: drop whatever is in flight over there
    uint8_t op = CLAW_LINK_OP_ABORT;

    return claw_link_write(&op, 1);
}

const char *claw_link_last_command(void)
{
    // Always a valid string: zero-initialized and only ever strlcpy'd into
    return s_last_command;
}
