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

#ifdef VL53LX_NEEDS_IPP
#  undef VL53LX_IPP_API
#  define VL53LX_IPP_API  __declspec(dllimport)
#  pragma comment (lib, "EwokPlus25API_IPP")
#endif

