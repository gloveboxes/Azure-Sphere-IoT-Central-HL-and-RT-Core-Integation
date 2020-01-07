#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>

#include <applibs/log.h>
#include <applibs/gpio.h>

// Azure IoT SDK
#include <iothub_client_core_common.h>
#include <iothub_device_client_ll.h>
#include <iothub_client_options.h>
#include <iothubtransportmqtt.h>
#include <iothub.h>
#include <azure_sphere_provisioning.h>

// Core to core communications libraries
#include <ctype.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <applibs/application.h>

#include <applibs/networking.h>
#include "epoll_timerfd_utilities.h"

// Grove Temperature and Humidity Sensor
#include "../MT3620_Grove_Shield/MT3620_Grove_Shield_Library/Grove.h"
#include "../MT3620_Grove_Shield/MT3620_Grove_Shield_Library/Sensors/GroveTempHumiSHT31.h"

#include "parson.h"

// GPIO Pins used in the HL App
#define SEND_STATUS_PIN 19
#define LIGHT_PIN 21
#define RELAY_PIN 0

#define JSON_MESSAGE_BYTES 100  // Number of bytes to allocate for the JSON telemetry message for IoT Central
#define SCOPEID_LENGTH 20

static volatile sig_atomic_t terminationRequired = false;

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

static char scopeId[SCOPEID_LENGTH]; // ScopeId for the Azure IoT Central application, set in app_manifest.json, CmdArgs

static IOTHUB_DEVICE_CLIENT_LL_HANDLE iothubClientHandle = NULL;
static const int keepalivePeriodSeconds = 20;
static bool iothubAuthenticated = false;


// Azure IoT poll periods
static const int AzureIoTDefaultPollPeriodSeconds = 5;
static const int AzureIoTMinReconnectPeriodSeconds = 60;
static const int AzureIoTMaxReconnectPeriodSeconds = 10 * 60;

static int azureIoTPollPeriodSeconds = 1;
static int msgId = 0;

static int i2cFd;
static void* sht31;

static Peripheral sending = { -1, SEND_STATUS_PIN, GPIO_Value_High, true, false, "SendStatus" };
static Peripheral light = { -1, LIGHT_PIN, GPIO_Value_High, true, false, "LightStatus" };
static Peripheral relay = { -1, RELAY_PIN, GPIO_Value_Low, false, false, "RelayStatus" };

static void AzureTimerEventHandler(EventData*);
static void AzureDoWorkTimerEventHandler(EventData*);
static void SocketEventHandler(EventData* eventData);
static void InterCoreHeartBeat(EventData* eventData);

static int epollFd = -1;
static Timer iotClientDoWork = { .eventData = {.eventHandler = &AzureDoWorkTimerEventHandler }, .period = { 1, 0 }, .name = "DoWork" };
static Timer iotClientMeasureSensor = { .eventData = {.eventHandler = &AzureTimerEventHandler }, .period = { 10, 0 }, .name = "MeasureSensor" };
static Timer rtCoreHeatBeat = { .eventData = {.eventHandler = &InterCoreHeartBeat }, .period = { 30, 0 }, .name = "rtCoreSend" };

//static int timerFd = -1;
static int sockFd = -1;
static const char rtAppComponentId[] = "6583cf17-d321-4d72-8283-0b7c5b56442b";
static void SendMessageToRTCore(void);
static EventData socketEventData = { .eventHandler = &SocketEventHandler };

// Forward signatures
static void TerminationHandler(int);
static void SendTelemetry(void);
static void SetupAzureClient(void);
static void SendMessageCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT, void*);
static void TwinReportState(const char*, bool);
static void SetDesiredState(JSON_Object*, Peripheral*);
static const char* GetReasonString(IOTHUB_CLIENT_CONNECTION_STATUS_REASON);
static const char* getAzureSphereProvisioningResultString(AZURE_SPHERE_PROV_RETURN_VALUE);
static int InitPeripheralsAndHandlers(void);
static void ClosePeripheralsAndHandlers(void);
static int InitInterCoreComms(void);



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

	InitInterCoreComms();  // Initialize Inter Core Communications
	SendMessageToRTCore(); // Prime RT Core with Component ID Signature

	// Start various timers
	StartTimer(&iotClientDoWork);
	StartTimer(&iotClientMeasureSensor);
	StartTimer(&rtCoreHeatBeat);

	return 0;
}

static int InitInterCoreComms() {
	// Open connection to real-time capable application.
	sockFd = Application_Socket(rtAppComponentId);
	if (sockFd == -1) {
		Log_Debug("ERROR: Unable to create socket: %d (%s)\n", errno, strerror(errno));
		return -1;
	}

	// Set timeout, to handle case where real-time capable application does not respond.
	static const struct timeval recvTimeout = { .tv_sec = 5, .tv_usec = 0 };
	int result = setsockopt(sockFd, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));
	if (result == -1) {
		Log_Debug("ERROR: Unable to set socket timeout: %d (%s)\n", errno, strerror(errno));
		return -1;
	}

	// Register handler for incoming messages from real-time capable application.
	if (RegisterEventHandlerToEpoll(epollFd, sockFd, &socketEventData, EPOLLIN) != 0) {
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
	CloseFdAndPrintError(iotClientMeasureSensor.fd, iotClientMeasureSensor.name);
	CloseFdAndPrintError(rtCoreHeatBeat.fd, rtCoreHeatBeat.name);
	CloseFdAndPrintError(sending.fd, sending.twinProperty);
	CloseFdAndPrintError(relay.fd, relay.twinProperty);
	CloseFdAndPrintError(epollFd, "Epoll");
}

/// <summary>
///     Callback invoked when a Device Twin update is received from IoT Hub.
///     Updates local state for 'showEvents' (bool).
/// </summary>
/// <param name="payload">contains the Device Twin JSON document (desired and reported)</param>
/// <param name="payloadSize">size of the Device Twin JSON document</param>
static void TwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char* payload,
	size_t payloadSize, void* userContextCallback)
{
	JSON_Value* root_value = NULL;
	JSON_Object* root_object = NULL;

	char* payLoadString = (char*)malloc(payloadSize + 1);
	if (payLoadString == NULL) {
		goto cleanup;
	}

	memcpy(payLoadString, payload, payloadSize);
	payLoadString[payloadSize] = 0; //null terminate string

	root_value = json_parse_string(payLoadString);
	if (root_value == NULL) {
		goto cleanup;
	}

	root_object = json_value_get_object(root_value);
	if (root_object == NULL) {
		goto cleanup;
	}


	JSON_Object* desiredProperties = json_object_dotget_object(root_object, "desired");
	if (desiredProperties == NULL) {
		desiredProperties = root_object;
	}

	SetDesiredState(desiredProperties, &relay);
	SetDesiredState(desiredProperties, &light);

cleanup:
	// Release the allocated memory.
	if (root_value != NULL) {
		json_value_free(root_value);
	}
	free(payLoadString);
}

static void SetDesiredState(JSON_Object* desiredProperties, Peripheral* peripheral) {
	JSON_Object* jsonObject = json_object_dotget_object(desiredProperties, peripheral->twinProperty);
	if (jsonObject != NULL) {
		peripheral->twinState = (bool)json_object_get_boolean(jsonObject, "value");
		if (peripheral->invertPin) {
			GPIO_SetValue(peripheral->fd, (peripheral->twinState == true ? GPIO_Value_Low : GPIO_Value_High));
		}
		else {
			GPIO_SetValue(peripheral->fd, (peripheral->twinState == true ? GPIO_Value_High : GPIO_Value_Low));
		}
		TwinReportState(peripheral->twinProperty, peripheral->twinState);
	}
}

static int AzureDirectMethodHandler(const char* method_name, const unsigned char* payload, size_t payloadSize,
	unsigned char** responsePayload, size_t* responsePayloadSize, void* userContextCallback) {

	const char* onSuccess = "\"Successfully invoke device method\"";
	const char* notFound = "\"No method found\"";

	const char* responseMessage = onSuccess;
	int result = 200;
	JSON_Value* root_value = NULL;
	JSON_Object* root_object = NULL;

	// Prepare the payload for the response. This is a heap allocated null terminated string.
	// The Azure IoT Hub SDK is responsible of freeing it.
	*responsePayload = NULL;  // Response payload content.
	*responsePayloadSize = 0; // Response payload content size.

	char* payLoadString = (char*)malloc(payloadSize + 1);
	if (payLoadString == NULL) {
		responseMessage = "payload memory failed";
		result = 500;
		goto cleanup;
	}

	memcpy(payLoadString, payload, payloadSize);
	payLoadString[payloadSize] = 0; //null terminate string

	root_value = json_parse_string(payLoadString);
	if (root_value == NULL) {
		responseMessage = "Invalid JSON";
		result = 500;
		goto cleanup;
	}

	root_object = json_value_get_object(root_value);
	if (root_object == NULL) {
		responseMessage = "Invalid JSON";
		result = 500;
		goto cleanup;
	}

	if (strcmp(method_name, "fanspeed") == 0)
	{
		int speed = (int)json_object_get_number(root_object, "speed");
		Log_Debug("Set fan speed %d", speed);
	}
	else
	{
		responseMessage = notFound;
		result = 404;
	}

cleanup:

	// Prepare the payload for the response. This is a heap allocated null terminated string.
	// The Azure IoT Hub SDK is responsible of freeing it.
	*responsePayloadSize = strlen(responseMessage);
	*responsePayload = (unsigned char*)malloc(*responsePayloadSize);
	strncpy((char*)(*responsePayload), responseMessage, *responsePayloadSize);

	if (root_value != NULL) {
		json_value_free(root_value);
	}
	free(payLoadString);

	return result;
}

/// <summary>
///     Callback invoked when the Device Twin reported properties are accepted by IoT Hub.
/// </summary>
static void ReportStatusCallback(int result, void* context)
{
	Log_Debug("INFO: Device Twin reported properties update result: HTTP status code %d\n", result);
}

static void TwinReportState(const char* propertyName, bool propertyValue)
{
	if (iothubClientHandle == NULL) {
		Log_Debug("ERROR: client not initialized\n");
	}
	else {
		static char reportedPropertiesString[30] = { 0 };
		int len = snprintf(reportedPropertiesString, 30, "{\"%s\":%s}", propertyName,
			(propertyValue == true ? "true" : "false"));
		if (len < 0)
			return;

		if (IoTHubDeviceClient_LL_SendReportedState(
			iothubClientHandle, (unsigned char*)reportedPropertiesString,
			strlen(reportedPropertiesString), ReportStatusCallback, 0) != IOTHUB_CLIENT_OK) {
			Log_Debug("ERROR: failed to set reported state for '%s'.\n", propertyName);
		}
		else {
			Log_Debug("INFO: Reported state for '%s' to value '%s'.\n", propertyName,
				(propertyValue == true ? "true" : "false"));
		}
	}
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

static void AzureDoWorkTimerEventHandler(EventData* eventData) {
	if (iothubClientHandle != NULL) {
		IoTHubDeviceClient_LL_DoWork(iothubClientHandle);
		//Log_Debug("do work");
	}
}

/// <summary>
/// Azure timer event:  Check connection status and send telemetry
/// </summary>
static void AzureTimerEventHandler(EventData* eventData)
{
	if (ConsumeTimerFdEvent(iotClientMeasureSensor.fd) != 0) {
		terminationRequired = true;
		return;
	}

	bool isNetworkReady = false;
	if (Networking_IsNetworkingReady(&isNetworkReady) != -1) {
		if (isNetworkReady && !iothubAuthenticated) {
			SetupAzureClient();
		}
	}
	else {
		Log_Debug("Failed to get Network state\n");
	}

	if (iothubAuthenticated) {
		SendTelemetry();
		IoTHubDeviceClient_LL_DoWork(iothubClientHandle);
	}
}

static void TerminationHandler(int signalNumber)
{
	// Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
	terminationRequired = true;
}

/// <summary>
///     Callback confirming message delivered to IoT Hub.
/// </summary>
/// <param name="result">Message delivery status</param>
/// <param name="context">User specified context</param>
static void SendMessageCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* context)
{
	Log_Debug("INFO: Message received by IoT Hub. Result is: %d\n", result);
}

/// <summary>
///     Sets the IoT Hub authentication state for the app
///     The SAS Token expires which will set the authentication state
/// </summary>
static void HubConnectionStatusCallback(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* userContextCallback)
{
	iothubAuthenticated = (result == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED);
	Log_Debug("IoT Hub Authenticated: %s\n", GetReasonString(reason));
}

/// <summary>
///     Sets up the Azure IoT Hub connection (creates the iothubClientHandle)
///     When the SAS Token for a device expires the connection needs to be recreated
///     which is why this is not simply a one time call.
/// </summary>
static void SetupAzureClient(void)
{
	if (iothubClientHandle != NULL) {
		IoTHubDeviceClient_LL_Destroy(iothubClientHandle);
	}

	AZURE_SPHERE_PROV_RETURN_VALUE provResult = IoTHubDeviceClient_LL_CreateWithAzureSphereDeviceAuthProvisioning(scopeId, 10000, &iothubClientHandle);
	Log_Debug("IoTHubDeviceClient_LL_CreateWithAzureSphereDeviceAuthProvisioning returned '%s'.\n", getAzureSphereProvisioningResultString(provResult));

	if (provResult.result != AZURE_SPHERE_PROV_RESULT_OK) {

		// If we fail to connect, reduce the polling frequency, starting at
		// AzureIoTMinReconnectPeriodSeconds and with a backoff up to
		// AzureIoTMaxReconnectPeriodSeconds
		if (azureIoTPollPeriodSeconds == AzureIoTDefaultPollPeriodSeconds) {
			azureIoTPollPeriodSeconds = AzureIoTMinReconnectPeriodSeconds;
		}
		else {
			azureIoTPollPeriodSeconds *= 2;
			if (azureIoTPollPeriodSeconds > AzureIoTMaxReconnectPeriodSeconds) {
				azureIoTPollPeriodSeconds = AzureIoTMaxReconnectPeriodSeconds;
			}
		}

		struct timespec azureTelemetryPeriod = { azureIoTPollPeriodSeconds, 0 };
		SetTimerFdToPeriod(iotClientMeasureSensor.fd, &azureTelemetryPeriod);

		Log_Debug("ERROR: failure to create IoTHub Handle - will retry in %i seconds.\n", azureIoTPollPeriodSeconds);
		return;
	}

	// Successfully connected, so make sure the polling frequency is back to the default
	azureIoTPollPeriodSeconds = AzureIoTDefaultPollPeriodSeconds;
	struct timespec azureTelemetryPeriod = { azureIoTPollPeriodSeconds, 0 };
	SetTimerFdToPeriod(iotClientMeasureSensor.fd, &azureTelemetryPeriod);

	iothubAuthenticated = true;

	if (IoTHubDeviceClient_LL_SetOption(iothubClientHandle, OPTION_KEEP_ALIVE, &keepalivePeriodSeconds) != IOTHUB_CLIENT_OK) {
		Log_Debug("ERROR: failure setting option \"%s\"\n", OPTION_KEEP_ALIVE);
		return;
	}

	IoTHubDeviceClient_LL_SetDeviceTwinCallback(iothubClientHandle, TwinCallback, NULL);
	IoTHubDeviceClient_LL_SetDeviceMethodCallback(iothubClientHandle, AzureDirectMethodHandler, NULL);
	IoTHubDeviceClient_LL_SetConnectionStatusCallback(iothubClientHandle, HubConnectionStatusCallback, NULL);

}

/// <summary>
///     Converts AZURE_SPHERE_PROV_RETURN_VALUE to a string.
/// </summary>
static const char* getAzureSphereProvisioningResultString(
	AZURE_SPHERE_PROV_RETURN_VALUE provisioningResult)
{
	switch (provisioningResult.result) {
	case AZURE_SPHERE_PROV_RESULT_OK:
		return "AZURE_SPHERE_PROV_RESULT_OK";
	case AZURE_SPHERE_PROV_RESULT_INVALID_PARAM:
		return "AZURE_SPHERE_PROV_RESULT_INVALID_PARAM";
	case AZURE_SPHERE_PROV_RESULT_NETWORK_NOT_READY:
		return "AZURE_SPHERE_PROV_RESULT_NETWORK_NOT_READY";
	case AZURE_SPHERE_PROV_RESULT_DEVICEAUTH_NOT_READY:
		return "AZURE_SPHERE_PROV_RESULT_DEVICEAUTH_NOT_READY";
	case AZURE_SPHERE_PROV_RESULT_PROV_DEVICE_ERROR:
		return "AZURE_SPHERE_PROV_RESULT_PROV_DEVICE_ERROR";
	case AZURE_SPHERE_PROV_RESULT_GENERIC_ERROR:
		return "AZURE_SPHERE_PROV_RESULT_GENERIC_ERROR";
	default:
		return "UNKNOWN_RETURN_VALUE";
	}
}

/// <summary>
///     Converts the IoT Hub connection status reason to a string.
/// </summary>
static const char* GetReasonString(IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason)
{
	static char* reasonString = "unknown reason";
	switch (reason) {
	case IOTHUB_CLIENT_CONNECTION_EXPIRED_SAS_TOKEN:
		reasonString = "IOTHUB_CLIENT_CONNECTION_EXPIRED_SAS_TOKEN";
		break;
	case IOTHUB_CLIENT_CONNECTION_DEVICE_DISABLED:
		reasonString = "IOTHUB_CLIENT_CONNECTION_DEVICE_DISABLED";
		break;
	case IOTHUB_CLIENT_CONNECTION_BAD_CREDENTIAL:
		reasonString = "IOTHUB_CLIENT_CONNECTION_BAD_CREDENTIAL";
		break;
	case IOTHUB_CLIENT_CONNECTION_RETRY_EXPIRED:
		reasonString = "IOTHUB_CLIENT_CONNECTION_RETRY_EXPIRED";
		break;
	case IOTHUB_CLIENT_CONNECTION_NO_NETWORK:
		reasonString = "IOTHUB_CLIENT_CONNECTION_NO_NETWORK";
		break;
	case IOTHUB_CLIENT_CONNECTION_COMMUNICATION_ERROR:
		reasonString = "IOTHUB_CLIENT_CONNECTION_COMMUNICATION_ERROR";
		break;
	case IOTHUB_CLIENT_CONNECTION_OK:
		reasonString = "IOTHUB_CLIENT_CONNECTION_OK";
		break;
	}
	return reasonString;
}


/// <summary>
///     Handle send timer event by writing data to the real-time capable application.
/// </summary>
static void InterCoreHeartBeat(EventData* eventData)
{
	if (ConsumeTimerFdEvent(rtCoreHeatBeat.fd) != 0) {
		terminationRequired = true;
		return;
	}

	SendMessageToRTCore();
}

/// <summary>
///     Helper function for TimerEventHandler sends message to real-time capable application.
/// </summary>
static void SendMessageToRTCore(void)
{
	static int iter = 0;

	static char txMessage[32];
	sprintf(txMessage, "HeartBeat-%d", iter++);

	int bytesSent = send(sockFd, txMessage, strlen(txMessage), 0);
	if (bytesSent == -1) {
		Log_Debug("ERROR: Unable to send message: %d (%s)\n", errno, strerror(errno));
		terminationRequired = true;
		return;
	}
}

/// <summary>
///     Handle socket event by reading incoming data from real-time capable application.
/// </summary>
static void SocketEventHandler(EventData* eventData)
{
	// Read response from real-time capable application.
	const struct timespec sleepTime = { 0, 100000000L };
	char rxBuf[32];
	char msg[32];
	memset(msg, 0, sizeof msg);

	int bytesReceived = recv(sockFd, rxBuf, sizeof(rxBuf), 0);

	if (bytesReceived == -1) {
		Log_Debug("ERROR: Unable to receive message: %d (%s)\n", errno, strerror(errno));
		terminationRequired = true;
	}

	//Log_Debug("Received %d bytes: ", bytesReceived);
	for (int i = 0; i < bytesReceived; ++i) {
		msg[i] = isprint(rxBuf[i]) ? rxBuf[i] : '.';
	}
	Log_Debug(msg);

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
