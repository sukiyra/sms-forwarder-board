#include "health_policy.h"

#include <cassert>
#include <iostream>

int main() {
  using healthpolicy::Level;
  using healthpolicy::Snapshot;

  assert(healthpolicy::usedPercent(0, 0) == 0);
  assert(healthpolicy::usedPercent(50, 100) == 50);
  assert(healthpolicy::usedPercent(100, 100) == 100);
  assert(healthpolicy::fragmentationPercent(100, 100) == 0);
  assert(healthpolicy::fragmentationPercent(100, 40) == 60);
  assert(healthpolicy::fragmentationPercent(0, 0) == 100);

  assert(healthpolicy::evaluate({320 * 1024, 150 * 1024, 90 * 1024,
                                 110 * 1024, true}) == Level::Healthy);
  assert(healthpolicy::evaluate({320 * 1024, 55 * 1024, 45 * 1024,
                                 28 * 1024, true}) == Level::Warning);
  assert(healthpolicy::evaluate({320 * 1024, 120 * 1024, 80 * 1024,
                                 30 * 1024, true}) == Level::Warning);
  assert(healthpolicy::evaluate({320 * 1024, 31 * 1024, 19 * 1024,
                                 11 * 1024, true}) == Level::Critical);
  assert(healthpolicy::evaluate({320 * 1024, 150 * 1024, 90 * 1024,
                                 110 * 1024, false}) == Level::Critical);
  assert(std::string(healthpolicy::levelName(Level::Healthy)) == "healthy");

  std::cout << "Device health policy tests passed\n";
  return 0;
}
