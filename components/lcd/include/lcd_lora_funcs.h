#ifndef LCD_LORA_FUNCS_H
#define LCD_LORA_FUNCS_H

#include "lvgl.h"

#include "esp_err.h"

#define MAX_LORA_OPTIONS 20
#define LORA_PLAN_SUBMENU_COUNT 8

#define LORA_OPTIONS_NS "lo_op_ns" // NVS namespace
#define LORA_OPTIONS_KEY_COUNT "lo_op_ke" // u8: number of option
#define LORA_OPTIONS_KEY_FMT "lo_op%02d" // lo_op00, lo_op01 …

#define LORA_ENC_NS "lo_en_ns" // NVS namespace
#define LORA_ENC_KEY_COUNT "lo_en_ke" // u8: number of user remotes
#define LORA_ENC_KEY_FMT "lo_en%02d" // lo_en00, lo_en01 …

// Forward-declare structs (from lcd_utils.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

typedef struct {
    char *options[MAX_LORA_OPTIONS];
    lv_obj_t *btns[MAX_LORA_OPTIONS];
    int size;
    int index;
    lv_obj_t *cont;
    lv_obj_t *lbl_receipt;
	lv_style_t btn_style;
	lv_style_t sel_style;
} lora_submenu_t;

typedef struct {
    char *options[MAX_LORA_OPTIONS];
    uint8_t *keys[16];
    lv_obj_t *btns[MAX_LORA_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
	lora_submenu_t submenu;
} lora_menu_t;
	
typedef struct {
    lv_obj_t *plan_cont;
    lv_obj_t *plan_btns[LORA_PLAN_SUBMENU_COUNT];
    lv_obj_t *lbl_days_ins;
    lv_style_t plan_btn_style;
    lv_style_t plan_sel_style;
    const char *plan_options[LORA_PLAN_SUBMENU_COUNT];
    int plan_index;
} lora_plan_menu_t;

extern lora_menu_t lora_menu; 
extern lora_plan_menu_t lora_plan_menu; 

/**
 * @brief Creates the central LoRa page then hides it for quick access
 *
 * @param [in] lora_menu LoRa menu structure
 */
void lcd_lora_setup_page(lora_menu_t *lora_menu);

/**
 * @brief Creates the LoRa subpage then hides it for quick access
 *
 * @param [in] lora_menu LoRa menu structure
 */
void lcd_lora_setup_subpage(lora_menu_t *lora_menu);

/**
 * @brief Creates the LoRa plan subpage then hides it for quick access
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] lora_plan_menu LoRa plan menu structure
 */
void lcd_lora_setup_plan_page(ui_menu_t *ui_menu, lora_plan_menu_t *lora_plan_menu);

/**
 * @brief Executes when navigating the LoRa subpage to display LoRa options in a container
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] lora_menu LoRa menu structure
 * @param [in] lora_plan_menu LoRa plan menu structure
 */
void lcd_lora_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu, lora_plan_menu_t *lora_plan_menu);

/**
 * @brief Updates the LoRa menu labels based on user menu input
 *
 * @param [in] lora_menu LoRa menu structure
 */
void lcd_lora_update_menu(lora_menu_t *lora_menu);

/**
 * @brief Updates the LoRa subpage labels based on user submenu input
 *
 * @param [in] lora_menu LoRa menu structure
 */
void lcd_lora_update_submenu(lora_menu_t *lora_menu);

/**
 * @brief Executes when user selects "AWAY" from LoRa subpage
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] lora_menu LoRa menu structure
 */
void lcd_lora_away_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu);

/**
 * @brief Executes when user selects "LOOP" from LoRa subpage
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] lora_menu LoRa menu structure
 */
void lcd_lora_loop_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu);

/**
 * @brief Executes when user selects "PLAN" from LoRa subpage
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] lora_menu LoRa menu structure
 * @param [in] lora_plan_menu LoRa plan menu structure
 */
void lcd_lora_plan_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu, lora_plan_menu_t *lora_plan_menu);

void lcd_lora_plan_confirm_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu, lora_plan_menu_t *lora_plan_menu);

/**
 * @brief Updates plan menu based on user selection
 *
 * @param [in] lora_plan_menu LoRa plan menu structure
 */
void lcd_lora_update_plan_menu(lora_plan_menu_t *lora_plan_menu);

/**
 * @brief Prompts user on how to pair a PolyPlug then sends a Semaphore to generate a random enc key to share
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] lora_menu LoRa menu structure
 */
void lcd_lora_create_enc_key(ui_menu_t *ui_menu, lora_menu_t *lora_menu);

/**
 * @brief Takes user input to create a name for/rename a designated PolyPlug
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] lora_menu LoRa menu structure
 */
void lcd_lora_create_custom_name(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu);

/**
 * @brief Saves new LoRa menu option and name to NVS
 *
 * @param [in] lora_menu LoRa menu structure
 *
 * @returns ESP error status
 */
esp_err_t lcd_lora_menu_nvs_save(const lora_menu_t *lora_menu);

/**
 * @brief Saves new LoRa enc key to NVS
 *
 * @param [in] lora_menu ESP-NOW menu structure
 *
 * @returns ESP error status
 */
esp_err_t lcd_lora_key_nvs_save(const lora_menu_t *lora_menu);

/**
 * @brief Loads all LoRa menu options and names from NVS
 *
 * @param [in] lora_menu ESP-NOW menu structure
 *
 * @returns ESP error status
 */
esp_err_t lcd_lora_menu_nvs_load(lora_menu_t *lora_menu);

/**
 * @brief Loads all LoRa enc keys from NVS
 *
 * @param [in] lora_menu ESP-NOW menu structure
 *
 * @returns ESP error status
 */
esp_err_t lcd_lora_key_nvs_load(lora_menu_t *lora_menu);

/**
 * @brief Deletes a given enc key from NVS
 *
 * @param [in] lora_menu ESP-NOW menu structure
 *
 * @returns ESP error status
 */
esp_err_t lcd_lora_key_nvs_delete(uint8_t del_idx);

/**
 * @brief Deletes a given menu option from NVS
 *
 * @param [in] lora_menu ESP-NOW menu structure
 *
 * @returns ESP error status
 */
esp_err_t lcd_lora_menu_nvs_delete(uint8_t del_idx);

#endif // LCD_LORA_FUNCS_H