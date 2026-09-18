#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <Arduino.h>

void otaManagerBegin();
void otaManagerConfirmBoot();
void otaManagerLoop();
bool otaManagerBusy();
bool otaManagerSupported();
size_t otaManagerPartitionSize();
String otaManagerRunningPartition();
String otaManagerStatusJson();
bool otaManagerQueueCheck(String &error);
bool otaManagerQueueInstall(String &error);

#endif
