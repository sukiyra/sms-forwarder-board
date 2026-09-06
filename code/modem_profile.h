#ifndef MODEM_PROFILE_H
#define MODEM_PROFILE_H

#include <Arduino.h>

enum ModemFamily {
  MODEM_FAMILY_UNKNOWN,
  MODEM_FAMILY_ML307A,
  MODEM_FAMILY_ML307C,
  MODEM_FAMILY_ML307R,
  MODEM_FAMILY_ML307Y
};

struct ModemProfile {
  ModemFamily family;
  String familyName;
  String fullModel;
  bool supported;
  bool mayProbePdpContext;
};

ModemProfile modemClassify(const String& modelText);
String modemIdentityValue(const String& response, const char* command,
                          const char* responsePrefix, bool preferModel);

#endif
