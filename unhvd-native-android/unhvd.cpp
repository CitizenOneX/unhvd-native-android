/*
 * UNHVD Network Hardware Video Decoder plugin C++ library implementation
 *
 * Copyright 2019-2020 (C) Bartosz Meglicki <meglickib@gmail.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

#include "unhvd.h"

// Network Hardware Video Decoder library
#include "nhvd.h"
// Hardware Depth Unprojector library
#include "hdu.h"
// Android Audio Output Stream
#include "aaos.h"

#include <thread>
#include <mutex>
#include <fstream>
#include <iostream>
#include <string.h> //memset
#include <android/log.h>
#include <libavutil/pixdesc.h>

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "unhvd_native_android", __VA_ARGS__))


using namespace std;

static void unhvd_network_decoder_thread(unhvd *n);
static int unhvd_unproject_depth_frame(unhvd *n, const AVFrame *depth_frame, const AVFrame *texture_frame, hdu_point_cloud *pc);
static unhvd *unhvd_close_and_return_null(unhvd *n, const char *msg);
static int UNHVD_ERROR_MSG(const char *msg);

struct unhvd
{
	nhvd *network_decoder;
	int decoders;
	int auxes;

	AVFrame *frame[UNHVD_MAX_DECODERS];
	nhvd_frame raws[UNHVD_MAX_DECODERS + UNHVD_MAX_AUX_CHANNELS];

	std::mutex mutex; //guards frame and point_cloud_shared

	hdu *hardware_unprojector;
	hdu_point_cloud point_cloud, point_cloud_shared;

	aaos* audio;

	thread network_thread;
	bool keep_working;

	unhvd():
			network_decoder(NULL),
			decoders(0),
			auxes(0),
			frame(), //zero out
			raws(),
			hardware_unprojector(NULL),
			point_cloud(),
			point_cloud_shared(),
			audio(NULL),
			keep_working(true)
	{}
};

struct unhvd *unhvd_init(
	const unhvd_net_config *net_config,
	const unhvd_hw_config *hw_config, int hw_size, int aux_size,
	const unhvd_depth_config *depth_config)
{
	
	LOGI("starting unhvd_init()");
	nhvd_net_config nhvd_net = {net_config->ip, net_config->port, net_config->timeout_ms};
	nhvd_hw_config nhvd_hw[UNHVD_MAX_DECODERS] = { {0} };

	if(hw_size > UNHVD_MAX_DECODERS)
		return unhvd_close_and_return_null(NULL, "the maximum number of decoders (compile time) exceeded");

	unhvd *u=new unhvd();

	if(u == NULL)
		return unhvd_close_and_return_null(NULL, "not enough memory for UNHVD");

	for(int i=0;i<hw_size;++i)
	{
		nhvd_hw_config hw = {hw_config[i].hardware, hw_config[i].codec, hw_config[i].device,
		hw_config[i].pixel_format, hw_config[i].width, hw_config[i].height, hw_config[i].profile};

		nhvd_hw[i] = hw;
	}

	if( (u->network_decoder = nhvd_init(&nhvd_net, nhvd_hw, hw_size, aux_size)) == NULL)
		return unhvd_close_and_return_null(u, "failed to initialize NHVD");

	u->decoders = hw_size;
	u->auxes = aux_size;

	for(int i=0;i<hw_size;++i)
	{
		LOGI("Allocating decoding frame %d", i);
		if( (u->frame[i] = av_frame_alloc() ) == NULL)
			return unhvd_close_and_return_null(u, "not enough memory for video frame");

		u->frame[i]->data[0] = NULL;
	}

	if(depth_config)
	{
		const unhvd_depth_config *dc = depth_config;
		const hdu_config hdu_cfg = {dc->ppx, dc->ppy, dc->fx, dc->fy, dc->depth_unit, dc->min_margin, dc->max_margin};
		LOGI("Initializing HDU: %f, %f, %f, %f, %f, %f, %f", dc->ppx, dc->ppy, dc->fx, dc->fy, dc->depth_unit, dc->min_margin, dc->max_margin);

		if( (u->hardware_unprojector = hdu_init(&hdu_cfg)) == NULL )
			return unhvd_close_and_return_null(u, "failed to initialize hardware unprojector");
	}

	// set up the native audio output
	u->audio = aaos_init();

	u->network_thread = thread(unhvd_network_decoder_thread, u);
	
	LOGI("unhvd: finishing unhvd_init()");
	return u;
}

static void unhvd_network_decoder_thread(unhvd *u)
{
	AVFrame *frames[UNHVD_MAX_DECODERS];
	// just try initialising these frames to NULL because for some reason
	// I'm getting a non-NULL texture frame when sending/decoding only depth data
	// I think this was a bug
	for (int i = 0; i < UNHVD_MAX_DECODERS; i++)
	{
		frames[i] = NULL;
	}

	int status;

	LOGI("Network decoder thread: %d decoders, hdu: %p", u->decoders, u->hardware_unprojector);


	while( u->keep_working &&
	     ((status = nhvd_receive_all(u->network_decoder, frames, u->raws) ) != NHVD_ERROR) )
	{
		if(status == NHVD_TIMEOUT)
			continue; //keep working

		// TODO sanity check the depth values then remove
		if (frames[0] && frames[0]->data[0])
		{
			uint16_t* depth_data = (uint16_t*)frames[0]->data[0];
			LOGI("Center depth point: %d", depth_data[frames[0]->linesize[0] * frames[0]->height / 4 + frames[0]->width / 2]); // seems to report real data (e.g. 1..1000)
		}

		if(u->hardware_unprojector && frames[0])
			if(unhvd_unproject_depth_frame(u, frames[0], frames[1], &u->point_cloud) != UNHVD_OK)
				break;

		// TODO try writing the first aux channel to the audio device for frames that include audio
		if (u->auxes == 1 && u->raws[u->decoders].size > 0)
		{
			// aux channel 1 is an array of bytes that are 2-byte int16_t encoded
			aaos_write(u->audio, (int16_t*)u->raws[u->decoders].data, u->raws[u->decoders].size / 2);
		}

		//the next call to nhvd_receive will unref the current
		//frames so we have to either consume set of frames or ref it
		std::lock_guard<std::mutex> frame_guard(u->mutex);

		for(int i=0;i<u->decoders;++i)
			if(frames[i])
			{
				av_frame_unref(u->frame[i]);
				av_frame_ref(u->frame[i], frames[i]);
			}

		if(u->hardware_unprojector && frames[0])
		{	//swap internal and shared point cloud (copy 2 ints and 2 pointers)
			hdu_point_cloud temp = u->point_cloud_shared;
			u->point_cloud_shared = u->point_cloud;
			u->point_cloud = temp;
		}

		// TODO remove after testing
		//LOGI("Frame sizes: %d, %d, %d, %d", u->raws[0].size, u->raws[1].size, u->raws[2].size, u->raws[3].size);
	}

	if (u->keep_working)
	{
		LOGI("unhvd: network decoder fatal error");
		u->keep_working = false; // signal the rest of the program that the network thread is finished
	}

	LOGI("unhvd: network decoder thread finished");
}

static int unhvd_unproject_depth_frame(unhvd *u, const AVFrame *depth_frame, const AVFrame *texture_frame, hdu_point_cloud *pc)
{
	//LOGI("Unprojecting depth frame: linesize: %d, width: %d, format: %d", depth_frame->linesize[0], depth_frame->width, depth_frame->format);
	if (depth_frame->linesize[0] / depth_frame->width != 2 ||
		(depth_frame->format != AV_PIX_FMT_P010LE && depth_frame->format != AV_PIX_FMT_P016LE && depth_frame->format != AV_PIX_FMT_YUV420P10LE))
	{
		// seems to be matching AV_PIX_FMT_YUV420P10LE(64)
		LOGI("Depth Linesize: %d,%d,%d, Depth Frame Width: %d, Depth Frame Format: %d", depth_frame->linesize[0], depth_frame->linesize[1], depth_frame->linesize[2], depth_frame->width, depth_frame->format);
		return UNHVD_ERROR_MSG("unhvd_unproject_depth_frame expects uint16 p010le/p016le data");
	}

	//LOGI("texture frame data? %p", texture_frame ? texture_frame->data[0] : NULL);
	if (texture_frame && texture_frame->data[0] &&
		texture_frame->format != AV_PIX_FMT_YUV420P)
	{
		// seems to be matching AV_PIX_FMT_YUV420P
		LOGI("Texture Frame Format: %d Linesize: %d,%d,%d", texture_frame->format, texture_frame->linesize[0], texture_frame->linesize[1], texture_frame->linesize[2]);
		return UNHVD_ERROR_MSG("unhvd_unproject_depth_frame expects YUV420P texture data");
	}

	int size = depth_frame->width * depth_frame->height;
	if(size != pc->size)
	{
		delete [] pc->data;
		delete [] pc->colors;
		pc->data = new float3[size];
		pc->colors = new color32[size];  // YUV420P uses 12bpp but hdu calculates RGBA from YUV
		pc->size = size;
		pc->used = 0;
	}

	uint16_t *depth_data = (uint16_t*)depth_frame->data[0];
	//LOGI("Center depth point: %d", depth_data[depth_frame->width * depth_frame->height / 2 + depth_frame->width / 2]); // seems to report real data (e.g. 149, 150, 151)

	//texture data is optional
	uint8_t *texture_data = texture_frame ? (uint8_t*)texture_frame->data[0] : NULL;
	int texture_linesize = texture_frame ? texture_frame->linesize[0] : 0;

	hdu_depth depth = {depth_data, texture_data, depth_frame->width, depth_frame->height,
		depth_frame->linesize[0], texture_linesize};

	//this could be moved to separate thread
	hdu_unproject(u->hardware_unprojector, &depth, pc);
	//LOGI("Sample projected point: %f, %f, %f", pc->data[320 * 120 + 160][0], pc->data[320 * 120 + 160][1], pc->data[320 * 120 + 160][2]);

	//zero out unused point cloud entries
	memset(pc->data + pc->used, 0, (pc->size-pc->used)*sizeof(pc->data[0]));
	memset(pc->colors + pc->used, 0, (pc->size-pc->used)*sizeof(pc->colors[0]));

	return UNHVD_OK;
}

//NULL if there is no fresh data, non NULL otherwise
int unhvd_get_begin(unhvd *u, unhvd_frame *frame, unhvd_point_cloud *pc)
{
	if(u == NULL)
		return UNHVD_ERROR;

	u->mutex.lock();

	bool new_data = false;

	// check for new data in any decoded channel
	for (int i = 0; i < u->decoders; ++i)
		if (u->frame[i]->data[0] != NULL)
		{
			new_data = true;
			break;
		}

	// check for new data in any auxilliary channel
	if (!new_data)
		for (int i = 0; i < u->auxes; ++i)
			if (u->raws[u->decoders + i].data != NULL)
			{
				new_data = true;
				break;
			}

	//for user convinience, return ERROR if there is no new data
	if(!new_data)
		return UNHVD_ERROR;

	if (frame)
	{
		for (int i = 0; i < u->decoders; ++i)
		{
			frame[i].width = u->frame[i]->width;
			frame[i].height = u->frame[i]->height;
			frame[i].format = u->frame[i]->format;

			//copy just a few ints and pointers, not the actual data
			memcpy(frame[i].linesize, u->frame[i]->linesize, sizeof(frame[i].linesize));
			memcpy(frame[i].data, u->frame[i]->data, sizeof(frame[i].data));
		}

		// copy auxilliary channels over
		for (int i = 0; i < u->auxes; ++i)
		{
			int j = u->decoders + i;
			frame[j].width = 0;
			frame[j].height = 0;
			frame[j].format = 0;

			//copy just an int and a pointer, not the actual data
			frame[j].linesize[0] = u->raws[j].size;
			frame[j].data[0] = u->raws[j].data;
		}
	}

	if(pc && u->hardware_unprojector)
	{
		//copy just two pointers and ints
		pc->data = u->point_cloud_shared.data;
		pc->colors = u->point_cloud_shared.colors;
		pc->size = u->point_cloud_shared.size;
		pc->used = u->point_cloud_shared.used;
	}

	return UNHVD_OK;
}

//returns UNHVD_OK on success, UNHVD_ERROR on fatal error
int unhvd_get_end(struct unhvd *u)
{
	if(u == NULL)
		return UNHVD_ERROR;

	for(int i=0;i<u->decoders;++i)
		av_frame_unref(u->frame[i]);

	// zero out the aux data so get_begin can tell if frames are new
	// TODO check if the data needs to be freed
	for (int i = 0; i < u->auxes; ++i)
		u->raws[u->decoders + i].data = NULL;

	u->mutex.unlock();

	return UNHVD_OK;
}

int unhvd_get_frame_begin(unhvd *u, unhvd_frame *frame)
{
	return unhvd_get_begin(u, frame, NULL);
}

int unhvd_get_frame_end(struct unhvd *u)
{
	return unhvd_get_end(u);
}

int unhvd_get_point_cloud_begin(unhvd *u, unhvd_point_cloud *pc)
{
	return unhvd_get_begin(u, NULL, pc);
}

int unhvd_get_point_cloud_end(unhvd *u)
{
	return unhvd_get_end(u);
}

static unhvd *unhvd_close_and_return_null(unhvd *u, const char *msg)
{
	if (msg)
	{
		LOGI("unhvd: %s", msg);
	}

	unhvd_close(u);

	return NULL;
}

void unhvd_close(unhvd *u)
{	//free mutex?
	if(u == NULL)
		return;

	u->keep_working=false;
	if(u->network_thread.joinable())
		u->network_thread.join();

	nhvd_close(u->network_decoder);

	for(int i=0;i<u->decoders;++i)
		av_frame_free(&u->frame[i]);

	hdu_close(u->hardware_unprojector);
	delete [] u->point_cloud.data;
	delete [] u->point_cloud.colors;
	delete [] u->point_cloud_shared.data;
	delete [] u->point_cloud_shared.colors;

	aaos_close(u->audio);

	delete u;
}

static int UNHVD_ERROR_MSG(const char *msg)
{
	LOGI("%s", msg);
	return UNHVD_ERROR;
}
