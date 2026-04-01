#include "sx126x_hal.h"

#include "driver/gpio.h"       // For GPIO control
#include "driver/spi_master.h" // For SPI communication

#include <string.h>
#include <stdlib.h>

#include "esp_log.h" // For logging
#include "freertos/FreeRTOS.h" // For vTaskDelay
#include "freertos/task.h" // For task delays

#include "polycast5_gpios.h"
#include "gpio_task.h"
#include "gpio_utils.h"

// Logging tag for debugging
static const char *TAG = "SX126X_HAL";

// Global SPI device handle
static spi_device_handle_t sx126x_spi = NULL;

static bool sx126x_wait_while_busy(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();

    while (gpio_get_level(SX126X_BUSY_PIN) == 1) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return true;
}

// Initialize the SX126x HAL with the SPI handle and configure GPIO pins
void sx126x_hal_init(spi_device_handle_t spi) {
    sx126x_spi = spi;

    // Configure GPIO pins
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SX126X_CS_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    io_conf.pin_bit_mask = (1ULL << SX126X_BUSY_PIN) | (1ULL << SX126X_DIO1_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE; // Internally pulled up in SX1262
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE; // Interrupts handled separately in lora_task.c
    gpio_config(&io_conf);

    // Set initial states
    gpio_set_level(SX126X_CS_PIN, 1); // CS high (inactive)
    gpio_utils_write_output(TCA9535_SX126X_NRST_PIN, 1); // Enable (active low)
}

// Reset the SX126x chip
sx126x_hal_status_t sx126x_hal_reset(const void *context) {
    // Ignore the context parameter
    (void)context;

    // Set reset pin low
    gpio_utils_write_output(TCA9535_SX126X_NRST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10)); // Delay 10ms

    // Set reset pin high
    gpio_utils_write_output(TCA9535_SX126X_NRST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10)); // Delay 10ms

    return SX126X_HAL_STATUS_OK;
}

// Wake up the SX126x chip
sx126x_hal_status_t sx126x_hal_wakeup(const void *context) {
    
    xSemaphoreTake(xSPIBusMutex, portMAX_DELAY); // Lock SPI bus
    
    // Ignore the context parameter
    (void)context;

    if (sx126x_spi == NULL) {
        ESP_LOGE(TAG, "SPI not initialized");
        xSemaphoreGive(xSPIBusMutex); // Release SPI bus before returning
        return SX126X_HAL_STATUS_ERROR;
    }

    // CS low to begin communication
    gpio_set_level(SX126X_CS_PIN, 0);

    // Send a dummy byte (NOP command)
    uint8_t dummy_byte = SX126X_NOP;
    spi_transaction_t trans = {
        .tx_buffer = &dummy_byte,
        .length = 8, // 1 byte (8 bits)
    };

    esp_err_t ret = spi_device_transmit(sx126x_spi, &trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI transmit failed: %s", esp_err_to_name(ret));
        gpio_set_level(SX126X_CS_PIN, 1); // CS high on failure
        xSemaphoreGive(xSPIBusMutex); // Release SPI bus before returning
        return SX126X_HAL_STATUS_ERROR;
    }

    // CS high when done
    gpio_set_level(SX126X_CS_PIN, 1);

    // Wait for SX126x to be ready (BUSY pin goes low)
    if (!sx126x_wait_while_busy(100)) {
        ESP_LOGE(TAG, "SX126x BUSY timeout in wakeup");
        xSemaphoreGive(xSPIBusMutex);
        return SX126X_HAL_STATUS_ERROR;
    }

    xSemaphoreGive(xSPIBusMutex); // Release SPI bus

    return SX126X_HAL_STATUS_OK;
}

// Write data to the SX126x chip
sx126x_hal_status_t sx126x_hal_write( const void      *ctx,
                                      const uint8_t   *cmd,
                                      uint16_t         cmd_len,
                                      const uint8_t   *data,
                                      uint16_t         data_len )
{
    xSemaphoreTake(xSPIBusMutex, portMAX_DELAY); // Lock SPI bus

    (void)ctx;
    // Wait for SX126x to be ready (BUSY pin goes low)
    if (!sx126x_wait_while_busy(100)) {
        ESP_LOGE(TAG, "SX126x BUSY timeout in write");
        xSemaphoreGive(xSPIBusMutex);
        return SX126X_HAL_STATUS_ERROR;
    }

    gpio_set_level(SX126X_CS_PIN, 0);           /* ↓CS */

    /* --- transmit command bytes --- */
    spi_transaction_t t = { 0 };
    t.tx_buffer = cmd;
    t.length    = cmd_len * 8;
    esp_err_t ret = spi_device_polling_transmit(sx126x_spi, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI cmd write failed: %s", esp_err_to_name(ret));
        gpio_set_level(SX126X_CS_PIN, 1);
        xSemaphoreGive(xSPIBusMutex);
        return SX126X_HAL_STATUS_ERROR;
    }

    /* --- transmit payload (if any) --- */
    if (data && data_len) {
        spi_transaction_t t2 = { 0 };           /* fresh struct -> rxlength = 0 */
        t2.tx_buffer = data;
        t2.length    = data_len * 8;
        ret = spi_device_polling_transmit(sx126x_spi, &t2);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI data write failed: %s", esp_err_to_name(ret));
            gpio_set_level(SX126X_CS_PIN, 1);
            xSemaphoreGive(xSPIBusMutex);
            return SX126X_HAL_STATUS_ERROR;
        }
    }

    gpio_set_level(SX126X_CS_PIN, 1);           /* ↑CS */

    xSemaphoreGive(xSPIBusMutex); // Release SPI bus

    return SX126X_HAL_STATUS_OK;
}

/* ---------- READ :  CS low – cmd – dummy/MOSI – read/MISO – CS high ----- */
sx126x_hal_status_t sx126x_hal_read( const void    *ctx,
                                     const uint8_t *cmd,
                                     uint16_t       cmd_len,
                                     uint8_t       *data,
                                     uint16_t       data_len )
{
    xSemaphoreTake(xSPIBusMutex, portMAX_DELAY); // Lock SPI bus
    
    (void)ctx;
    // Wait for SX126x to be ready (BUSY pin goes low)
    if (!sx126x_wait_while_busy(100)) {
        ESP_LOGE(TAG, "SX126x BUSY timeout in read");
        xSemaphoreGive(xSPIBusMutex);
        return SX126X_HAL_STATUS_ERROR;
    }

    gpio_set_level(SX126X_CS_PIN, 0);           /* ↓CS */

    /* send command (bus lock released afterwards) */
    spi_transaction_t t_cmd = { 0 };
    t_cmd.tx_buffer = cmd;
    t_cmd.length    = cmd_len * 8;
    esp_err_t ret = spi_device_polling_transmit(sx126x_spi, &t_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI cmd read failed: %s", esp_err_to_name(ret));
        gpio_set_level(SX126X_CS_PIN, 1);
        xSemaphoreGive(xSPIBusMutex);
        return SX126X_HAL_STATUS_ERROR;
    }

    /* clock out 'data_len' dummy bytes while reading MISO */
    spi_transaction_t t_rd  = { 0 };
    t_rd.tx_buffer = NULL;            /* NULL tx sends zeros */
    t_rd.length    = data_len * 8;    /* bits out  */
    t_rd.rx_buffer = data;
    t_rd.rxlength  = data_len * 8;    /* bits in   */
    ret = spi_device_polling_transmit(sx126x_spi, &t_rd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI data read failed: %s", esp_err_to_name(ret));
        gpio_set_level(SX126X_CS_PIN, 1);
        xSemaphoreGive(xSPIBusMutex);
        return SX126X_HAL_STATUS_ERROR;
    }

    gpio_set_level(SX126X_CS_PIN, 1);           /* ↑CS */

    xSemaphoreGive(xSPIBusMutex); // Release SPI bus

    return SX126X_HAL_STATUS_OK;
}