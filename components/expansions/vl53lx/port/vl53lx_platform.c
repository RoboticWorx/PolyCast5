/*
 * ESP-IDF platform layer for ST's VL53LX bare driver.
 *
 * The core calls exactly ten of these (RdByte, WrByte, RdWord, WrWord, ReadMulti,
 * WriteMulti, WaitUs, WaitMs, GetTickCount, WaitValueMaskEx); the DWord pair is provided
 * because vl53lx_platform.h declares it and --gc-sections drops what is unused. None of
 * the GPIO or comms-init entry points in that header are referenced by the core, so this
 * module deliberately does not implement them - the module has XSHUT and GPIO1 tied off
 * and everything below is polled.
 *
 * TWO THINGS HERE ARE LOAD-BEARING - read before changing them.
 *
 * 1. VL53LX_WaitMs gives xI2CBusMutex back for the duration of the delay and re-takes it
 *    afterwards. ST's core sleeps inside its own poll loops (boot completion is bounded at
 *    500 ms, range completion at 2000 ms), and holding the shared bus across those stalls
 *    gpio_task's button polling and leaves the haptic motor buzzing - the exact failure the
 *    hand-rolled driver's vl_bus_yield() existed to avoid. The give/take is gated on
 *    s_bus_held so that a call made outside the lock cannot give a mutex it does not own;
 *    vl53l4cx.c sets that flag around every ST API call.
 *
 * 2. Every wait is rounded UP to a whole FreeRTOS tick. CONFIG_FREERTOS_HZ is 100 here, so
 *    pdMS_TO_TICKS() of anything under 10 ms is 0 ticks - and ST polls with
 *    VL53LX_POLLING_DELAY_MS = 1. Left alone that turns every poll loop into a busy-spin
 *    hammering the I2C bus for up to two seconds. One tick (10 ms) is the same interval the
 *    hand-rolled driver polled at and is comfortably finer than the ~100 ms timing budget.
 *
 * Caller must hold xI2CBusMutex for every function here (see note 1 for what WaitMs does
 * with it). Register indices are 16-bit big-endian, as the part requires.
 */

#include "vl53lx_platform.h"
#include "vl53lx_port.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_rom_sys.h"

#include "gpio_task.h" // xI2CBusMutex

#define I2C_TIMEOUT_MS 100

// Whether the caller is holding xI2CBusMutex across the current ST API call. Only
// VL53LX_WaitMs consults it, and only to decide whether it may hand the bus back.
static bool s_bus_held;

void vl53lx_port_set_bus_held(bool held)
{
    s_bus_held = held;
}

static VL53LX_Error vl53lx_status(esp_err_t err)
{
    return (err == ESP_OK) ? VL53LX_ERROR_NONE : VL53LX_ERROR_CONTROL_INTERFACE;
}

VL53LX_Error VL53LX_WriteMulti(VL53LX_DEV Dev, uint16_t index, uint8_t *pdata, uint32_t count)
{
    if (Dev == NULL || Dev->i2c == NULL) {
        return VL53LX_ERROR_CONTROL_INTERFACE;
    }

    // The index and the payload go out as one transaction without being copied into a
    // staging buffer first. The largest block the core writes is the combined
    // static+customer NVM+static+general+timing+dynamic+system config, 134 bytes, which a
    // fixed stack buffer would have had to be sized for and checked against.
    const uint8_t addr[2] = { (uint8_t)(index >> 8), (uint8_t)(index & 0xFF) };
    i2c_master_transmit_multi_buffer_info_t bufs[2] = {
        { .write_buffer = (uint8_t *)addr, .buffer_size = sizeof(addr) },
        { .write_buffer = pdata, .buffer_size = (size_t)count },
    };

    return vl53lx_status(i2c_master_multi_buffer_transmit(Dev->i2c, bufs,
            (count > 0) ? 2 : 1, I2C_TIMEOUT_MS));
}

VL53LX_Error VL53LX_ReadMulti(VL53LX_DEV Dev, uint16_t index, uint8_t *pdata, uint32_t count)
{
    if (Dev == NULL || Dev->i2c == NULL) {
        return VL53LX_ERROR_CONTROL_INTERFACE;
    }

    const uint8_t addr[2] = { (uint8_t)(index >> 8), (uint8_t)(index & 0xFF) };
    return vl53lx_status(i2c_master_transmit_receive(Dev->i2c, addr, sizeof(addr),
            pdata, (size_t)count, I2C_TIMEOUT_MS));
}

VL53LX_Error VL53LX_WrByte(VL53LX_DEV Dev, uint16_t index, uint8_t data)
{
    return VL53LX_WriteMulti(Dev, index, &data, 1);
}

VL53LX_Error VL53LX_WrWord(VL53LX_DEV Dev, uint16_t index, uint16_t data)
{
    uint8_t b[2] = { (uint8_t)(data >> 8), (uint8_t)(data & 0xFF) };
    return VL53LX_WriteMulti(Dev, index, b, sizeof(b));
}

VL53LX_Error VL53LX_WrDWord(VL53LX_DEV Dev, uint16_t index, uint32_t data)
{
    uint8_t b[4] = {
        (uint8_t)(data >> 24), (uint8_t)(data >> 16),
        (uint8_t)(data >> 8), (uint8_t)data,
    };
    return VL53LX_WriteMulti(Dev, index, b, sizeof(b));
}

VL53LX_Error VL53LX_RdByte(VL53LX_DEV Dev, uint16_t index, uint8_t *pdata)
{
    return VL53LX_ReadMulti(Dev, index, pdata, 1);
}

VL53LX_Error VL53LX_RdWord(VL53LX_DEV Dev, uint16_t index, uint16_t *pdata)
{
    uint8_t b[2];
    const VL53LX_Error st = VL53LX_ReadMulti(Dev, index, b, sizeof(b));
    if (st == VL53LX_ERROR_NONE) {
        *pdata = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
    }
    return st;
}

VL53LX_Error VL53LX_RdDWord(VL53LX_DEV Dev, uint16_t index, uint32_t *pdata)
{
    uint8_t b[4];
    const VL53LX_Error st = VL53LX_ReadMulti(Dev, index, b, sizeof(b));
    if (st == VL53LX_ERROR_NONE) {
        *pdata = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
                | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    }
    return st;
}

// Sub-millisecond waits only - too short for vTaskDelay, which cannot express less than
// one 10 ms tick. The bus is kept for the duration; the longest ST asks for here is the
// few hundred microseconds around a soft reset.
VL53LX_Error VL53LX_WaitUs(VL53LX_DEV Dev, int32_t wait_us)
{
    (void)Dev;
    if (wait_us > 0) {
        esp_rom_delay_us((uint32_t)wait_us);
    }
    return VL53LX_ERROR_NONE;
}

// See note 1 and note 2 in the file header - both apply to this function.
VL53LX_Error VL53LX_WaitMs(VL53LX_DEV Dev, int32_t wait_ms)
{
    (void)Dev;
    if (wait_ms <= 0) {
        return VL53LX_ERROR_NONE;
    }

    TickType_t ticks = pdMS_TO_TICKS((uint32_t)wait_ms);
    if (ticks == 0) {
        ticks = 1; // A 100 Hz tick cannot express ST's 1 ms poll interval
    }

    if (s_bus_held) {
        xSemaphoreGive(xI2CBusMutex);
        vTaskDelay(ticks);
        xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    } else {
        vTaskDelay(ticks);
    }
    return VL53LX_ERROR_NONE;
}

VL53LX_Error VL53LX_GetTickCount(VL53LX_DEV Dev, uint32_t *ptime_ms)
{
    (void)Dev;
    *ptime_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    return VL53LX_ERROR_NONE;
}

VL53LX_Error VL53LX_WaitValueMaskEx(VL53LX_DEV Dev, uint32_t timeout_ms, uint16_t index,
        uint8_t value, uint8_t mask, uint32_t poll_delay_ms)
{
    uint32_t start_ms = 0;
    uint32_t now_ms = 0;

    VL53LX_Error status = VL53LX_GetTickCount(Dev, &start_ms);
    if (status != VL53LX_ERROR_NONE) {
        return status;
    }

    for (;;) {
        uint8_t byte_value = 0;
        status = VL53LX_RdByte(Dev, index, &byte_value);
        if (status != VL53LX_ERROR_NONE) {
            return status;
        }
        if ((byte_value & mask) == value) {
            return VL53LX_ERROR_NONE;
        }

        status = VL53LX_GetTickCount(Dev, &now_ms);
        if (status != VL53LX_ERROR_NONE) {
            return status;
        }
        if ((now_ms - start_ms) >= timeout_ms) { // Unsigned, so a tick rollover is harmless
            return VL53LX_ERROR_TIME_OUT;
        }

        status = VL53LX_WaitMs(Dev, (int32_t)poll_delay_ms);
        if (status != VL53LX_ERROR_NONE) {
            return status;
        }
    }
}
