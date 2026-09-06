#ifndef CONFIG_H
#define CONFIG_H

#include "globals.h"

void saveConfig();
void loadConfig();
bool isPushChannelValid(const PushChannel& ch);
bool isConfigValid();
String getDeviceUrl();
uint32_t configTaskLastRun(int index);
void configRecordTaskRun(int index, uint32_t epoch);

#endif
