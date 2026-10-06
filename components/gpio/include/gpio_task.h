#ifndef GPIO_TASK_H
#define GPIO_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/idf_additions.h"

// Buttons, in gpio_task's polling order
typedef enum {
    GPIO_BTN_SELECT = 0,
    GPIO_BTN_HOME,
    GPIO_BTN_UP,
    GPIO_BTN_DOWN,
    GPIO_BTN_LEFT,
    GPIO_BTN_RIGHT,
} gpio_btn_t;

// Mutex
extern SemaphoreHandle_t xSPIBusMutex;
extern SemaphoreHandle_t xI2CBusMutex;
extern SemaphoreHandle_t xHapticsMutex;
extern SemaphoreHandle_t xRgbLedMutex;
extern SemaphoreHandle_t xLEDCMutex;
extern SemaphoreHandle_t xGpioLeftBtnMutex;

// Regular
extern SemaphoreHandle_t xPowerButtonSemaphore;
extern SemaphoreHandle_t xStartAdcBatSemaphore;
extern SemaphoreHandle_t xAdcBatDoneSemaphore; // Given by adc_task when a battery reading + cutoff evaluation finishes

// Short presses
extern SemaphoreHandle_t xUpButtonSemaphore; // Up btn pressed
extern SemaphoreHandle_t xDownButtonSemaphore; // Down btn pressed
extern SemaphoreHandle_t xRightButtonSemaphore; // Right btn pressed
extern SemaphoreHandle_t xLeftButtonSemaphore; // Left btn pressed
extern SemaphoreHandle_t xHomeButtonSemaphore; // Back btn pressed
extern SemaphoreHandle_t xSelectButtonSemaphore; // Select btn pressed

// Long presses
extern SemaphoreHandle_t xSelectButtonLongSemaphore;  
extern SemaphoreHandle_t xHomeButtonLongSemaphore;    
extern SemaphoreHandle_t xUpButtonLongSemaphore;      
extern SemaphoreHandle_t xDownButtonLongSemaphore;    
extern SemaphoreHandle_t xLeftButtonLongSemaphore;    
extern SemaphoreHandle_t xRightButtonLongSemaphore; 

extern SemaphoreHandle_t xIsChargingSemaphore;
extern SemaphoreHandle_t xNotChargingSemaphore;

extern SemaphoreHandle_t xLEDCSemaphore;
extern SemaphoreHandle_t xReadAccelSemaphore;
extern SemaphoreHandle_t xReadMagSemaphore;

// Queues
extern QueueHandle_t xAdcBatReadingQueue;
extern QueueHandle_t xAdcBatBluetoothQueue;
extern QueueHandle_t xLEDQueue;
extern QueueHandle_t xAccelReadingsQueue;
extern QueueHandle_t xMagReadingsQueue;

// Latest raw measured battery voltage (volts), updated by adc_task
extern volatile float gpio_battery_voltage;

// True while the physical button is down, sampled every poll
extern volatile bool gpio_select_btn_held;
extern volatile bool gpio_up_btn_held;
extern volatile bool gpio_down_btn_held;
extern volatile bool gpio_left_btn_held;
extern volatile bool gpio_right_btn_held;

/**
 * @brief  Take a button's waiting long press and end the press that gave it: no more
 *         auto-repeats or release short follow, and a short it already queued is dropped.
 *
 * @param [in] btn Button
 * @param [out] own_short Optional: set true if the last short given came from that same press
 *                        (it was dropped; a copy already taken this tick is the caller's to drop)
 *
 * @returns True if a long press was taken
 */
bool gpio_take_long_press(gpio_btn_t btn, bool *own_short);

/**
 * @brief  End a button's current press: no more long press, auto-repeats or release short,
 *         and a queued repeat is dropped.
 *
 * @param [in] btn Button
 */
void gpio_end_hold(gpio_btn_t btn);

/**
 * @brief  For hold-to-repeat: take the waiting long press only if it came from the press still
 *         held (then as gpio_take_long_press). One from an earlier press is dropped.
 *
 * @param [in] btn Button
 *
 * @returns True if a long press was taken
 */
bool gpio_take_held_long_press(gpio_btn_t btn);

/**
 * @brief  Whether the button's last short press was an auto-repeat rather than a release.
 *
 * @param [in] btn Button
 *
 * @returns True for an auto-repeat
 */
bool gpio_short_was_repeat(gpio_btn_t btn);

/**
 * @brief  lcd_task finished a page pass. A long press left untaken across two passes is dropped
 *         at the button's next press.
 */
void gpio_lcd_pass_done(void);

/**
 * @brief  Number of the button's current (or last) physical press; changes on every new press.
 *
 * @param [in] btn Button
 *
 * @returns Press number, 0 before the first press
 */
uint32_t gpio_press_seq(gpio_btn_t btn);

/**
 * @brief  Drop every queued short press (they count taps).
 */
void gpio_flush_shorts(void);

/**
 * @brief  Drop one button's queued short presses.
 *
 * @param [in] btn Button
 */
void gpio_flush_short(gpio_btn_t btn);


/**
 * @brief  On a page change: end presses held past the long-press point or down when another
 *         button's event fired, drop waiting long presses, and drop or ignore HOME taps (and RIGHT
 *         when landing on the homescreen) for SCREEN_SETTLE_MS: a second press would fire a
 *         homescreen hotkey. Other taps carry over, so quick taps go a level deeper. Raw held flags
 *         are untouched.
 *
 * @param [in] to_home The new page is the homescreen
 */
void gpio_swallow_holds(bool to_home);

/**
 * @brief  A blocking prompt or destructive confirm page is up: drop queued taps and waiting long
 *         presses and end every press still down, so nothing aimed elsewhere confirms it. Raw held
 *         flags are untouched.
 */
void gpio_screen_changed(void);

/**
 * @brief  A button just woke the device from light sleep: a press found down at gpio_task's next
 *         good read is the one that woke it and gives no events. Later presses count. Call before
 *         lcd_task next blocks, so gpio_task can't read in between.
 */
void gpio_woke_by_button(void);

/**
 * @brief  Board hardware revision, for firmware paths that differ between boards. Read from eFuse
 *         BLOCK_USR_DATA bytes 0-1, burned once by flash.py.
 *
 * @returns Hardware revision (POLYCAST5_HW_VERSION_BLANK if the eFuse is blank)
 */
uint16_t gpio_get_hw_version(void);

/**
 * @brief  Create the GPIO task.
 *         Internally it calls gpio_utils_init(), then
 *         polls P0.0–P0.7 and mirrors each bit to P1.0–P1.7.
 */
void gpio_task_create(void);

#endif // GPIO_TASK_H
