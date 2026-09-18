#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <Arduino.h>

constexpr uint16_t DISCOVERY_PORT = 37888;

void discoveryBegin();
void discoveryLoop();

#endif
