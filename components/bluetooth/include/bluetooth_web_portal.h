#ifndef BLUETOOTH_WEB_PORTAL_H
#define BLUETOOTH_WEB_PORTAL_H

#include "esp_err.h"

#define MAX_KEYBOARD_SCRIPTS 100
#define NUM_KEYBOARD_BASE 2
#define BT_SCRIPT_LABEL_MAX_LEN 32
#define BT_SCRIPT_BODY_MAX_LEN 500

#define BT_PORTAL_SSID "PolyCast5-BT-Portal"

#define MAX_CATEGORIES 20
#define BT_CAT_LABEL_MAX_LEN 32

/** 
 * @brief Starts SoftAP and HTTP server for web bluetooth script entry portal
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_web_portal_start(void);

/** 
 * @brief Stops SoftAP and HTTP server for web bluetooth script entry portal
 */
void bluetooth_web_portal_stop(void);

/** 
 * @brief Gets the current web portal IP
 *
 * @returns Web portal IP
 */
const char *bluetooth_web_portal_get_ip(void);


/* =============== NVS =============== */

/** 
 * @brief Gets the number of user-added categories
 *
 * @returns Number of user added categories
 */
uint8_t bluetooth_category_count_get_nvs(void);

/** 
 * @brief Save the category index (cat) for a script index
 *
 * @param [in] idx Index
 * @param [in] cat Category index
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_script_cat_set_nvs(uint8_t idx, uint8_t cat);

/** 
 * @brief Gets the name of a given category
 *
 * @param [in] idx Index of category name to get
 * @param [in] buf Buffer to copy the name into
 * @param [in] buflen Length of buf
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_category_name_get_nvs(uint8_t idx, char *buf, size_t buflen);

/** 
 * @brief Sets (adds/edits) a category name
 *
 * @param [in] idx Index to set (if beyond count, adds)
 * @param [in] name Name to set
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_category_set_nvs(uint8_t idx, const char *name);

/** 
 * @brief Deletes a category and shifts higher ones down, updating script cats
 *
 * @param [in] idx Index to delete
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_category_delete_nvs(uint8_t idx);

/** 
 * @brief Gets the category index of a given script
 *
 * @param [in] idx Index of script category to get
 * @param [out] cat Buffer to copy the category index into
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_script_cat_get_nvs(uint8_t idx, uint8_t *cat);

/** 
 * @brief Gets the number of user-added scripts
 *
 * @returns Number of user added scripts
 */
uint8_t bluetooth_script_count_get_nvs(void);

/** 
 * @brief Gets the label of a given script
 *
 * @param [in] idx Index of script label to get
 * @param [in] buf Buffer to copy the label into
 * @param [in] buflen Length of buf
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_script_label_get_nvs(uint8_t idx, char *buf, size_t buflen);

/** 
 * @brief Gets the body of a given script
 *
 * @param [in] idx Index of script body to get
 * @param [in] buf Buffer to copy the body into
 * @param [in] buflen Length of buf
 * @param [in] outlen Length of the actual body string
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_script_body_get_nvs(uint8_t idx, char *buf, size_t buflen, size_t *outlen);

/** 
 * @brief Save a randomly generated Wi-Fi password to NVS
 *
 * @param [in] val Password string to save
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_wifi_pass_save_nvs(const char *val);

/** 
 * @brief Load the randomly generated Wi-Fi password from NVS
 *
 * @param [out] out Pointer to the string to load the password in to
 * @param [out] out_sz Size of password
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_wifi_pass_load_nvs(char *out, size_t out_sz);

#endif // BLUETOOTH_WEB_PORTAL_H
