#include "globals.h"

Config config;
Preferences preferences;
PDU pdu = PDU(4096);
WiFiClientSecure ssl_client;
SMTPClient smtp(ssl_client);
WebServer server(80);
bool configValid = false;
bool modemReady = false;
String detectedModemManufacturer = "未知";
String detectedModemModel = "ML307";
String detectedModemFirmware = "未知";
String detectedModemFamily = "未知";
unsigned long lastPrintTime = 0;
ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];
