#ifndef OPERATOR_MANAGER_H
#define OPERATOR_MANAGER_H

#include <Arduino.h>

void operatorManagerBegin();
void operatorManagerLoop();

bool operatorManagerIsBusy();
bool operatorManagerStartScan(String& message);
bool operatorManagerStartSelect(const String& numeric, int act, String& message);
bool operatorManagerStartAuto(String& message);
bool operatorManagerStartAutoForEsim(String& message);
bool operatorManagerLastJobSucceeded();
String operatorManagerLastJobMessage();
bool operatorManagerIsAutomaticSelection();

String operatorManagerJson();
String operatorCurrentLabel();
String operatorCurrentNumeric();
int operatorCurrentAct();
String operatorCurrentActName();
void operatorManagerInvalidate();

#endif
