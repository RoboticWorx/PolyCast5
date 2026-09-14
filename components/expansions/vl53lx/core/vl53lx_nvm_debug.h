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





#ifndef _VL53LX_NVM_DEBUG_H_
#define _VL53LX_NVM_DEBUG_H_

#include "vl53lx_ll_def.h"
#include "vl53lx_nvm_structs.h"



#ifdef __cplusplus
extern "C"
{
#endif

#ifdef VL53LX_LOG_ENABLE



void VL53LX_print_nvm_raw_data(
	uint8_t                       *pnvm_raw_data,
	uint32_t                       trace_flags);




void VL53LX_print_decoded_nvm_data(
	VL53LX_decoded_nvm_data_t *pdata,
	char                      *pprefix,
	uint32_t                   trace_flags);




void VL53LX_print_decoded_nvm_fmt_range_data(
	VL53LX_decoded_nvm_fmt_range_data_t *pdata,
	char                                *pprefix,
	uint32_t                             trace_flags);




void VL53LX_print_decoded_nvm_fmt_info(
	VL53LX_decoded_nvm_fmt_info_t *pdata,
	char                          *pprefix,
	uint32_t                       trace_flags);



void VL53LX_print_decoded_nvm_ews_info(
	VL53LX_decoded_nvm_ews_info_t *pdata,
	char                          *pprefix,
	uint32_t                       trace_flags);

#endif

#ifdef __cplusplus
}
#endif

#endif

