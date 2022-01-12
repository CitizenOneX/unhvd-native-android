#pragma once
#include <android/log.h>

class unhvd_native_android
{
public:
	const char * getPlatformABI();
	unhvd_native_android();
	~unhvd_native_android();
};

