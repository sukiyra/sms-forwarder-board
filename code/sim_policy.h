#ifndef SIM_POLICY_H
#define SIM_POLICY_H

#include <stdint.h>

namespace simpolicy {

constexpr uint32_t HARDWARE_ERROR_GRACE_MS = 30000UL;
constexpr uint32_t CONFIGURATION_ERROR_GRACE_MS = 60000UL;

bool hardwareErrorIsPersistent(uint32_t elapsedMs);
bool configurationErrorIsPersistent(uint32_t elapsedMs);

}  // namespace simpolicy

#endif
