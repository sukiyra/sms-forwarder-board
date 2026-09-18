#include "sim_policy.h"

#include <cassert>
#include <iostream>

int main() {
  assert(!simpolicy::hardwareErrorIsPersistent(0));
  assert(!simpolicy::hardwareErrorIsPersistent(29999));
  assert(simpolicy::hardwareErrorIsPersistent(30000));
  assert(!simpolicy::configurationErrorIsPersistent(59999));
  assert(simpolicy::configurationErrorIsPersistent(60000));
  std::cout << "SIM recovery policy tests passed\n";
  return 0;
}
