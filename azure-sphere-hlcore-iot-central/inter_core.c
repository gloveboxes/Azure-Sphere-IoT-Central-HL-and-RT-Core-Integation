#include "inter_core.h"

EventData socketEventData = { .eventHandler = &SocketEventHandler };
void (*_interCoreCallback)(char*);

void SendEventMsg(IOTHUB_DEVICE_CLIENT_LL_HANDLE iothubClientHandle, bool iothubAuthenticated) {
	static int buttonPressCount = 0;

	if (iothubAuthenticated) {

		static char eventBuffer[JSON_MESSAGE_BYTES] = { 0 };
		static const char* EventMsgTemplate = "{ \"ButtonPressed\": %d }";

		snprintf(eventBuffer, JSON_MESSAGE_BYTES, EventMsgTemplate, ++buttonPressCount);

		IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromString(eventBuffer);

		if (messageHandle == 0) {
			Log_Debug("WARNING: unable to create a new IoTHubMessage\n");
			return;
		}

		if (IoTHubDeviceClient_LL_SendEventAsync(iothubClientHandle, messageHandle, NULL,
			/*&callback_param*/ 0) != IOTHUB_CLIENT_OK) {
			Log_Debug("WARNING: failed to hand over the message to IoTHubClient\n");
		}
		else {
			Log_Debug("INFO: IoTHubClient accepted the message for delivery\n");
		}

		IoTHubMessage_Destroy(messageHandle);
	}
}


bool SendMessageToRTCore(int sockFd)
{
	static int iter = 0;
	static char txMessage[32];

	sprintf(txMessage, "HeartBeat-%d", iter++);

	int bytesSent = send(sockFd, txMessage, strlen(txMessage), 0);
	if (bytesSent == -1) {
		Log_Debug("ERROR: Unable to send message: %d (%s)\n", errno, strerror(errno));
		return false;
	}

	return true;
}

int InitInterCoreComms(void (*interCoreCallback)(char*)) {
	_interCoreCallback = interCoreCallback;
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
///     Handle socket event by reading incoming data from real-time capable application.
/// </summary>
void SocketEventHandler(EventData* eventData)
{
	if (!ProcessMsg(&relay, sockFd)) {
		terminationRequired = true;
	}

	//bool isNetworkReady = false;
	//if (Networking_IsNetworkingReady(&isNetworkReady) != -1) {
	//	if (isNetworkReady && !iothubAuthenticated) {
	//		SetupAzureClient(&iotClientMeasureSensor);
	//	}
	//}

	if (iothubAuthenticated) {
		SendEventMsg(iothubClientHandle, iothubAuthenticated);
	}
}

/// <summary>
///     Handle socket event by reading incoming data from real-time capable application.
/// </summary>
bool ProcessMsg()
{

	char rxBuf[32];
	char msg[32];
	memset(msg, 0, sizeof msg);

	int bytesReceived = recv(sockFd, rxBuf, sizeof(rxBuf), 0);

	if (bytesReceived == -1) {
		//Log_Debug("ERROR: Unable to receive message: %d (%s)\n", errno, strerror(errno));
		return false;
	}

	//Log_Debug("Received %d bytes: ", bytesReceived);
	for (int i = 0; i < bytesReceived; ++i) {
		msg[i] = isprint(rxBuf[i]) ? rxBuf[i] : '.';
	}
	Log_Debug(msg);

	_interCoreCallback(msg);

	return true;
}