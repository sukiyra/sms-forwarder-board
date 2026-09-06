#include "api_handlers.h"

#include "auth.h"
#include "config.h"
#include "esim_manager.h"
#include "modem.h"
#include "operator_manager.h"
#include "push.h"
#include "sim_manager.h"
#include "sms_process.h"
#include "sms_store.h"
#include "web_handlers.h"

namespace {

enum OutboundSmsState {
  OUTBOUND_IDLE,
  OUTBOUND_QUEUED,
  OUTBOUND_SENDING,
  OUTBOUND_SUCCESS,
  OUTBOUND_FAILED
};

OutboundSmsState outboundSmsState = OUTBOUND_IDLE;
String outboundSmsPhone;
String outboundSmsContent;
String outboundSmsMessage = "尚未提交短信";
unsigned long outboundSmsUpdatedAt = 0;
bool outboundSmsProcessing = false;

const char* outboundSmsStateName() {
  switch (outboundSmsState) {
    case OUTBOUND_QUEUED: return "queued";
    case OUTBOUND_SENDING: return "sending";
    case OUTBOUND_SUCCESS: return "success";
    case OUTBOUND_FAILED: return "failed";
    default: return "idle";
  }
}

bool outboundSmsBusyInternal() {
  return outboundSmsState == OUTBOUND_QUEUED || outboundSmsState == OUTBOUND_SENDING;
}

String outboundSmsJson() {
  return "{\"ok\":true,\"state\":\"" + String(outboundSmsStateName()) +
         "\",\"busy\":" + String(outboundSmsBusyInternal() ? "true" : "false") +
         ",\"success\":" + String(outboundSmsState == OUTBOUND_SUCCESS ? "true" : "false") +
         ",\"message\":\"" + jsonEscape(outboundSmsMessage) + "\",\"updatedAt\":" +
         String(outboundSmsUpdatedAt / 1000) + "}";
}

bool validSmsPhone(const String& phone) {
  if (phone.length() < 3 || phone.length() > 24) return false;
  for (size_t i = 0; i < phone.length(); ++i) {
    char c = phone.charAt(i);
    if (c == '+' && i == 0) continue;
    if (!isDigit(c)) return false;
  }
  return true;
}


bool validLength(const String &value, size_t maxLength) {
  return value.length() <= maxLength;
}

void handleApiStatus() {
  if (!authRequire()) return;
  int enabledPush = 0;
  for (int i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    if (config.pushChannels[i].enabled) enabledPush++;
  }
  String activeProfile = esimActiveProfileLabel();
  String esimSupportedJson = !esimCapabilityKnown()
                                 ? "null"
                                 : (esimIsSupported() ? "true" : "false");
  String networkOperator = operatorCurrentLabel();
  String networkPlmn = operatorCurrentNumeric();
  int networkAct = operatorCurrentAct();
  String networkActName = operatorCurrentActName();
  if (!simManagerIsPresent()) {
    activeProfile = "";
    networkOperator = "";
    networkPlmn = "";
    networkAct = -1;
    networkActName = "未知";
  }
  bool signalKnown = simManagerIsPresent() && simManagerSignalKnown();
  bool rsrqKnown = signalKnown && simManagerSignalRsrqKnown();
  String rsrpJson = signalKnown ? String(simManagerSignalRsrpDbm()) : String("null");
  String rsrpRawJson = signalKnown ? String(simManagerSignalRsrpRaw()) : String("null");
  String rsrqJson = rsrqKnown
                        ? String(simManagerSignalRsrqTenthsDb() / 10.0f, 1)
                        : String("null");
  unsigned long signalUpdatedAt = signalKnown ? simManagerSignalUpdatedAt() : 0;
  unsigned long signalAge = signalKnown ? millis() - signalUpdatedAt : 0;
  String json;
  json.reserve(1400);
  json = "{\"ok\":true,\"firmware\":\"" FIRMWARE_VERSION "\",\"uptime\":" + String(millis() / 1000) +
         ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"epoch\":" + String(static_cast<unsigned long>(time(nullptr))) +
         ",\"wifi\":{\"connected\":" + String(WiFi.isConnected() ? "true" : "false") +
         ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",\"rssi\":" + String(WiFi.RSSI()) +
         ",\"ip\":\"" + WiFi.localIP().toString() + "\"},\"modem\":{\"ready\":" +
         String(modemReady ? "true" : "false") + ",\"model\":\"" + jsonEscape(detectedModemModel) +
         "\",\"family\":\"" + jsonEscape(detectedModemFamily) +
         "\",\"supported\":" + String(modemModelSupported() ? "true" : "false") +
         ",\"firmware\":\"" + jsonEscape(detectedModemFirmware) +
         "\",\"smsDelivery\":\"" + jsonEscape(modemSmsDeliveryMode()) + "\",\"operator\":\"" +
         jsonEscape(networkOperator) + "\",\"busy\":" +
         String(modemIsBusy() ? "true" : "false") +
         ",\"plmn\":\"" + jsonEscape(networkPlmn) + "\",\"act\":" + String(networkAct) +
         ",\"actName\":\"" + jsonEscape(networkActName) + "\"" +
         ",\"registration\":\"" + String(modemReady ? "已注册" : "未注册") +
         "\",\"rsrp\":" + rsrpJson + ",\"rsrq\":" + rsrqJson +
         ",\"signal\":{\"known\":" + String(signalKnown ? "true" : "false") +
         ",\"metric\":\"RSRP\",\"dbm\":" + rsrpJson + ",\"raw\":" + rsrpRawJson +
         ",\"rsrq\":" + rsrqJson + ",\"updatedAt\":" + String(signalUpdatedAt) +
         ",\"ageMs\":" + String(signalAge) + "}},\"sim\":{\"state\":\"" + simManagerStateName() +
         "\",\"known\":" + String(simManagerIsKnown() ? "true" : "false") +
         ",\"present\":" + String(simManagerIsPresent() ? "true" : "false") +
         ",\"ready\":" + String(simManagerIsReady() ? "true" : "false") +
         ",\"smsReady\":" + String(simManagerSmsReady() ? "true" : "false") +
         ",\"phoneNumber\":\"" + jsonEscape(simManagerPhoneNumber()) + "\"" +
         ",\"homePlmn\":\"" + jsonEscape(simManagerHomePlmn()) + "\"" +
         ",\"registrationCode\":" + String(simManagerRegistrationStatus()) +
         ",\"roaming\":" + String(simManagerIsRoaming() ? "true" : "false") +
         ",\"iccidTail\":\"" + jsonEscape(simManagerIccidTail()) + "\"" +
         ",\"mode\":\"" + String(esimModeName()) + "\",\"esimSupported\":" +
         esimSupportedJson +
         ",\"message\":\"" + jsonEscape(simManagerMessage()) +
         "\",\"generation\":" + String(simManagerGeneration()) +
         ",\"changedAt\":" + String(simManagerChangedAt()) + ",\"profile\":\"" +
         jsonEscape(activeProfile) + "\",\"name\":\"" + jsonEscape(activeProfile) +
         "\",\"profileName\":\"" + jsonEscape(activeProfile) + "\"},\"sms\":{\"stored\":" + String(smsStoreCount()) +
         ",\"unread\":" + String(smsStoreUnread()) + ",\"capacity\":50},\"push\":{\"enabled\":" +
         String(enabledPush) + "},\"job\":" + esimJobJson() +
         ",\"outboundSms\":" + outboundSmsJson() + "}";
  sendJsonResponse(200, json);
}

void handleApiSmsList() {
  if (!authRequire()) return;
  String messages = smsStoreListJson();
  String json = "{\"ok\":true,\"count\":" + String(smsStoreCount()) + ",\"unread\":" +
                String(smsStoreUnread()) + ",\"capacity\":50,\"messages\":" + messages + "}";
  sendJsonResponse(200, json);
}

uint32_t requestId() {
  String value = server.arg("id");
  if (!value.length() || value.length() > 10) return 0;
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isDigit(value[i])) return 0;
  }
  return static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
}

void handleApiSmsRead() {
  if (!authRequireCsrf()) return;
  uint32_t id = requestId();
  if (!id || !smsStoreMarkRead(id)) {
    sendJsonResponse(404, "{\"ok\":false,\"error\":\"not_found\"}");
    return;
  }
  sendJsonResponse(200, "{\"ok\":true}");
}

void handleApiSmsDelete() {
  if (!authRequireCsrf()) return;
  uint32_t id = requestId();
  if (!id || !smsStoreDelete(id)) {
    sendJsonResponse(404, "{\"ok\":false,\"error\":\"not_found\"}");
    return;
  }
  sendJsonResponse(200, "{\"ok\":true}");
}

void handleApiSmsClear() {
  if (!authRequireCsrf()) return;
  if (!smsStoreClear()) {
    sendJsonResponse(500, "{\"ok\":false,\"error\":\"storage\"}");
    return;
  }
  sendJsonResponse(200, "{\"ok\":true}");
}

void handleApiEsimProfiles() {
  if (!authRequire()) return;
  if (!simManagerIsReady()) {
    sendJsonResponse(200, esimProfilesJson());
    return;
  }
  if (modemIsBusy()) {
    if (esimProfilesLoaded()) {
      sendJsonResponse(200, esimProfilesJson());
      return;
    }
    sendJsonResponse(503, "{\"ok\":false,\"error\":\"modem_busy\",\"message\":\"模组正在初始化，请稍后重试\"}");
    return;
  }
  if (!esimProfilesLoaded() && !esimIsBusy()) {
    String error;
    if (!esimRefreshProfiles(error)) {
      if (esimCapabilityKnown() && !esimIsSupported()) {
        sendJsonResponse(200, esimProfilesJson());
        return;
      }
      sendJsonResponse(503, "{\"ok\":false,\"error\":\"esim\",\"message\":\"" + jsonEscape(error) + "\"}");
      return;
    }
  }
  sendJsonResponse(200, esimProfilesJson());
}

void handleApiEsimRefresh() {
  if (!authRequireCsrf()) return;
  if (!simManagerIsReady()) {
    sendJsonResponse(409, "{\"ok\":false,\"error\":\"sim_not_ready\",\"message\":\"SIM 尚未就绪\"}");
    return;
  }
  if (modemIsBusy()) {
    sendJsonResponse(409, "{\"ok\":false,\"error\":\"modem_busy\",\"message\":\"模组正在处理其他通信，请稍后重试\"}");
    return;
  }
  esimManagerResetCapability();
  String error;
  if (!esimRefreshProfiles(error)) {
    if (esimCapabilityKnown() && !esimIsSupported()) {
      sendJsonResponse(200, esimProfilesJson());
      return;
    }
    sendJsonResponse(503, "{\"ok\":false,\"error\":\"esim\",\"message\":\"" + jsonEscape(error) + "\"}");
    return;
  }
  sendJsonResponse(200, esimProfilesJson());
}

void handleApiEsimSwitch() {
  if (!authRequireCsrf()) return;
  if (!simManagerIsReady()) {
    sendJsonResponse(409, "{\"ok\":false,\"error\":\"sim_not_ready\",\"message\":\"SIM 尚未就绪\"}");
    return;
  }
  if (modemIsBusy()) {
    sendJsonResponse(409, "{\"ok\":false,\"error\":\"modem_busy\",\"message\":\"模组正在处理其他通信，请稍后重试\"}");
    return;
  }
  String id = server.arg("id");
  if (id.length() != 32) {
    sendJsonResponse(400, "{\"ok\":false,\"error\":\"invalid_profile\"}");
    return;
  }
  String message;
  if (!esimStartSwitch(id, message)) {
    sendJsonResponse(esimIsBusy() ? 409 : 400,
             "{\"ok\":false,\"message\":\"" + jsonEscape(message) + "\"}");
    return;
  }
  sendJsonResponse(202, "{\"ok\":true,\"message\":\"" + jsonEscape(message) + "\",\"job\":" + esimJobJson() + "}");
}

void handleApiEsimEid() {
  if (!authRequire()) return;
  if (!simManagerIsReady()) {
    sendJsonResponse(200, "{\"ok\":true,\"eid\":\"\"}");
    return;
  }
  if (esimCapabilityKnown() && !esimIsSupported()) {
    sendJsonResponse(200, "{\"ok\":true,\"supported\":false,\"mode\":\"physical\",\"eid\":\"\",\"message\":\"实体 SIM 没有 EID\"}");
    return;
  }
  if (modemIsBusy()) {
    sendJsonResponse(409, "{\"ok\":false,\"error\":\"modem_busy\",\"message\":\"模组正在处理其他通信，请稍后重试\"}");
    return;
  }
  String error;
  String eid = esimGetEid(error);
  if (!eid.length()) {
    if (esimCapabilityKnown() && !esimIsSupported()) {
      sendJsonResponse(200, "{\"ok\":true,\"supported\":false,\"mode\":\"physical\",\"eid\":\"\",\"message\":\"实体 SIM 没有 EID\"}");
      return;
    }
    sendJsonResponse(503, "{\"ok\":false,\"error\":\"esim\",\"message\":\"" + jsonEscape(error) + "\"}");
    return;
  }
  sendJsonResponse(200, "{\"ok\":true,\"eid\":\"" + jsonEscape(eid) + "\"}");
}

void handleApiEsimDelete() {
  if (!authRequireCsrf()) return;
  if (!simManagerIsReady()) {
    sendJsonResponse(409, "{\"ok\":false,\"error\":\"sim_not_ready\",\"message\":\"SIM 尚未就绪\"}");
    return;
  }
  if (modemIsBusy()) {
    sendJsonResponse(409, "{\"ok\":false,\"error\":\"modem_busy\",\"message\":\"模组正在处理其他通信，请稍后重试\"}");
    return;
  }
  String id = server.arg("id");
  if (id.length() != 32) {
    sendJsonResponse(400, "{\"ok\":false,\"error\":\"invalid_profile\"}");
    return;
  }
  String message;
  if (!esimDeleteProfile(id, message)) {
    sendJsonResponse(400, "{\"ok\":false,\"message\":\"" + jsonEscape(message) + "\"}");
    return;
  }
  sendJsonResponse(200, "{\"ok\":true,\"message\":\"Profile 已删除\"}");
}

void handleApiOperatorGet() {
  if (!authRequire()) return;
  sendJsonResponse(200, operatorManagerJson());
}

void handleApiOperatorScan() {
  if (!authRequireCsrf()) return;
  String message;
  if (!operatorManagerStartScan(message)) {
    sendJsonResponse(409, "{\"ok\":false,\"message\":\"" + jsonEscape(message) + "\"}");
    return;
  }
  sendJsonResponse(202, operatorManagerJson());
}

void handleApiOperatorSelect() {
  if (!authRequireCsrf()) return;
  String numeric = server.arg("numeric");
  numeric.trim();
  String actText = server.arg("act");
  if (!actText.length() || actText.length() > 2) {
    sendJsonResponse(400, "{\"ok\":false,\"message\":\"网络制式参数无效\"}");
    return;
  }
  for (size_t i = 0; i < actText.length(); ++i) {
    if (!isDigit(actText[i])) {
      sendJsonResponse(400, "{\"ok\":false,\"message\":\"网络制式参数无效\"}");
      return;
    }
  }
  String message;
  if (!operatorManagerStartSelect(numeric, actText.toInt(), message)) {
    sendJsonResponse(409, "{\"ok\":false,\"message\":\"" + jsonEscape(message) + "\"}");
    return;
  }
  sendJsonResponse(202, operatorManagerJson());
}

void handleApiOperatorAuto() {
  if (!authRequireCsrf()) return;
  String message;
  if (!operatorManagerStartAuto(message)) {
    sendJsonResponse(409, "{\"ok\":false,\"message\":\"" + jsonEscape(message) + "\"}");
    return;
  }
  sendJsonResponse(202, operatorManagerJson());
}

void handleApiWifiPost() {
  if (!authRequireCsrf()) return;
  if (server.hasArg("clearAll")) {
    for (int i = 0; i < WIFI_NETS_MAX; ++i) {
      config.wifiNets[i].ssid = "";
      config.wifiNets[i].pass = "";
    }
    saveConfig();
    sendJsonResponse(200, "{\"ok\":true,\"message\":\"已清空全部WiFi，重启后设备开启配置热点 sms-forwarder\"}");
    delay(800);
    ESP.restart();
    return;
  }
  bool anySsid = false;
  WifiNet updated[WIFI_NETS_MAX];
  for (int i = 0; i < WIFI_NETS_MAX; ++i) {
    String ssid = server.arg("wifiSsid" + String(i));
    String pass = server.arg("wifiPass" + String(i));
    ssid.trim();
    if (ssid.length() > 32 || pass.length() > 64) {
      sendJsonResponse(400, "{\"ok\":false,\"message\":\"WiFi 名称或密码长度无效\"}");
      return;
    }
    if (pass.length() == 0 && ssid.length() > 0) {
      for (int k = 0; k < WIFI_NETS_MAX; ++k) {
        if (config.wifiNets[k].ssid == ssid) {
          pass = config.wifiNets[k].pass;  // 同名网络未填密码时保留旧密码
          break;
        }
      }
    }
    if (ssid.length() > 0) anySsid = true;
    updated[i].ssid = ssid;
    updated[i].pass = pass;
  }
  if (!anySsid) {
    sendJsonResponse(400, "{\"ok\":false,\"message\":\"至少需要填写一个 WiFi 名称\"}");
    return;
  }
  for (int i = 0; i < WIFI_NETS_MAX; ++i) config.wifiNets[i] = updated[i];
  saveConfig();
  sendJsonResponse(200, "{\"ok\":true,\"message\":\"WiFi 已保存，设备即将重启并按主备顺序连接\"}");
  delay(800);
  ESP.restart();
}

void handleApiReboot() {
  if (!authRequireCsrf()) return;
  sendJsonResponse(200, "{\"ok\":true,\"message\":\"设备正在重启，约 40 秒后恢复\"}");
  delay(800);
  ESP.restart();
}

void handleApiPushTest() {
  if (!authRequire()) return;
  if (WiFi.status() != WL_CONNECTED) {
    sendJsonResponse(409, "{\"ok\":false,\"message\":\"WiFi 未连接\"}");
    return;
  }
  String ch = server.arg("channel");
  String timestamp = String(time(nullptr));
  if (ch == "email") {
    bool emailOk = config.smtpServer.length() > 0 && config.smtpUser.length() > 0 &&
                   config.smtpPass.length() > 0 && config.smtpSendTo.length() > 0;
    if (!emailOk) {
      sendJsonResponse(400, "{\"ok\":false,\"message\":\"邮件通知未配置\"}");
      return;
    }
    sendEmailNotification("推送测试", "这是一条来自设备的邮件通道测试");
    sendJsonResponse(200, "{\"ok\":true,\"message\":\"测试邮件已发送，请查收\"}");
    return;
  }
  int idx = ch.toInt();
  if (idx < 0 || idx >= MAX_PUSH_CHANNELS || !isPushChannelValid(config.pushChannels[idx])) {
    sendJsonResponse(400, "{\"ok\":false,\"message\":\"通道未配置或编号无效\"}");
    return;
  }
  sendToChannel(config.pushChannels[idx], "测试", "推送通道测试消息", timestamp.c_str());
  sendJsonResponse(200, "{\"ok\":true,\"message\":\"已向通道 " + String(idx + 1) + " 发送测试，请查收\"}");
}

void handleApiFactoryReset() {
  if (!authRequireCsrf()) return;
  sendJsonResponse(200, "{\"ok\":true,\"message\":\"正在恢复出厂设置，设备即将重启并开启配置热点\"}");
  delay(800);
  preferences.begin("sms_config", false);
  preferences.clear();
  preferences.end();
  smsStoreClear();
  ESP.restart();
}

// 公开的品牌信息(仅展示用途,不含敏感数据)
void handleApiBrand() {
  sendJsonResponse(200, "{\"ok\":true,\"title\":\"" + jsonEscape(config.brandTitle) +
                    "\",\"sub\":\"" + jsonEscape(config.brandSub) + "\"}");
}

void handleApiSmsSend() {
  if (!authRequire()) return;
  if (!simManagerIsReady() || !simManagerSmsReady()) {
    sendJsonResponse(409, "{\"ok\":false,\"message\":\"SIM 或短信服务尚未就绪\"}");
    return;
  }
  if (outboundSmsBusyInternal()) {
    sendJsonResponse(409, "{\"ok\":false,\"state\":\"" + String(outboundSmsStateName()) +
                           "\",\"message\":\"上一条短信仍在发送，请稍候\"}");
    return;
  }
  if (esimIsBusy() || operatorManagerIsBusy()) {
    sendJsonResponse(409, "{\"ok\":false,\"message\":\"SIM 或运营商任务正在执行，请稍后重试\"}");
    return;
  }
  String phone = server.arg("phone");
  String content = server.arg("content");
  phone.trim();
  content.trim();
  if (phone.length() == 0 || content.length() == 0) {
    sendJsonResponse(400, "{\"ok\":false,\"message\":\"请填写目标号码和短信内容\"}");
    return;
  }
  if (!validSmsPhone(phone)) {
    sendJsonResponse(400, "{\"ok\":false,\"message\":\"目标号码格式不正确，只能包含数字和开头的 +\"}");
    return;
  }
  if (content.length() > 1024) {
    sendJsonResponse(400, "{\"ok\":false,\"message\":\"短信内容过长\"}");
    return;
  }
  outboundSmsPhone = phone;
  outboundSmsContent = content;
  outboundSmsState = OUTBOUND_QUEUED;
  outboundSmsMessage = "短信已提交，等待模组发送";
  outboundSmsUpdatedAt = millis();
  logCaptureLn("网页短信已入队");
  sendJsonResponse(202, outboundSmsJson());
}

void handleApiSmsSendStatus() {
  if (!authRequire()) return;
  sendJsonResponse(200, outboundSmsJson());
}

void handleApiConfigGet() {
  if (!authRequire()) return;
  String json;
  json.reserve(4096);
  json = "{\"ok\":true,\"webUser\":\"" + jsonEscape(config.webUser) +
         "\",\"mustChangePassword\":" + String(config.webPass == DEFAULT_WEB_PASS ? "true" : "false") +
         ",\"adminPhone\":\"" + jsonEscape(config.adminPhone) + "\",\"numberBlackList\":\"" +
         jsonEscape(config.numberBlackList) + "\",\"smtp\":{\"server\":\"" +
         jsonEscape(config.smtpServer) + "\",\"port\":" + String(config.smtpPort) +
         ",\"user\":\"" + jsonEscape(config.smtpUser) + "\",\"recipient\":\"" +
         jsonEscape(config.smtpSendTo) + "\",\"passwordSet\":" +
         String(config.smtpPass.length() ? "true" : "false") + "},\"push\":[";
  for (int i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    if (i) json += ',';
    const PushChannel &ch = config.pushChannels[i];
    json += "{\"enabled\":" + String(ch.enabled ? "true" : "false") + ",\"type\":" +
            String(static_cast<int>(ch.type)) + ",\"name\":\"" + jsonEscape(ch.name) +
            "\",\"urlSet\":" + String(ch.url.length() ? "true" : "false") +
            ",\"key1Set\":" + String(ch.key1.length() ? "true" : "false") +
            ",\"key2Set\":" + String(ch.key2.length() ? "true" : "false") +
            ",\"urlConfigured\":" + String(ch.url.length() ? "true" : "false") +
            ",\"key1Configured\":" + String(ch.key1.length() ? "true" : "false") +
             ",\"key2Configured\":" + String(ch.key2.length() ? "true" : "false") +
             ",\"customBody\":\"" + jsonEscape(ch.customBody) + "\"}";
   }
  json += "],\"wifi\":{\"nets\":[";
  for (int i = 0; i < WIFI_NETS_MAX; ++i) {
    if (i) json += ',';
    json += "{\"ssid\":\"" + jsonEscape(config.wifiNets[i].ssid) +
            "\",\"passSet\":" + String(config.wifiNets[i].pass.length() ? "true" : "false") + "}";
  }
  json += "],\"apMode\":" + String(WiFi.getMode() == WIFI_AP ? "true" : "false") +
          "},\"brand\":{\"title\":\"" + jsonEscape(config.brandTitle) +
          "\",\"sub\":\"" + jsonEscape(config.brandSub) + "\"},\"tasks\":[";
  for (int i = 0; i < MAX_CUSTOM_TASKS; ++i) {
    if (i) json += ',';
    json += "{\"type\":" + String(config.tasks[i].type) +
            ",\"name\":\"" + jsonEscape(config.tasks[i].name) +
            "\",\"mode\":" + String(config.tasks[i].mode) +
            ",\"intervalSeconds\":" + String(config.tasks[i].intervalSeconds) +
            ",\"hour\":" + String(config.tasks[i].hour) +
            ",\"minute\":" + String(config.tasks[i].minute) +
            ",\"weekday\":" + String(config.tasks[i].weekday) +
            ",\"dayOfMonth\":" + String(config.tasks[i].dayOfMonth) +
            ",\"param\":\"" + jsonEscape(config.tasks[i].param) + "\",\"httpMethod\":" +
            String(config.tasks[i].httpMethod) + ",\"headers\":\"" + jsonEscape(config.tasks[i].headers) +
            "\",\"body\":\"" + jsonEscape(config.tasks[i].body) + "\"}";
  }
  json += "],\"pollSeconds\":" + String(config.pollSeconds) +
         ",\"apiToken\":\"" + jsonEscape(config.apiToken) + "\"}";
  sendJsonResponse(200, json);
}

void handleApiConfigPost() {
  if (!authRequireCsrf()) return;
  String webUser = server.arg("webUser");
  webUser.trim();
  String webPass = server.arg("webPass");
  String adminPhone = server.arg("adminPhone");
  String blacklist = server.arg("numberBlackList");
  if (!validLength(webUser, 64) || !validLength(webPass, 128) || !validLength(adminPhone, 32) ||
      !validLength(blacklist, 512)) {
    sendJsonResponse(400, "{\"ok\":false,\"error\":\"input_too_long\"}");
    return;
  }

  bool credentialsChanged = false;
  if (webUser.length() && webUser != config.webUser) {
    config.webUser = webUser;
    credentialsChanged = true;
  }
  if (webPass.length() && webPass != config.webPass) {
    if (webPass.length() < 8) {
      sendJsonResponse(400, "{\"ok\":false,\"error\":\"password_too_short\"}");
      return;
    }
    config.webPass = webPass;
    credentialsChanged = true;
  }
  if (server.hasArg("adminPhone")) config.adminPhone = adminPhone;
  if (server.hasArg("numberBlackList")) config.numberBlackList = blacklist;
  if (server.hasArg("pollSeconds")) {
    config.pollSeconds = (uint8_t)constrain(server.arg("pollSeconds").toInt(), 1, 60);
  }
  if (server.hasArg("apiToken")) {
    String t = server.arg("apiToken");
    t.trim();
    if (t.length() > 64) t = t.substring(0, 64);
    config.apiToken = t;
  }
  if (server.hasArg("brandTitle")) {
    String t = server.arg("brandTitle");
    t.trim();
    if (t.length() > 24) t = t.substring(0, 24);
    config.brandTitle = t;
  }
  if (server.hasArg("brandSub")) {
    String s = server.arg("brandSub");
    s.trim();
    if (s.length() > 40) s = s.substring(0, 40);
    config.brandSub = s;
  }

  int taskCount = -1;
  if (server.hasArg("taskCount")) {
    taskCount = constrain(server.arg("taskCount").toInt(), 0, MAX_CUSTOM_TASKS);
  }
  for (int i = 0; i < MAX_CUSTOM_TASKS; ++i) {
    String p = "task" + String(i);
    if (taskCount >= 0 && i >= taskCount) {
      config.tasks[i] = CustomTask();
      continue;
    }
    if (server.hasArg(p + "type")) {
      uint8_t ty = (uint8_t)constrain(server.arg(p + "type").toInt(), 0, 3);
      config.tasks[i].type = ty;
      if (ty != TASK_NONE && !server.hasArg(p + "intervalSeconds")) {
        sendJsonResponse(400, "{\"ok\":false,\"message\":\"缺少任务间隔\"}");
        return;
      }
    }
    if (server.hasArg(p + "param")) config.tasks[i].param = server.arg(p + "param").substring(0, 256);
    if (server.hasArg(p + "name")) config.tasks[i].name = server.arg(p + "name").substring(0, 24);
    if (server.hasArg(p + "mode")) config.tasks[i].mode = (uint8_t)constrain(server.arg(p + "mode").toInt(), 0, 3);
    if (server.hasArg(p + "intervalSeconds")) config.tasks[i].intervalSeconds = (uint32_t)constrain(strtoul(server.arg(p + "intervalSeconds").c_str(), nullptr, 10), 0UL, 31536000UL);
    if (server.hasArg(p + "hour")) config.tasks[i].hour = (uint8_t)constrain(server.arg(p + "hour").toInt(), 0, 23);
    if (server.hasArg(p + "minute")) config.tasks[i].minute = (uint8_t)constrain(server.arg(p + "minute").toInt(), 0, 59);
    if (server.hasArg(p + "weekday")) config.tasks[i].weekday = (uint8_t)constrain(server.arg(p + "weekday").toInt(), 0, 6);
    if (server.hasArg(p + "mday")) config.tasks[i].dayOfMonth = (uint8_t)constrain(server.arg(p + "mday").toInt(), 1, 28);
    if (server.hasArg(p + "hm")) config.tasks[i].httpMethod = (uint8_t)constrain(server.arg(p + "hm").toInt(), 0, 1);
    if (server.hasArg(p + "hdrs")) config.tasks[i].headers = server.arg(p + "hdrs").substring(0, 384);
    if (server.hasArg(p + "tbody")) config.tasks[i].body = server.arg(p + "tbody").substring(0, 512);
  }

  if (server.hasArg("smtpServer")) config.smtpServer = server.arg("smtpServer").substring(0, 128);
  if (server.hasArg("smtpPort")) config.smtpPort = constrain(server.arg("smtpPort").toInt(), 1, 65535);
  if (server.hasArg("smtpUser")) config.smtpUser = server.arg("smtpUser").substring(0, 128);
  if (server.hasArg("smtpPass") && server.arg("smtpPass").length()) config.smtpPass = server.arg("smtpPass").substring(0, 192);
  if (server.hasArg("smtpSendTo")) config.smtpSendTo = server.arg("smtpSendTo").substring(0, 128);

  // pushCount 存在时,超出部分视为已删除的通道并清空
  int pushCount = -1;
  if (server.hasArg("pushCount")) {
    pushCount = constrain(server.arg("pushCount").toInt(), 0, MAX_PUSH_CHANNELS);
  }
  for (int i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    String p = "push" + String(i);
    if (pushCount >= 0 && i >= pushCount) {
      config.pushChannels[i] = PushChannel();
      continue;
    }
    bool channelPresent = server.hasArg(p + "en") || server.hasArg(p + "type") ||
                          server.hasArg(p + "name") || server.hasArg(p + "url") ||
                          server.hasArg(p + "k1") || server.hasArg(p + "k2") ||
                          server.hasArg(p + "body");
    if (!channelPresent) continue;
    config.pushChannels[i].enabled = server.hasArg(p + "en");
    if (server.hasArg(p + "type")) config.pushChannels[i].type = static_cast<PushType>(constrain(server.arg(p + "type").toInt(), 0, 11));
    if (server.hasArg(p + "name")) config.pushChannels[i].name = server.arg(p + "name").substring(0, 48);
    if (server.hasArg(p + "url") && server.arg(p + "url").length()) config.pushChannels[i].url = server.arg(p + "url").substring(0, 512);
    if (server.hasArg(p + "k1") && server.arg(p + "k1").length()) config.pushChannels[i].key1 = server.arg(p + "k1").substring(0, 256);
    if (server.hasArg(p + "k2") && server.arg(p + "k2").length()) config.pushChannels[i].key2 = server.arg(p + "k2").substring(0, 256);
    if (server.hasArg(p + "body")) config.pushChannels[i].customBody = server.arg(p + "body").substring(0, 768);
  }

  saveConfig();
  configValid = isConfigValid();
  sendJsonResponse(200, "{\"ok\":true,\"reauth\":" + String(credentialsChanged ? "true" : "false") + "}");
  if (credentialsChanged) authInvalidateAll();
}

}  // namespace

bool pendingWebSmsBusy() {
  return outboundSmsBusyInternal();
}

void processPendingWebSms() {
  if (outboundSmsProcessing || outboundSmsState != OUTBOUND_QUEUED) return;
  if (millis() - outboundSmsUpdatedAt < 100) return;
  if (!simManagerIsReady() || !simManagerSmsReady()) {
    outboundSmsPhone = "";
    outboundSmsContent = "";
    outboundSmsState = OUTBOUND_FAILED;
    outboundSmsMessage = "SIM 或短信服务已变为不可用";
    outboundSmsUpdatedAt = millis();
    return;
  }
  if (esimIsBusy() || operatorManagerIsBusy() || simManagerIsBusy() ||
      smsStoredMessageIsBusy() || modemIsBusy()) return;

  outboundSmsProcessing = true;
  outboundSmsState = OUTBOUND_SENDING;
  outboundSmsMessage = "模组正在发送，通常需要数秒";
  outboundSmsUpdatedAt = millis();
  bool success = sendSMS(outboundSmsPhone.c_str(), outboundSmsContent.c_str());
  outboundSmsPhone = "";
  outboundSmsContent = "";
  outboundSmsState = success ? OUTBOUND_SUCCESS : OUTBOUND_FAILED;
  outboundSmsMessage = success ? "运营商已接收发送请求；这不代表对方已经收到" : "短信发送失败，请查看诊断日志";
  outboundSmsUpdatedAt = millis();
  outboundSmsProcessing = false;
}

void registerApiRoutes() {
  server.on("/api/session", HTTP_GET, handleApiSession);
  server.on("/api/login", HTTP_POST, handleApiLogin);
  server.on("/api/logout", HTTP_POST, handleApiLogout);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/sms", HTTP_GET, handleApiSmsList);
  server.on("/api/sms/read", HTTP_POST, handleApiSmsRead);
  server.on("/api/sms/delete", HTTP_POST, handleApiSmsDelete);
  server.on("/api/sms/clear", HTTP_POST, handleApiSmsClear);
  server.on("/api/esim/profiles", HTTP_GET, handleApiEsimProfiles);
  server.on("/api/esim/refresh", HTTP_POST, handleApiEsimRefresh);
  server.on("/api/esim/switch", HTTP_POST, handleApiEsimSwitch);
  server.on("/api/esim/eid", HTTP_GET, handleApiEsimEid);
  server.on("/api/esim/delete", HTTP_POST, handleApiEsimDelete);
  server.on("/api/operator", HTTP_GET, handleApiOperatorGet);
  server.on("/api/operator/scan", HTTP_POST, handleApiOperatorScan);
  server.on("/api/operator/select", HTTP_POST, handleApiOperatorSelect);
  server.on("/api/operator/auto", HTTP_POST, handleApiOperatorAuto);
  server.on("/api/config", HTTP_GET, handleApiConfigGet);
  server.on("/api/config", HTTP_POST, handleApiConfigPost);
  server.on("/api/wifi", HTTP_POST, handleApiWifiPost);
  server.on("/api/sms/send", HTTP_POST, handleApiSmsSend);
  server.on("/api/sms/send/status", HTTP_GET, handleApiSmsSendStatus);
  server.on("/sendsms", HTTP_POST, handleApiSmsSend);
  server.on("/api/reboot", handleApiReboot);
  server.on("/api/push/test", HTTP_POST, handleApiPushTest);
  server.on("/api/factory/reset", handleApiFactoryReset);
  server.on("/api/brand", HTTP_GET, handleApiBrand);
}
