#ifndef OTA_POLICY_H
#define OTA_POLICY_H

#include <stddef.h>

namespace otapolicy {

bool parseVersion(const char *value, unsigned long parts[3]);
int compareVersions(const char *candidate, const char *current);
bool isNewerVersion(const char *candidate, const char *current);
bool validSha256Digest(const char *value);
bool validOtaAsset(const char *tag, const char *assetName, const char *digest,
                   size_t size, size_t partitionSize);

}  // namespace otapolicy

#endif
