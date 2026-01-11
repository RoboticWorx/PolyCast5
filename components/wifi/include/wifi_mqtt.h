#ifndef WIFI_MQTT_H
#define WIFI_MQTT_H

typedef struct {
    uint8_t key[16];
    char payload[4];
} wifi_mqtt_t;

/**
 * @brief Initialize the MQTT client
 */
void wifi_mqtt_client_init(void);

/**
 * @brief Destroy/deinitialize the MQTT client
 */
void wifi_mqtt_client_destroy(void);

/**
 * @brief Disconnect the MQTT client
 */
void wifi_mqtt_client_disconnect(void);

/**
 * @brief Stop the MQTT client
 */
void wifi_mqtt_client_stop(void);

/**
 * @brief Start the MQTT client
 */
void wifi_mqtt_client_start(void);

/**
 * @brief Sends data via MQTT to receiver
 *
 * @param [in] payload Data to send
 * @param [in] key Unique topic key to filter
 */
void wifi_mqtt_client_publish(char *payload, const uint8_t key[16]);

#endif // WIFI_MQTT_H