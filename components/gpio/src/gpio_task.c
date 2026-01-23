#include "freertos/idf_additions.h"
#include "polycast5_macros.h"
#include "polycast5_gpios.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "portmacro.h"

#include "hal/adc_hal.h"
#include "esp_log.h"

#include "gpio_task.h"
#include "gpio_utils.h"

#define TAG "GPIO_TASK"

#define POLL_MS 20
#define REPEAT_NEXT_MS 100
#define LONG_PRESS_THRESHOLD_MS 500 // Trigger long press
#define REPEAT_START_MS (LONG_PRESS_THRESHOLD_MS + 100) // Start repeating short presses

SemaphoreHandle_t xSPIBusMutex;
SemaphoreHandle_t xI2CBusMutex;
SemaphoreHandle_t xHapticsMutex;
SemaphoreHandle_t xRgbLedMutex;
SemaphoreHandle_t xLEDCMutex;
SemaphoreHandle_t xGpioLeftBtnMutex;

SemaphoreHandle_t xPowerButtonSemaphore;
SemaphoreHandle_t xStartAdcBatSemaphore;

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

QueueHandle_t xAdcBatReadingQueue;
QueueHandle_t xAdcBatBluetoothQueue;
QueueHandle_t xLEDQueue;

typedef struct {
    uint8_t pin; // Expander pin number
    uint16_t ticks; // Ticks until next event
    bool prev; // Last sampled state (1 = released, 0 = pressed)
    TickType_t press_start_tick; // When we first saw the press
    bool long_press_fired; // Have we already sent the long-press event
} btn_state_t;

volatile uint8_t haptic_len_ms = 20; // Default buzz 20ms
volatile bool haptic_btns[6] = {true, false, false, false, false, false}; // Default buzz on select

// True while the physical SELECT button is held (0 = pressed, 1 = released)
volatile bool gpio_select_btn_held = false;
volatile bool gpio_left_to_exit = false;
volatile bool gpio_waiting_for_left = false;

int8_t lcd_ledc_brightness = 100;

static const TickType_t adc_timer_interval = pdMS_TO_TICKS(20000); // 20s

static uint8_t rgb_data = 255;

static const uint32_t lcd_max_duty = (1 << LCD_LEDC_RESOLUTION) - 1;

// Buttons and states: same order as buttonSemaphores
static btn_state_t buttons[6] = {
    {TCA9535_USER_BUTTON_SELECT_PIN, 0, 1, 0, false},
    {TCA9535_USER_BUTTON_HOME_PIN,   0, 1, 0, false},
    {TCA9535_USER_BUTTON_UP_PIN,     0, 1, 0, false},
    {TCA9535_USER_BUTTON_DOWN_PIN,   0, 1, 0, false},
    {TCA9535_USER_BUTTON_LEFT_PIN,   0, 1, 0, false},
    {TCA9535_USER_BUTTON_RIGHT_PIN,  0, 1, 0, false},
};

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

// Helper to give the semaphore based on index
static inline void give_short(size_t i) {
    xSemaphoreGive(*shortSems[i]);
}
static inline void give_long(size_t i) {
    xSemaphoreGive(*longSems[i]);
}

static void adc_task(void *arg)
{
    static uint8_t last_percentage = 100;
    
    // Get battery charge on start
    gpio_utils_init_battery_adc();
    float v = gpio_utils_get_battery_voltage();
    #ifdef POLYCAST5_DEBUG_ADC
    ESP_LOGI(TAG, "Startup voltage: %f", v);
    #endif
    gpio_utils_deinit_battery_adc();
        
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
                        
            uint8_t percentage = gpio_utils_volts_to_soc(v);
            
            #ifdef POLYCAST5_DEBUG_ADC
            ESP_LOGI(TAG, "Battery voltage: %f", v);
            ESP_LOGI(TAG, "Battery percentage: %u%%", percentage);
            #endif
            
            // If fluctuating by one, ignore
            if (percentage == last_percentage + 1) {
                percentage = last_percentage;
            } else {
                last_percentage = percentage;
            }
            
            #ifdef POLYCAST5_DEBUG_ADC
            ESP_LOGI(TAG, "NEW battery percentage: %u%%", percentage);
            #endif
            
            // Send value to LCD
            if (xQueueSend(xAdcBatReadingQueue, &percentage, portMAX_DELAY) != pdPASS) {
                ESP_LOGE(TAG, "Failed to send xAdcBatReadingQueue: %u%%", percentage);
            }
            // And to bluetooth
            if (xQueueSend(xAdcBatBluetoothQueue, &percentage, portMAX_DELAY) != pdPASS) {
                ESP_LOGE(TAG, "Failed to send xAdcBatBluetoothQueue: %u%%", percentage);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void gpio_task(void *arg)
{
    xUpButtonSemaphore = xSemaphoreCreateBinary();
    configASSERT(xUpButtonSemaphore);
    xDownButtonSemaphore = xSemaphoreCreateBinary();
    configASSERT(xDownButtonSemaphore);
    xRightButtonSemaphore = xSemaphoreCreateBinary();
    configASSERT(xRightButtonSemaphore);
    xLeftButtonSemaphore = xSemaphoreCreateBinary();
    configASSERT(xLeftButtonSemaphore);
    xHomeButtonSemaphore = xSemaphoreCreateBinary();
    configASSERT(xHomeButtonSemaphore);
    xSelectButtonSemaphore = xSemaphoreCreateBinary();
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
    
    xLEDCSemaphore = xSemaphoreCreateBinary();
    configASSERT(xLEDCSemaphore);

    xGpioLeftBtnMutex = xSemaphoreCreateMutex();
    configASSERT(xGpioLeftBtnMutex);
    
    xAdcBatReadingQueue = xQueueCreate(1, sizeof(uint8_t));
    configASSERT(xAdcBatReadingQueue);
    xAdcBatBluetoothQueue = xQueueCreate(1, sizeof(uint8_t));
    configASSERT(xAdcBatBluetoothQueue);
    xLEDQueue = xQueueCreate(1, sizeof(uint8_t));
    configASSERT(xLEDQueue);

    // Default states set in gpio_utils_init()
    
    #ifdef POLYCAST5_CYCLE_RGB_ON_BOOT
    gpio_utils_cycle_rgb();
    #endif
    
    if (xTaskCreate(adc_task, "adc_task", 1024 * 2, NULL, POLYCAST5_PRIORITY_LOW, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start adc_task");
    }
    
    // Get opposite initial charging state to update once
    bool was_charging = !(gpio_utils_read_input(TCA9535_CHG_IND_PIN) == 0);
    
    while (1) 
    {
        // Press + auto-repeat state machine
        for (size_t i = 0; i < 6; ++i) {
            btn_state_t *b = &buttons[i]; // Get the button
            bool level = gpio_utils_read_input(b->pin); // Read its state: 0 = pressed, 1 = released

            // Track held state for SELECT (buttons[0])
            if (i == 0) {
                gpio_select_btn_held = (level == 0);
            }

            // Button pressed
            if (level == 0) {
                // New press
                if (b->prev == 1) {
                    b->press_start_tick = xTaskGetTickCount();
                    b->long_press_fired = false;
                    b->ticks = REPEAT_START_MS / POLL_MS; // Convert ms to poll cycles
                    
                    // Give haptic feedback if button is enabled
                    xSemaphoreTake(xHapticsMutex, portMAX_DELAY); // Lock haptics
                    if (haptic_btns[i]) {
                        gpio_utils_spin_haptic(haptic_len_ms);
                    }
                    xSemaphoreGive(xHapticsMutex); // Release haptics
                } else { // Else long-press
                    // If not yet fired long press
                    if (!b->long_press_fired) {
                        // Get elapsed time
                        TickType_t held = xTaskGetTickCount() - b->press_start_tick;
                        
                        // If time is above LONG_PRESS_THRESHOLD_MS
                        if (held >= pdMS_TO_TICKS(LONG_PRESS_THRESHOLD_MS)) {
                            b->long_press_fired = true;
                            give_long(i);
                            #ifdef POLYCAST5_DEBUG_GPIO
                            ESP_LOGI(TAG, "Long press fired");
                            #endif
                        }
                    }
                    // Auto-repeat short-press every b->ticks
                    if (b->ticks == 0) {
                        give_short(i);
                        b->ticks = REPEAT_NEXT_MS / POLL_MS;
                        #ifdef POLYCAST5_DEBUG_GPIO
                        ESP_LOGI(TAG, "Auto repeat give short");
                        #endif
                    } else {
                        b->ticks--;
                    }
                }
            } else if (b->prev == 0) { // Button released
                // Reset ticks
                b->ticks = 0;
                
                // If wasn't a long press, give short press
                if (!b->long_press_fired) {
                    xSemaphoreTake(xGpioLeftBtnMutex, portMAX_DELAY); // Lock left button mutex
                    if (gpio_waiting_for_left && i == 4) { // Left button special case to exit lcd loop
                        gpio_left_to_exit = true;
                    } else {
                        give_short(i);
                        gpio_left_to_exit = false;
                    }
                    xSemaphoreGive(xGpioLeftBtnMutex); // Release left button mutex
                    #ifdef POLYCAST5_DEBUG_GPIO
                    ESP_LOGI(TAG, "Btn release give short");
                    #endif
                }
            }
            
            // Set previous
            b->prev = level;
        }
        
        // Go to sleep requested
        if (gpio_utils_read_input(TCA9535_USER_BUTTON_POWER_PIN) == 0) {
            xSemaphoreGive(xPowerButtonSemaphore);
        }
        
        // Update LCD based on if charging or not
        bool is_charging = (gpio_utils_read_input(TCA9535_CHG_IND_PIN) == 0);
        if (is_charging != was_charging) { // Only update on state change
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
        
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        
        //gpio_utils_cycle_rgb(); // Test RGB LED
    }
}

void gpio_task_create(void)
{
    if (xTaskCreate(gpio_task, "gpio_task", 1024 * 2, NULL, POLYCAST5_PRIORITY_MEDIUM, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start gpio_task");
    }
}
