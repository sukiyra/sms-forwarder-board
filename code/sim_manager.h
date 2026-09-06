#ifndef SIM_MANAGER_H
#define SIM_MANAGER_H

#include <Arduino.h>

void simManagerBegin();
void simManagerLoop();
void simManagerInvalidate();
void simManagerRestoreSmsConfiguration();

bool simManagerIsBusy();
bool simManagerIsKnown();
bool simManagerIsPresent();
bool simManagerIsReady();
bool simManagerSmsReady();
String simManagerIccidTail();
String simManagerPhoneNumber();
String simManagerHomePlmn();
int simManagerRegistrationStatus();
bool simManagerIsRoaming();
void simManagerCaptureIccid(const String &response);
bool simManagerSignalKnown();
bool simManagerSignalRsrqKnown();
int simManagerSignalRsrpDbm();
int simManagerSignalRsrqTenthsDb();
int simManagerSignalRsrpRaw();
unsigned long simManagerSignalUpdatedAt();
const char* simManagerStateName();
const char* simManagerMessage();
uint32_t simManagerGeneration();
unsigned long simManagerChangedAt();

#endif
