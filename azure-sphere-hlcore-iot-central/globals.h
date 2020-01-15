#ifndef models_h
#define models_h

#include "epoll_timerfd_utilities.h"
#include <applibs/gpio.h>
#include <signal.h>
#include <stdbool.h>

#define SCOPEID_LENGTH 20
#define RT_APP_COMPONENT_LENGTH 36 + 1  // GUID 36 Char + 1 NULL terminate)
#define NELEMS(x)  (sizeof(x) / sizeof((x)[0]))

extern char scopeId[SCOPEID_LENGTH]; // ScopeId for the Azure IoT Central application, set in app_manifest.json, CmdArgs

extern volatile sig_atomic_t terminationRequired;

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

#endif