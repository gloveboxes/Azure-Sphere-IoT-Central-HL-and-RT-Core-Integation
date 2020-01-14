#include "device_twins.h"


/// <summary>
///     Callback invoked when a Device Twin update is received from IoT Hub.
///     Updates local state for 'showEvents' (bool).
/// </summary>
/// <param name="payload">contains the Device Twin JSON document (desired and reported)</param>
/// <param name="payloadSize">size of the Device Twin JSON document</param>
void TwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char* payload,
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

void SetDesiredState(JSON_Object* desiredProperties, Peripheral* peripheral) {
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

void TwinReportState(const char* propertyName, bool propertyValue)
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
///     Callback invoked when the Device Twin reported properties are accepted by IoT Hub.
/// </summary>
void ReportStatusCallback(int result, void* context)
{
	Log_Debug("INFO: Device Twin reported properties update result: HTTP status code %d\n", result);
}
