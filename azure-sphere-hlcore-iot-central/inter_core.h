#ifndef inter_core_h
#define inter_core_h

#include "globals.h"
#include "iot_hub.h"
#include <applibs/application.h>
#include <applibs/gpio.h>
#include <applibs/log.h>
#include <ctype.h>
#include <errno.h>
#include <iothub_device_client_ll.h>
#include <iothub_message.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

//void SendEventMsg(IOTHUB_DEVICE_CLIENT_LL_HANDLE, bool);
bool ProcessMsg(void);
bool SendMessageToRTCore(void);
int InitInterCoreComms(int epollFd, const char* rtAppComponentId, void (*interCoreCallback)(char*));
void SocketEventHandler(EventData* eventData);

#endif