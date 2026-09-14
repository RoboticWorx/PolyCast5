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


#ifndef _VL53LX_PLATFORM_USER_DEFINES_H_
#define _VL53LX_PLATFORM_USER_DEFINES_H_

#ifdef __cplusplus
extern "C"
{
#endif





#define do_division_u(dividend, divisor) (dividend / divisor)



#define do_division_s(dividend, divisor) (dividend / divisor)



#define WARN_OVERRIDE_STATUS(__X__)\
	trace_print (VL53LX_TRACE_LEVEL_WARNING, #__X__);


#ifdef _MSC_VER
#define DISABLE_WARNINGS() { \
	__pragma (warning (push)); \
	__pragma (warning (disable:4127)); \
	}
#define ENABLE_WARNINGS() { \
	__pragma (warning (pop)); \
	}
#else
	#define DISABLE_WARNINGS()
	#define ENABLE_WARNINGS()
#endif


#ifdef __cplusplus
}
#endif

#endif


