#ifndef SMS_PROCESS_H
#define SMS_PROCESS_H

#include "globals.h"

void initConcatBuffer();
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts);
String assembleConcatSms(int slot);
void clearConcatSlot(int slot);
void checkConcatTimeout();
void checkSmsReceiveTimeout();
bool smsReceiverAwaitingPdu();
void smsStoredMessageLoop();
bool smsStoredMessageIsBusy();
void smsScanStoredMessages(const char* memory, uint16_t lastIndex);
void smsResetForSimChange();
String readSerialLine(HardwareSerial& port);
void dispatchSerial1Byte(char value, bool debugLog = false);
bool isHexString(const String& str);
bool isInNumberBlackList(const char* sender);
bool isAdmin(const char* sender);
void processAdminCommand(const char* sender, const char* text);
bool processSmsContent(const char* sender, const char* text, const char* timestamp);
void checkSerial1URC();

#endif
