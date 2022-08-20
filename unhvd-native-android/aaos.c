#include "aaos.h"

#include <malloc.h>
#include <aaudio/AAudio.h>

#include <android/log.h>
#define LOGV(...) ((void)__android_log_print(ANDROID_LOG_VERBOSE, "unhvd_native_android", __VA_ARGS__))
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "unhvd_native_android", __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, "unhvd_native_android", __VA_ARGS__))

struct aaos
{
	AAudioStream* stream;
};

static struct aaos* aaos_close_and_return_null(struct aaos* a, const char* msg);

struct aaos* aaos_init()
{
	LOGI("aaos_init()");
	struct aaos* a, zero_aaos = { 0 };

	if ((a = (struct aaos*)malloc(sizeof(struct aaos))) == NULL)
		return aaos_close_and_return_null(NULL, "not enough memory for aaos");

	*a = zero_aaos;

	AAudioStreamBuilder* builder = NULL;
	aaudio_result_t result = AAudio_createStreamBuilder(&builder);
	AAudioStreamBuilder_setChannelCount(builder, 1); // Mono
	AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED); // Maybe we need EX mode if we want LOW_LATENCY? Doesn't seem to give it to me anyway
	AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE); // doesn't seem to respect LOW_LATENCY, but does give me POWER_SAVING if I ask. But does lower buffer sizes.
	AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16); // signed int16 data
	AAudioStreamBuilder_setSampleRate(builder, 24000);

	LOGI("aaos: Created the AAudioStreamBuilder: %s", AAudio_convertResultToText(result));

	AAudioStream* stream;
	result = AAudioStreamBuilder_openStream(builder, &stream);

	LOGI("aaos: Created the AAudioStream - result %s", AAudio_convertResultToText(result));

	// log the specifics of the output AAudioStream we got given by the system
	LOGI("aaos: Stream deviceId:%d, sharingMode:%d, sampleRate:%d, samplesPerFrame:%d, channelCount:%d, format:%d, bufferCapacity:%d, bufferSize:%d, framesPerBurst:%d, performanceMode:%d",
		AAudioStream_getDeviceId(stream), AAudioStream_getSharingMode(stream), AAudioStream_getSampleRate(stream),
		AAudioStream_getSamplesPerFrame(stream), AAudioStream_getChannelCount(stream), AAudioStream_getFormat(stream),
		AAudioStream_getBufferCapacityInFrames(stream), AAudioStream_getBufferSizeInFrames(stream), AAudioStream_getFramesPerBurst(stream),
		AAudioStream_getPerformanceMode(stream));

	// clean up the builder
	result = AAudioStreamBuilder_delete(builder);

	LOGI("aaos: Deleted the AAudioStreamBuilder: %s", AAudio_convertResultToText(result));

	result = AAudioStream_requestStart(stream);

	LOGI("aaos: Requested the AAudioStream to start: %s", AAudio_convertResultToText(result));

	a->stream = stream;
	return a;
}

void aaos_close(struct aaos* a)
{
	if (a == NULL)
		return;

	aaudio_result_t result = AAudioStream_requestStop(a->stream);
	LOGI("aaos: Requested the AAudioStream to stop: %s", AAudio_convertResultToText(result));

	result = AAudioStream_close(a->stream);
	LOGI("aaos: Closed the AAudioStream: %s", AAudio_convertResultToText(result));

	free(a);

	LOGI("aaos_close()");
	return;
}

static struct aaos* aaos_close_and_return_null(struct aaos* a, const char* msg)
{
	if (msg)
		LOGI("aaos: %s", msg);

	aaos_close(a);

	return NULL;
}

// push the samples directly to the buffer
int32_t aaos_write(struct aaos* a, const int16_t* buffer, const int32_t buflen)
{
	// write the audio data non-blocking (write, or dump if the buffer is full?)
	aaudio_result_t result = AAudioStream_write(a->stream, buffer, buflen, 0);
	if (result < 0)
	{
		LOGE("aaos: error writing to stream: %s", AAudio_convertResultToText(result));
	}
	// debug only, if samples are not being written for some reason (also returned to caller)
	//else
	//{
	//	LOGV("aaos: wrote %d/%d audio samples", result, buflen);
	//}
	return result;
}