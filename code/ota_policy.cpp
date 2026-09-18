#include "ota_policy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace otapolicy {

namespace {

const char *versionBody(const char *value) {
  if (!value) return nullptr;
  if (value[0] == 'v') return value + 1;
  static const char prefix[] = "sukiyra-";
  if (strncmp(value, prefix, sizeof(prefix) - 1) == 0) return value + sizeof(prefix) - 1;
  return nullptr;
}

bool hexDigit(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

}  // namespace

bool parseVersion(const char *value, unsigned long parts[3]) {
  const char *cursor = versionBody(value);
  if (!cursor || !*cursor) return false;
  for (int part = 0; part < 3; ++part) {
    if (!isdigit(static_cast<unsigned char>(*cursor))) return false;
    unsigned long number = 0;
    int digits = 0;
    while (isdigit(static_cast<unsigned char>(*cursor))) {
      if (++digits > 5) return false;
      number = number * 10UL + static_cast<unsigned long>(*cursor - '0');
      cursor++;
    }
    parts[part] = number;
    if (part < 2) {
      if (*cursor != '.') return false;
      cursor++;
    }
  }
  return *cursor == '\0';
}

int compareVersions(const char *candidate, const char *current) {
  unsigned long lhs[3] = {0, 0, 0};
  unsigned long rhs[3] = {0, 0, 0};
  if (!parseVersion(candidate, lhs) || !parseVersion(current, rhs)) return -2;
  for (int i = 0; i < 3; ++i) {
    if (lhs[i] > rhs[i]) return 1;
    if (lhs[i] < rhs[i]) return -1;
  }
  return 0;
}

bool isNewerVersion(const char *candidate, const char *current) {
  return compareVersions(candidate, current) == 1;
}

bool validSha256Digest(const char *value) {
  static const char prefix[] = "sha256:";
  if (!value || strncmp(value, prefix, sizeof(prefix) - 1) != 0) return false;
  value += sizeof(prefix) - 1;
  if (strlen(value) != 64) return false;
  for (size_t i = 0; i < 64; ++i) {
    if (!hexDigit(value[i])) return false;
  }
  return true;
}

bool validGithubAssetApiUrl(const char *value) {
  static const char prefix[] =
      "https://api.github.com/repos/sukiyra/sms-forwarder-board/releases/assets/";
  if (!value || strncmp(value, prefix, sizeof(prefix) - 1) != 0) return false;
  value += sizeof(prefix) - 1;
  if (!*value) return false;
  for (; *value; ++value) {
    if (!isdigit(static_cast<unsigned char>(*value))) return false;
  }
  return true;
}

bool validOtaAsset(const char *tag, const char *assetName, const char *digest,
                   size_t size, size_t partitionSize) {
  if (!tag || !assetName || !validSha256Digest(digest)) return false;
  char expected[96];
  int written = snprintf(expected, sizeof(expected), "sms-forwarder-ota-%s.bin", tag);
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(expected) ||
      strcmp(expected, assetName) != 0) {
    return false;
  }
  return size >= 128 * 1024 && size <= partitionSize;
}

}  // namespace otapolicy
