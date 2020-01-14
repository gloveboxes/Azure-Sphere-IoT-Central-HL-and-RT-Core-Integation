//static int AzureDirectMethodHandler(const char* method_name, const unsigned char* payload, size_t payloadSize,
//	unsigned char** responsePayload, size_t* responsePayloadSize, void* userContextCallback) {
//
//	const char* onSuccess = "\"Successfully invoke device method\"";
//	const char* notFound = "\"No method found\"";
//
//	const char* responseMessage = onSuccess;
//	int result = 200;
//	JSON_Value* root_value = NULL;
//	JSON_Object* root_object = NULL;
//
//	// Prepare the payload for the response. This is a heap allocated null terminated string.
//	// The Azure IoT Hub SDK is responsible of freeing it.
//	*responsePayload = NULL;  // Response payload content.
//	*responsePayloadSize = 0; // Response payload content size.
//
//	char* payLoadString = (char*)malloc(payloadSize + 1);
//	if (payLoadString == NULL) {
//		responseMessage = "payload memory failed";
//		result = 500;
//		goto cleanup;
//	}
//
//	memcpy(payLoadString, payload, payloadSize);
//	payLoadString[payloadSize] = 0; //null terminate string
//
//	root_value = json_parse_string(payLoadString);
//	if (root_value == NULL) {
//		responseMessage = "Invalid JSON";
//		result = 500;
//		goto cleanup;
//	}
//
//	root_object = json_value_get_object(root_value);
//	if (root_object == NULL) {
//		responseMessage = "Invalid JSON";
//		result = 500;
//		goto cleanup;
//	}
//
//	if (strcmp(method_name, "fanspeed") == 0)
//	{
//		int speed = (int)json_object_get_number(root_object, "speed");
//		Log_Debug("Set fan speed %d", speed);
//	}
//	else
//	{
//		responseMessage = notFound;
//		result = 404;
//	}
//
//cleanup:
//
//	// Prepare the payload for the response. This is a heap allocated null terminated string.
//	// The Azure IoT Hub SDK is responsible of freeing it.
//	*responsePayloadSize = strlen(responseMessage);
//	*responsePayload = (unsigned char*)malloc(*responsePayloadSize);
//	strncpy((char*)(*responsePayload), responseMessage, *responsePayloadSize);
//
//	if (root_value != NULL) {
//		json_value_free(root_value);
//	}
//	free(payLoadString);
//
//	return result;
//}