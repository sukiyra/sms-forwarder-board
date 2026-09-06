#include "modem_profile.h"

namespace {

String cleanIdentityLine(String line, const char* responsePrefix) {
  line.trim();
  if (responsePrefix && responsePrefix[0] && line.startsWith(responsePrefix)) {
    line.remove(0, strlen(responsePrefix));
    line.trim();
  }
  if (line.startsWith(":")) {
    line.remove(0, 1);
    line.trim();
  }
  if (line.length() >= 2 && line.startsWith("\"") && line.endsWith("\"")) {
    line = line.substring(1, line.length() - 1);
  }
  line.trim();
  return line;
}

bool isTerminalLine(const String& line) {
  return line == "OK" || line == "ERROR" || line.startsWith("+CME ERROR:") ||
         line.startsWith("+CMS ERROR:");
}

}  // namespace

ModemProfile modemClassify(const String& modelText) {
  String upper = modelText;
  upper.toUpperCase();
  ModemProfile profile = {MODEM_FAMILY_UNKNOWN, "未知", modelText, false, false};
  if (upper.indexOf("ML307A") >= 0) {
    profile.family = MODEM_FAMILY_ML307A;
    profile.familyName = "ML307A";
  } else if (upper.indexOf("ML307C") >= 0) {
    profile.family = MODEM_FAMILY_ML307C;
    profile.familyName = "ML307C";
  } else if (upper.indexOf("ML307R") >= 0) {
    profile.family = MODEM_FAMILY_ML307R;
    profile.familyName = "ML307R";
  } else if (upper.indexOf("ML307Y") >= 0) {
    profile.family = MODEM_FAMILY_ML307Y;
    profile.familyName = "ML307Y";
  }
  profile.supported = profile.family != MODEM_FAMILY_UNKNOWN;
  // Querying CGACT support is safe on known A/C/R firmware. Field testing found
  // that some ML307Y AT builds accept the query but fail the actual toggle, so Y
  // deliberately uses the SMS-only path.
  profile.mayProbePdpContext = profile.family == MODEM_FAMILY_ML307A ||
                               profile.family == MODEM_FAMILY_ML307C ||
                               profile.family == MODEM_FAMILY_ML307R;
  if (!profile.fullModel.length()) profile.fullModel = profile.familyName;
  return profile;
}

String modemIdentityValue(const String& response, const char* command,
                          const char* responsePrefix, bool preferModel) {
  String fallback;
  int start = 0;
  while (start <= response.length()) {
    int end = response.indexOf('\n', start);
    if (end < 0) end = response.length();
    String line = cleanIdentityLine(response.substring(start, end), responsePrefix);
    String upper = line;
    upper.toUpperCase();
    String commandUpper(command ? command : "");
    commandUpper.toUpperCase();
    if (line.length() && upper != commandUpper && !isTerminalLine(upper)) {
      if (preferModel && upper.indexOf("ML307") >= 0) return line;
      if (!fallback.length()) fallback = line;
    }
    if (end >= response.length()) break;
    start = end + 1;
  }
  return fallback;
}
