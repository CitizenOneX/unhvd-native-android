/*
 * HDU - Hardware Depth Unprojector C library implementation
 *
 * Copyright 2020 (C) Bartosz Meglicki <meglickib@gmail.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

#include "hdu.h"

#include <stdlib.h> //malloc
#include <android/log.h>

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "unhvd_native_android", __VA_ARGS__))

//in binary 10 ones followed by 6 zeroes
static const uint16_t P010LE_MAX = 0xFFC0;

struct hdu
{
	float ppx;
	float ppy;
	float fx;
	float fy;
	float depth_unit;
	float min_depth;
	float max_depth;
};

struct hdu *hdu_init(const struct hdu_config *c)
{
	struct hdu *h;

	if( ( h = (struct hdu*)malloc(sizeof(struct hdu))) == NULL )
	{
		LOGI("hdu: not enough memory for hdu");
		//errno = ENOMEM;
		return NULL;
	}

	h->ppx = c->ppx;
	h->ppy = c->ppy;
	h->fx = c->fx;
	h->fy = c->fy;
	h->depth_unit = c->depth_unit;
	h->min_depth = c->min_margin;
	h->max_depth = P010LE_MAX * c->depth_unit - c->max_margin;

	return h;
}

void hdu_close(struct hdu *h)
{
	if(h == NULL)
		return;

	free(h);
}

void hdu_unproject(const struct hdu *h, const struct hdu_depth *depth, struct hdu_point_cloud *pc)
{
	//LOGI("hdu_unproject hdu_depth: %dx%d %d", depth->width, depth->height, depth->depth_stride);
	//LOGI("hdu_unproject hdu      : %f,%f,%f,%f,%f,%f,%f", h->fx, h->fy, h->ppx, h->ppy, h->min_depth, h->max_depth, h->depth_unit);
	//LOGI("hdu_unproject pc       : %d,%d,%p,%p", pc->size, pc->used, pc->data, pc->colors);
	const int pc_size = pc->size;
	const uint8_t default_color = 0xFF; // 0xFFFFFFFF for 32-bit
	int points=0;
	float d;

	for(int r=0;r<depth->height;++r)
		for(int c=0;c<depth->width && points < pc_size;++c)
		{
			d = depth->data[r * depth->depth_stride / 2 + c] * h->depth_unit;
			//if (d <= h->min_depth || d > h->max_depth)
			//	continue;

			pc->data[points][0] = d * (c - h->ppx) / h->fx;
			pc->data[points][1] = -d * (r - h->ppy) / h->fy;
			pc->data[points][2] = d;

			// TODO debug point
			//if (r==120 && c==160)
			//	LOGI("pc point: %d: (%f, %f, %f)", points, pc->data[points][0], pc->data[points][1], pc->data[points][2]);

			if (depth->colors)
			{
				// if texture contains 32-bit RGB0 colors
				//const uint32_t* color_line = (uint32_t*)(((uint8_t*)depth->colors) + r * depth->color_stride);
				//pc->colors[points] = color_line[c];
				
				// But my color line is an 8-bit Y plane only for now, (plus U and V planes) 
				// so put Y into uint8_t color array
				const uint8_t* color_line = (((uint8_t*)depth->colors) + r * depth->color_stride);
				//pc->colors[points] = (color_line[c] << 24) + (color_line[c] << 16) + (color_line[c] << 8) + 255;
				pc->colors[points] = color_line[c];

				//if (r == 120 && c == 160)
				//	LOGI("Middle pixel color in native lib: %d 0x%x, stride:%d", color_line[c], pc->colors[points], depth->color_stride);
				//pc->colors[points] = default_color; // or just use white for now
			}
			else
			{
				pc->colors[points] = default_color;
			}

			++points;
		}

	pc->used = points;
	return;
}
