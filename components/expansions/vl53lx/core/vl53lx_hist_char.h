/**
 ******************************************************************************
 * Copyright (c) 2020, STMicroelectronics - All Rights Reserved
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */





#ifndef _VL53LX_HIST_CHAR_H_
#define _VL53LX_HIST_CHAR_H_

#include "vl53lx_platform.h"

#ifdef __cplusplus
extern "C"
{
#endif




VL53LX_Error VL53LX_set_calib_config(
	VL53LX_DEV      Dev,
	uint8_t         vcsel_delay__a0,
	uint8_t         calib_1,
	uint8_t         calib_2,
	uint8_t         calib_3,
	uint8_t         calib_2__a0,
	uint8_t         spad_readout);




VL53LX_Error VL53LX_set_hist_calib_pulse_delay(
	VL53LX_DEV      Dev,
	uint8_t         calib_delay);




VL53LX_Error VL53LX_disable_calib_pulse_delay(
	VL53LX_DEV      Dev);


#ifdef __cplusplus
}
#endif

#endif

