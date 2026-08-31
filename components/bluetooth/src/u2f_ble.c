#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_bt.h" // esp_bt_controller_get_status

#include "nimble/nimble_port.h"          // nimble_port_init/deinit/run/stop
#include "nimble/nimble_port_freertos.h" // esp_nimble_enable, nimble_port_freertos_deinit
#include "nimble/ble.h"                  // BLE_OWN_ADDR_RANDOM, BLE_ERR_REM_USER_CONN_TERM
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/dis/ble_svc_dis.h"

#include "polycast5_macros.h"
#include "bluetooth_utils.h" // bluetooth_state_t / bluetooth_state / bluetooth_persona
#include "bluetooth_nvs.h"
#include "ble_flood.h"
#include "u2f.h"
#include "u2f_nvs.h"
#include "ctap2.h"

#define TAG "U2F_BLE"

// Declaration of extern esp function
void ble_store_config_init(void);

/* ===================== FIDO BLE transport constants ===================== */

#define U2F_SVC_UUID16 0xFFFD

/* Command and status bytes of the BLE framing layer (U2F BT 1.2 section 6.1) */
#define U2F_BLE_CMD_PING      0x81
#define U2F_BLE_CMD_KEEPALIVE 0x82
#define U2F_BLE_CMD_MSG       0x83
#define U2F_BLE_CMD_ERROR     0xBF

/* KEEPALIVE status codes */
#define U2F_KEEPALIVE_PROCESSING 0x01
#define U2F_KEEPALIVE_TUP_NEEDED 0x02

/* ERROR codes */
#define U2F_ERR_INVALID_CMD 0x01
#define U2F_ERR_INVALID_PAR 0x02
#define U2F_ERR_INVALID_LEN 0x03
#define U2F_ERR_INVALID_SEQ 0x04
#define U2F_ERR_REQ_TIMEOUT 0x05
#define U2F_ERR_OTHER       0x7F

/* This persona is exposed to WebAuthn clients as FIDO2. Advertising a single
 * revision also makes it the protocol default, so clients do not need to write
 * a selection before sending their first command. */
#define U2F_REVISION_FIDO2     0x20
#define U2F_REVISION_BITFIELD  U2F_REVISION_FIDO2

/* Largest single ATT write or notification. Kept under the negotiated MTU and
 * reported to the host through fidoControlPointLength. */
#define U2F_FRAG_MAX 244

#define U2F_KEEPALIVE_MS   500   // Spec cadence while a request is parked
#define U2F_PRESENCE_MS    30000 // How long to wait for the user before giving up
#define U2F_SYNC_WAIT_MS   2000  // Controller sync budget at bring-up
#define U2F_NOTIFY_TRIES   50    // Retries while the NimBLE mbuf pool is dry

#define U2F_DEVICE_NAME "PolyCast5-U2F"

/* ===================== Module state ===================== */

typedef enum {
    U2F_ST_IDLE = 0,
    U2F_ST_WAIT_PRESENCE,
} u2f_state_t;

static volatile bool s_active = false;
static volatile bool s_synced = false;
static volatile bool s_bonded = false;
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_handle = 0;
static volatile bool s_status_subscribed = false;

/* Request reassembly. Written by the NimBLE host task inside the control-point
 * access callback, consumed by bluetooth_task in u2f_service(). The C5 is
 * single core, so the volatile s_rx_complete flag is the whole handshake: the
 * producer publishes it last and refuses new writes until the consumer clears
 * it, which makes this a strict single-producer/single-consumer exchange. */
POLYCAST5_USE_PSRAM_BSS static uint8_t s_rx_buf[U2F_REQ_MAX_LEN];
POLYCAST5_USE_PSRAM_BSS static uint8_t s_tx_buf[U2F_RSP_MAX_LEN];

static volatile bool s_rx_complete = false;
static volatile uint8_t s_rx_error = 0; // Framing error owed to the host, 0 if none

/* Volatile because the producer is the host task and the consumer is
 * bluetooth_task: without it the compiler may cache these or hoist a read above
 * the s_rx_complete test that is supposed to publish them. */
static volatile uint8_t s_rx_cmd = 0;
static volatile size_t s_rx_expected = 0;
static volatile size_t s_rx_len = 0;
static volatile uint8_t s_rx_seq = 0;

/* The connection a parked request arrived on. A response must never be handed
 * to whoever holds the link later: the user approved it for that host, and the
 * assertion is over a challenge only that host chose. */
static volatile uint16_t s_req_conn = BLE_HS_CONN_HANDLE_NONE;

/* Which protocol the client selected on fidoServiceRevisionBitfield, and which
 * one the parked request is being handled as. */
static volatile uint8_t s_revision = 0;
static bool s_req_ctap2 = false;

static volatile u2f_state_t s_state = U2F_ST_IDLE;
static bool s_presence_approved = false;
static bool s_presence_denied = false;
static TickType_t s_presence_start = 0;
static TickType_t s_keepalive_last = 0;

static uint8_t s_own_addr[6] = {0};

/* Whether to advertise as available for pairing. The FIDO BT spec draws a hard
 * line here: a device in pairing mode sets a discoverable flag and service-data
 * bit 7, and one that is already bonded must clear both. A client hunting for an
 * authenticator to *use* can reasonably skip one still asking to be paired. */
static volatile bool s_pairing_mode = true;

/* A peer waiting to be recorded in the PolyCast5 index. The NimBLE host task
 * only stages it here; bluetooth_task does the flash write. */
static ble_addr_t s_pending_peer;
static volatile bool s_peer_pending = false;

/* ===================== GATT service ===================== */

static int u2f_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
        struct ble_gatt_access_ctxt *ctxt, void *arg);

static const ble_uuid16_t svc_fido_uuid = BLE_UUID16_INIT(U2F_SVC_UUID16);

/* F1D0FFFx-DEAA-ECEE-B42F-C9BA7ED623BB, byte-reversed as NimBLE stores 128-bit
 * UUIDs little-endian. Index 12 is the byte that varies per characteristic. */
#define U2F_CHR_UUID(sel) BLE_UUID128_INIT( \
        0xbb, 0x23, 0xd6, 0x7e, 0xba, 0xc9, 0x2f, 0xb4, \
        0xee, 0xec, 0xaa, 0xde, (sel), 0xff, 0xd0, 0xf1)

static const ble_uuid128_t chr_control_point_uuid = U2F_CHR_UUID(0xf1);
static const ble_uuid128_t chr_status_uuid = U2F_CHR_UUID(0xf2);
static const ble_uuid128_t chr_cp_length_uuid = U2F_CHR_UUID(0xf3);
static const ble_uuid128_t chr_revision_uuid = U2F_CHR_UUID(0xf4);

/* The _ENC and _AUTHEN flags are what make an unbonded host get
 * BLE_ATT_ERR_INSUFFICIENT_ENC and start pairing, which the FIDO BLE transport
 * requires before any message is exchanged. */
static const struct ble_gatt_svc_def u2f_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_fido_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &chr_control_point_uuid.u,
                .access_cb = u2f_gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC
                        | BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {
                .uuid = &chr_status_uuid.u,
                .access_cb = u2f_gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_handle,
            },
            {
                .uuid = &chr_cp_length_uuid.u,
                .access_cb = u2f_gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC
                        | BLE_GATT_CHR_F_READ_AUTHEN,
            },
            {
                .uuid = &chr_revision_uuid.u,
                .access_cb = u2f_gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE
                        | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC
                        | BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {
                0, // Sentinel
            },
        },
    },
    {
        0, // Sentinel
    },
};

/* ===================== Framing ===================== */

/**
 * Largest fragment this link can carry, which is also the value reported through
 * fidoControlPointLength.
 */
static uint16_t u2f_max_frag(uint16_t conn_handle)
{
    uint16_t mtu = ble_att_mtu(conn_handle);

    if (mtu < BLE_ATT_MTU_DFLT) {
        mtu = BLE_ATT_MTU_DFLT;
    }

    // An ATT notification spends 3 bytes on the opcode and attribute handle
    uint16_t frag = mtu - 3;

    if (frag > U2F_FRAG_MAX) {
        frag = U2F_FRAG_MAX;
    }
    if (frag < 20) {
        frag = 20; // Spec floor
    }

    return frag;
}

/**
 * Decide whether the assembled request is CTAP2 CBOR or a CTAP1 APDU.
 *
 * Normally the client says so by writing its choice to the revision bitfield.
 * Some clients skip that, so fall back to the payload: a U2F APDU always opens
 * with a CLA of 0x00, and no CTAP2 command byte is zero.
 */
static bool request_is_ctap2(const uint8_t *req, size_t len)
{
    if (s_revision & U2F_REVISION_FIDO2) {
        return true;
    }
    return len > 0 && req[0] != 0x00;
}

static void u2f_rx_reset(void)
{
    s_rx_cmd = 0;
    s_rx_expected = 0;
    s_rx_len = 0;
    s_rx_seq = 0;
    s_rx_complete = false;
}

/**
 * Push one fragment onto fidoStatus, retrying while the mbuf pool is dry.
 * Mirrors the retry the HID path uses for the same reason.
 */
static int u2f_notify_raw(uint16_t conn_handle, const uint8_t *buf, size_t len)
{
    for (int attempt = 0; attempt < U2F_NOTIFY_TRIES; attempt++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);

        if (om != NULL) {
            // Takes ownership of om on every path, success or not
            int rc = ble_gatts_notify_custom(conn_handle, s_status_handle, om);
            if (rc == 0) {
                return 0;
            }
            if (rc != BLE_HS_ENOMEM) {
                ESP_LOGE(TAG, "ble_gatts_notify_custom failed: %d", rc);
                return rc;
            }
        }

        vTaskDelay(1);
    }

    ESP_LOGE(TAG, "Notify gave up: mbuf pool stayed empty");
    return BLE_HS_ENOMEM;
}

/**
 * Send a complete response frame, fragmenting it across notifications.
 *
 * The first fragment carries the command and the 16-bit total length; every
 * later fragment carries a 7-bit sequence number that wraps at 0x7f.
 */
static int u2f_notify_frame(uint8_t cmd, const uint8_t *data, size_t len)
{
    uint16_t conn_handle = s_conn_handle;

    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return BLE_HS_ENOTCONN;
    }
    if (!s_status_subscribed) {
        ESP_LOGW(TAG, "Host has not subscribed to fidoStatus, dropping response");
        return BLE_HS_ENOTCONN;
    }

    uint16_t max_frag = u2f_max_frag(conn_handle);
    uint8_t frag[U2F_FRAG_MAX];

    size_t chunk = (len < (size_t)(max_frag - 3)) ? len : (size_t)(max_frag - 3);

    frag[0] = cmd;
    frag[1] = (uint8_t)(len >> 8);
    frag[2] = (uint8_t)(len & 0xFF);
    if (chunk > 0) {
        memcpy(frag + 3, data, chunk);
    }

    int rc = u2f_notify_raw(conn_handle, frag, chunk + 3);
    if (rc != 0) {
        return rc;
    }

    size_t off = chunk;
    uint8_t seq = 0;

    while (off < len) {
        chunk = (len - off < (size_t)(max_frag - 1)) ? (len - off) : (size_t)(max_frag - 1);

        frag[0] = seq;
        memcpy(frag + 1, data + off, chunk);

        rc = u2f_notify_raw(conn_handle, frag, chunk + 1);
        if (rc != 0) {
            return rc;
        }

        off += chunk;
        seq = (uint8_t)((seq + 1) & 0x7F);
    }

    return 0;
}

static void u2f_notify_error(uint8_t code)
{
    u2f_notify_frame(U2F_BLE_CMD_ERROR, &code, 1);
}

/**
 * Handle one write to fidoControlPoint.
 *
 * Runs on the NimBLE host task, so it only reassembles: no crypto and no
 * notifications happen here. A framing error is recorded and emitted later by
 * u2f_service(), which keeps this callback free of any re-entrant host calls.
 */
static int u2f_control_point_write(uint16_t conn_handle, struct os_mbuf *om)
{
    if (s_rx_complete || s_rx_error != 0) {
        // The previous request has not been answered yet
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t buf[U2F_FRAG_MAX];
    uint16_t len = 0;

    if (ble_hs_mbuf_to_flat(om, buf, sizeof(buf), &len) != 0 || len < 1) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (buf[0] & 0x80) {
        // Initialization fragment: command, then the 16-bit total length
        if (len < 3) {
            u2f_rx_reset();
            s_rx_error = U2F_ERR_INVALID_LEN;
            return 0;
        }

        s_rx_cmd = buf[0];
        s_rx_expected = ((size_t)buf[1] << 8) | buf[2];
        s_rx_len = 0;
        s_rx_seq = 0;

        if (s_rx_expected > sizeof(s_rx_buf)) {
            ESP_LOGW(TAG, "Request of %u bytes exceeds the reassembly buffer",
                    (unsigned)s_rx_expected);
            u2f_rx_reset();
            s_rx_error = U2F_ERR_INVALID_LEN;
            return 0;
        }

        size_t chunk = len - 3;
        if (chunk > s_rx_expected) {
            chunk = s_rx_expected;
        }
        memcpy(s_rx_buf, buf + 3, chunk);
        s_rx_len = chunk;
    } else {
        // Continuation fragment: sequence number, then more payload
        if (s_rx_cmd == 0) {
            s_rx_error = U2F_ERR_INVALID_SEQ;
            return 0;
        }

        if (buf[0] != s_rx_seq) {
            ESP_LOGW(TAG, "Fragment out of order: got %u, expected %u", buf[0], s_rx_seq);
            u2f_rx_reset();
            s_rx_error = U2F_ERR_INVALID_SEQ;
            return 0;
        }

        s_rx_seq = (uint8_t)((s_rx_seq + 1) & 0x7F);

        size_t chunk = len - 1;
        if (chunk > s_rx_expected - s_rx_len) {
            chunk = s_rx_expected - s_rx_len;
        }
        memcpy(s_rx_buf + s_rx_len, buf + 1, chunk);
        s_rx_len += chunk;
    }

    if (s_rx_len >= s_rx_expected) {
        // Published last: bluetooth_task keys off this and nothing else
        s_rx_complete = true;
    }

    return 0;
}

static int u2f_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* ctxt->chr and ctxt->dsc share a union, so reading chr on a descriptor
     * access would dereference the wrong member. No custom descriptors are
     * defined here, but this keeps the callback correct if that ever changes. */
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR && ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const ble_uuid_t *uuid = ctxt->chr->uuid;

    if (ble_uuid_cmp(uuid, &chr_control_point_uuid.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        }
        ESP_LOGI(TAG, "GATT: host wrote fidoControlPoint (%u bytes)",
                (unsigned)OS_MBUF_PKTLEN(ctxt->om));
        return u2f_control_point_write(conn_handle, ctxt->om);
    }

    if (ble_uuid_cmp(uuid, &chr_cp_length_uuid.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        uint16_t frag = u2f_max_frag(conn_handle);
        ESP_LOGI(TAG, "GATT: host read fidoControlPointLength -> %u", (unsigned)frag);
        uint8_t val[2] = { (uint8_t)(frag >> 8), (uint8_t)(frag & 0xFF) };
        return os_mbuf_append(ctxt->om, val, sizeof(val)) == 0
                ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ble_uuid_cmp(uuid, &chr_revision_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            uint8_t val = U2F_REVISION_BITFIELD;
            ESP_LOGI(TAG, "GATT: host read fidoServiceRevisionBitfield -> 0x%02x", val);
            return os_mbuf_append(ctxt->om, &val, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        // The host writes back the single revision it selected
        uint8_t sel = 0;
        uint16_t got = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, &sel, sizeof(sel), &got) != 0 || got != 1) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        ESP_LOGI(TAG, "GATT: host selected revision 0x%02x", sel);
        if (sel != U2F_REVISION_FIDO2) {
            ESP_LOGW(TAG, "Host selected unsupported revision 0x%02x", sel);
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        s_revision = sel;
        return 0;
    }

    // fidoStatus is notify-only
    ESP_LOGW(TAG, "GATT: unexpected access op=%d on an unhandled characteristic",
            ctxt->op);
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

/* ===================== GAP ===================== */

static int u2f_gap_event(struct ble_gap_event *event, void *arg);

static void u2f_advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    // Discoverable only while looking to pair; still connectable either way, so
    // a host that already holds a bond can always reach us
    fields.flags = s_pairing_mode
            ? (BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP)
            : BLE_HS_ADV_F_BREDR_UNSUP;

    // Hosts enumerate FIDO authenticators by this service UUID
    static const ble_uuid16_t adv_uuid = BLE_UUID16_INIT(U2F_SVC_UUID16);
    fields.uuids16 = &adv_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    /* NimBLE emits this buffer as the raw AD payload, so it has to carry the
     * 16-bit UUID itself before the payload. Flag bit 7 says the device is in
     * pairing mode, bit 6 that pairing needs the passkey shown on the LCD. */
    static uint8_t svc_data[] = { 0xFD, 0xFF, 0x00 };
    svc_data[2] = s_pairing_mode ? 0xC0 : 0x00;
    fields.svc_data_uuid16 = svc_data;
    fields.svc_data_uuid16_len = sizeof(svc_data);

    fields.name = (const uint8_t *)U2F_DEVICE_NAME;
    fields.name_len = strlen(U2F_DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);

    rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER, &adv_params,
            u2f_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    }
}

static int u2f_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_bonded = false;
            s_status_subscribed = false;
            /* A one-bit revision field makes that revision the default. */
            s_revision = U2F_REVISION_BITFIELD;
            u2f_rx_reset();
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Host connected, handle %u", (unsigned)s_conn_handle);
#endif
        } else {
            ESP_LOGW(TAG, "Connection attempt failed: %d", event->connect.status);
            u2f_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Host disconnected, reason %d", event->disconnect.reason);
#endif
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_bonded = false;
        s_status_subscribed = false;
        s_revision = 0;
        u2f_rx_reset();
        s_rx_error = 0;
        s_req_conn = BLE_HS_CONN_HANDLE_NONE;
        s_state = U2F_ST_IDLE;

        if (s_active) {
            u2f_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_active) {
            u2f_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        if (rc == 0 && desc.sec_state.encrypted) {
            s_bonded = true;

            /* bluetooth_utils_reconcile_bonds() drops any NimBLE bond missing
             * from the PolyCast5 peer index, so this host has to be recorded or
             * the next HID init would silently unpair the security key.
             *
             * The write is staged rather than done here: this runs on the
             * NimBLE host task, and an nvs_commit() erase can stall it for
             * over a hundred milliseconds. Stalling the host immediately after
             * encryption is exactly when the peer is mid service discovery. */
            s_pending_peer = desc.peer_id_addr;
            s_peer_pending = true;
            s_pairing_mode = false;

            /* Force the peer to re-discover the GATT database. The persona keeps
             * one BLE address across firmware builds while its service layout
             * moves underneath it, and a host caches GATT per bond, so a stale
             * cache is a real hazard while the table is still changing.
             *
             * This is insurance, not a fix for any observed failure: a host
             * working from a stale cache would never reach this callback at all,
             * because its reads would land on whatever now occupies the old
             * handles. Firing on every encryption costs a full re-discovery per
             * connection, which is worth it while the layout is in flux and
             * should be gated on a stored layout version once it settles. */
            ble_svc_gatt_changed(0x0001, 0xffff);
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Link encrypted, authenticated=%d; sent Service Changed",
                    desc.sec_state.authenticated);
#endif
        } else {
            ESP_LOGW(TAG, "Encryption failed: status %d", event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "GATT: subscription event attr=%u, fidoStatus=%u, notify=%d, reason=%d",
                (unsigned)event->subscribe.attr_handle, (unsigned)s_status_handle,
                event->subscribe.cur_notify, event->subscribe.reason);
        if (event->subscribe.attr_handle == s_status_handle) {
            s_status_subscribed = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG, "GATT: fidoStatus notifications %s",
                    s_status_subscribed ? "enabled" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "MTU now %u, max fragment %u", (unsigned)event->mtu.value,
                (unsigned)u2f_max_frag(event->mtu.conn_handle));
#endif
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // Drop the stale bond and let the host pair again
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc != 0) {
            return 0;
        }
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pkey = {0};

        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            uint32_t pairing_key = 0;
            bluetooth_nvs_pairing_key_load(&pairing_key);
            if (pairing_key == 0) {
                ESP_LOGE(TAG, "No pairing key in NVS");
            }

            // The same code the U2F screen shows, so the host prompt matches
            pkey.action = event->passkey.params.action;
            pkey.passkey = pairing_key;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            pkey.action = event->passkey.params.action;
            pkey.numcmp_accept = 1;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        }
        return 0;
    }

    default:
        return 0;
    }
}

/* ===================== Host lifecycle ===================== */

static void u2f_on_sync(void)
{
    s_synced = true;
}

static void u2f_on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "NimBLE host reset, reason %d", reason);
}

static void u2f_host_task(void *arg)
{
    // Blocks until nimble_port_stop() is called from bluetooth_task
    nimble_port_run();

    // nimble_port_freertos_deinit() deletes this task; nothing after it runs
    nimble_port_freertos_deinit();
}

bool u2f_ble_is_active(void)
{
    return s_active;
}

bool u2f_ble_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_bonded;
}

esp_err_t u2f_ble_start(void)
{
    if (s_active) {
        return ESP_ERR_INVALID_STATE;
    }

    // One persona owns the radio at a time
    if (bluetooth_state != BT_STATE_OFF) {
        ESP_LOGE(TAG, "Refusing U2F start: HID Bluetooth is up (state=%d)", bluetooth_state);
        return ESP_ERR_INVALID_STATE;
    }
    if (ble_flood_is_active()) {
        ESP_LOGE(TAG, "Refusing U2F start: BLE flood broadcaster is active");
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        ESP_LOGE(TAG, "Refusing U2F start: BT controller not idle");
        return ESP_ERR_INVALID_STATE;
    }

    bluetooth_state = BT_STATE_INITING;

    esp_err_t err = u2f_crypto_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "u2f_crypto_init failed: %s", esp_err_to_name(err));
        bluetooth_state = BT_STATE_OFF;
        return err;
    }

    err = u2f_nvs_load_ble_addr(s_own_addr);
    if (err != ESP_OK) {
        u2f_crypto_deinit();
        bluetooth_state = BT_STATE_OFF;
        return err;
    }

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        u2f_crypto_deinit();
        bluetooth_state = BT_STATE_OFF;
        return ESP_FAIL;
    }

    s_synced = false;
    s_status_handle = 0;
    s_status_subscribed = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_bonded = false;
    s_state = U2F_ST_IDLE;
    s_rx_error = 0;
    u2f_rx_reset();

    ble_hs_cfg.sync_cb = u2f_on_sync;
    ble_hs_cfg.reset_cb = u2f_on_reset;

    /* The FIDO BLE transport requires an encrypted, bonded link. This mirrors
     * the HID persona so the device shows one consistent pairing experience. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    /* Deliberately do NOT distribute our identity key. NimBLE keeps a single
     * device-wide IRK (ble_hs_pvcy_irk), so handing it out here would give the
     * host the same IRK it already holds for the HID keyboard persona, paired
     * with a different identity address. A host that keys bonds by IRK sees
     * that as one device contradicting itself. This persona advertises a fixed
     * static random address that never rotates, so the peer can identify it by
     * address alone and needs no IRK. */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    // The FIDO BLE spec makes the Device Information Service mandatory
    ble_svc_dis_init();
    ble_svc_dis_manufacturer_name_set("RoboticWorx");
    ble_svc_dis_model_number_set("PolyCast5");
    ble_svc_dis_firmware_revision_set("1.0");

    /* PnP ID is served through strlen(), so a zero byte anywhere would truncate
     * it. These values are chosen to avoid one:
     * source 0x02 (USB IF), vendor 0x16C0, product 0x05E1, version 0x0101. */
    ble_svc_dis_pnp_id_set("\x02\xC0\x16\xE1\x05\x01\x01");

    ble_svc_gap_device_name_set(U2F_DEVICE_NAME);

    int rc = ble_gatts_count_cfg(u2f_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        goto fail;
    }

    rc = ble_gatts_add_svcs(u2f_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        goto fail;
    }

    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    if (esp_nimble_enable(u2f_host_task) != ESP_OK) {
        ESP_LOGE(TAG, "esp_nimble_enable failed");
        goto fail;
    }

    s_active = true;

    // Wait for controller sync before touching the identity or advertising
    for (int i = 0; !s_synced && i < (U2F_SYNC_WAIT_MS / 10); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_synced) {
        ESP_LOGE(TAG, "Controller never synced");
        u2f_ble_stop();
        return ESP_ERR_TIMEOUT;
    }

    /* Advertise under an address of our own so hosts treat the security key as a
     * device distinct from the HID keyboard. Reusing one address for two
     * different GATT tables makes hosts serve a stale cached table. */
    rc = ble_hs_id_set_rnd(s_own_addr);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_set_rnd failed: %d", rc);
        u2f_ble_stop();
        return ESP_FAIL;
    }

    /* Already bonded to someone? Then come up ready to be used rather than
     * ready to pair. Clearing all bonds (Bluetooth > Forget All Devices) puts
     * the device back into pairing mode on the next start. */
    ble_addr_t bonded[CONFIG_BT_NIMBLE_MAX_BONDS];
    int bonded_count = 0;
    if (ble_store_util_bonded_peers(bonded, &bonded_count,
            (int)(sizeof(bonded) / sizeof(bonded[0]))) != 0) {
        bonded_count = 0;
    }
    s_pairing_mode = (bonded_count == 0);

    ESP_LOGI(TAG, "%d bond(s) stored, advertising in %s mode",
            bonded_count, s_pairing_mode ? "pairing" : "non-pairing");

    u2f_advertise();

    bluetooth_state = BT_STATE_RUNNING;
    bluetooth_persona = BT_PERSONA_U2F;

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "U2F persona advertising as \"%s\"", U2F_DEVICE_NAME);
#endif

    return ESP_OK;

fail:
    nimble_port_deinit();
    u2f_crypto_deinit();
    bluetooth_state = BT_STATE_OFF;
    return ESP_FAIL;
}

void u2f_ble_stop(void)
{
    if (!s_active) {
        return;
    }

    bluetooth_state = BT_STATE_DEINITING;
    s_active = false;

    ble_gap_adv_stop();

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    /* Blocking. Unlike the HID persona there is no esp_hid teardown here, so the
     * host stop and nimble_port_deinit() perform exactly one ble_gatts_stop()
     * and the double-stop fault documented in bluetooth_utils_deinit() cannot
     * happen on this path. */
    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_stop failed: %d", rc);
    }

    nimble_port_deinit();
    u2f_crypto_deinit();

    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_bonded = false;
    s_status_subscribed = false;
    s_status_handle = 0;
    s_state = U2F_ST_IDLE;
    s_rx_error = 0;
    u2f_rx_reset();

    bluetooth_persona = BT_PERSONA_NONE;
    bluetooth_state = BT_STATE_OFF;

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "U2F persona stopped");
#endif
}

/* ===================== Request pump ===================== */

void u2f_presence_approve(void)
{
    s_presence_approved = true;
}

void u2f_presence_deny(void)
{
    s_presence_denied = true;
}

bool u2f_presence_pending(void)
{
    return s_active && s_state == U2F_ST_WAIT_PRESENCE;
}

/**
 * Answer the parked request, then go back to waiting for the next one.
 */
static void u2f_respond_msg(bool presence)
{
    size_t n = s_req_ctap2
            ? ctap2_core_handle(s_rx_buf, s_rx_len, s_tx_buf, sizeof(s_tx_buf), presence)
            : u2f_core_handle_apdu(s_rx_buf, s_rx_len, s_tx_buf, sizeof(s_tx_buf), presence);

    uint16_t origin = s_req_conn;

    u2f_rx_reset();
    s_state = U2F_ST_IDLE;
    s_presence_approved = false;
    s_presence_denied = false;
    s_req_conn = BLE_HS_CONN_HANDLE_NONE;

    /* Signing takes tens of milliseconds, so the link can drop underneath it.
     * Answering a connection that is no longer the one that asked would hand a
     * fresh assertion to a host that never requested it. */
    if (s_conn_handle != origin) {
        ESP_LOGW(TAG, "Dropping a response whose connection went away");
        return;
    }

    if (n > 0) {
        u2f_notify_frame(U2F_BLE_CMD_MSG, s_tx_buf, n);
    }
}

void u2f_service(void)
{
    if (!s_active) {
        return;
    }

    // Flash write deferred off the NimBLE host task by the ENC_CHANGE handler
    if (s_peer_pending) {
        ble_addr_t peer = s_pending_peer;
        s_peer_pending = false;
        bluetooth_nvs_add_to_peers_list(&peer);
    }

    // A framing error is owed to the host before anything else
    if (s_rx_error != 0) {
        uint8_t code = s_rx_error;
        s_rx_error = 0;
        u2f_notify_error(code);
        return;
    }

    switch (s_state) {
    case U2F_ST_IDLE:
        if (!s_rx_complete) {
            return;
        }

        // Bind the request to its connection before anything can answer it
        s_req_conn = s_conn_handle;

        if (s_rx_cmd == U2F_BLE_CMD_PING) {
            // Echo the payload back unchanged
            size_t n = s_rx_len;
            memcpy(s_tx_buf, s_rx_buf, n);
            u2f_rx_reset();
            u2f_notify_frame(U2F_BLE_CMD_PING, s_tx_buf, n);
        } else if (s_rx_cmd == U2F_BLE_CMD_MSG) {
            s_req_ctap2 = request_is_ctap2(s_rx_buf, s_rx_len);

            bool needs_presence = s_req_ctap2
                    ? ctap2_core_needs_presence(s_rx_buf, s_rx_len)
                    : u2f_core_needs_presence(s_rx_buf, s_rx_len);

            if (needs_presence) {
                s_state = U2F_ST_WAIT_PRESENCE;
                s_presence_approved = false;
                s_presence_denied = false;
                s_presence_start = xTaskGetTickCount();

                // Backdate so the first keepalive goes out immediately
                s_keepalive_last = s_presence_start - pdMS_TO_TICKS(U2F_KEEPALIVE_MS);
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Waiting for the user to approve");
#endif
            } else {
                u2f_respond_msg(false);
            }
        } else {
            ESP_LOGW(TAG, "Unsupported BLE command 0x%02x", s_rx_cmd);
            u2f_rx_reset();
            u2f_notify_error(U2F_ERR_INVALID_CMD);
        }
        return;

    case U2F_ST_WAIT_PRESENCE:
        // The host that asked is gone; abandon rather than answer its successor
        if (s_conn_handle != s_req_conn) {
            ESP_LOGW(TAG, "Abandoning a parked request, the host disconnected");
            u2f_rx_reset();
            s_state = U2F_ST_IDLE;
            s_presence_approved = false;
            s_presence_denied = false;
            s_req_conn = BLE_HS_CONN_HANDLE_NONE;
            return;
        }

        if (s_presence_approved) {
            u2f_respond_msg(true);
            return;
        }
        if (s_presence_denied) {
            u2f_respond_msg(false);
            return;
        }

        if (xTaskGetTickCount() - s_presence_start >= pdMS_TO_TICKS(U2F_PRESENCE_MS)) {
            ESP_LOGW(TAG, "User presence timed out");
            // The APDU layer turns this into SW_CONDITIONS_NOT_SATISFIED
            u2f_respond_msg(false);
            return;
        }

        if (xTaskGetTickCount() - s_keepalive_last >= pdMS_TO_TICKS(U2F_KEEPALIVE_MS)) {
            s_keepalive_last = xTaskGetTickCount();

            // Tells the host the request is alive and parked on the button
            uint8_t status = U2F_KEEPALIVE_TUP_NEEDED;
            u2f_notify_frame(U2F_BLE_CMD_KEEPALIVE, &status, 1);
        }
        return;
    }
}
