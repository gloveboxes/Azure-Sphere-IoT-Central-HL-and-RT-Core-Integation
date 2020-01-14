#ifndef device_twins_h
#define device_twins_h

#include "globals.h"
#include "parson.h"
#include <applibs/gpio.h>
#include <applibs/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void TwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char* payload, size_t payloadSize, void* userContextCallback);
void SetDesiredState(JSON_Object* desiredProperties, Peripheral* peripheral);
void TwinReportState(const char* propertyName, bool propertyValue);
void ReportStatusCallback(int result, void* context);

#endif