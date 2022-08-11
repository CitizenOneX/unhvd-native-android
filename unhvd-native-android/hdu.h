/*
 * HDU - Hardware Depth Unprojector C library
 *
 * Copyright 2020 (C) Bartosz Meglicki <meglickib@gmail.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

/**
 ******************************************************************************
 *
 *  \mainpage hdu documentation
 *  \see https://github.com/bmegli/hardware-depth-unprojector
 *
 *  \copyright  Copyright (C) 2020 Bartosz Meglicki
 *  \file       hdu.h
 *  \brief      Library public interface header
 *
 ******************************************************************************
 */

#ifndef HDU_H
#define HDU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h> //uint16_t

/** \addtogroup interface Public interface
 *  @{
 */

struct hdu;

struct hdu_depth
{
	uint16_t *data;
	uint8_t *colors;
	int width;
	int height;
	int depth_stride;
	int color_stride;
};

typedef float float3[3];
typedef uint32_t color32;

struct hdu_point_cloud
{
	float3 *data;
	color32 *colors;
	int size;
	int used;
};


/**
 * @struct hdu_depth_config
 * @brief Depth unprojection configuration.
 *
 * If non-zero min_margin and max_margin are designed to ignore depth values
 * - <= min_margin
 * - > max representable depth - max_margin
 *
 * max representable depth is calculated as P010LE_MAX * depth_unit
 *
 * @see hdu_init
 */
struct hdu_config
{
	float ppx; //!< principal point x pixel coordinates (center of projection)
	float ppy; //!< principal point y pixel coordinates (center of projection)
	float fx; //!< focal length in pixel width unit
	float fy; //!< focal length in pixel height unit
	float depth_unit; //!< multiplier for raw depth data;
	float min_margin; //!< minimal margin to treat as valid in result unit (raw data * depth_unit);
	float max_margin; //!< maximal margin to treat as valid in result unit (raw data * depth_unit);
};

//NULL on ERROR
struct hdu *hdu_init(const struct hdu_config *cfg);
void hdu_close(struct hdu *h);

void hdu_unproject(const struct hdu *h, const struct hdu_depth *depth, struct hdu_point_cloud *pc);

/** @}*/

#ifdef __cplusplus
}
#endif

#endif
