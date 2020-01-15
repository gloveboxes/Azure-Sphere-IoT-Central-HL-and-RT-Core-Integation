#include "globals.h"

char scopeId[SCOPEID_LENGTH];
char msgBuffer[JSON_MESSAGE_BYTES] = { 0 };

volatile sig_atomic_t terminationRequired = false;
