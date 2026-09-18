#include "sim_policy.h"

namespace simpolicy {

bool hardwareErrorIsPersistent(uint32_t elapsedMs) {
  return elapsedMs >= HARDWARE_ERROR_GRACE_MS;
}

bool configurationErrorIsPersistent(uint32_t elapsedMs) {
  return elapsedMs >= CONFIGURATION_ERROR_GRACE_MS;
}

}  // namespace simpolicy
