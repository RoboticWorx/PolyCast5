/*
 * PolyCast5 additions to ST's platform contract.
 *
 * ST's vl53lx_platform.h is vendored verbatim and declares only what the bare driver
 * itself calls. This header carries the one hook the port needs on top of it.
 */

#ifndef VL53LX_PORT_H
#define VL53LX_PORT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tell the platform layer whether the caller is holding xI2CBusMutex
 *
 * VL53LX_WaitMs() hands the shared bus back for the duration of any delay so that a poll
 * loop inside ST's core - bounded at up to two seconds - does not stall gpio_task. It may
 * only do that if the bus is actually held, so vl53l4cx.c raises this around every ST API
 * call and lowers it afterwards. Leaving it false simply makes the waits non-yielding.
 *
 * @param [in] held  true while xI2CBusMutex is held by the calling task
 */
void vl53lx_port_set_bus_held(bool held);

#ifdef __cplusplus
}
#endif

#endif
