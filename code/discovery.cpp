#include "discovery.h"

#include "globals.h"
#include "ota_manager.h"
#include "push.h"
#include "web_handlers.h"

#include <WiFiUdp.h>

namespace {

constexpr char REQUEST_PREFIX[] = "SMS_FORWARDER_DISCOVER_V1 ";
constexpr size_t NONCE_LENGTH = 16;
constexpr size_t MAX_PACKET_SIZE = 96;
constexpr unsigned long REPLY_RATE_LIMIT_MS = 250;

WiFiUDP discoveryUdp;
bool udpStarted = false;
wifi_mode_t boundMode = WIFI_MODE_NULL;
IPAddress boundStationIp;
IPAddress boundApIp;
unsigned long lastReplyAt = 0;

bool isZeroIp(const IPAddress &ip) {
  return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

bool sameSubnet(const IPAddress &left, const IPAddress &right, const IPAddress &mask) {
  for (uint8_t i = 0; i < 4; ++i) {
    if ((left[i] & mask[i]) != (right[i] & mask[i])) return false;
  }
  return true;
}

bool validHexNonce(const char *value) {
  for (size_t i = 0; i < NONCE_LENGTH; ++i) {
    char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return value[NONCE_LENGTH] == '\0';
}

String chipId() {
  uint64_t value = ESP.getEfuseMac();
  char buffer[13];
  snprintf(buffer, sizeof(buffer), "%04X%08X",
           static_cast<uint16_t>(value >> 32), static_cast<uint32_t>(value));
  return String(buffer);
}

bool selectReplyAddress(const IPAddress &remote, IPAddress &replyIp, bool &apMode) {
  if (WiFi.status() == WL_CONNECTED && !isZeroIp(WiFi.localIP()) &&
      sameSubnet(remote, WiFi.localIP(), WiFi.subnetMask())) {
    replyIp = WiFi.localIP();
    apMode = false;
    return true;
  }
  wifi_mode_t mode = WiFi.getMode();
  if ((mode == WIFI_AP || mode == WIFI_AP_STA) && !isZeroIp(WiFi.softAPIP())) {
    IPAddress apMask(255, 255, 255, 0);
    if (sameSubnet(remote, WiFi.softAPIP(), apMask)) {
      replyIp = WiFi.softAPIP();
      apMode = true;
      return true;
    }
  }
  return false;
}

void ensureUdpSocket() {
  wifi_mode_t mode = WiFi.getMode();
  IPAddress stationIp = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : IPAddress();
  IPAddress apIp = (mode == WIFI_AP || mode == WIFI_AP_STA) ? WiFi.softAPIP() : IPAddress();
  bool hasNetwork = !isZeroIp(stationIp) || !isZeroIp(apIp);
  if (!hasNetwork) {
    if (udpStarted) discoveryUdp.stop();
    udpStarted = false;
    boundMode = WIFI_MODE_NULL;
    boundStationIp = IPAddress();
    boundApIp = IPAddress();
    return;
  }
  if (udpStarted && mode == boundMode && stationIp == boundStationIp && apIp == boundApIp) {
    return;
  }
  if (udpStarted) discoveryUdp.stop();
  udpStarted = discoveryUdp.begin(DISCOVERY_PORT) == 1;
  boundMode = mode;
  boundStationIp = stationIp;
  boundApIp = apIp;
  if (udpStarted) {
    logCaptureLn("局域网发现服务已启动，UDP " + String(DISCOVERY_PORT));
  } else {
    logCaptureLn("局域网发现服务启动失败，将自动重试");
  }
}

void handlePacket(int packetSize) {
  if (packetSize <= 0 || packetSize >= static_cast<int>(MAX_PACKET_SIZE)) {
    while (discoveryUdp.available()) discoveryUdp.read();
    return;
  }
  char packet[MAX_PACKET_SIZE] = {};
  int read = discoveryUdp.read(reinterpret_cast<uint8_t *>(packet), sizeof(packet) - 1);
  if (read <= 0) return;
  packet[read] = '\0';
  size_t prefixLength = strlen(REQUEST_PREFIX);
  if (static_cast<size_t>(read) != prefixLength + NONCE_LENGTH ||
      strncmp(packet, REQUEST_PREFIX, prefixLength) != 0 ||
      !validHexNonce(packet + prefixLength)) {
    return;
  }
  unsigned long now = millis();
  if (lastReplyAt && now - lastReplyAt < REPLY_RATE_LIMIT_MS) return;

  IPAddress remote = discoveryUdp.remoteIP();
  IPAddress replyIp;
  bool apMode = false;
  if (!selectReplyAddress(remote, replyIp, apMode)) return;

  String response;
  response.reserve(360);
  response = "{\"protocol\":1,\"product\":\"sms-forwarder-board\",\"nonce\":\"";
  response += packet + prefixLength;
  response += "\",\"firmware\":\"" FIRMWARE_VERSION "\",\"ip\":\"";
  response += replyIp.toString();
  response += "\",\"port\":80,\"mac\":\"";
  response += apMode ? WiFi.softAPmacAddress() : WiFi.macAddress();
  response += "\",\"chipId\":\"" + chipId() + "\",\"modem\":\"";
  response += jsonEscape(detectedModemFamily);
  response += "\",\"ota\":" + String(otaManagerSupported() ? "true" : "false");
  response += ",\"apMode\":" + String(apMode ? "true" : "false") + "}";

  if (discoveryUdp.beginPacket(remote, discoveryUdp.remotePort()) == 1) {
    discoveryUdp.write(reinterpret_cast<const uint8_t *>(response.c_str()), response.length());
    discoveryUdp.endPacket();
    lastReplyAt = now;
  }
}

}  // namespace

void discoveryBegin() {
  udpStarted = false;
  boundMode = WIFI_MODE_NULL;
  boundStationIp = IPAddress();
  boundApIp = IPAddress();
  lastReplyAt = 0;
  ensureUdpSocket();
}

void discoveryLoop() {
  ensureUdpSocket();
  if (!udpStarted) return;
  for (uint8_t handled = 0; handled < 4; ++handled) {
    int packetSize = discoveryUdp.parsePacket();
    if (!packetSize) break;
    handlePacket(packetSize);
  }
}
