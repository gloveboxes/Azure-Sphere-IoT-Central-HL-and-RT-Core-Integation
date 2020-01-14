#include "globals.h"

IOTHUB_DEVICE_CLIENT_LL_HANDLE iothubClientHandle = NULL;
int AzureIoTDefaultPollPeriodSeconds = 5;
int AzureIoTMinReconnectPeriodSeconds = 60;
int AzureIoTMaxReconnectPeriodSeconds = 10 * 60;

Peripheral sending = { -1, SEND_STATUS_PIN, GPIO_Value_High, true, false, "SendStatus" };
Peripheral light = { -1, LIGHT_PIN, GPIO_Value_High, true, false, "LightStatus" };
Peripheral relay = { -1, RELAY_PIN, GPIO_Value_Low, false, false, "RelayStatus" };

int epollFd = -1;
int sockFd = -1;

int azureIoTPollPeriodSeconds = 1;
bool iothubAuthenticated = false;
const int keepalivePeriodSeconds = 20;

volatile sig_atomic_t terminationRequired = false;
const char rtAppComponentId[] = "6583cf17-d321-4d72-8283-0b7c5b56442b";

char scopeId[SCOPEID_LENGTH];