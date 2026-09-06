#include "config.h"
#include <nvs.h>
#include "web_handlers.h"

// 保存配置到NVS。
// 空槽位不写键并主动清理旧键:全量写约230个键会把默认20KB的NVS
// 分区碎片化,导致新字符串(如apiToken)找不到连续条目而写入失败。
static bool putStrIf(const String &key, const String &value) {
  if (value.length() > 0) return preferences.putString(key.c_str(), value);
  return preferences.remove(key.c_str()) || true;
}

void saveConfig() {
  preferences.begin("sms_config", false);
  putStrIf("apiToken", config.apiToken);
  putStrIf("smtpServer", config.smtpServer);
  preferences.putInt("smtpPort", config.smtpPort);
  putStrIf("smtpUser", config.smtpUser);
  putStrIf("smtpPass", config.smtpPass);
  putStrIf("smtpSendTo", config.smtpSendTo);
  putStrIf("adminPhone", config.adminPhone);
  preferences.putString("webUser", config.webUser);
  preferences.putString("webPass", config.webPass);
  putStrIf("numBlkList", config.numberBlackList);
  for (int i = 0; i < WIFI_NETS_MAX; i++) {
    String prefix = "wifi" + String(i);
    const char *wkeys[] = {"ssid", "pass", "ip", "gw", "mask", "dns"};
    if (config.wifiNets[i].ssid.length() > 0) {
      preferences.putString((prefix + "ssid").c_str(), config.wifiNets[i].ssid);
      putStrIf((prefix + "pass"), config.wifiNets[i].pass);
      putStrIf((prefix + "ip"), config.wifiNets[i].ip);
      putStrIf((prefix + "gw"), config.wifiNets[i].gw);
      putStrIf((prefix + "mask"), config.wifiNets[i].mask);
      putStrIf((prefix + "dns"), config.wifiNets[i].dns);
    } else {
      for (const char *k : wkeys) preferences.remove((prefix + k).c_str());
    }
  }
  putStrIf("brandTitle", config.brandTitle);
  putStrIf("brandSub", config.brandSub);
  preferences.putUChar("pollSeconds", config.pollSeconds);
  for (int i = 0; i < MAX_CUSTOM_TASKS; i++) {
    String p = "task" + String(i);
    const CustomTask &t = config.tasks[i];
    if (t.type != TASK_NONE) {
      preferences.putUChar((p + "type").c_str(), t.type);
      putStrIf((p + "name"), t.name);
      preferences.putUChar((p + "mode").c_str(), t.mode);
      preferences.putUInt((p + "secs").c_str(), t.intervalSeconds);
      preferences.putUChar((p + "hour").c_str(), t.hour);
      preferences.putUChar((p + "min").c_str(), t.minute);
      preferences.putUChar((p + "wday").c_str(), t.weekday);
      preferences.putUChar((p + "mday").c_str(), t.dayOfMonth);
      putStrIf((p + "param"), t.param);
      preferences.putUChar((p + "hm").c_str(), t.httpMethod);
      putStrIf((p + "hdrs"), t.headers);
      putStrIf((p + "tbody"), t.body);
    } else {
      static const char *keys[] = {"type", "name", "mode", "secs", "hour", "min",
                                   "wday", "mday", "param", "hm", "hdrs", "tbody", "interval", "last"};
      for (const char *k : keys) preferences.remove((p + k).c_str());
    }
  }
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    const PushChannel &ch = config.pushChannels[i];
    bool used = ch.enabled || ch.url.length() || ch.key1.length() || ch.key2.length() || ch.customBody.length();
    if (used) {
      preferences.putBool((prefix + "en").c_str(), ch.enabled);
      preferences.putUChar((prefix + "type").c_str(), (uint8_t)ch.type);
      putStrIf((prefix + "url"), ch.url);
      putStrIf((prefix + "name"), ch.name);
      putStrIf((prefix + "k1"), ch.key1);
      putStrIf((prefix + "k2"), ch.key2);
      putStrIf((prefix + "body"), ch.customBody);
    } else {
      static const char *keys[] = {"en", "type", "url", "name", "k1", "k2", "body"};
      for (const char *k : keys) preferences.remove((prefix + k).c_str());
    }
  }

  preferences.end();
  nvs_stats_t st = {};
  nvs_get_stats("nvs", &st);
  logCaptureLn(String("配置已保存, NVS剩余条目=") + String(st.free_entries));
}

// 从NVS加载配置
void loadConfig() {
  preferences.begin("sms_config", true);
  config.smtpServer = preferences.getString("smtpServer", "");
  config.smtpPort = preferences.getInt("smtpPort", 465);
  config.smtpUser = preferences.getString("smtpUser", "");
  config.smtpPass = preferences.getString("smtpPass", "");
  config.smtpSendTo = preferences.getString("smtpSendTo", "");
  config.adminPhone = preferences.getString("adminPhone", "");
  config.webUser = preferences.getString("webUser", DEFAULT_WEB_USER);
  config.webPass = preferences.getString("webPass", DEFAULT_WEB_PASS);
  config.numberBlackList = preferences.getString("numBlkList", "");
  for (int i = 0; i < WIFI_NETS_MAX; i++) {
    String prefix = "wifi" + String(i);
    config.wifiNets[i].ssid = preferences.getString((prefix + "ssid").c_str(), "");
    config.wifiNets[i].pass = preferences.getString((prefix + "pass").c_str(), "");
    config.wifiNets[i].ip = preferences.getString((prefix + "ip").c_str(), "");
    config.wifiNets[i].gw = preferences.getString((prefix + "gw").c_str(), "");
    config.wifiNets[i].mask = preferences.getString((prefix + "mask").c_str(), "");
    config.wifiNets[i].dns = preferences.getString((prefix + "dns").c_str(), "");
  }
  // 迁移旧版单 WiFi 键到网络列表首位
  if (config.wifiNets[0].ssid.length() == 0) {
    String legacySsid = preferences.getString("webWifiSsid", "");
    String legacyPass = preferences.getString("webWifiPass", "");
    if (legacySsid.length() > 0) {
      config.wifiNets[0].ssid = legacySsid;
      config.wifiNets[0].pass = legacyPass;
      logCaptureLn(String("已迁移旧版WiFi配置到网络列表: ") + legacySsid);
    }
  }
  config.brandTitle = preferences.getString("brandTitle", "SMS FWD");
  config.brandSub = preferences.getString("brandSub", "愿你天黑有灯,下雨有伞");
  for (int i = 0; i < MAX_CUSTOM_TASKS; i++) {
    String p = "task" + String(i);
    config.tasks[i].type = preferences.getUChar((p + "type").c_str(), TASK_NONE);
    config.tasks[i].name = preferences.getString((p + "name").c_str(), "任务" + String(i + 1));
    config.tasks[i].mode = preferences.getUChar((p + "mode").c_str(), 0);
    config.tasks[i].intervalSeconds = preferences.getUInt((p + "secs").c_str(), 0);
    if (config.tasks[i].intervalSeconds == 0) {
      uint32_t legacyHours = preferences.getUInt((p + "interval").c_str(), 0);
      if (legacyHours > 0) config.tasks[i].intervalSeconds = legacyHours * 3600UL;
    }
    config.tasks[i].hour = preferences.getUChar((p + "hour").c_str(), 1);
    config.tasks[i].minute = preferences.getUChar((p + "min").c_str(), 30);
    config.tasks[i].weekday = preferences.getUChar((p + "wday").c_str(), 1);
    config.tasks[i].dayOfMonth = preferences.getUChar((p + "mday").c_str(), 1);
    config.tasks[i].param = preferences.getString((p + "param").c_str(), "");
    config.tasks[i].httpMethod = preferences.getUChar((p + "hm").c_str(), 0);
    config.tasks[i].headers = preferences.getString((p + "hdrs").c_str(), "");
    config.tasks[i].body = preferences.getString((p + "tbody").c_str(), "");
  }
  config.pollSeconds = preferences.getUChar("pollSeconds", 5);
  config.apiToken = preferences.getString("apiToken", "");

  // 加载推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    config.pushChannels[i].enabled = preferences.getBool((prefix + "en").c_str(), false);
    config.pushChannels[i].type = (PushType)preferences.getUChar((prefix + "type").c_str(), PUSH_TYPE_POST_JSON);
    config.pushChannels[i].url = preferences.getString((prefix + "url").c_str(), "");
    config.pushChannels[i].name = preferences.getString((prefix + "name").c_str(), "通道" + String(i + 1));
    config.pushChannels[i].key1 = preferences.getString((prefix + "k1").c_str(), "");
    config.pushChannels[i].key2 = preferences.getString((prefix + "k2").c_str(), "");
    config.pushChannels[i].customBody = preferences.getString((prefix + "body").c_str(), "");
  }

  // 兼容旧配置：如果有旧的httpUrl配置，迁移到第一个通道
  String oldHttpUrl = preferences.getString("httpUrl", "");
  if (oldHttpUrl.length() > 0 && !config.pushChannels[0].enabled) {
    config.pushChannels[0].enabled = true;
    config.pushChannels[0].url = oldHttpUrl;
    config.pushChannels[0].type = preferences.getUChar("barkMode", 0) != 0 ? PUSH_TYPE_BARK : PUSH_TYPE_POST_JSON;
    config.pushChannels[0].name = "迁移通道";
    logCaptureLn(String("已迁移旧HTTP配置到推送通道1"));
  }

  preferences.end();
  logCaptureLn(String("配置已加载"));
}

// 检查推送通道是否有效配置
bool isPushChannelValid(const PushChannel& ch) {
  if (!ch.enabled) return false;

  switch (ch.type) {
    case PUSH_TYPE_POST_JSON:
    case PUSH_TYPE_BARK:
    case PUSH_TYPE_GET:
    case PUSH_TYPE_DINGTALK:
    case PUSH_TYPE_FEISHU:
    case PUSH_TYPE_CUSTOM:
      return ch.url.length() > 0;
    case PUSH_TYPE_WECOM:
      return ch.url.startsWith("https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=") &&
             ch.url.length() > strlen("https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=") &&
             ch.url.indexOf(' ') < 0 && ch.url.indexOf('\r') < 0 && ch.url.indexOf('\n') < 0;
    case PUSH_TYPE_PUSHPLUS:
    case PUSH_TYPE_SERVERCHAN:
      return ch.key1.length() > 0;  // 这两个主要靠key1（token/sendkey）
    case PUSH_TYPE_GOTIFY:
      return ch.url.length() > 0 && ch.key1.length() > 0;  // 需要URL和Token
    case PUSH_TYPE_TELEGRAM:
      return ch.key1.length() > 0 && ch.key2.length() > 0; // 需要Chat ID和Token
    default:
      return false;
  }
}

// 检查配置是否有效（至少配置了邮件或任一推送通道）
bool isConfigValid() {
  bool emailValid = config.smtpServer.length() > 0 &&
                    config.smtpUser.length() > 0 &&
                    config.smtpPass.length() > 0 &&
                    config.smtpSendTo.length() > 0;

  bool pushValid = false;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      pushValid = true;
      break;
    }
  }

  return emailValid || pushValid;
}

// 获取当前设备URL
String getDeviceUrl() {
  return "http://" + WiFi.localIP().toString() + "/";
}

// lastRun 缓存:调度器每分钟询问全部任务,避免反复打开 NVS 读 flash
static uint32_t s_taskLastRunCache[MAX_CUSTOM_TASKS];
static bool s_taskLastRunLoaded[MAX_CUSTOM_TASKS];

uint32_t configTaskLastRun(int index) {
  if (index < 0 || index >= MAX_CUSTOM_TASKS) return 0;
  if (!s_taskLastRunLoaded[index]) {
    preferences.begin("sms_config", true);
    s_taskLastRunCache[index] = preferences.getUInt(("task" + String(index) + "last").c_str(), 0);
    preferences.end();
    s_taskLastRunLoaded[index] = true;
  }
  return s_taskLastRunCache[index];
}

void configRecordTaskRun(int index, uint32_t epoch) {
  if (index < 0 || index >= MAX_CUSTOM_TASKS) return;
  preferences.begin("sms_config", false);
  preferences.putUInt(("task" + String(index) + "last").c_str(), epoch);
  preferences.end();
  s_taskLastRunCache[index] = epoch;
  s_taskLastRunLoaded[index] = true;
}
