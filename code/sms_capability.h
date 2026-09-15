#ifndef SMS_CAPABILITY_H
#define SMS_CAPABILITY_H

enum SmsCarrierSupport {
  SMS_CARRIER_UNKNOWN,
  SMS_CARRIER_SUPPORTED,
  SMS_CARRIER_UNSUPPORTED
};

SmsCarrierSupport smsCarrierSupportFor(const char* modemFamily, const char* plmn,
                                       const char* operatorName);
const char* smsCarrierSupportName(SmsCarrierSupport support);

#endif
