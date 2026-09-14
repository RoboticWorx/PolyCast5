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



#ifndef _VL53LX_PLATFORM_IPP_H_
#define _VL53LX_PLATFORM_IPP_H_

#include <vl53lx_platform_user_data.h>

#ifdef __cplusplus
extern "C"
{
#endif





VL53LX_Error VL53LX_ipp_hist_process_data(
	VL53LX_DEV                         Dev,
	VL53LX_dmax_calibration_data_t    *pdmax_cal,
	VL53LX_hist_gen3_dmax_config_t    *pdmax_cfg,
	VL53LX_hist_post_process_config_t *ppost_cfg,
	VL53LX_histogram_bin_data_t       *pbins,
	VL53LX_xtalk_histogram_data_t     *pxtalk,
	uint8_t                           *pArea1,
	uint8_t                           *pArea2,
	uint8_t                           *phisto_merge_nb,
	VL53LX_range_results_t            *presults);




VL53LX_Error VL53LX_ipp_hist_ambient_dmax(
	VL53LX_DEV                         Dev,
	uint16_t                           target_reflectance,
	VL53LX_dmax_calibration_data_t    *pdmax_cal,
	VL53LX_hist_gen3_dmax_config_t    *pdmax_cfg,
	VL53LX_histogram_bin_data_t       *pbins,
	int16_t                           *pambient_dmax_mm);




VL53LX_Error VL53LX_ipp_xtalk_calibration_process_data(
	VL53LX_DEV                          Dev,
	VL53LX_xtalk_range_results_t       *pxtalk_ranges,
	VL53LX_xtalk_histogram_data_t      *pxtalk_shape,
	VL53LX_xtalk_calibration_results_t *pxtalk_cal);


#ifdef __cplusplus
}
#endif

#endif


