/*
 * HVD Hardware Video Decoder C library imlementation
 *
 * Copyright 2019-2020 (C) Bartosz Meglicki <meglickib@gmail.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

#include "hvd.h"

// FFmpeg
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

#include <stdlib.h> //malloc
#include <android/log.h>

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "unhvd_native_android", __VA_ARGS__))

//internal library data passed around by the user
struct hvd
{
	AVBufferRef* hw_device_ctx;
	enum AVPixelFormat hw_pix_fmt;
	enum AVPixelFormat sw_pix_fmt;
	AVCodecContext* decoder_ctx;
	AVFrame *sw_frame;
	AVFrame *hw_frame;
	AVPacket av_packet;
};

static struct hvd *hvd_close_and_return_null(struct hvd *h, const char *msg, const char *msg_details);
static AVFrame *NULL_MSG(const char *msg, const char *msg_details);

//NULL on error
struct hvd *hvd_init(const struct hvd_config *config)
{
	struct hvd *h, zero_hvd = {0};
	AVCodec *decoder = NULL;
	int err;

	if( ( h = (struct hvd*)malloc(sizeof(struct hvd))) == NULL )
		return hvd_close_and_return_null(NULL, "not enough memory for hvd", NULL);

	*h = zero_hvd; //set all members of dynamically allocated struct to 0 in a portable way

	avcodec_register_all();
	av_log_set_level(AV_LOG_VERBOSE);

	if( ( decoder = avcodec_find_decoder_by_name(config->codec) ) == NULL)
		return hvd_close_and_return_null(h, "cannot find decoder", config->codec);

	if (!(h->decoder_ctx = avcodec_alloc_context3(decoder)))
		return hvd_close_and_return_null(h, "failed to alloc decoder context, no memory?", NULL);

	h->decoder_ctx->width = config->width;
	h->decoder_ctx->height = config->height;

	if(config->profile)
		h->decoder_ctx->profile = config->profile;

	LOGI("Available pixel formats from Codec %s:", decoder->name);
	if (decoder->pix_fmts != NULL)
	{
		const enum AVPixelFormat* iterator = decoder->pix_fmts;
		while (*iterator != AV_PIX_FMT_NONE)
		{
			LOGI("%d : %s", *iterator, av_get_pix_fmt_name(*iterator));
			++iterator;
		}
	}
	else
	{
		LOGI("Pixel formats unknown for decoder %s", decoder->long_name);
	}

	// Android decoders (h264, hevc, maybe vp*? only seem to give frames back in YUV, couldn't get any RGB)
	h->hw_pix_fmt = AV_PIX_FMT_YUV420P;
	h->sw_pix_fmt = AV_PIX_FMT_YUV420P;

	if (( err = avcodec_open2(h->decoder_ctx, decoder, NULL)) < 0)
		return hvd_close_and_return_null(h, "failed to initialize decoder context for", decoder->name);
	LOGI("Opened decoder: %s, pixfmt:%d", decoder->long_name, h->decoder_ctx->pix_fmt);

	av_init_packet(&h->av_packet);
	h->av_packet.data = NULL;
	h->av_packet.size = 0;

	return h;
}

void hvd_close(struct hvd* h)
{
	if(h == NULL)
		return;

	av_frame_free(&h->sw_frame);
	av_frame_free(&h->hw_frame);

	avcodec_free_context(&h->decoder_ctx);
	av_buffer_unref(&h->hw_device_ctx);

	free(h);
}

static struct hvd *hvd_close_and_return_null(struct hvd *h, const char *msg, const char *msg_details)
{
	if(msg)
		LOGI("hvd: %s %s", msg, (msg_details) ? msg_details : "");

	hvd_close(h);

	return NULL;
}

int hvd_send_packet(struct hvd *h,struct hvd_packet *packet)
{
	int err;

	//NULL packet is legal and means user requested flushing
	h->av_packet.data = (packet) ? packet->data : NULL;
	h->av_packet.size = (packet) ? packet->size : 0;


	// MLSP allows for larger packet buffer (defaults to 32, increase to 64) 
	// to allow for overreads by optimized decoders
	// looks like this addresses the invalid packet decoding issue;

	//WARNING The input buffer, av_packet->data must be AV_INPUT_BUFFER_PADDING_SIZE
	//larger than the actual read bytes because some optimized bitstream readers
	// read 32 or 64 bits at once and could read over the end. (MLSP takes care of this)
	if ( (err = avcodec_send_packet(h->decoder_ctx, &h->av_packet) ) < 0 )
	{
		LOGI("hvd: send_packet error %s", av_err2str(err));

		//e.g. non-existing PPS referenced, could not find ref with POC, keep pushing packets
		if(err == AVERROR_INVALIDDATA || err == AVERROR(EIO))
			return HVD_OK;

		//EAGAIN means that we need to read data with avcodec_receive_frame before we can push more data to decoder
		return ( err == AVERROR(EAGAIN) ) ? HVD_AGAIN : HVD_ERROR;
	}

	return HVD_OK;
}

//returns:
//- non NULL on success
//- NULL and error == HVD_OK if more data is needed or flushed completely
//- NULL and error == HVD_ERROR if error occured
//the ownership of returned AVFrame* remains with the library
AVFrame *hvd_receive_frame(struct hvd *h, int *error)
{
	AVCodecContext *avctx=h->decoder_ctx;
	int ret = 0;

	*error = HVD_ERROR;
	//free the leftovers from the last call (if any)
	//this will happen here or in hvd_close, whichever is first
	av_frame_free(&h->hw_frame);  //NOTE - use hw_frame for SOFTWARE, not HARDWARE here

	if ( !( h->hw_frame = av_frame_alloc() ) ) //|| !( h->sw_frame = av_frame_alloc() ) )
		return NULL_MSG("unable to av_frame_alloc frame", NULL);

	if ( (ret = avcodec_receive_frame(avctx, h->hw_frame) ) < 0 )
	{	//EAGAIN - we need to push more data with avcodec_send_packet
		//EOF  - the decoder was flushed, no more data
		*error = (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? HVD_OK : HVD_ERROR;

		//be nice to the user and prepare the decoder for new stream for him
		//if he wants to continue the decoding (startover)
		if(ret == AVERROR_EOF)
			avcodec_flush_buffers(avctx);

		if(*error)
			LOGI("hvd: error while decoding - \"%s\"", av_err2str(ret));

		return NULL;
	}

	*error = HVD_OK;
	return h->hw_frame;
}

static AVFrame *NULL_MSG(const char *msg, const char *msg_details)
{
	if(msg)
		LOGI("hvd: %s %s", msg, msg_details ? msg_details : "");

	return NULL;
}

