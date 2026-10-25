#ifndef PUSH_H
#define PUSH_H

#include "globals.h"
#include "push_protocol.h"

bool sendEmailNotification(const char* subject, const char* body);
pushprotocol::NotificationContext notificationContext(bool complete = true, bool test = false);
void sendSMSToServer(const char* sender, const char* message, const char* timestamp,
                     const pushprotocol::NotificationContext& context);
bool sendToChannel(const PushChannel& channel, const char* sender, const char* message,
                    const char* timestamp, const pushprotocol::NotificationContext& context);
String urlEncode(const String& str);
String jsonEscape(const String& str);
String notificationDisplayTime(const char* timestamp);
String dingtalkSign(const String& secret, int64_t timestamp);
int64_t getUtcMillis();

#endif
