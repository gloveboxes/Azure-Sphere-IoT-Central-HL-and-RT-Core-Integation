#ifndef models_h
#define models_h

#include "epoll_timerfd_utilities.h"
#include <applibs/gpio.h>
#include <signal.h>
#include <stdbool.h>
#include "parson.h"

#define SCOPEID_LENGTH 20
#define RT_APP_COMPONENT_LENGTH 36 + 1  // GUID 36 Char + 1 NULL terminate)
#define NELEMS(x)  (sizeof(x) / sizeof((x)[0]))

extern char scopeId[SCOPEID_LENGTH]; // ScopeId for the Azure IoT Central application, set in app_manifest.json, CmdArgs

extern volatile sig_atomic_t terminationRequired;

struct _peripheral {
	int fd;
	unsigned char pin;
	GPIO_Value initialState;
	bool invertPin;
	void (*initialise)(struct _peripheral* peripheral);
	char* name;
};

typedef struct _peripheral Peripheral;

typedef struct {
	Peripheral peripheral;
	bool twinState;
	const char* twinProperty;
	void (*handler)(JSON_Object* json, Peripheral* peripheral);
} DeviceTwinPeripheral;

typedef struct {
	Peripheral peripheral;
	const char* methodName;
	void (*handler)(JSON_Object* json, Peripheral* peripheral);
} DirectMethodPeripheral;

typedef struct {
	EventData eventData;
	struct timespec period;
	int fd;
	const char* name;
} Timer;

#endif