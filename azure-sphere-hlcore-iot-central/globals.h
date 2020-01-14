#ifndef models_h
#define models_h

#include "epoll_timerfd_utilities.h"
#include <applibs/gpio.h>
#include <iothub_device_client_ll.h>
#include <signal.h>
#include <stdbool.h>

#define JSON_MESSAGE_BYTES 100  // Number of bytes to allocate for the JSON telemetry message for IoT Central
#define SCOPEID_LENGTH 20

extern IOTHUB_DEVICE_CLIENT_LL_HANDLE iothubClientHandle;
extern char scopeId[SCOPEID_LENGTH]; // ScopeId for the Azure IoT Central application, set in app_manifest.json, CmdArgs


// Azure IoT poll periods
extern int AzureIoTDefaultPollPeriodSeconds;
extern int AzureIoTMinReconnectPeriodSeconds;
extern int AzureIoTMaxReconnectPeriodSeconds;

extern int azureIoTPollPeriodSeconds;
extern bool iothubAuthenticated;
extern const int keepalivePeriodSeconds;

// GPIO Pins used in the HL App
#define SEND_STATUS_PIN 19
#define LIGHT_PIN 21
#define RELAY_PIN 0

extern int epollFd;
extern int sockFd;

extern volatile sig_atomic_t terminationRequired;
extern const char rtAppComponentId[];


typedef struct {
	int fd;
	int pin;
	GPIO_Value initialState;
	bool invertPin;
	bool twinState;
	const char* twinProperty;
} Peripheral;

typedef struct {
	EventData eventData;
	struct timespec period;
	int fd;
	const char* name;
} Timer;


extern Peripheral sending;
extern Peripheral light;
extern Peripheral relay;


#endif