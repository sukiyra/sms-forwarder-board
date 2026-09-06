#ifndef API_HANDLERS_H
#define API_HANDLERS_H

#include "globals.h"

void registerApiRoutes();
void processPendingWebSms();
bool pendingWebSmsBusy();

#endif
