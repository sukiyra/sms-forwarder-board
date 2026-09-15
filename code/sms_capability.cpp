#include "sms_capability.h"

#include <cstring>

namespace {

bool contains(const char* value, const char* needle) {
  return value && needle && std::strstr(value, needle) != nullptr;
}

bool plmnIs(const char* plmn, const char* value) {
  return plmn && value && std::strncmp(plmn, value, 5) == 0;
}

bool isChinaMobile(const char* plmn) {
  return plmnIs(plmn, "46000") || plmnIs(plmn, "46002") ||
         plmnIs(plmn, "46004") || plmnIs(plmn, "46007") ||
         plmnIs(plmn, "46008") || plmnIs(plmn, "46013") ||
         plmnIs(plmn, "46020");
}

bool isChinaUnicom(const char* plmn) {
  return plmnIs(plmn, "46001") || plmnIs(plmn, "46006") ||
         plmnIs(plmn, "46009") || plmnIs(plmn, "46010");
}

bool isMainlandPlmn(const char* plmn) {
  return plmn && std::strncmp(plmn, "460", 3) == 0;
}

}  // namespace

SmsCarrierSupport smsCarrierSupportFor(const char* modemFamily, const char* plmn,
                                       const char* operatorName) {
  // The domestic ML307C can register for packet data on all three mainland
  // carriers. This product variant only promises circuit SMS on Mobile and
  // Unicom, so keep network registration separate from SMS availability.
  if (!contains(modemFamily, "ML307C")) return SMS_CARRIER_UNKNOWN;

  if (isChinaMobile(plmn) || isChinaUnicom(plmn)) return SMS_CARRIER_SUPPORTED;
  if (isMainlandPlmn(plmn)) return SMS_CARRIER_UNSUPPORTED;

  // COPS can briefly omit the numeric PLMN while still returning a name.
  if (contains(operatorName, "中国移动") || contains(operatorName, "CHINA MOBILE") ||
      contains(operatorName, "CMCC") || contains(operatorName, "中国联通") ||
      contains(operatorName, "CHINA UNICOM") || contains(operatorName, "UNICOM")) {
    return SMS_CARRIER_SUPPORTED;
  }
  if (contains(operatorName, "中国电信") || contains(operatorName, "CHINA TELECOM") ||
      contains(operatorName, "CHN-CT") || contains(operatorName, "CTCC")) {
    return SMS_CARRIER_UNSUPPORTED;
  }
  return SMS_CARRIER_UNKNOWN;
}

const char* smsCarrierSupportName(SmsCarrierSupport support) {
  switch (support) {
    case SMS_CARRIER_SUPPORTED: return "supported";
    case SMS_CARRIER_UNSUPPORTED: return "unsupported";
    default: return "unknown";
  }
}
