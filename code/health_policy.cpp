#include "health_policy.h"

namespace healthpolicy {

uint8_t usedPercent(size_t used, size_t total) {
  if (total == 0) return 0;
  if (used >= total) return 100;
  return static_cast<uint8_t>((used * 100ULL + total / 2) / total);
}

uint8_t fragmentationPercent(size_t freeHeap, size_t largestFreeBlock) {
  if (freeHeap == 0) return 100;
  if (largestFreeBlock >= freeHeap) return 0;
  return static_cast<uint8_t>(100 -
                              (largestFreeBlock * 100ULL + freeHeap / 2) / freeHeap);
}

Level evaluate(const Snapshot& snapshot) {
  if (!snapshot.storageReady || snapshot.heapTotal == 0 ||
      snapshot.heapFree < 32 * 1024 || snapshot.minFreeHeap < 20 * 1024 ||
      snapshot.largestFreeBlock < 12 * 1024) {
    return Level::Critical;
  }

  if (snapshot.heapFree < 64 * 1024 || snapshot.minFreeHeap < 32 * 1024 ||
      snapshot.largestFreeBlock < 24 * 1024 ||
      fragmentationPercent(snapshot.heapFree, snapshot.largestFreeBlock) >= 65) {
    return Level::Warning;
  }

  return Level::Healthy;
}

const char* levelName(Level level) {
  switch (level) {
    case Level::Healthy: return "healthy";
    case Level::Warning: return "warning";
    default: return "critical";
  }
}

}  // namespace healthpolicy
