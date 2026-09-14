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





#ifndef _VL53LX_PLATFORM_USER_CONFIG_H_
#define _VL53LX_PLATFORM_USER_CONFIG_H_

#define    VL53LX_BYTES_PER_WORD              2
#define    VL53LX_BYTES_PER_DWORD             4


#define VL53LX_BOOT_COMPLETION_POLLING_TIMEOUT_MS     500
#define VL53LX_RANGE_COMPLETION_POLLING_TIMEOUT_MS   2000
#define VL53LX_TEST_COMPLETION_POLLING_TIMEOUT_MS   60000

#define VL53LX_POLLING_DELAY_MS                         1


#define VL53LX_TUNINGPARM_PUBLIC_PAGE_BASE_ADDRESS  0x8000
#define VL53LX_TUNINGPARM_PRIVATE_PAGE_BASE_ADDRESS 0xC000

#define VL53LX_GAIN_FACTOR__STANDARD_DEFAULT       0x0800

#define VL53LX_GAIN_FACTOR__HISTOGRAM_DEFAULT      0x0800



#define VL53LX_OFFSET_CAL_MIN_EFFECTIVE_SPADS  0x0500


#define VL53LX_OFFSET_CAL_MAX_PRE_PEAK_RATE_MCPS   0x1900


#define VL53LX_OFFSET_CAL_MAX_SIGMA_MM             0x0040



#define VL53LX_ZONE_CAL_MAX_PRE_PEAK_RATE_MCPS     0x1900


#define VL53LX_ZONE_CAL_MAX_SIGMA_MM               0x0040



#define VL53LX_XTALK_EXTRACT_MAX_SIGMA_MM          0x008C


#define VL53LX_MAX_USER_ZONES                5

#define VL53LX_MAX_RANGE_RESULTS              4



#define VL53LX_MAX_STRING_LENGTH 512

#endif


