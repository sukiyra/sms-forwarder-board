#include "factory_serial.h"

#include "esim_manager.h"
#include "globals.h"
#include "modem.h"
#include "operator_manager.h"
#include "sim_manager.h"

namespace {

constexpr size_t FACTORY_LINE_CAPACITY = 160;
char serialInput[FACTORY_LINE_CAPACITY];
size_t serialInputLength = 0;
unsigned long serialInputAt = 0;

String jsonEscapeFactory(const String& value) {
  String result;
  result.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    if (c == '\\' || c == '"') {
      result += '\\';
      result += c;
    } else if (c == '\n') {
      result += "\\n";
    } else if (c == '\r') {
      result += "\\r";
    } else if (static_cast<uint8_t>(c) >= 0x20) {
      result += c;
    }
  }
  return result;
}

String chipId() {
  const uint64_t mac = ESP.getEfuseMac();
  char value[13];
  snprintf(value, sizeof(value), "%04X%08X",
           static_cast<unsigned>((mac >> 32) & 0xFFFF),
           static_cast<unsigned>(mac & 0xFFFFFFFF));
  return String(value);
}

void printFactoryStatus() {
  const String json =
      String("{\"protocol\":1,\"firmware\":\"") + FIRMWARE_VERSION +
      "\",\"chip\":{\"model\":\"" + jsonEscapeFactory(ESP.getChipModel()) +
      "\",\"revision\":" + String(ESP.getChipRevision()) +
      ",\"id\":\"" + chipId() + "\",\"flashBytes\":" + String(ESP.getFlashChipSize()) +
      ",\"heap\":" + String(ESP.getFreeHeap()) + "},\"modem\":{\"supported\":" +
      String(modemModelSupported() ? "true" : "false") +
      ",\"family\":\"" + jsonEscapeFactory(detectedModemFamily) +
      "\",\"model\":\"" + jsonEscapeFactory(detectedModemModel) +
      "\",\"manufacturer\":\"" + jsonEscapeFactory(detectedModemManufacturer) +
      "\",\"firmware\":\"" + jsonEscapeFactory(detectedModemFirmware) +
      "\",\"smsMode\":\"" + jsonEscapeFactory(modemSmsDeliveryMode()) +
      "\",\"registered\":" + String(modemReady ? "true" : "false") +
      "},\"sim\":{\"known\":" + String(simManagerIsKnown() ? "true" : "false") +
      ",\"present\":" + String(simManagerIsPresent() ? "true" : "false") +
      ",\"ready\":" + String(simManagerIsReady() ? "true" : "false") +
      ",\"smsReady\":" + String(simManagerSmsReady() ? "true" : "false") +
      ",\"type\":\"" + jsonEscapeFactory(esimModeName()) +
      "\",\"iccidTail\":\"" + jsonEscapeFactory(simManagerIccidTail()) +
      "\",\"homePlmn\":\"" + jsonEscapeFactory(simManagerHomePlmn()) +
      "\"},\"network\":{\"operator\":\"" + jsonEscapeFactory(operatorCurrentLabel()) +
      "\",\"plmn\":\"" + jsonEscapeFactory(operatorCurrentNumeric()) +
      "\",\"act\":\"" + jsonEscapeFactory(operatorCurrentActName()) +
      "\",\"registrationCode\":" + String(simManagerRegistrationStatus()) + "}}";
  Serial.print("@@FACTORY:");
  Serial.println(json);
}

void handleLine() {
  String line(serialInput, serialInputLength);
  line.trim();
  serialInputLength = 0;
  if (!line.length()) return;
  if (line.equalsIgnoreCase("FACTORY STATUS") || line.equalsIgnoreCase("FACTORY?")) {
    printFactoryStatus();
    return;
  }
  // Compatibility with the original firmware's USB-to-modem AT bridge.
  Serial1.print(line);
  Serial1.print("\r\n");
}

void flushPartialAsPassthrough() {
  if (!serialInputLength) return;
  Serial1.write(reinterpret_cast<const uint8_t*>(serialInput), serialInputLength);
  serialInputLength = 0;
}

}  // namespace

void factorySerialLoop() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    serialInputAt = millis();
    if (c == '\r' || c == '\n') {
      handleLine();
    } else if (serialInputLength + 1 < FACTORY_LINE_CAPACITY) {
      serialInput[serialInputLength++] = c;
      serialInput[serialInputLength] = '\0';
    } else {
      flushPartialAsPassthrough();
      Serial1.write(c);
    }
  }
  // Interactive serial tools may send characters without a line ending.
  if (serialInputLength && millis() - serialInputAt > 250) flushPartialAsPassthrough();
}
