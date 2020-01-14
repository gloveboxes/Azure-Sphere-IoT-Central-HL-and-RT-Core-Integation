#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>

#include <applibs/log.h>
#include <applibs/gpio.h>

// Grove Temperature and Humidity Sensor
#include "../MT3620_Grove_Shield/MT3620_Grove_Shield_Library/Grove.h"
#include "../MT3620_Grove_Shield/MT3620_Grove_Shield_Library/Sensors/GroveTempHumiSHT31.h"

#include "globals.h"
#include "inter_core.h"
#include "iot_hub.h"

static int msgId = 0;

static int i2cFd;
static void* sht31;

static void AzureMeasureSensorEventHandler(EventData*);
void InterCoreHeartBeat(EventData*);

//static int epollFd = -1;
static Timer iotClientDoWork = { .eventData = {.eventHandler = &AzureDoWorkTimerEventHandler }, .period = { 1, 0 }, .name = "DoWork" };
static Timer iotClientMeasureSensor = { .eventData = {.eventHandler = &AzureMeasureSensorEventHandler }, .period = { 10, 0 }, .name = "MeasureSensor" };
static Timer rtCoreHeatBeat = { .eventData = {.eventHandler = &InterCoreHeartBeat }, .period = { 30, 0 }, .name = "rtCoreSend" };


// Forward signatures
static void TerminationHandler(int);
static void SendTelemetry(void);

static int InitPeripheralsAndHandlers(void);
static void ClosePeripheralsAndHandlers(void);


int main(int argc, char* argv[])
{
	Log_Debug("IoT Hub/Central Application starting.\n");

	if (argc == 2) {
		Log_Debug("Setting Azure Scope ID %s\n", argv[1]);
		strncpy(scopeId, argv[1], SCOPEID_LENGTH);
	}
	else {
		Log_Debug("ScopeId needs to be set in the app_manifest CmdArgs\n");
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
	GroveTempHumiSHT31_Read(sht31);
	float temperature = GroveTempHumiSHT31_GetTemperature(sht31);
	float humidity = GroveTempHumiSHT31_GetHumidity(sht31);

	static const char* EventMsgTemplate = "{ \"Temperature\": \"%3.2f\", \"Humidity\": \"%3.1f\", \"MsgId\":%d }";
	return snprintf(eventBuffer, len, EventMsgTemplate, temperature, humidity, msgId++);
}

static void preSendTelemtry(void) {
	GPIO_SetValue(sending.fd, GPIO_Value_Low);
}

static void postSendTelemetry(void) {
	GPIO_SetValue(sending.fd, GPIO_Value_High);
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

static void InterCoreCallBack(char * msg) {
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

	// Open Various Peripherals
	OpenPeripheral(&sending);
	OpenPeripheral(&relay);
	OpenPeripheral(&light);

	// Initialize Grove Shield and Grove Temperature and Humidity Sensor
	GroveShield_Initialize(&i2cFd, 115200);
	sht31 = GroveTempHumiSHT31_Open(i2cFd);

	InitInterCoreComms(InterCoreCallBack);  // Initialize Inter Core Communications
	SendMessageToRTCore(sockFd); // Prime RT Core with Component ID Signature

	// Start various timers
	StartTimer(&iotClientDoWork);
	StartTimer(&iotClientMeasureSensor);
	StartTimer(&rtCoreHeatBeat);

	return 0;
}



/// <summary>
///     Close peripherals and handlers.
/// </summary>
static void ClosePeripheralsAndHandlers(void)
{
	Log_Debug("Closing file descriptors\n");
	CloseFdAndPrintError(iotClientDoWork.fd, iotClientDoWork.name);
	CloseFdAndPrintError(iotClientMeasureSensor.fd, iotClientMeasureSensor.name);
	CloseFdAndPrintError(rtCoreHeatBeat.fd, rtCoreHeatBeat.name);
	CloseFdAndPrintError(sending.fd, sending.twinProperty);
	CloseFdAndPrintError(relay.fd, relay.twinProperty);
	CloseFdAndPrintError(epollFd, "Epoll");
}


/// <summary>
///     Sends telemetry to Azure IoT Central
/// </summary>
static void SendTelemetry(void)
{
	preSendTelemtry();

	static char eventBuffer[JSON_MESSAGE_BYTES] = { 0 };

	int len = ReadTelemetry(eventBuffer, sizeof(eventBuffer));

	if (len < 0)
		return;

	Log_Debug("Sending IoT Hub Message: %s\n", eventBuffer);

	IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromString(eventBuffer);

	if (messageHandle == 0) {
		Log_Debug("WARNING: unable to create a new IoTHubMessage\n");
		return;
	}

	if (IoTHubDeviceClient_LL_SendEventAsync(iothubClientHandle, messageHandle, SendMessageCallback,
		/*&callback_param*/ 0) != IOTHUB_CLIENT_OK) {
		Log_Debug("WARNING: failed to hand over the message to IoTHubClient\n");
	}
	else {
		Log_Debug("INFO: IoTHubClient accepted the message for delivery\n");
	}

	IoTHubMessage_Destroy(messageHandle);

	postSendTelemetry();
}


/// <summary>
/// Azure timer event:  Check connection status and send telemetry
/// </summary>
static void AzureMeasureSensorEventHandler(EventData* eventData)
{
	if (ConsumeTimerFdEvent(iotClientMeasureSensor.fd) != 0) {
		terminationRequired = true;
		return;
	}

	bool isNetworkReady = false;
	if (Networking_IsNetworkingReady(&isNetworkReady) != -1) {
		if (isNetworkReady && !iothubAuthenticated) {
			SetupAzureClient(&iotClientMeasureSensor);
		}
	}
	else {
		Log_Debug("Failed to get Network state\n");
	}

	if (iothubAuthenticated) {
		SendTelemetry();
	}
}

static void TerminationHandler(int signalNumber)
{
	// Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
	terminationRequired = true;
}




/// <summary>
///     Handle send timer event by writing data to the real-time capable application.
/// </summary>
void InterCoreHeartBeat(EventData* eventData)
{
	if (ConsumeTimerFdEvent(rtCoreHeatBeat.fd) != 0) {
		terminationRequired = true;
		return;
	}

	//SendToRTCore();
	SendMessageToRTCore(sockFd);
}
