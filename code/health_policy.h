#ifndef HEALTH_POLICY_H
#define HEALTH_POLICY_H

#include <stddef.h>
#include <stdint.h>

namespace healthpolicy {

enum class Level : uint8_t {
  Healthy,
  Warning,
  Critical,
};

struct Snapshot {
  size_t heapTotal;
  size_t heapFree;
  size_t minFreeHeap;
  size_t largestFreeBlock;
  bool storageReady;
};

uint8_t usedPercent(size_t used, size_t total);
uint8_t fragmentationPercent(size_t freeHeap, size_t largestFreeBlock);
Level evaluate(const Snapshot& snapshot);
const char* levelName(Level level);

}  // namespace healthpolicy

#endif
