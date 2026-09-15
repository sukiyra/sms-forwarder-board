#include "sms_capability.h"

#include <cassert>
#include <iostream>

int main() {
  assert(smsCarrierSupportFor("ML307C", "46000", "") == SMS_CARRIER_SUPPORTED);
  assert(smsCarrierSupportFor("ML307C-DL-CN", "46001", "") == SMS_CARRIER_SUPPORTED);
  assert(smsCarrierSupportFor("ML307C-DL-CN", "46010", "") == SMS_CARRIER_SUPPORTED);
  assert(smsCarrierSupportFor("ML307C", "46011", "") == SMS_CARRIER_UNSUPPORTED);
  assert(smsCarrierSupportFor("ML307C", "46015", "") == SMS_CARRIER_UNSUPPORTED);
  assert(smsCarrierSupportFor("ML307C", "", "中国联通") == SMS_CARRIER_SUPPORTED);
  assert(smsCarrierSupportFor("ML307C", "", "CHN-CT") == SMS_CARRIER_UNSUPPORTED);
  assert(smsCarrierSupportFor("ML307C", "", "") == SMS_CARRIER_UNKNOWN);
  assert(smsCarrierSupportFor("ML307Y", "46011", "中国电信") == SMS_CARRIER_UNKNOWN);
  std::cout << "SMS carrier capability tests passed\n";
  return 0;
}
