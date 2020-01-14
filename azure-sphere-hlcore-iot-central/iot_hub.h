#ifndef iot_hub_h
#define iot_hub_h

#include "device_twins.h"
#include "globals.h"
#include <applibs/log.h>
#include <azure_sphere_provisioning.h>
#include <errno.h>
#include <iothub_client_options.h>
#include <iothub_device_client_ll.h>

const char* getAzureSphereProvisioningResultString(AZURE_SPHERE_PROV_RETURN_VALUE provisioningResult);
const char* GetReasonString(IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason);
void SendMessageCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT, void* );
void SetupAzureClient(Timer *);
void HubConnectionStatusCallback(IOTHUB_CLIENT_CONNECTION_STATUS, IOTHUB_CLIENT_CONNECTION_STATUS_REASON, void*);
void AzureDoWorkTimerEventHandler(EventData*);

#endif