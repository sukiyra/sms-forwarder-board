#ifndef MODEM_H
#define MODEM_H

#include "globals.h"
#include "sms_capability.h"

String sendATCommand(const char* cmd, unsigned long timeout);
void modemPowerCycle();
void resetModule();
void modemInit();
bool sendATandWaitOK(const char* cmd, unsigned long timeout);
bool waitCEREG();
void blink_short(unsigned long gap_time = 500);
bool sendSMS(const char* phoneNumber, const char* message);
bool modemIsBusy();
bool modemIsBooting();
bool modemSupportsPdpContextControl();
bool modemModelSupported();
const char* modemSmsDeliveryMode();
void modemSetSmsDeliveryMode(const char* mode);
SmsCarrierSupport modemSmsCarrierSupport();
const char* modemSmsCarrierSupportName();
String modemSmsCarrierMessage();
bool modemSmsCarrierBlocked();
bool modemAcquireExclusive();
void modemReleaseExclusive();

#endif
