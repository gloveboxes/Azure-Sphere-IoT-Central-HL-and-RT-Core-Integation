#include "../MT3620_Grove_Shield/MT3620_Grove_Shield_Library/Grove.h"
#include "../MT3620_Grove_Shield/MT3620_Grove_Shield_Library/Sensors/GroveTempHumiSHT31.h"
#include "globals.h"
#include "inter_core.h"
#include "iot_hub.h"
#include <applibs/gpio.h>
#include <applibs/log.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

// GPIO Pins used in the High Level (HL) Application
#define SEND_STATUS_PIN 19
#define LIGHT_PIN 21
#define RELAY_PIN 0
#define JSON_MESSAGE_BYTES 100  // Number of bytes to allocate for the JSON telemetry message for IoT Central

static char msgBuffer[JSON_MESSAGE_BYTES] = { 0 };
static char rtAppComponentId[RT_APP_COMPONENT_LENGTH];  //initialized from cmdline argument
//static const char* rtAppComponentId = "6583cf17-d321-4d72-8283-0b7c5b56442b";

static int epollFd = -1;
static int i2cFd;
static void* sht31;

static Peripheral sendStatus = {
	.fd = -1,
	.pin = SEND_STATUS_PIN,
	.initialState = GPIO_Value_High,
	.invertPin = true,
	.twinState = false,
	.twinProperty = "SendStatus"
};
static Peripheral light = {
	.fd = -1,
	.pin = LIGHT_PIN,
	.initialState = GPIO_Value_High,
	.invertPin = true,
	.twinState = false,
	.twinProperty = "LightStatus"
};
static Peripheral relay = { 
	.fd = -1, 
	.pin = RELAY_PIN,
	.initialState =  GPIO_Value_Low,
	.invertPin = false,
	.twinState = false,
	.twinProperty = "RelayStatus"
};

// Forward signatures
static void TerminationHandler(int signalNumber);
static int InitPeripheralsAndHandlers(void);
static void ClosePeripheralsAndHandlers(void);
static void InterCoreCallBack(char* msg);
static void MeasureSendEventHandler(EventData* eventData);
static void RtCoreHeartBeat(EventData* eventData);
static int OpenPeripheral(Peripheral* peripheral);
static int StartTimer(Timer* timer);

static Timer iotClientDoWork = { 
	.eventData = {.eventHandler = &AzureDoWorkTimerEventHandler }, 
	.period = { 1, 0 }, 
	.name = "DoWork" 
};
static Timer measureSensor = { 
	.eventData = {.eventHandler = &MeasureSendEventHandler }, 
	.period = { 10, 0 }, 
	.name = "MeasureSensor" 
};
static Timer rtCoreHeatBeat = { 
	.eventData = {.eventHandler = &RtCoreHeartBeat }, 
	.period = { 30, 0 }, 
	.name = "rtCoreSend" 
};

Peripheral* deviceTwins[] = { &relay, &light };

int main(int argc, char* argv[])
{
	Log_Debug("IoT Hub/Central Application starting.\n");

	if (argc == 3) {
		Log_Debug("Setting Azure Scope ID %s\n", argv[1]);
		strncpy(scopeId, argv[1], SCOPEID_LENGTH);

		Log_Debug("Setting RT Core Component Id %s\n", argv[2]);
		strncpy(rtAppComponentId, argv[2], RT_APP_COMPONENT_LENGTH);
	}
	else {
		Log_Debug("ScopeId and RT Core ComponentId need to be set in the app_manifest CmdArgs\n");
		return -1;
	}

	if (InitPeripheralsAndHandlers() != 0) {
		terminationRequired = true;
	}

	// Main loop
	while (!terminationRequired) {
		if (WaitForEventAndCallHandler(epollFd) != 0) {
			terminationRequired = true;
		}
	}

	ClosePeripheralsAndHandlers();

	Log_Debug("Application exiting.\n");

	return 0;
}

/// <summary>
///     Reads telemetry and returns the data as a JSON object.
/// </summary>
static int ReadTelemetry(char eventBuffer[], size_t len) {
	static int msgId = 0;
	GroveTempHumiSHT31_Read(sht31);
	float temperature = GroveTempHumiSHT31_GetTemperature(sht31);
	float humidity = GroveTempHumiSHT31_GetHumidity(sht31);

	static const char* EventMsgTemplate = "{ \"Temperature\": \"%3.2f\", \"Humidity\": \"%3.1f\", \"MsgId\":%d }";
	return snprintf(eventBuffer, len, EventMsgTemplate, temperature, humidity, msgId++);
}

static void preSendTelemtry(void) {
	GPIO_SetValue(sendStatus.fd, GPIO_Value_Low);
}

static void postSendTelemetry(void) {
	GPIO_SetValue(sendStatus.fd, GPIO_Value_High);
}

/// <summary>
/// Azure timer event:  Check connection status and send telemetry
/// </summary>
static void MeasureSendEventHandler(EventData* eventData)
{
	if (ConsumeTimerFdEvent(measureSensor.fd) != 0) {
		terminationRequired = true;
		return;
	}

	preSendTelemtry();

	if (ReadTelemetry(msgBuffer, JSON_MESSAGE_BYTES) > 0) {
		SendMsg(msgBuffer);
	}

	postSendTelemetry();
}

static void InterCoreCallBack(char* msg) {
	static int buttonPressCount = 0;

	const struct timespec sleepTime = { 0, 100000000L };
	if (relay.twinState) {
		GPIO_SetValue(relay.fd, GPIO_Value_Low);
	}
	else {
		GPIO_SetValue(relay.fd, GPIO_Value_High);
	}

	nanosleep(&sleepTime, NULL);

	if (relay.twinState) {
		GPIO_SetValue(relay.fd, GPIO_Value_High);
	}
	else {
		GPIO_SetValue(relay.fd, GPIO_Value_Low);
	}

	if (snprintf(msgBuffer, JSON_MESSAGE_BYTES, "{ \"ButtonPressed\": %d }", ++buttonPressCount) > 0) {
		SendMsg(msgBuffer);
	}
}

/// <summary>
///     Set up SIGTERM termination handler, initialize peripherals, and set up event handlers.
/// </summary>
/// <returns>0 on success, or -1 on failure</returns>
static int InitPeripheralsAndHandlers(void)
{
	struct sigaction action;
	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = TerminationHandler;
	sigaction(SIGTERM, &action, NULL);

	epollFd = CreateEpollFd();
	if (epollFd < 0) {
		return -1;
	}

	OpenPeripheral(&sendStatus);
	OpenPeripheral(&relay);
	OpenPeripheral(&light);

	InitDeviceTwins(deviceTwins, NELEMS(deviceTwins));

	// Initialize Grove Shield and Grove Temperature and Humidity Sensor
	GroveShield_Initialize(&i2cFd, 115200);
	sht31 = GroveTempHumiSHT31_Open(i2cFd);

	InitInterCoreComms(epollFd, rtAppComponentId, InterCoreCallBack);  // Initialize Inter Core Communications
	SendMessageToRTCore("Heart Beat"); // Prime RT Core with Component ID Signature

	// Start various timers
	StartTimer(&iotClientDoWork);
	StartTimer(&measureSensor);
	StartTimer(&rtCoreHeatBeat);

	return 0;
}

static int OpenPeripheral(Peripheral* peripheral) {
	peripheral->fd = GPIO_OpenAsOutput(peripheral->pin, GPIO_OutputMode_PushPull, peripheral->initialState);
	if (peripheral->fd < 0) {
		Log_Debug(
			"Error opening GPIO: %s (%d). Check that app_manifest.json includes the GPIO used.\n",
			strerror(errno), errno);
		return -1;
	}
	return 0;
}

static int StartTimer(Timer* timer) {
	timer->fd = CreateTimerFdAndAddToEpoll(epollFd, &timer->period, &timer->eventData, EPOLLIN);
	if (timer->fd < 0) {
		return -1;
	}
	return 0;
}

/// <summary>
///     Close peripherals and handlers.
/// </summary>
static void ClosePeripheralsAndHandlers(void)
{
	Log_Debug("Closing file descriptors\n");
	CloseFdAndPrintError(iotClientDoWork.fd, iotClientDoWork.name);
	CloseFdAndPrintError(measureSensor.fd, measureSensor.name);
	CloseFdAndPrintError(rtCoreHeatBeat.fd, rtCoreHeatBeat.name);
	CloseFdAndPrintError(sendStatus.fd, sendStatus.twinProperty);
	CloseFdAndPrintError(relay.fd, relay.twinProperty);
	CloseFdAndPrintError(epollFd, "Epoll");
}


static void TerminationHandler(int signalNumber)
{
	// Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
	terminationRequired = true;
}

/// <summary>
///     Handle send timer event by writing data to the real-time capable application.
/// </summary>
static void RtCoreHeartBeat(EventData* eventData)
{
	static int heartBeatCount = 0;

	if (ConsumeTimerFdEvent(rtCoreHeatBeat.fd) != 0) {
		terminationRequired = true;
		return;
	}

	if (sprintf(msgBuffer, "HeartBeat-%d", heartBeatCount++) > 0) {
		SendMessageToRTCore(msgBuffer);
	}
}
