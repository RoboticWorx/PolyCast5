#include "freertos/idf_additions.h"
#include "polycast5_macros.h"
#include "polycast5_gpios.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "portmacro.h"

#include "hal/adc_hal.h"
#include "esp_log.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"

#include "lis2dh12.h"
#include "mmc5603.h"
#include "gpio_task.h"
#include "gpio_utils.h"

#define TAG "GPIO_TASK"

#define POLL_MS 20
#define REPEAT_NEXT_MS 100
#define LONG_PRESS_THRESHOLD_MS 500 // Trigger long press
#define REPEAT_START_MS (LONG_PRESS_THRESHOLD_MS + 100) // Start repeating short presses
#define READ_FAIL_RELEASE_POLLS 10 // Failed input reads in a row before buttons count as released
#define SHORT_PRESS_QUEUE 4 // Taps each short semaphore counts: lcd_task takes one per 200 ms pass
#define SCREEN_SETTLE_MS 250 // A HOME/RIGHT press starting this soon after a page change wasn't aimed at it

SemaphoreHandle_t xSPIBusMutex;
SemaphoreHandle_t xI2CBusMutex;
SemaphoreHandle_t xHapticsMutex;
SemaphoreHandle_t xRgbLedMutex;
SemaphoreHandle_t xLEDCMutex;
SemaphoreHandle_t xGpioLeftBtnMutex;

SemaphoreHandle_t xPowerButtonSemaphore;
SemaphoreHandle_t xStartAdcBatSemaphore;
SemaphoreHandle_t xAdcBatDoneSemaphore;

// Short presses
SemaphoreHandle_t xUpButtonSemaphore;
SemaphoreHandle_t xDownButtonSemaphore;
SemaphoreHandle_t xRightButtonSemaphore;
SemaphoreHandle_t xLeftButtonSemaphore;
SemaphoreHandle_t xHomeButtonSemaphore;
SemaphoreHandle_t xSelectButtonSemaphore;

// Long presses
SemaphoreHandle_t xSelectButtonLongSemaphore;  
SemaphoreHandle_t xHomeButtonLongSemaphore;    
SemaphoreHandle_t xUpButtonLongSemaphore;      
SemaphoreHandle_t xDownButtonLongSemaphore;    
SemaphoreHandle_t xLeftButtonLongSemaphore;    
SemaphoreHandle_t xRightButtonLongSemaphore; 

SemaphoreHandle_t xIsChargingSemaphore;
SemaphoreHandle_t xNotChargingSemaphore;

SemaphoreHandle_t xLEDCSemaphore;
SemaphoreHandle_t xReadAccelSemaphore;
SemaphoreHandle_t xReadMagSemaphore;

QueueHandle_t xAdcBatReadingQueue;
QueueHandle_t xAdcBatBluetoothQueue;
QueueHandle_t xLEDQueue;
QueueHandle_t xAccelReadingsQueue;
QueueHandle_t xMagReadingsQueue;

typedef struct {
    uint8_t pin; // Expander pin number
    uint16_t ticks; // Ticks until next event
    bool prev; // Last sampled state (1 = released, 0 = pressed)
    TickType_t press_start_tick; // When we first saw the press
    bool long_press_fired; // Have we already sent the long-press event
    bool repeats; // Auto-repeat shorts while held
    bool repeated; // This hold already gave a repeat short
    uint32_t long_pass; // lcd_passes when the last long press was given
} btn_state_t;

volatile uint8_t haptic_len_ms = 20; // Default buzz 20ms
volatile bool haptic_btns[6] = {true, false, false, false, false, false}; // Default buzz on select

// True while the physical button is held (0 = pressed, 1 = released).
// Raw held-state for hold-to-act input the short/long semaphores can't express
// (e.g. Doom movement, push-to-talk, SELECT hold-to-repeat)
volatile bool gpio_select_btn_held = false;
volatile bool gpio_up_btn_held = false;
volatile bool gpio_down_btn_held = false;
volatile bool gpio_left_btn_held = false;
volatile bool gpio_right_btn_held = false;
volatile bool gpio_left_to_exit = false;
volatile bool gpio_waiting_for_left = false;

int8_t lcd_ledc_brightness = 100;

static const TickType_t adc_timer_interval = pdMS_TO_TICKS(20000); // 20s

// Latest raw measured battery voltage (volts, pre-SoC-offset), updated by adc_task
volatile float gpio_battery_voltage = 0.0f;

static uint8_t rgb_data = 255;

static const uint32_t lcd_max_duty = (1 << LCD_LEDC_RESOLUTION) - 1;

// Buttons and states: same order as shortSems/longSems and gpio_btn_t
// SELECT and HOME confirm or leave a page, so they never repeat: a hold no page consumes gives one short on release
static btn_state_t buttons[6] = {
    {TCA9535_USER_BUTTON_SELECT_PIN, 0, 1, 0, false, false, false, 0},
    {TCA9535_USER_BUTTON_HOME_PIN,   0, 1, 0, false, false, false, 0},
    {TCA9535_USER_BUTTON_UP_PIN,     0, 1, 0, false, true,  false, 0},
    {TCA9535_USER_BUTTON_DOWN_PIN,   0, 1, 0, false, true,  false, 0},
    {TCA9535_USER_BUTTON_LEFT_PIN,   0, 1, 0, false, true,  false, 0},
    {TCA9535_USER_BUTTON_RIGHT_PIN,  0, 1, 0, false, true,  false, 0},
};

// Per-button press numbers
// A press is swallowed (no more long, repeats or release short) once the LCD consumed it
static volatile uint32_t press_seq[6] = {0}; // Bumped on every new press
static volatile uint32_t long_seq[6] = {0}; // Press that gave the waiting long press
static volatile uint32_t swallow_seq[6] = {0}; // Last press the LCD consumed
static volatile bool short_repeat[6] = {false}; // Last short given was an auto-repeat (not a release)
static volatile uint32_t short_seq[6] = {0}; // Press that gave the last short
static volatile uint32_t overlap_seq[6] = {0}; // Press that was down when another button's event was given

static volatile uint32_t lcd_passes = 0; // Page passes lcd_task has finished
static volatile TickType_t btn_settle_until[6] = {0}; // Per button: new presses are swallowed until this tick
static volatile bool wake_read_pending = false; // A button woke the device: the next good read finds its press

static inline bool swallowed(size_t i)
{
    return swallow_seq[i] == press_seq[i];
}

// Button i is giving an event: every other button down now overlaps it, so if the event changes
// the page, those presses belong to the page just left
static void mark_overlaps(size_t i)
{
    for (size_t j = 0; j < 6; ++j) {
        if (j != i && buttons[j].prev == 0) {
            overlap_seq[j] = press_seq[j];
        }
    }
}

// Within SCREEN_SETTLE_MS before until. Unsigned, so an old deadline never reads as future after a wrap
static inline bool settling(TickType_t until)
{
    TickType_t left = until - xTaskGetTickCount();
    return left != 0 && left <= pdMS_TO_TICKS(SCREEN_SETTLE_MS);
}

// Drop button btn's queued taps, end its press if down, and ignore its new presses for SCREEN_SETTLE_MS
static void settle_button(gpio_btn_t btn)
{
    gpio_flush_short(btn);
    if (buttons[btn].prev == 0) {
        swallow_seq[btn] = press_seq[btn];
    }
    btn_settle_until[btn] = xTaskGetTickCount() + pdMS_TO_TICKS(SCREEN_SETTLE_MS);
}

// Raw held state for the buttons that have a flag (HOME has none)
static inline void publish_held(size_t i, bool held)
{
    switch (i) {
        case 0: gpio_select_btn_held = held; break; // SELECT
        case 2: gpio_up_btn_held     = held; break; // UP
        case 3: gpio_down_btn_held   = held; break; // DOWN
        case 4: gpio_left_btn_held   = held; break; // LEFT
        case 5: gpio_right_btn_held  = held; break; // RIGHT
    }
}

static SemaphoreHandle_t *shortSems[6] = {
    &xSelectButtonSemaphore,
    &xHomeButtonSemaphore,
    &xUpButtonSemaphore,
    &xDownButtonSemaphore,
    &xLeftButtonSemaphore,
    &xRightButtonSemaphore,
};

static SemaphoreHandle_t *longSems[6] = {
    &xSelectButtonLongSemaphore,
    &xHomeButtonLongSemaphore,
    &xUpButtonLongSemaphore,
    &xDownButtonLongSemaphore,
    &xLeftButtonLongSemaphore,
    &xRightButtonLongSemaphore,
};

// Give, unless the LCD swallowed this press
// Re-checked after the give: lcd_task (higher priority) can swallow the press between the check and the give
static void give_unless_swallowed(SemaphoreHandle_t sem, size_t i)
{
    if (swallowed(i)) {
        return;
    }
    if (xSemaphoreGive(sem) == pdTRUE && swallowed(i)) {
        xSemaphoreTake(sem, 0);
    }
}

// Helper to give the semaphore based on index
static inline void give_short(size_t i) {
    give_unless_swallowed(*shortSems[i], i);
}
static inline void give_long(size_t i) {
    give_unless_swallowed(*longSems[i], i);
}

bool gpio_take_long_press(gpio_btn_t btn, bool *own_short)
{
    // Semaphores are created by gpio_task itself, possibly after lcd_task starts
    if (*longSems[btn] == NULL || xSemaphoreTake(*longSems[btn], 0) != pdTRUE) {
        return false;
    }

    // End the press that gave it. An older one has ended already, and moving swallow_seq back
    // could revive a newer press that was ended
    if (long_seq[btn] == press_seq[btn]) {
        swallow_seq[btn] = long_seq[btn];
    }

    // Drop its own repeat or release short, never a newer press's tap
    bool own = (short_seq[btn] == long_seq[btn]);
    if (own) {
        xSemaphoreTake(*shortSems[btn], 0);
    }
    if (own_short) {
        *own_short = own;
    }

    return true;
}

void gpio_end_hold(gpio_btn_t btn)
{
    swallow_seq[btn] = press_seq[btn];
    if (*shortSems[btn] != NULL && short_seq[btn] == press_seq[btn]) {
        xSemaphoreTake(*shortSems[btn], 0); // Its queued repeat, not an earlier tap
    }
}

void gpio_flush_short(gpio_btn_t btn)
{
    while (*shortSems[btn] != NULL && xSemaphoreTake(*shortSems[btn], 0) == pdTRUE) {
    }
}

void gpio_flush_shorts(void)
{
    for (size_t i = 0; i < 6; ++i) {
        gpio_flush_short((gpio_btn_t)i);
    }
}

bool gpio_take_held_long_press(gpio_btn_t btn)
{
    if (*longSems[btn] == NULL) {
        return false;
    }

    // A long from an earlier press was already served by its release short: drop it
    if (long_seq[btn] != press_seq[btn] || buttons[btn].prev != 0) {
        if (long_seq[btn] != press_seq[btn]) {
            xSemaphoreTake(*longSems[btn], 0);
        }
        return false;
    }

    return gpio_take_long_press(btn, NULL);
}

uint32_t gpio_press_seq(gpio_btn_t btn)
{
    return press_seq[btn];
}

bool gpio_short_was_repeat(gpio_btn_t btn)
{
    return short_repeat[btn];
}

void gpio_lcd_pass_done(void)
{
    lcd_passes++;
}

void gpio_swallow_holds(bool to_home)
{
    for (size_t i = 0; i < 6; ++i) {
        if (*longSems[i] == NULL || *shortSems[i] == NULL) {
            continue;
        }
        xSemaphoreTake(*longSems[i], 0); // Given on the page just left

        // Presses past the long-press point, or down when another button's event fired, belong to the
        // page just left. One started after that event is a fresh tap for the new page
        if (buttons[i].prev == 0 && (buttons[i].long_press_fired || overlap_seq[i] == press_seq[i])) {
            swallow_seq[i] = press_seq[i];
            if (buttons[i].repeated && short_seq[i] == press_seq[i]) {
                xSemaphoreTake(*shortSems[i], 0); // Its queued repeat
            }
        }
    }

    // HOME never goes a level deeper, and on the homescreen HOME and RIGHT fire hotkeys: a double
    // tap's second press must not
    settle_button(GPIO_BTN_HOME);
    if (to_home) {
        settle_button(GPIO_BTN_RIGHT);
    }
}

void gpio_screen_changed(void)
{
    // A press only counts on the screen it was made on: queued taps, waiting long presses and presses
    // still down were all aimed at the old one
    gpio_flush_shorts();
    for (size_t i = 0; i < 6; ++i) {
        if (*longSems[i] != NULL) {
            xSemaphoreTake(*longSems[i], 0);
        }
        if (buttons[i].prev == 0) {
            swallow_seq[i] = press_seq[i];
        }
    }
}

void gpio_woke_by_button(void)
{
    wake_read_pending = true;
}

uint16_t gpio_get_hw_version(void)
{
    uint16_t ver = 0; // Stays 0 while the eFuse is blank
    esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &ver, 16);
    return ver ? ver : POLYCAST5_HW_VERSION_BLANK;
}

// Release or re-assert the 3V3_EN power latch based on measured battery voltage
static void battery_power_latch_update(float vbat)
{
    static bool gpio_battery_latched_off = false;

    // Latch state is shared with the boot check (gpio_utils.c) so a cutoff that
    // happened before the tasks started is still re-asserted on recovery here
    if (vbat < BATT_CUTOFF_MIN_VALID) {
        return; // Failed / implausible ADC read: ignore
    }

    if (vbat < BATT_CUTOFF_VBAT) {
        if (!gpio_battery_latched_off) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGW(TAG, "Battery %.2fV below %.2fV cutoff; dropping 3V3_EN to power off", vbat, BATT_CUTOFF_VBAT);
#endif

            // Only latch once the write actually lands: on I2C failure leave the
            // flag clear so the next reading retries instead of silently giving
            // up on the cutoff
            if (gpio_utils_write_output(TCA9535_3V3_EN_PIN, 0) == ESP_OK) { // USB 5V holds power if present
                gpio_battery_latched_off = true;
            }
        }
    } else if (vbat >= BATT_CUTOFF_RECOVER && gpio_battery_latched_off) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Battery %.2fV recovered; re-asserting 3V3_EN latch", vbat);
#endif
        if (gpio_utils_write_output(TCA9535_3V3_EN_PIN, 1) == ESP_OK) {
            gpio_battery_latched_off = false; // Retry on next reading if the write failed
        }
    }
}

static void adc_task(void *arg)
{
    static uint8_t last_percentage = 100;
    static bool rise_pending = false; // Last reading was a held-back last_percentage + 1
    
    // Get battery charge on start
    gpio_utils_init_battery_adc();
    float v = gpio_utils_get_battery_voltage();
#ifdef POLYCAST5_DEBUG_ADC
    ESP_LOGI(TAG, "Startup voltage: %f", v);
#endif
    gpio_utils_deinit_battery_adc();
    battery_power_latch_update(v);
    if (v >= BATT_CUTOFF_MIN_VALID) {
        gpio_battery_voltage = v; // Expose the latest reading (also goes to system info page)
    }

    uint8_t percentage = gpio_utils_volts_to_soc(v);
#ifdef POLYCAST5_DEBUG_ADC
    ESP_LOGI(TAG, "Startup percentage: %u%%", percentage);
#endif
    
    last_percentage = percentage;
    
    // Send startup value to LCD
    if (xQueueSend(xAdcBatReadingQueue, &percentage, portMAX_DELAY) != pdPASS) {
        ESP_LOGE(TAG, "Failed to send xAdcBatReadingQueue: %%%u", percentage);
    }
    
    TickType_t adc_timer_last = xTaskGetTickCount();
    
    while (1) {
        // Update battery status every adc_timer_interval
        if ((xTaskGetTickCount() - adc_timer_last >= adc_timer_interval) || (xSemaphoreTake(xStartAdcBatSemaphore, 0) == pdTRUE)) {
            adc_timer_last = xTaskGetTickCount();
                
            gpio_utils_init_battery_adc();
            float v = gpio_utils_get_battery_voltage();
            gpio_utils_deinit_battery_adc();
            battery_power_latch_update(v);
            xSemaphoreGive(xAdcBatDoneSemaphore); // Reading + cutoff evaluation done (the sleep loop waits on this)

            // A failed ADC read returns 0.0f: keep the last shown gauge/voltage
            // rather than blipping to 1% / "0.00 V" (the cutoff already ignores it)
            if (v < BATT_CUTOFF_MIN_VALID) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            gpio_battery_voltage = v; // Expose the latest reading

            uint8_t percentage = gpio_utils_volts_to_soc(v);
            
#ifdef POLYCAST5_DEBUG_ADC
            ESP_LOGI(TAG, "Battery voltage: %f", v);
            ESP_LOGI(TAG, "Battery percentage: %u%%", percentage);
#endif
            
            // Hold back a +1 as jitter, but take a second 100 in a row: 99 can't rise by 2
            if (percentage == last_percentage + 1 && !(percentage == 100 && rise_pending)) {
                rise_pending = true;
                percentage = last_percentage;
            } else {
                rise_pending = false;
                last_percentage = percentage;
            }
            
#ifdef POLYCAST5_DEBUG_ADC
            ESP_LOGI(TAG, "NEW battery percentage: %u%%", percentage);
#endif
            
            // Send value to LCD
            if (xQueueOverwrite(xAdcBatReadingQueue, &percentage) != pdPASS) {
                ESP_LOGE(TAG, "Failed to send xAdcBatReadingQueue: %u%%", percentage);
            }
            // And to bluetooth
            if (xQueueOverwrite(xAdcBatBluetoothQueue, &percentage) != pdPASS) {
                ESP_LOGE(TAG, "Failed to send xAdcBatBluetoothQueue: %u%%", percentage);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void gpio_task(void *arg)
{
    // Counting: taps closer together than one lcd_task pass each count
    xUpButtonSemaphore = xSemaphoreCreateCounting(SHORT_PRESS_QUEUE, 0);
    configASSERT(xUpButtonSemaphore);
    xDownButtonSemaphore = xSemaphoreCreateCounting(SHORT_PRESS_QUEUE, 0);
    configASSERT(xDownButtonSemaphore);
    xRightButtonSemaphore = xSemaphoreCreateCounting(SHORT_PRESS_QUEUE, 0);
    configASSERT(xRightButtonSemaphore);
    xLeftButtonSemaphore = xSemaphoreCreateCounting(SHORT_PRESS_QUEUE, 0);
    configASSERT(xLeftButtonSemaphore);
    xHomeButtonSemaphore = xSemaphoreCreateCounting(SHORT_PRESS_QUEUE, 0);
    configASSERT(xHomeButtonSemaphore);
    xSelectButtonSemaphore = xSemaphoreCreateCounting(SHORT_PRESS_QUEUE, 0);
    configASSERT(xSelectButtonSemaphore);
    
    xUpButtonLongSemaphore = xSemaphoreCreateBinary();
    configASSERT(xUpButtonLongSemaphore);
    xSelectButtonLongSemaphore = xSemaphoreCreateBinary();
    configASSERT(xSelectButtonLongSemaphore);
    xHomeButtonLongSemaphore = xSemaphoreCreateBinary();
    configASSERT(xHomeButtonLongSemaphore);
    xDownButtonLongSemaphore = xSemaphoreCreateBinary();
    configASSERT(xDownButtonLongSemaphore);
    xLeftButtonLongSemaphore = xSemaphoreCreateBinary();
    configASSERT(xLeftButtonLongSemaphore);
    xRightButtonLongSemaphore = xSemaphoreCreateBinary();
    configASSERT(xRightButtonLongSemaphore);
    
    xIsChargingSemaphore = xSemaphoreCreateBinary();
    configASSERT(xIsChargingSemaphore);
    xNotChargingSemaphore = xSemaphoreCreateBinary();
    configASSERT(xNotChargingSemaphore);
    
    xStartAdcBatSemaphore = xSemaphoreCreateBinary();
    configASSERT(xStartAdcBatSemaphore);
    xAdcBatDoneSemaphore = xSemaphoreCreateBinary();
    configASSERT(xAdcBatDoneSemaphore);

    xLEDCSemaphore = xSemaphoreCreateBinary();
    configASSERT(xLEDCSemaphore);
    xReadAccelSemaphore = xSemaphoreCreateBinary();
    configASSERT(xReadAccelSemaphore);
    xReadMagSemaphore = xSemaphoreCreateBinary();
    configASSERT(xReadMagSemaphore);

    xGpioLeftBtnMutex = xSemaphoreCreateMutex();
    configASSERT(xGpioLeftBtnMutex);
    
    xAdcBatReadingQueue = xQueueCreate(1, sizeof(uint8_t));
    configASSERT(xAdcBatReadingQueue);
    xAdcBatBluetoothQueue = xQueueCreate(1, sizeof(uint8_t));
    configASSERT(xAdcBatBluetoothQueue);
    xLEDQueue = xQueueCreate(1, sizeof(uint8_t));
    configASSERT(xLEDQueue);
    xAccelReadingsQueue = xQueueCreate(1, sizeof(accel_deg_t));
    configASSERT(xAccelReadingsQueue);
    xMagReadingsQueue = xQueueCreate(1, sizeof(mmc5603_reading_t));
    configASSERT(xMagReadingsQueue);

    // Default states set in gpio_utils_init()
    
#ifdef POLYCAST5_CYCLE_RGB_ON_BOOT
    gpio_utils_cycle_rgb();
#endif
    
    if (xTaskCreate(adc_task, "adc_task", 1024 * 2, NULL, POLYCAST5_PRIORITY_LOW, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start adc_task");
    }
    
    // Get opposite initial charging state to update once
    bool was_charging = !(gpio_utils_read_input(TCA9535_CHG_IND_PIN) == 0);

    uint8_t read_fails = 0; // Failed input reads in a row

    while (1)
    {
        // One I2C read serves every input below
        uint8_t tca_inputs = 0xFF; // 0xFF on failure = all released (active low)
        bool inputs_ok = (gpio_utils_read_inputs(&tca_inputs) == ESP_OK);
        read_fails = inputs_ok ? 0 : (read_fails < UINT8_MAX ? read_fails + 1 : read_fails);

        // A brief read glitch is no change; a lasting fault releases everything instead of holding forever
        bool buttons_valid = inputs_ok || read_fails > READ_FAIL_RELEASE_POLLS;

        // First good read since a button wake: a press found down now is the one that woke the device
        bool wake_read = inputs_ok && wake_read_pending;
        if (wake_read) {
            wake_read_pending = false;
        }

        // Press + auto-repeat state machine
        for (size_t i = 0; buttons_valid && i < 6; ++i) {
            btn_state_t *b = &buttons[i]; // Get the button
            bool level = (tca_inputs >> b->pin) & 0x1; // Its state: 0 = pressed, 1 = released

            // Raw held state: cleared before a release is handled, set after a new press is numbered
            if (level == 1) {
                publish_held(i, false);
            }

            // Button pressed
            if (level == 0) {
                // New press
                if (b->prev == 1) {
                    // A long press a whole page pass went by without taking was not wanted. A fresher
                    // one stays: its press is told apart by long_seq
                    if (lcd_passes - b->long_pass >= 2) {
                        xSemaphoreTake(*longSems[i], 0);
                    }

                    // The last hold's repeat, still queued, would arrive ahead of this tap. It is the
                    // only one of this button queued: repeats are given only when none is
                    if (short_repeat[i] && short_seq[i] == press_seq[i]) {
                        xSemaphoreTake(*shortSems[i], 0);
                    }
                    press_seq[i]++;
                    if (wake_read || settling(btn_settle_until[i])) {
                        swallow_seq[i] = press_seq[i]; // The wake press, or HOME/RIGHT too soon after a page change
                    }

                    b->press_start_tick = xTaskGetTickCount();
                    b->long_press_fired = false;
                    b->repeated = false;
                    b->ticks = REPEAT_START_MS / POLL_MS; // Convert ms to poll cycles

                    // Down from here: the haptic write below can block, and a screen change meanwhile
                    // must see this press
                    b->prev = 0;
                    publish_held(i, true);

                    // Give haptic feedback if button is enabled
                    xSemaphoreTake(xHapticsMutex, portMAX_DELAY); // Lock haptics
                    if (haptic_btns[i]) {
                        gpio_utils_spin_haptic(haptic_len_ms);
                    }
                    xSemaphoreGive(xHapticsMutex); // Release haptics
                } else if (!swallowed(i)) { // Else long-press, unless the LCD consumed this press
                    // If not yet fired long press
                    if (!b->long_press_fired) {
                        // Get elapsed time
                        TickType_t held = xTaskGetTickCount() - b->press_start_tick;
                        
                        // If time is above LONG_PRESS_THRESHOLD_MS
                        if (held >= pdMS_TO_TICKS(LONG_PRESS_THRESHOLD_MS)) {
                            b->long_press_fired = true;
                            b->long_pass = lcd_passes;
                            long_seq[i] = press_seq[i];
                            mark_overlaps(i);
                            give_long(i);
#ifdef POLYCAST5_DEBUG_GPIO
                            ESP_LOGI(TAG, "Long press fired");
#endif
                        }
                    }
                    // Auto-repeat short-press every b->ticks
                    if (b->repeats) {
                        if (b->ticks == 0) {
                            mark_overlaps(i);
                            xSemaphoreTake(xGpioLeftBtnMutex, portMAX_DELAY); // Lock left button mutex
                            if (gpio_waiting_for_left && i == 4) { // A held LEFT also exits the lcd loop
                                gpio_left_to_exit = true;
                            } else if (uxSemaphoreGetCount(*shortSems[i]) == 0) { // Repeats never queue up
                                short_repeat[i] = true;
                                short_seq[i] = press_seq[i];
                                give_short(i);
                            }
                            xSemaphoreGive(xGpioLeftBtnMutex); // Release left button mutex
                            b->repeated = true;
                            b->ticks = REPEAT_NEXT_MS / POLL_MS;
#ifdef POLYCAST5_DEBUG_GPIO
                            ESP_LOGI(TAG, "Auto repeat give short");
#endif
                        } else {
                            b->ticks--;
                        }
                    }
                }
            } else if (b->prev == 0) { // Button released
                // Reset ticks
                b->ticks = 0;
                
                // Short for any press the LCD didn't consume, unless repeats already served it
                if (!swallowed(i) && !b->repeated) {
                    mark_overlaps(i);
                    xSemaphoreTake(xGpioLeftBtnMutex, portMAX_DELAY); // Lock left button mutex
                    if (gpio_waiting_for_left && i == 4) { // Left button special case to exit lcd loop
                        gpio_left_to_exit = true;
                    } else {
                        short_repeat[i] = false;
                        short_seq[i] = press_seq[i];
                        give_short(i);
                        gpio_left_to_exit = false;
                    }
                    xSemaphoreGive(xGpioLeftBtnMutex); // Release left button mutex
#ifdef POLYCAST5_DEBUG_GPIO
                    ESP_LOGI(TAG, "Btn release give short");
#endif
                }
            }
            
            if (level == 0) {
                publish_held(i, true);
            }

            // Set previous
            b->prev = level;
        }

        if (xSemaphoreTake(xReadAccelSemaphore, 0) == pdTRUE) {
            float pitch, roll;
            if (lis2dh12_read_deg(&pitch, &roll) == ESP_OK) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Accel deg: pitch=%.3f roll=%.3f", pitch, roll);
#endif
                accel_deg_t accel_deg = {
                    .pitch = pitch,
                    .roll = roll,
                };

                // Send to LCD
                xQueueOverwrite(xAccelReadingsQueue, &accel_deg);
            }
        }

        // Magnetometer read, triggered independently so pages without a compass don't pay for it
        if (xSemaphoreTake(xReadMagSemaphore, 0) == pdTRUE) {
            float mx, my, mz;
            if (mmc5603_read_ut(&mx, &my, &mz) == ESP_OK) {
                mmc5603_reading_t mag = { .x = mx, .y = my, .z = mz };
                xQueueOverwrite(xMagReadingsQueue, &mag);
            }
        }

        // Go to sleep requested. Same snapshot, so every input in this pass is consistent
        if (((tca_inputs >> TCA9535_USER_BUTTON_POWER_PIN) & 0x1) == 0) {
            xSemaphoreGive(xPowerButtonSemaphore);
        }
        
        // Update LCD based on if charging or not
        bool is_charging = (((tca_inputs >> TCA9535_CHG_IND_PIN) & 0x1) == 0);
        if (inputs_ok && is_charging != was_charging) { // Only update on state change
            // LiPo is charging    
            if (is_charging) {
                xSemaphoreGive(xIsChargingSemaphore);
            } else { // LiPo is not charging
                xSemaphoreGive(xNotChargingSemaphore);
            }
            xSemaphoreGive(xStartAdcBatSemaphore); // Update battery reading
            
            was_charging = is_charging;
        }
        
        // RGB LED handling
        if (xQueueReceive(xLEDQueue, &rgb_data, 0) == pdTRUE) {
            gpio_utils_rgb_indicate(rgb_data);
        }
        
        // Update LEDC
        if (xSemaphoreTake(xLEDCSemaphore, 0) == pdTRUE) {
            xSemaphoreTake(xLEDCMutex, portMAX_DELAY); // Lock LEDC
            // Clamp values
            if (lcd_ledc_brightness > 100) {
                lcd_ledc_brightness = 100;
            } else if (lcd_ledc_brightness < 0) {
                lcd_ledc_brightness = 0;
            }

            // Scale to duty cycle (higher duty = brighter)
            uint32_t duty = (lcd_ledc_brightness * lcd_max_duty) / 100;

            // Set and update duty
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CHANNEL, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CHANNEL);
            
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Brightness set to %u%% (duty: %u)\n", lcd_ledc_brightness, duty);
#endif
            xSemaphoreGive(xLEDCMutex); // Release LEDC
        }
        
        // Backstop for a lost haptic OFF write
        gpio_utils_haptic_watchdog();

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        //gpio_utils_cycle_rgb(); // Test RGB LED
    }
}

void gpio_task_create(void)
{
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Hardware version: v%u", gpio_get_hw_version());
#endif

    if (xTaskCreate(gpio_task, "gpio_task", 1024 * 2, NULL, POLYCAST5_PRIORITY_MEDIUM, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start gpio_task");
    }
}
