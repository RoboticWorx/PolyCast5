/*
 * Device handle for ST's VL53LX bare driver, PolyCast5 port.
 *
 * Replaces the x-cube-tof1 version, which wires the handle to ST's BSP layer
 * (VL53L4CX_Object_t, function-pointer IO, <math.h>). None of that is wanted here: the
 * BSP result structs are the only float in the whole driver, and the ESP32-C5 has no FPU.
 *
 * The ONLY departure from ST's shape is the i2c handle carried alongside Data, so the
 * platform layer can reach the bus from the VL53LX_DEV the core hands it. ST's own
 * reference ports do the same thing with an address field.
 */

#ifndef _VL53LX_PLATFORM_USER_DATA_H_
#define _VL53LX_PLATFORM_USER_DATA_H_

#include "vl53lx_def.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VL53LX_DevData_t Data; /*!< Bare driver context - 9.4 kB, lives in PSRAM (see vl53l4cx.c) */
    i2c_master_dev_handle_t i2c; /*!< Set once by vl53l4cx_init() before any API call */
} VL53LX_Dev_t;

typedef VL53LX_Dev_t *VL53LX_DEV;

#define VL53LXDevDataGet(Dev, field) (Dev->Data.field)
#define VL53LXDevDataSet(Dev, field, data) ((Dev->Data.field) = (data))
#define PALDevDataGet(Dev, field) (Dev->Data.field)
#define PALDevDataSet(Dev, field, value) ((Dev->Data.field) = (value))
#define VL53LXDevStructGetLLDriverHandle(Dev) (&Dev->Data.LLData)
#define VL53LXDevStructGetLLResultsHandle(Dev) (&Dev->Data.llresults)

#ifdef __cplusplus
}
#endif

#endif
