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

#define WIFI_PORTAL_START_AI_BIT  (1U << 0)
#define WIFI_PORTAL_START_BTC_BIT (1U << 1)
#define WIFI_PORTAL_START_BT_BIT (1U << 2)
extern EventGroupHandle_t xWiFiPortalEventGroup;

extern QueueHandle_t xWifiScanQueue;
extern QueueHandle_t xWifiSelectedNetworkQueue;
extern QueueHandle_t xWifiSniffQueue;
extern QueueHandle_t xWifiBeaconQueue;
extern QueueHandle_t xWifiDataQueue;
extern QueueHandle_t xWifiMqttCmdQueue;
extern QueueHandle_t xWifiPingQueue;

extern SemaphoreHandle_t xWifiStartScanSemaphore;
extern SemaphoreHandle_t xWifiNetworkConnectedSemaphore;
extern SemaphoreHandle_t xWifiNetworkDisconnectedSemaphore;
extern SemaphoreHandle_t xWifiDisconnectSemaphore;
extern SemaphoreHandle_t xWifiConnectingSemaphore;
extern SemaphoreHandle_t xWifiReconnectSemaphore;
extern SemaphoreHandle_t xWifiCanSleepSemaphore;
extern SemaphoreHandle_t xWifiMqttSuccessSemaphore;
extern SemaphoreHandle_t xWifiMqttConnectedSemaphore;
extern SemaphoreHandle_t xWifiMqttDisconnectedSemaphore;
extern SemaphoreHandle_t xWifiCycleSemaphore;
extern SemaphoreHandle_t xWifiPingSemaphore;

// OTA
extern QueueHandle_t xWifiOtaPctQueue;
extern SemaphoreHandle_t xWifiOtaAvailableSemaphore;

/**
 * @brief Create the Wi-Fi task
 */
void wifi_task_create(void);


#endif // WIFI_TASK_H