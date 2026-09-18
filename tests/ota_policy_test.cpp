#include "ota_policy.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
  unsigned long parts[3] = {};
  assert(otapolicy::parseVersion("v1.5.0", parts));
  assert(parts[0] == 1 && parts[1] == 5 && parts[2] == 0);
  assert(otapolicy::parseVersion("sukiyra-12.34.56", parts));
  assert(!otapolicy::parseVersion("1.5.0", parts));
  assert(!otapolicy::parseVersion("v1.5", parts));
  assert(!otapolicy::parseVersion("v1.5.0-beta", parts));

  assert(otapolicy::compareVersions("v1.5.1", "sukiyra-1.5.0") == 1);
  assert(otapolicy::compareVersions("v1.5.0", "sukiyra-1.5.0") == 0);
  assert(otapolicy::compareVersions("v1.4.9", "sukiyra-1.5.0") == -1);
  assert(otapolicy::compareVersions("broken", "sukiyra-1.5.0") == -2);

  std::string digest = "sha256:" + std::string(64, 'a');
  assert(otapolicy::validSha256Digest(digest.c_str()));
  assert(!otapolicy::validSha256Digest(("sha256:" + std::string(63, 'a')).c_str()));
  assert(!otapolicy::validSha256Digest(("md5:" + std::string(64, 'a')).c_str()));
  assert(!otapolicy::validSha256Digest(("sha256:" + std::string(63, 'a') + "z").c_str()));

  assert(otapolicy::validOtaAsset("v1.5.1", "sms-forwarder-ota-v1.5.1.bin",
                                  digest.c_str(), 1700000, 0x1e0000));
  assert(!otapolicy::validOtaAsset("v1.5.1", "firmware.bin", digest.c_str(),
                                   1700000, 0x1e0000));
  assert(!otapolicy::validOtaAsset("v1.5.1", "sms-forwarder-ota-v1.5.1.bin",
                                   digest.c_str(), 0x1e0001, 0x1e0000));

  std::cout << "OTA version, digest and partition policy tests passed\n";
  return 0;
}
