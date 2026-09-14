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




#ifndef _VL53LX_SILICON_CORE_H_
#define _VL53LX_SILICON_CORE_H_

#include "vl53lx_platform.h"

#ifdef __cplusplus
extern "C" {
#endif




VL53LX_Error VL53LX_is_firmware_ready_silicon(
	VL53LX_DEV      Dev,
	uint8_t        *pready);


#ifdef __cplusplus
}
#endif

#endif

