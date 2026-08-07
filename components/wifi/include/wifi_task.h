#ifndef WIFI_TASK_H
#define WIFI_TASK_H

#include "freertos/idf_additions.h"

// Connection icon bits
#define ICON_BIT_WIFI_CONNECTED (1U << 0)
#define ICON_BIT_BT_CONNECTED   (1U << 1)
#define ICON_BIT_HOTKEY_ACTIVE  (1U << 2)
enum {
    ICON_NONE, // Default starting state
    ICON_BLUETOOTH_CONNECTED,
    ICON_BLUETOOTH_DISCONNECTED,
    ICON_WIFI_CONNECTED,
    ICON_WIFI_DISCONNECTED,
    ICON_HOTKEY_ACTIVE,
    ICON_HOTKEY_INACTIVE,
};  
typedef struct {
    uint8_t icon_wifi;
    uint8_t icon_bluetooth;
    uint8_t icon_hotkey;
} icon_state_t;
extern EventGroupHandle_t xConnectionIconEventGroup;

#define WIFI_PORTAL_AI_KEY_START_BIT            (1U << 0)
#define WIFI_PORTAL_AI_PKT_ANALYSIS_START_BIT   (1U << 1)
#define WIFI_PORTAL_BTC_START_BIT               (1U << 2)
#define WIFI_PORTAL_BT_START_BIT                (1U << 3)
#define WIFI_PORTAL_CLAUDE_START_BIT            (1U << 4)
#define WIFI_PORTAL_MESHTASTIC_START_BIT        (1U << 5)
extern EventGroupHandle_t xWiFiPortalEventGroup;

#define WIFI_SCAN_NETWORKS_BIT     (1U << 0)
#define WIFI_SCAN_DEAUTH_BIT       (1U << 1)
#define WIFI_CONNECTING_BIT        (1U << 2)
#define WIFI_CONNECTED_BIT         (1U << 3)
#define WIFI_CONNECTING_FAILED_BIT (1U << 4) // For LCD "Connecting..."
#define WIFI_DISCONNECT_BIT        (1U << 5)
#define WIFI_RECONNECT_BIT         (1U << 6)
#define WIFI_MQTT_CONNECTED_BIT    (1U << 7)
#define WIFI_MQTT_SUCCESS_BIT      (1U << 8)
#define WIFI_CHECK_OTA_ON_CONN_BIT (1U << 9)
#define WIFI_OTA_AVAILABLE_BIT     (1U << 10)
#define WIFI_GET_DATE_TIME_BIT     (1U << 11)
#define WIFI_GOT_DATE_TIME_BIT     (1U << 12)
#define WIFI_STOP_DEAUTH_BIT       (1U << 13)
#define WIFI_DATE_TIME_FAILED_BIT  (1U << 14) // SNTP sync gave up (pairs with WIFI_GOT_DATE_TIME_BIT)
#define WIFI_STOP_ARP_SPOOF_BIT    (1U << 15)
#define WIFI_STOP_NDP_SPOOF_BIT    (1U << 16)
extern EventGroupHandle_t xWifiEventGroup;

extern QueueHandle_t xWifiScanQueue;
extern QueueHandle_t xWifiDeauthScanQueue;
extern QueueHandle_t xWifiDeauthTargetQueue;
extern QueueHandle_t xWifiDeauthStatsQueue;
extern QueueHandle_t xWifiArpSpoofTargetQueue;
extern QueueHandle_t xWifiArpSpoofStatsQueue;
extern QueueHandle_t xWifiNdpSpoofTargetQueue;
extern QueueHandle_t xWifiNdpSpoofStatsQueue;
extern QueueHandle_t xWifiSelectedNetworkQueue;
extern QueueHandle_t xWifiSniffQueue;
extern QueueHandle_t xWifiBeaconQueue;
extern QueueHandle_t xWifiDataQueue;
extern QueueHandle_t xWifiMqttCmdQueue;
extern QueueHandle_t xWifiPingQueue;
extern QueueHandle_t xWifiAiRawSniffQueue;

extern SemaphoreHandle_t xWifiCanSleepSemaphore;
extern SemaphoreHandle_t xWifiCycleSemaphore;
extern SemaphoreHandle_t xWifiPingSemaphore;

extern SemaphoreHandle_t xWifiRawFramesMutex;

// OTA
extern QueueHandle_t xWifiOtaPctQueue;

/**
 * @brief Create the Wi-Fi task
 */
void wifi_task_create(void);


#endif // WIFI_TASK_H