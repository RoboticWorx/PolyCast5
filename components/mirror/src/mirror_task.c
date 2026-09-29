#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "polycast5_macros.h"

#include "polycast5_gpios.h" // TCA9535_USER_BUTTON_*_PIN

#include "gpio_utils.h" // Remote button injection
#include "wifi_task.h" // xWifiEventGroup, WIFI_CONNECTED_BIT
#include "wifi_utils.h" // wifi_utils_relay_lowlatency

#include "mirror.h"
#include "mirror_priv.h"
#include "mirror_proto.h"
#include "mirror_quality.h"

#define TAG "MIRROR"

// Relay endpoint. Overridable from NVS so moving the relay does not need an OTA, the
// same escape hatch wifi_claude.c uses for its companion host
#define MIRROR_RELAY_URI_DEFAULT "wss://polycast5-mirror.polycast5.workers.dev/d"
#define MIRROR_NVS_NS "mirror"
#define MIRROR_NVS_KEY_URI "uri"

#define MIRROR_TASK_STACK 6144
// A send timeout aborts the whole websocket connection, so this must only fire on a
// genuinely dead link, not on a momentary stall: kept below network_timeout_ms (8000).
// The ack window does the pacing, not this
#define MIRROR_TX_TIMEOUT_MS 5000
#define MIRROR_IDLE_POLL_MS 20

// Frame pacing. A successful send only means the relay took it, so unacked frames are
// capped: without this the device streams blind and the relay buffers
#define MIRROR_MAX_INFLIGHT 2
#define MIRROR_MIN_FRAME_MS 50 // 20 fps ceiling
#define MIRROR_KEYFRAME_MS 60000 // Periodic resync; the viewer requests one on decode failure
#define MIRROR_ACK_TIMEOUT_MS 3000 // A viewer this quiet has lost an ack, not fallen behind
#define MIRROR_CONNECT_TIMEOUT_MS 45000 // Unreachable relay: say so instead of spinning

// Session bounds
#define MIRROR_WAIT_TIMEOUT_MS 300000 // No viewer within 5 min, give up
#define MIRROR_SESSION_MAX_MS 3600000 // One hour, then make them re-pair

_Static_assert(MIRROR_QC_WORST == MIRROR_Q_COUNT - 1, "The controller's ladder is mirror_quality_t");
_Static_assert(MIRROR_QC_INFLIGHT == MIRROR_MAX_INFLIGHT, "The controller trims one stall's acks");

char mirror_code[12] = { 0 };
char mirror_check[4] = { 0 };
char mirror_error[40] = { 0 };

static esp_websocket_client_handle_t s_client = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_run = false;
static volatile bool s_approved = false; // The attached viewer has been approved on the device
static volatile bool s_want_keyframe = false;
static volatile uint16_t s_last_acked = 0;
static volatile uint8_t s_pinned_level = 0; // 0 = automatic
static volatile uint8_t s_stop_reason = MIRROR_BYE_USER;
static volatile bool s_sleep_req = false;
static volatile bool s_text_state_stale = true; // Force a TEXTSTATE to a freshly attached viewer
static volatile bool s_status_stale = false; // Owe a MIRROR_MSG_STATUS after an attach or approval
static volatile bool s_lock_stale = false; // Owe the relay an "approved" for s_approved_check
static volatile bool s_relay_locked = false; // Relay confirmed only the approved viewer attaches
static volatile uint8_t s_viewer_gen = 0; // Bumped when a viewer is approved or resumes
static volatile bool s_viewer_new = false; // Set with an approval's bump: its path is unmeasured

// The approved viewer's check. Written only under s_viewer_mux, together with s_approved,
// mirror_check and the viewer states, so lcd_task never approves a half-applied attach
static char s_approved_check[sizeof(mirror_check)] = { 0 };
static portMUX_TYPE s_viewer_mux = portMUX_INITIALIZER_UNLOCKED;

static uint16_t s_seq = 0;
static mirror_quality_t s_level = MIRROR_Q_EXACT;

// Frame round trips. Stamped by mirror_task, answered by the websocket task's ack handler,
// which also moves s_last_acked under the same lock
static mirror_rtt_t s_rtt;
static portMUX_TYPE s_rtt_mux = portMUX_INITIALIZER_UNLOCKED;

static const mirror_text_sink_t *s_text = NULL;

POLYCAST5_USE_PSRAM_BSS static uint8_t s_txbuf[MIRROR_MSG_MAX_BYTES];

void mirror_set_text_sink(const mirror_text_sink_t *sink)
{
    s_text = sink;
}

bool mirror_has_viewer(void)
{
    return (mirror_state == MIRROR_LIVE && s_approved);
}

bool mirror_relay_locked(void)
{
    return s_relay_locked;
}

void mirror_viewer_approve(const char *check)
{
    // SELECT on the mirror page, from lcd_task. Only a pending viewer of a session that is
    // not tearing down can be approved, and only the one whose check the user was shown
    portENTER_CRITICAL(&s_viewer_mux);
    if (s_run && mirror_state == MIRROR_PENDING && check != NULL &&
            strcmp(check, mirror_check) == 0) {
        memcpy(s_approved_check, mirror_check, sizeof(s_approved_check));
        s_approved = true;
        s_lock_stale = true;        // Nothing streams until the relay confirms the lock
        s_want_keyframe = true;
        s_last_acked = s_seq;       // The viewer owes nothing for frames it never saw
        s_text_state_stale = true;  // It has been told nothing yet
        s_status_stale = true;      // Tell it frames are starting
        s_viewer_new = true;        // Its path is unmeasured
        s_viewer_gen++;
        mirror_state = MIRROR_LIVE;
    }
    portEXIT_CRITICAL(&s_viewer_mux);
}

// Void the approval. Caller holds s_viewer_mux
static void approval_clear(void)
{
    s_approved = false;
    s_lock_stale = false;
    s_relay_locked = false;
    s_approved_check[0] = '\0';
}

bool mirror_take_sleep_request(void)
{
    if (!s_sleep_req) {
        return false;
    }

    s_sleep_req = false;
    return true;
}

// Pull a quoted string value out of a small trusted-shape JSON object. The relay is ours
// and the messages are three fields, so this avoids pulling in a parser and any malloc
static bool json_str(const char *json, size_t len, const char *key, char *out, size_t out_size)
{
    char pat[24];
    const int pat_len = snprintf(pat, sizeof(pat), "\"%s\"", key);

    if (pat_len <= 0 || (size_t)pat_len >= sizeof(pat) || out_size == 0) {
        return false;
    }

    for (size_t i = 0; i + (size_t)pat_len < len; i++) {
        if (memcmp(&json[i], pat, (size_t)pat_len) != 0) {
            continue;
        }

        size_t p = i + (size_t)pat_len;

        while (p < len && (json[p] == ' ' || json[p] == ':')) {
            p++;
        }
        if (p >= len || json[p] != '"') {
            return false;
        }
        p++;

        size_t n = 0;
        while (p < len && json[p] != '"' && n + 1 < out_size) {
            out[n++] = json[p++];
        }
        out[n] = '\0';

        return (n > 0);
    }

    return false;
}

static void handle_relay_text(const char *data, size_t len)
{
    char type[16];

    // A stop is tearing the session down: an attach behind it must not revive it, nor a
    // relay bye relabel why it ended
    if (!s_run || !json_str(data, len, "t", type, sizeof(type))) {
        return;
    }

    if (strcmp(type, "ready") == 0) {
        if (json_str(data, len, "code", mirror_code, sizeof(mirror_code))) {
            mirror_error[0] = '\0';
            mirror_state = MIRROR_WAITING;

            ESP_LOGI(TAG, "Relay ready, waiting for a viewer");
        }
    } else if (strcmp(type, "viewer") == 0) {
        char st[16];

        if (json_str(data, len, "state", st, sizeof(st))) {
            if (strcmp(st, "attached") == 0) {
                // The relay's check code, shown on the device so the user confirms this
                // viewer before approving it
                char check[sizeof(mirror_check)];

                if (!json_str(data, len, "check", check, sizeof(check))) {
                    check[0] = '\0';
                }

                // Every attach is a new socket. A refreshed tab's attach can arrive with no
                // detach first, and the old one's holds and queued clicks must not carry over
                gpio_utils_remote_buttons_clear();

                portENTER_CRITICAL(&s_viewer_mux);
                // Once the relay has confirmed its lock, the approved check comes back only
                // with that viewer's resume token: resume streaming without asking again
                const bool resumed = (s_approved && s_relay_locked && check[0] != '\0' &&
                        strcmp(check, s_approved_check) == 0);

                if (resumed) {
                    s_want_keyframe = true;
                    s_last_acked = s_seq; // Owes nothing for frames it never saw
                    s_text_state_stale = true;
                    s_viewer_gen++; // Same viewer, so the path's baseline stands
                } else {
                    // Without a confirmed lock this is a new viewer, so a later repeat of
                    // the approved check must not ride on the old approval
                    approval_clear();
                }

                memcpy(mirror_check, check, sizeof(mirror_check));
                mirror_state = resumed ? MIRROR_LIVE : MIRROR_PENDING;
                s_status_stale = true; // Tell the viewer whether frames are coming
                portEXIT_CRITICAL(&s_viewer_mux);

                ESP_LOGI(TAG, "Viewer attached (%s)", resumed ? "approved" : "pending");
            } else {
                // A departed viewer's holds and queued clicks must not outlive it, nor merge
                // into the first press of a re-attached one
                gpio_utils_remote_buttons_clear();

                // The approval stays: if the relay locked, only that viewer can come back
                portENTER_CRITICAL(&s_viewer_mux);
                mirror_check[0] = '\0';
                mirror_state = MIRROR_WAITING;
                portEXIT_CRITICAL(&s_viewer_mux);

                ESP_LOGI(TAG, "Viewer detached");
            }
        }
    } else if (strcmp(type, "locked") == 0) {
        // The relay admits only the approved viewer's token from here on, so the slot can
        // no longer change hands and streaming may start
        char check[sizeof(mirror_check)];

        if (json_str(data, len, "check", check, sizeof(check))) {
            portENTER_CRITICAL(&s_viewer_mux);
            const bool mine = (s_approved && strcmp(check, s_approved_check) == 0);

            if (mine) {
                s_relay_locked = true;
            }
            portEXIT_CRITICAL(&s_viewer_mux);

            ESP_LOGI(TAG, "Relay lock %s", mine ? "confirmed" : "ignored");
        }
    } else if (strcmp(type, "bye") == 0) {
        char reason[24];

        if (!json_str(data, len, "reason", reason, sizeof(reason))) {
            reason[0] = '\0';
        }

        // Teardown publishes the state, so the pairing page never sees a terminal
        // state while a task is still winding down
        snprintf(mirror_error, sizeof(mirror_error), "Relay closed: %s", reason);
        s_run = false;
    }
}

// Map a protocol button id onto its expander pin. POWER is absent on purpose: see below
static int btn_pin(uint8_t id)
{
    switch (id) {
        case MIRROR_BTN_SELECT: return TCA9535_USER_BUTTON_SELECT_PIN;
        case MIRROR_BTN_HOME:   return TCA9535_USER_BUTTON_HOME_PIN;
        case MIRROR_BTN_UP:     return TCA9535_USER_BUTTON_UP_PIN;
        case MIRROR_BTN_DOWN:   return TCA9535_USER_BUTTON_DOWN_PIN;
        case MIRROR_BTN_LEFT:   return TCA9535_USER_BUTTON_LEFT_PIN;
        case MIRROR_BTN_RIGHT:  return TCA9535_USER_BUTTON_RIGHT_PIN;
        default:                return -1;
    }
}

static void handle_input(const uint8_t *d, size_t len)
{
    // A stop already released every injected button; a message still in flight must not
    // re-arm one behind it
    if (len < 1 || !s_run) {
        return;
    }

    // A viewer that is only PENDING has no approved input path: it cannot drive the device,
    // type, change quality or end the session. ACK and KEYFRAME_REQ are harmless to accept
    const bool live = (mirror_state == MIRROR_LIVE);

    switch (d[0]) {
        case MIRROR_MSG_BTN: {
            if (len < 3 || !live) {
                return;
            }

            // POWER is rejected, not forwarded. It would sleep the device, which kills the
            // panel, the radio and the session with no way back in from the web. The page
            // offers an explicit "sleep device" control that ends the session first
            const int pin = btn_pin(d[1]);

            if (pin < 0) {
                return;
            }

            switch (d[2]) {
                case MIRROR_BTN_ACTION_TAP:
                    // The browser timed the click, so the device performs one whole press:
                    // network delay can no longer stretch a tap into a hold
                    gpio_utils_remote_button_tap((uint8_t)pin, GPIO_REMOTE_TAP_HOLD_MS);
                    break;
                case MIRROR_BTN_ACTION_DOWN:
                    gpio_utils_remote_button_set((uint8_t)pin, true, GPIO_REMOTE_HOLD_MAX_MS);
                    break;
                case MIRROR_BTN_ACTION_UP:
                default:
                    gpio_utils_remote_button_set((uint8_t)pin, false, GPIO_REMOTE_MIN_HOLD_MS);
                    break;
            }

            // A stop between the s_run check above and here would have cleared the overlay;
            // this press must not survive it
            if (!s_run) {
                gpio_utils_remote_buttons_clear();
            }
            break;
        }

        case MIRROR_MSG_ACK:
            if (len >= 3) {
                // Accept only forward acks: a stale or reordered one must not drag the
                // window backwards and wedge the pacing gate. A shared seq's second ack
                // (FRAME after THERMAL) is not forward, so each seq times one round trip
                const uint16_t a = (uint16_t)(d[1] | (d[2] << 8));
                const uint32_t at = (uint32_t)esp_timer_get_time();

                portENTER_CRITICAL(&s_rtt_mux);
                if ((int16_t)(a - s_last_acked) > 0) {
                    s_last_acked = a;
                    mirror_rtt_acked(&s_rtt, a, at);
                }
                portEXIT_CRITICAL(&s_rtt_mux);
            }
            break;

        case MIRROR_MSG_TEXT:
            if (len >= 2 && live && s_text && s_text->type) {
                const size_t n = (d[1] < len - 2) ? d[1] : len - 2;
                (void)s_text->type((const char *)&d[2], n);
            }
            break;

        case MIRROR_MSG_TEXTKEY:
            if (len >= 2 && live && s_text && s_text->key) {
                s_text->key((int)d[1]);
            }
            break;

        case MIRROR_MSG_KEYFRAME_REQ:
            s_want_keyframe = true;
            s_text_state_stale = true; // Resend the entry state alongside the full repaint
            break;

        case MIRROR_MSG_QUALITY:
            if (len >= 2 && live && d[1] < (uint8_t)MIRROR_Q_COUNT + 1) {
                s_pinned_level = d[1];
                s_want_keyframe = true;
            }
            break;

        case MIRROR_MSG_END_SESSION:
            if (!live) {
                break;
            }

            // The LCD task turns this into a power press; the mirror cannot sleep the
            // device itself because that path is LVGL's
            if (len >= 2 && d[1] != 0) {
                s_sleep_req = true;
            }

            mirror_stop(MIRROR_BYE_USER);
            break;

        default:
            break;
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)event_data;

    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Relay connected");
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            // Anything a viewer was holding must not survive the link
            gpio_utils_remote_buttons_clear();

            // The relay issues a fresh code on every /d connect, so the one on screen is
            // dead the moment the socket drops. Showing it would send the user to a 404
            mirror_code[0] = '\0';

            // A fresh /d connect means a new code and an unlocked session, so the previous
            // approval is void: the next attach must be approved again
            portENTER_CRITICAL(&s_viewer_mux);
            approval_clear();
            mirror_check[0] = '\0';

            if (mirror_state == MIRROR_LIVE || mirror_state == MIRROR_PENDING ||
                    mirror_state == MIRROR_WAITING) {
                mirror_state = MIRROR_STARTING;
            }
            portEXIT_CRITICAL(&s_viewer_mux);
            break;

        case WEBSOCKET_EVENT_CLOSED:
            // A server close with no bye first (a relay restart, a protocol error) ends the
            // client task: no DISCONNECTED or reconnect follows, so the session must end too.
            // Our own close at teardown lands here after s_run is already clear
            if (s_run) {
                snprintf(mirror_error, sizeof(mirror_error), "Relay closed the link");
                s_run = false;
            }
            break;

        case WEBSOCKET_EVENT_DATA:
            if (e->data_len <= 0 || e->payload_offset != 0) {
                break; // Ignore continuations: every message we define fits one frame
            }

            if (e->op_code == 0x01) { // Text: relay control
                handle_relay_text((const char *)e->data_ptr, (size_t)e->data_len);
            } else if (e->op_code == 0x02) { // Binary: viewer input
                handle_input((const uint8_t *)e->data_ptr, (size_t)e->data_len);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "Relay error");
            break;

        default:
            break;
    }
}

static void load_relay_uri(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", MIRROR_RELAY_URI_DEFAULT);

    nvs_handle_t h;

    if (nvs_open(MIRROR_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    size_t len = out_size;

    if (nvs_get_str(h, MIRROR_NVS_KEY_URI, out, &len) != ESP_OK) {
        snprintf(out, out_size, "%s", MIRROR_RELAY_URI_DEFAULT);
    }

    nvs_close(h);
}

// An approved viewer, with the relay's lock to it confirmed. Until then the relay forwards
// to whoever holds the slot, which can change hands; after it, only the approved viewer can
// hold it. Re-checked before every message, so a detach stops the frame there
static bool may_stream(void)
{
    return (mirror_state == MIRROR_LIVE && s_relay_locked);
}

// Time one send call into the window. The total is only logged: preemption and TLS count
// in it too. One call held past MIRROR_QC_BLOCKED_MS is the socket pushing back
static void note_send(mirror_window_t *win, int64_t t0, int64_t t1)
{
    const uint32_t ms = (uint32_t)((t1 - t0) / 1000);

    win->stall_ms += ms;

    if (ms > win->longest_send_ms) {
        win->longest_send_ms = ms;
    }
}

// Start a seq's round trip once the message its ack answers has gone to the socket, so the
// device's own encode and preemption stay out of it
static void rtt_stamp(uint16_t seq, int64_t t)
{
    portENTER_CRITICAL(&s_rtt_mux);
    mirror_rtt_sent(&s_rtt, seq, (uint32_t)t);
    portEXIT_CRITICAL(&s_rtt_mux);
}

// Move finished round trips into the window. Every pass, since 20 fps outruns the ring
static void rtt_collect(mirror_window_t *win)
{
    uint32_t us;

    for (;;) {
        portENTER_CRITICAL(&s_rtt_mux);
        const bool got = mirror_rtt_take(&s_rtt, &us);
        portEXIT_CRITICAL(&s_rtt_mux);

        if (!got) {
            break;
        }

        mirror_window_rtt(win, us);
    }
}

static void rtt_reset(void)
{
    portENTER_CRITICAL(&s_rtt_mux);
    mirror_rtt_reset(&s_rtt);
    portEXIT_CRITICAL(&s_rtt_mux);
}

// Send every message of one encoded frame. Tiles in a message that fails to go out are
// returned to the dirty set, so nothing is silently lost. share_seq reuses the seq of the
// THERMAL just sent, so one tick holds one in-flight slot and that THERMAL's ack retires it
static size_t send_frame(bool keyframe, bool share_seq, mirror_window_t *win)
{
    const uint16_t tiles = mirror_encode_frame(s_level);

    if (tiles == 0) {
        return 0;
    }

    const uint16_t msgs = mirror_encode_msg_count();
    const uint16_t seq = share_seq ? s_seq : (uint16_t)(s_seq + 1);
    size_t sent = 0;

    for (uint16_t i = 0; i < msgs; i++) {
        const size_t n = mirror_encode_build_msg(i, seq, keyframe, s_level,
                s_txbuf, sizeof(s_txbuf));

        if (n == 0) {
            mirror_encode_restore_msg(i);
            continue;
        }

        const int64_t t0 = esp_timer_get_time();
        const int w = may_stream() ? esp_websocket_client_send_bin(s_client,
                (const char *)s_txbuf, (int)n, pdMS_TO_TICKS(MIRROR_TX_TIMEOUT_MS)) : -1;
        const int64_t t1 = esp_timer_get_time();

        note_send(win, t0, t1);

        if (w < (int)n) {
            // The link is behind, or the viewer changed. Give back every message from here
            // on, not just this one: their tiles were encoded and the reference already
            // advanced, so anything left unrestored would never be sent again
            for (uint16_t j = i; j < msgs; j++) {
                mirror_encode_restore_msg(j);
            }
            break;
        }

        sent += n;

        // The viewer acks the LAST message. A shared seq keeps the THERMAL's stamp: that
        // ack arrives first, and this one is dropped as not forward
        if (i == msgs - 1 && !share_seq) {
            rtt_stamp(seq, t1);
        }

        // Yield so the same-priority websocket task can take client->lock and dispatch any
        // queued viewer input between our messages, rather than after the whole frame
        taskYIELD();
    }

    // Only claim the sequence number if something actually went out
    if (sent > 0) {
        s_seq = seq;
    }

    return sent;
}

// Send the thermal channel's latest state if the viewer is owed it. Runs ahead of the tile
// frame, so a change in which tiles it paints lands before the tiles that change hands.
// ok goes false when a message would not go out, and the tile frame must then wait too
static size_t send_thermal(bool keyframe, int64_t now, int64_t *last_ack_us, mirror_window_t *win,
        bool *ok)
{
    const uint16_t seq = (uint16_t)(s_seq + 1);
    const size_t n = mirror_thermal_encode(keyframe, seq, s_txbuf, sizeof(s_txbuf));

    if (n == 0) {
        return 0;
    }

    // Pipeline empty: start the ack clock from this message
    if (s_seq == s_last_acked) {
        *last_ack_us = now;
    }

    const int64_t t0 = esp_timer_get_time();
    const int w = may_stream() ? esp_websocket_client_send_bin(s_client,
            (const char *)s_txbuf, (int)n, pdMS_TO_TICKS(MIRROR_TX_TIMEOUT_MS)) : -1;
    const int64_t t1 = esp_timer_get_time();

    note_send(win, t0, t1);

    if (w < (int)n) {
        *ok = false; // Nothing adopted; rebuilt from the latest state next tick
        return 0;
    }

    rtt_stamp(seq, t1); // The viewer acks a THERMAL on arrival
    s_seq = seq;
    mirror_thermal_commit();
    taskYIELD();

    return n;
}

// Tell the viewer whether an entry screen is open, so it can raise a keyboard, and
// whether that screen refuses remote input because it holds a credential
static void send_text_state(void)
{
    static bool last_open = false;
    static bool last_sensitive = false;
    static char last_buf[64] = { 0 };

    // The frame before this yielded, so the viewer may have changed since the gate
    if (!may_stream()) {
        return;
    }

    bool sensitive = false;
    char buf[64] = { 0 };
    const bool open = (s_text && s_text->state) ? s_text->state(&sensitive, buf, sizeof(buf)) : false;

    // A freshly attached viewer has been told nothing, so the dedupe must not suppress
    // the first message just because the device's own state has not moved
    if (!s_text_state_stale && open == last_open && sensitive == last_sensitive &&
            strcmp(buf, last_buf) == 0) {
        return;
    }

    s_text_state_stale = false;

    last_open = open;
    last_sensitive = sensitive;
    snprintf(last_buf, sizeof(last_buf), "%s", buf);

    const size_t n = strlen(buf);
    uint8_t msg[4 + sizeof(buf)];

    msg[0] = MIRROR_MSG_TEXTSTATE;
    msg[1] = open ? 1 : 0;
    msg[2] = sensitive ? 1 : 0;
    msg[3] = (uint8_t)n;
    memcpy(&msg[4], buf, n);

    const int w = esp_websocket_client_send_bin(s_client, (const char *)msg, (int)(4 + n),
            pdMS_TO_TICKS(MIRROR_TX_TIMEOUT_MS));

    if (w < (int)(4 + n)) {
        s_text_state_stale = true; // Never landed whole; retry next tick
    }
}

// Ask the relay to lock the session to the approved viewer, so only its resume token can
// re-attach. The relay ignores a check that is no longer the attached viewer's
static void send_approval(void)
{
    char check[sizeof(s_approved_check)];

    portENTER_CRITICAL(&s_viewer_mux);
    s_lock_stale = false;
    memcpy(check, s_approved_check, sizeof(check));
    const bool approved = s_approved;
    portEXIT_CRITICAL(&s_viewer_mux);

    if (!approved || check[0] == '\0') {
        return; // Voided since, or nothing to lock to
    }

    char msg[40];
    const int n = snprintf(msg, sizeof(msg), "{\"t\":\"approved\",\"check\":\"%s\"}", check);

    const int w = esp_websocket_client_send_text(s_client, msg, n,
            pdMS_TO_TICKS(MIRROR_TX_TIMEOUT_MS));

    if (w < n) {
        s_lock_stale = true; // Never landed whole; retry next tick
    }
}

// Tell the viewer whether it is approved and streaming yet, so a PENDING viewer shows a
// "waiting for approval" state instead of a frozen canvas
static void send_status(void)
{
    // LIVE waits for the relay's lock, as the frames do: before it the slot can change
    // hands, and a viewer that swapped in would have its controls unlocked
    portENTER_CRITICAL(&s_viewer_mux);
    const bool live = (mirror_state == MIRROR_LIVE);
    const bool owed = (!live || s_relay_locked);

    if (owed) {
        s_status_stale = false;
    }
    portEXIT_CRITICAL(&s_viewer_mux);

    if (!owed) {
        return; // Still stale; goes out once the lock is confirmed
    }

    const uint8_t msg[2] = {
        MIRROR_MSG_STATUS,
        (uint8_t)(live ? MIRROR_STATUS_LIVE : MIRROR_STATUS_PENDING),
    };

    const int w = esp_websocket_client_send_bin(s_client, (const char *)msg, sizeof(msg),
            pdMS_TO_TICKS(MIRROR_TX_TIMEOUT_MS));

    if (w < (int)sizeof(msg)) {
        s_status_stale = true; // Never landed; retry next tick
    }
}

// Close one window and apply the controller's level. The link decides, through the viewer's
// ack round trips: send time is mostly this CPU's contention, not the network's
static void close_window(mirror_quality_ctl_t *qc, mirror_window_t *win, int64_t start_us)
{
    const int64_t now = esp_timer_get_time();
    const uint32_t ms = (uint32_t)((now - start_us) / 1000) + 1; // Over 1 s if a send ran long

    rtt_collect(win);

    portENTER_CRITICAL(&s_rtt_mux);
    const uint16_t acked = s_last_acked;
    win->pending_ms = mirror_rtt_pending_us(&s_rtt, acked, (uint32_t)now) / 1000;
    portEXIT_CRITICAL(&s_rtt_mux);

    win->waiting = (s_seq != acked);

    const uint8_t level = mirror_quality_window(qc, win, s_pinned_level);

    ESP_LOGD(TAG, "Window %u ms: rtt %u base %u queue %u n %u, %u B, %u fr, stall %u/%u ms, "
            "pend %u", (unsigned)ms, (unsigned)qc->rtt_ms, (unsigned)qc->base_ms,
            (unsigned)qc->queue_ms, (unsigned)win->samples, (unsigned)win->bytes,
            (unsigned)win->frames, (unsigned)win->stall_ms, (unsigned)win->longest_send_ms,
            (unsigned)win->pending_ms);

    if (level != (uint8_t)s_level) {
        ESP_LOGI(TAG, "Quality %s to %u: %s (rtt %u ms, base %u, queue %u, %u KB/s, %u fps, "
                "stall %u ms, longest send %u ms, up after %u clear s)",
                (level > (uint8_t)s_level) ? "down" : "up", (unsigned)level,
                qc->why ? qc->why : "?", (unsigned)qc->rtt_ms, (unsigned)qc->base_ms,
                (unsigned)qc->queue_ms, (unsigned)(win->bytes / 1024u * 1000u / ms),
                (unsigned)(win->frames * 1000u / ms), (unsigned)win->stall_ms,
                (unsigned)win->longest_send_ms, (unsigned)mirror_quality_up_after(qc));

        s_level = (mirror_quality_t)level;
        s_want_keyframe = true; // The reference holds values quantized at the old level
    }

    memset(win, 0, sizeof(*win));
}

static void mirror_task(void *arg)
{
    // Hold the radio awake and the CPU pinned for the life of the session, so relayed
    // input and acks are not delayed by modem sleep. Released on every exit path below
    wifi_utils_relay_lowlatency(true);

    char uri[128];
    load_relay_uri(uri, sizeof(uri));

    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .subprotocol = "pc5.mirror.v1",
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 8000,
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 45,
        // Sizes BOTH the tx and rx buffers, and writes larger than this are chunked with
        // the full timeout applied per chunk. One message must fit in one write
        .buffer_size = 4096,
        .task_stack = 5120,
        .task_prio = POLYCAST5_PRIORITY_MEDIUM,
    };

    s_client = esp_websocket_client_init(&cfg);

    if (s_client == NULL) {
        wifi_utils_relay_lowlatency(false); // Release the hold taken at entry
        snprintf(mirror_error, sizeof(mirror_error), "Relay client init failed");
        mirror_state = MIRROR_ERROR;
        s_run = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(s_client);

    const int64_t started_us = esp_timer_get_time();
    int64_t window_us = started_us;
    int64_t last_frame_us = 0;
    int64_t last_keyframe_us = 0;

    int64_t last_ack_us = started_us;
    int64_t viewerless_since_us = started_us;
    int64_t connecting_since_us = started_us;
    uint16_t last_seen_ack = 0;
    uint8_t seen_viewer_gen = s_viewer_gen;

    mirror_quality_ctl_t qc;
    mirror_window_t win;

    mirror_quality_init(&qc, (uint8_t)s_level);
    memset(&win, 0, sizeof(win));
    rtt_reset();

    while (s_run) {
        const int64_t now = esp_timer_get_time();

        // The link is the session: losing Wi-Fi, or a config portal taking the radio into
        // AP mode, both end it rather than leaving the socket to time out mysteriously
        if (xWifiEventGroup != NULL &&
                (xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT) == 0) {
            snprintf(mirror_error, sizeof(mirror_error), "Wi-Fi disconnected");
            mirror_stop(MIRROR_BYE_WIFI_LOST);
            break;
        }

        // An unreachable relay would otherwise sit on "Connecting..." until the session
        // cap, with no error shown. Measured from the last time we were past STARTING,
        // so an ordinary reconnect still gets the full window
        if (mirror_state != MIRROR_STARTING) {
            connecting_since_us = now;
        } else if ((now - connecting_since_us) > (int64_t)MIRROR_CONNECT_TIMEOUT_MS * 1000) {
            snprintf(mirror_error, sizeof(mirror_error), "Cannot reach the relay");
            mirror_stop(MIRROR_BYE_TIMEOUT);
            break;
        }

        // Measured from the last moment an approved viewer was present, not from session
        // start: someone refreshing their tab an hour in should not find the device gone.
        // A pending viewer does not count as present
        if (mirror_state == MIRROR_LIVE) {
            viewerless_since_us = now;
        }

        if ((mirror_state == MIRROR_WAITING || mirror_state == MIRROR_PENDING) &&
                (now - viewerless_since_us) > (int64_t)MIRROR_WAIT_TIMEOUT_MS * 1000) {
            snprintf(mirror_error, sizeof(mirror_error), "No viewer connected");
            mirror_stop(MIRROR_BYE_TIMEOUT);
            break;
        }

        if ((now - started_us) > (int64_t)MIRROR_SESSION_MAX_MS * 1000) {
            snprintf(mirror_error, sizeof(mirror_error), "Session expired");
            mirror_stop(MIRROR_BYE_TIMEOUT);
            break;
        }

        // Ask the relay to lock to the approved viewer. Nothing the approval unlocks goes
        // out until the relay confirms with "locked"
        if (s_lock_stale && esp_websocket_client_is_connected(s_client)) {
            send_approval();
        }

        // A PENDING viewer never reaches the streaming gate below, so send the owed status
        // here while the link is up
        if (s_status_stale && esp_websocket_client_is_connected(s_client)) {
            send_status();
        }

        if (!may_stream() || !esp_websocket_client_is_connected(s_client)) {
            vTaskDelay(pdMS_TO_TICKS(MIRROR_IDLE_POLL_MS));
            continue;
        }

        // A new or resumed viewer: drop the old socket's round trips and start a fresh window,
        // which the controller skips as the keyframe's
        if (s_viewer_gen != seen_viewer_gen) {
            portENTER_CRITICAL(&s_viewer_mux);
            seen_viewer_gen = s_viewer_gen;
            const bool new_path = s_viewer_new;
            s_viewer_new = false;
            portEXIT_CRITICAL(&s_viewer_mux);

            rtt_reset();
            mirror_quality_viewer(&qc, new_path);
            memset(&win, 0, sizeof(win));
            window_us = now;
        }

        rtt_collect(&win);

        // Track ack progress so a lost one cannot wedge the gate forever
        if (s_last_acked != last_seen_ack) {
            last_seen_ack = s_last_acked;
            last_ack_us = now;
        }

        // Hold off while the viewer is behind. Skipped frames coalesce for free: more
        // tiles are simply dirty next round, and each tile still goes out exactly once
        bool behind = (uint16_t)(s_seq - s_last_acked) >= MIRROR_MAX_INFLIGHT;

        // If a frame's last message never landed, its ack never comes and the gate would
        // stay shut for the rest of the session. Resync and repaint instead
        if (behind && (now - last_ack_us) > (int64_t)MIRROR_ACK_TIMEOUT_MS * 1000) {
            // Acks still owed from before are no longer round trips
            portENTER_CRITICAL(&s_rtt_mux);
            s_last_acked = s_seq;
            mirror_rtt_reset(&s_rtt);
            portEXIT_CRITICAL(&s_rtt_mux);

            last_seen_ack = s_seq;
            last_ack_us = now;
            s_want_keyframe = true;
            win.resync = true; // Three seconds without an ack is congestion
            behind = false;

            ESP_LOGW(TAG, "Ack timeout, resyncing");
        }

        const bool too_soon = (now - last_frame_us) < (int64_t)MIRROR_MIN_FRAME_MS * 1000;

        if (behind || too_soon) {
            vTaskDelay(pdMS_TO_TICKS(MIRROR_IDLE_POLL_MS));
        } else {
            bool keyframe = s_want_keyframe;

            if (!keyframe && (now - last_keyframe_us) > (int64_t)MIRROR_KEYFRAME_MS * 1000) {
                keyframe = true;
            }

            if (keyframe) {
                s_want_keyframe = false;
                last_keyframe_us = now;
                mirror_force_keyframe();
            }

            bool thermal_ok = true;
            const size_t thermal = send_thermal(keyframe, now, &last_ack_us, &win, &thermal_ok);
            size_t frame = 0;

            if (thermal_ok && (keyframe || mirror_dirty_any())) {
                // Pipeline empty: start the ack clock from this frame, so the timeout is
                // not measured from a stale ack that has nothing outstanding behind it
                if (s_seq == s_last_acked) {
                    last_ack_us = now;
                }

                frame = send_frame(keyframe, thermal > 0, &win);
                last_frame_us = now;
            } else if (thermal > 0) {
                last_frame_us = now;
            } else {
                vTaskDelay(pdMS_TO_TICKS(MIRROR_IDLE_POLL_MS));
            }

            win.bytes += (uint32_t)(thermal + frame);

            if (thermal > 0 || frame > 0) {
                win.frames++; // One seq per tick: FRAME shares a THERMAL's
            }
        }

        send_text_state();

        if ((now - window_us) >= 1000000) {
            close_window(&qc, &win, window_us);
            window_us = now;
        }
    }

    gpio_utils_remote_buttons_clear();

    if (s_client) {
        // Say why before hanging up, so the viewer can explain itself rather than just
        // going dark
        if (esp_websocket_client_is_connected(s_client)) {
            const uint8_t bye[2] = { MIRROR_MSG_BYE, s_stop_reason };

            (void)esp_websocket_client_send_bin(s_client, (const char *)bye, sizeof(bye),
                    pdMS_TO_TICKS(100));
        }

        esp_websocket_client_close(s_client, pdMS_TO_TICKS(500));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }

    portENTER_CRITICAL(&s_viewer_mux);
    approval_clear();
    mirror_check[0] = '\0';
    portEXIT_CRITICAL(&s_viewer_mux);
    mirror_code[0] = '\0';

    // Release the low-latency hold taken at entry, once, before the task ends
    wifi_utils_relay_lowlatency(false);

    // Publish the state first, then release the slot. The other order lets a session
    // started in between be stamped OFF by this dying task. mirror_start() waits for the
    // handle to clear, so a page reacting to the state cannot race ahead of the teardown
    mirror_state = (mirror_error[0] != '\0') ? MIRROR_ERROR : MIRROR_OFF;

    if (s_task == xTaskGetCurrentTaskHandle()) {
        s_task = NULL;
    }

    vTaskDelete(NULL);
}

esp_err_t mirror_start(void)
{
    if (s_task != NULL && s_run) {
        return ESP_OK; // Already running
    }

    // A previous session may still be tearing down. It owns the websocket client, so wait
    // for it rather than racing a second task onto the same handle
    for (int i = 0; s_task != NULL && i < 40; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_task != NULL) {
        snprintf(mirror_error, sizeof(mirror_error), "Still stopping, try again");
        mirror_state = MIRROR_ERROR;
        return ESP_ERR_INVALID_STATE;
    }

    if (xWifiEventGroup == NULL ||
            (xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT) == 0) {
        snprintf(mirror_error, sizeof(mirror_error), "Connect to Wi-Fi first");
        mirror_state = MIRROR_ERROR;
        return ESP_ERR_INVALID_STATE;
    }

    mirror_code[0] = '\0';
    mirror_check[0] = '\0';
    mirror_error[0] = '\0';
    s_approved = false;
    s_lock_stale = false;
    s_relay_locked = false;
    s_approved_check[0] = '\0';
    s_seq = 0;
    s_last_acked = 0;
    s_level = MIRROR_Q_EXACT;
    s_pinned_level = 0;
    s_want_keyframe = true;
    s_stop_reason = MIRROR_BYE_USER;
    s_sleep_req = false;
    s_text_state_stale = true;
    s_status_stale = false; // No viewer yet; nothing to tell
    s_viewer_new = false;
    s_run = true;
    mirror_state = MIRROR_STARTING;

    // Nothing in the shadow is trustworthy until the screen has been repainted once, so
    // the first frame is always a full one
    mirror_force_keyframe();
    mirror_thermal_reset();

    if (xTaskCreatePinnedToCore(mirror_task, "mirror_task", MIRROR_TASK_STACK, NULL,
            POLYCAST5_PRIORITY_MEDIUM, &s_task, 0) != pdPASS) {
        s_run = false;
        s_task = NULL;
        snprintf(mirror_error, sizeof(mirror_error), "Out of memory");
        mirror_state = MIRROR_ERROR;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void mirror_stop(uint8_t reason)
{
    // Only flags here. The socket is owned by mirror_task and torn down there, so callers
    // on other tasks (the LCD task on sleep, low battery or a user stop) cannot race the
    // destroy. The task sends the BYE on its way out
    s_stop_reason = reason;
    s_run = false; // Must precede the clear: handle_input re-checks s_run after injecting

    portENTER_CRITICAL(&s_viewer_mux);
    approval_clear();
    portEXIT_CRITICAL(&s_viewer_mux);

    if (reason == MIRROR_BYE_USER || reason == MIRROR_BYE_DENIED) {
        mirror_error[0] = '\0'; // A deliberate stop is not a failure to report
    }

    gpio_utils_remote_buttons_clear();
}
