#include "web_handlers.h"
#include "web_html.h"
#include "config.h"
#include "modem.h"
#include "push.h"
#include "auth.h"
#include "esim_manager.h"
#include "sim_manager.h"
#include "sms_process.h"

// ---- 日志环形缓冲区 ----
String logBuffer[LOG_BUF_SIZE];
int logBufIdx = 0;
int logBufCount = 0;
static String _logLine;  // 行缓冲：logCapture 写入这里，logCaptureLn 提交整行

static void _logAppend(const String& line) {
  logBuffer[logBufIdx] = line;
  logBufIdx = (logBufIdx + 1) % LOG_BUF_SIZE;
  if (logBufCount < LOG_BUF_SIZE) logBufCount++;
}

static void _logCommit() {
  if (_logLine.length() > 0) {
    _logAppend(_logLine);
    _logLine = "";
  }
}

void logCapture(const String& msg) {
  Serial.print(msg);
  _logLine += msg;
}

void logCapture(const char* msg) {
  Serial.print(msg);
  _logLine += msg;
}

void logCaptureF(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
  _logLine += buf;
  // 如果格式化字符串以 \n 结尾，则提交此行
  size_t len = strlen(buf);
  if (len > 0 && buf[len - 1] == '\n') {
    _logLine.trim();  // 去掉尾部空格和可能多余的 \n
    _logCommit();
  }
}

void logCaptureLn(const String& msg) {
  Serial.println(msg);
  _logLine += msg;
  _logCommit();
}

void logCaptureLn(const char* msg) {
  Serial.println(msg);
  _logLine += msg;
  _logCommit();
}

// 所有管理路由统一的 Cookie 会话保护。
void sendJsonResponse(int status, const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send(status, "application/json", json);
}

bool checkAuth() {
  return authRequire();
}

// 查询类 action 允许 GET;写操作(开关/重启等)仅限 POST
static bool requirePostMethod() {
  if (server.method() != HTTP_GET) return true;
  server.send(405, "application/json", "{\"success\":false,\"message\":\"写操作请使用 POST\"}");
  return false;
}

static bool requireModemRouteReady() {
  if (!modemIsBusy()) return true;
  server.send(409, "application/json",
              "{\"success\":false,\"message\":\"模组正在初始化或处理其他通信，请稍后重试\"}");
  return false;
}

// 依次尝试已配置的 WiFi 网络,成功返回 true(需在 HTTP 服务启动后调用)
bool wifiApplyStableIp(int idx);
bool wifiConnectAll() {
  if (WiFi.getMode() != WIFI_STA) WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setScanMethod(WIFI_FAST_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  for (int i = 0; i < WIFI_NETS_MAX; i++) {
    if (config.wifiNets[i].ssid.length() == 0) continue;
    logCaptureLn(String("尝试连接WiFi: ") + config.wifiNets[i].ssid);
    WiFi.disconnect(true);
    delay(200);
    WiFi.begin(config.wifiNets[i].ssid.c_str(), config.wifiNets[i].pass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
      server.handleClient();
      delay(50);
    }
    if (WiFi.status() == WL_CONNECTED) {
      if (wifiApplyStableIp(i)) return true;
      logCaptureLn(String("IP 记忆回退后仍失败: ") + config.wifiNets[i].ssid);
    }
    logCaptureLn(String("WiFi 连接失败: ") + config.wifiNets[i].ssid);
  }
  return false;
}

// IP 记忆:同网络复用首次地址;网关或掩码变化时重学。
// 回切失败时回退 DHCP 重连,仍失败返回 false 视为连接失败。
bool wifiApplyStableIp(int idx) {
  if (idx < 0 || idx >= WIFI_NETS_MAX) return true;
  WifiNet &net = config.wifiNets[idx];
  String curIp = WiFi.localIP().toString();
  String curGw = WiFi.gatewayIP().toString();
  String curMask = WiFi.subnetMask().toString();
  String curDns = WiFi.dnsIP().toString();
  if (net.ip.length() == 0 || net.gw != curGw || net.mask != curMask) {
    net.ip = curIp;
    net.gw = curGw;
    net.mask = curMask;
    net.dns = curDns;
    saveConfig();
    logCaptureLn(String("IP 记忆已更新: ") + curIp);
    return true;
  }
  if (curIp == net.ip) return true;
  IPAddress bindIp, bindGw, bindMask, bindDns;
  if (!bindIp.fromString(net.ip) || !bindGw.fromString(net.gw) ||
      !bindMask.fromString(net.mask) ||
      !(net.dns.length() == 0 || bindDns.fromString(net.dns))) {
    return true;
  }
  if (net.dns.length() == 0) bindDns = bindGw;
  logCaptureLn(String("切回记忆 IP: ") + net.ip);
  WiFi.config(bindIp, bindGw, bindMask, bindDns);
  WiFi.reconnect();
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    server.handleClient();
    delay(50);
  }
  if (WiFi.status() == WL_CONNECTED) return true;
  logCaptureLn(String("记忆 IP 回切失败,回退 DHCP"));
  IPAddress zero = IPAddress(0, 0, 0, 0);
  WiFi.config(zero, zero, zero, zero);
  WiFi.reconnect();
  start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    server.handleClient();
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

// 无可用WiFi时开放配置热点,供首次配置使用
void wifiStartAp() {
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP("sms-forwarder");
  if (ok) {
    logCaptureLn(String("⚠️ 无可用WiFi，已开启配置热点 sms-forwarder，请连接后访问 http://192.168.4.1"));
  } else {
    logCaptureLn(String("⚠️ 配置热点开启失败，设备将持续重试"));
    for (int i = 0; i < 3; i++) {
      delay(1000);
      if (WiFi.softAP("sms-forwarder")) {
        logCaptureLn(String("配置热点重试成功 sms-forwarder，http://192.168.4.1"));
        return;
      }
    }
  }
}

// 处理配置页面请求
void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send_P(200, "text/html; charset=utf-8", htmlPage);
  return;
#if 0
  if (!checkAuth()) return;

  String html = String(htmlPage);
  html.replace("%IP%", WiFi.localIP().toString());
  html.replace("%WIFI_SSID%", String(WiFi.SSID()));
  html.replace("%FREE_HEAP%", String(ESP.getFreeHeap() / 1024) + " KB");
  long uptimeSec = millis() / 1000;
  char uptimeBuf[16];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%ld:%02ld:%02ld", uptimeSec / 3600, (uptimeSec % 3600) / 60, uptimeSec % 60);
  html.replace("%UPTIME%", String(uptimeBuf));
  html.replace("%WEB_USER%", config.webUser);
  html.replace("%WEB_PASS%", config.webPass);
  html.replace("%SMTP_SERVER%", config.smtpServer);
  html.replace("%SMTP_PORT%", String(config.smtpPort));
  html.replace("%SMTP_USER%", config.smtpUser);
  html.replace("%SMTP_PASS%", config.smtpPass);
  html.replace("%SMTP_SEND_TO%", config.smtpSendTo);
  html.replace("%ADMIN_PHONE%", config.adminPhone);
  html.replace("%NUMBER_BLACK_LIST%", config.numberBlackList);

  // 概览页面的配置状态
  bool emailOk = config.smtpServer.length() > 0 && config.smtpUser.length() > 0 &&
                 config.smtpPass.length() > 0 && config.smtpSendTo.length() > 0;
  html.replace("%SMTP_CHECK%", emailOk ? "已配置" : "未配置");
  html.replace("%MODEM_CHECK%", modemReady ? "已就绪" : "未就绪");
  int pushCount = 0;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (config.pushChannels[i].enabled) pushCount++;
  }
  html.replace("%PUSH_COUNT%", String(pushCount));

  // 生成推送通道HTML
  String channelsHtml = "";
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String idx = String(i);
    String enabledClass = config.pushChannels[i].enabled ? " enabled" : "";
    String checked = config.pushChannels[i].enabled ? " checked" : "";

    channelsHtml += "<div class=\"push-channel" + enabledClass + "\" id=\"channel" + idx + "\">";
    channelsHtml += "<div class=\"push-channel-header\">";
    channelsHtml += "<input type=\"checkbox\" name=\"push" + idx + "en\" id=\"push" + idx + "en\" onchange=\"toggleChannel(" + idx + ")\"" + checked + ">";
    channelsHtml += "<label for=\"push" + idx + "en\" class=\"label-inline\">启用推送通道 " + String(i + 1) + "</label>";
    channelsHtml += "</div>";
    channelsHtml += "<div class=\"push-channel-body\">";

    // 通道名称
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>通道名称</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "name\" value=\"" + config.pushChannels[i].name + "\" placeholder=\"自定义名称\">";
    channelsHtml += "</div>";

    // 推送类型
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>推送方式</label>";
    channelsHtml += "<select name=\"push" + idx + "type\" id=\"push" + idx + "type\" onchange=\"updateTypeHint(" + idx + ")\">";
    channelsHtml += "<option value=\"1\"" + String(config.pushChannels[i].type == PUSH_TYPE_POST_JSON ? " selected" : "") + ">POST JSON（通用格式）</option>";
    channelsHtml += "<option value=\"2\"" + String(config.pushChannels[i].type == PUSH_TYPE_BARK ? " selected" : "") + ">Bark（iOS推送）</option>";
    channelsHtml += "<option value=\"3\"" + String(config.pushChannels[i].type == PUSH_TYPE_GET ? " selected" : "") + ">GET请求（参数在URL中）</option>";
    channelsHtml += "<option value=\"4\"" + String(config.pushChannels[i].type == PUSH_TYPE_DINGTALK ? " selected" : "") + ">钉钉机器人</option>";
    channelsHtml += "<option value=\"5\"" + String(config.pushChannels[i].type == PUSH_TYPE_PUSHPLUS ? " selected" : "") + ">PushPlus</option>";
    channelsHtml += "<option value=\"6\"" + String(config.pushChannels[i].type == PUSH_TYPE_SERVERCHAN ? " selected" : "") + ">Server酱</option>";
    channelsHtml += "<option value=\"7\"" + String(config.pushChannels[i].type == PUSH_TYPE_CUSTOM ? " selected" : "") + ">自定义模板</option>";
    channelsHtml += "<option value=\"8\"" + String(config.pushChannels[i].type == PUSH_TYPE_FEISHU ? " selected" : "") + ">飞书机器人</option>";
    channelsHtml += "<option value=\"9\"" + String(config.pushChannels[i].type == PUSH_TYPE_GOTIFY ? " selected" : "") + ">Gotify</option>";
    channelsHtml += "<option value=\"10\"" + String(config.pushChannels[i].type == PUSH_TYPE_TELEGRAM ? " selected" : "") + ">Telegram Bot</option>";
    channelsHtml += "</select>";
    channelsHtml += "<div class=\"push-type-hint\" id=\"hint" + idx + "\"></div>";
    channelsHtml += "</div>";

    // URL
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>推送URL/Webhook</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "url\" value=\"" + config.pushChannels[i].url + "\" placeholder=\"http://your-server.com/api 或 webhook地址\">";
    channelsHtml += "</div>";

    // 额外参数区域（钉钉/PushPlus/Server酱等需要）
    channelsHtml += "<div id=\"extra" + idx + "\" style=\"display:none;\">";
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label id=\"key1label" + idx + "\">参数1</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "key1\" id=\"key1" + idx + "\" value=\"" + config.pushChannels[i].key1 + "\">";
    channelsHtml += "</div>";
    channelsHtml += "<div class=\"form-group\" id=\"key2group" + idx + "\">";
    channelsHtml += "<label id=\"key2label" + idx + "\">参数2</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "key2\" id=\"key2" + idx + "\" value=\"" + config.pushChannels[i].key2 + "\">";
    channelsHtml += "</div>";
    channelsHtml += "</div>";

    // 自定义模板区域
    channelsHtml += "<div id=\"custom" + idx + "\" style=\"display:none;\">";
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>请求体模板（使用 {sender} {message} {timestamp} 占位符）</label>";
    channelsHtml += "<textarea name=\"push" + idx + "body\" rows=\"4\" style=\"width:100%;font-family:monospace;\">" + config.pushChannels[i].customBody + "</textarea>";
    channelsHtml += "</div>";
    channelsHtml += "</div>";

    channelsHtml += "</div></div>";
  }
  html.replace("%PUSH_CHANNELS%", channelsHtml);

  server.send(200, "text/html", html);
#endif
}



// 处理AT指令测试请求
void handleATCommand() {
  if (!authRequireCsrf()) return;
  if (esimIsBusy()) {
    server.send(409, "application/json", "{\"success\":false,\"message\":\"eSIM切换中，AT终端已锁定\"}");
    return;
  }
  if (!requireModemRouteReady()) return;

  String cmd = server.arg("cmd");
  bool success = false;
  String message = "";

  if (cmd.length() == 0) {
    message = "错误：指令不能为空";
  } else {
    logCaptureLn(String("网页端发送AT指令: " + cmd));
    String resp = sendATCommand(cmd.c_str(), 5000);
    logCaptureLn(String("模组响应: " + resp));

    if (resp.length() > 0) {
      success = true;
      message = resp;
    } else {
      message = "超时或无响应";
    }
  }

  String json = "{";
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + jsonEscape(message) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

// 处理模组信息查询请求
void handleQuery() {
  if (!checkAuth()) return;
  if (esimIsBusy()) {
    server.send(409, "application/json", "{\"success\":false,\"message\":\"eSIM切换中，请稍后查询\"}");
    return;
  }
  if (!requireModemRouteReady()) return;

  String type = server.arg("type");
  String json = "{";
  bool success = false;
  String message = "";

  if (type == "ati") {
    // 固件信息查询
    String resp = sendATCommand("ATI", 2000);
    logCaptureLn(String("ATI响应: " + resp));

    if (resp.indexOf("OK") >= 0) {
      success = true;
      // 解析ATI响应
      String manufacturer = "未知";
      String model = "未知";
      String version = "未知";

      // 按行解析
      int lineStart = 0;
      int lineNum = 0;
      for (int i = 0; i < resp.length(); i++) {
        if (resp.charAt(i) == '\n' || i == resp.length() - 1) {
          String line = resp.substring(lineStart, i);
          line.trim();
          if (line.length() > 0 && line != "ATI" && line != "OK") {
            lineNum++;
            if (lineNum == 1) manufacturer = line;
            else if (lineNum == 2) model = line;
            else if (lineNum == 3) version = line;
          }
          lineStart = i + 1;
        }
      }

      message = "<table class='info-table'>";
      message += "<tr><td>制造商</td><td>" + manufacturer + "</td></tr>";
      message += "<tr><td>模组型号</td><td>" + model + "</td></tr>";
      message += "<tr><td>固件版本</td><td>" + version + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (type == "signal") {
    // 信号质量查询
    String resp = sendATCommand("AT+CESQ", 2000);
    logCaptureLn(String("CESQ响应: " + resp));

    if (resp.indexOf("+CESQ:") >= 0) {
      success = true;
      // 解析 +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>
      int idx = resp.indexOf("+CESQ:");
      String params = resp.substring(idx + 6);
      int endIdx = params.indexOf('\r');
      if (endIdx < 0) endIdx = params.indexOf('\n');
      if (endIdx > 0) params = params.substring(0, endIdx);
      params.trim();

      // 分割参数
      String values[6];
      int valIdx = 0;
      int startPos = 0;
      for (int i = 0; i <= params.length() && valIdx < 6; i++) {
        if (i == params.length() || params.charAt(i) == ',') {
          values[valIdx] = params.substring(startPos, i);
          values[valIdx].trim();
          valIdx++;
          startPos = i + 1;
        }
      }

      // RSRP转换为dBm (0-97映射到-140到-44 dBm, 99表示未知)
      int rsrp = values[5].toInt();
      String rsrpStr;
      if (rsrp == 99 || rsrp == 255) {
        rsrpStr = "未知";
      } else {
        int rsrpDbm = -140 + rsrp;
        rsrpStr = String(rsrpDbm) + " dBm";
        if (rsrpDbm >= -80) rsrpStr += " (信号极好)";
        else if (rsrpDbm >= -90) rsrpStr += " (信号良好)";
        else if (rsrpDbm >= -100) rsrpStr += " (信号一般)";
        else if (rsrpDbm >= -110) rsrpStr += " (信号较弱)";
        else rsrpStr += " (信号很差)";
      }

      // RSRQ转换 (0-34映射到-19.5到-3 dB)
      int rsrq = values[4].toInt();
      String rsrqStr;
      if (rsrq == 99 || rsrq == 255) {
        rsrqStr = "未知";
      } else {
        float rsrqDb = -19.5 + rsrq * 0.5;
        rsrqStr = String(rsrqDb, 1) + " dB";
      }

      message = "<table class='info-table'>";
      message += "<tr><td>信号强度 (RSRP)</td><td>" + rsrpStr + "</td></tr>";
      message += "<tr><td>信号质量 (RSRQ)</td><td>" + rsrqStr + "</td></tr>";
      message += "<tr><td>原始数据</td><td>" + params + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (type == "siminfo") {
    // SIM卡信息查询
    success = true;
    message = "<table class='info-table'>";

    // 查询IMSI
    String resp = sendATCommand("AT+CIMI", 2000);
    String imsi = "未知";
    if (resp.indexOf("OK") >= 0) {
      int start = resp.indexOf('\n');
      if (start >= 0) {
        int end = resp.indexOf('\n', start + 1);
        if (end < 0) end = resp.indexOf('\r', start + 1);
        if (end > start) {
          imsi = resp.substring(start + 1, end);
          imsi.trim();
          if (imsi == "OK" || imsi.length() < 10) imsi = "未知";
        }
      }
    }
    message += "<tr><td>IMSI</td><td>" + imsi + "</td></tr>";

    // 查询ICCID
    resp = sendATCommand("AT+ICCID", 2000);
    String iccid = "未知";
    if (resp.indexOf("+ICCID:") >= 0) {
      int idx = resp.indexOf("+ICCID:");
      String tmp = resp.substring(idx + 7);
      int endIdx = tmp.indexOf('\r');
      if (endIdx < 0) endIdx = tmp.indexOf('\n');
      if (endIdx > 0) iccid = tmp.substring(0, endIdx);
      iccid.trim();
    }
    message += "<tr><td>ICCID</td><td>" + iccid + "</td></tr>";

    // 查询本机号码 (如果SIM卡支持)
    resp = sendATCommand("AT+CNUM", 2000);
    String phoneNum = "未存储或不支持";
    if (resp.indexOf("+CNUM:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        int endIdx = resp.indexOf("\"", idx + 2);
        if (endIdx > idx) {
          phoneNum = resp.substring(idx + 2, endIdx);
        }
      }
    }
    message += "<tr><td>本机号码</td><td>" + phoneNum + "</td></tr>";

    message += "</table>";
  }
  else if (type == "network") {
    // 网络状态查询
    success = true;
    message = "<table class='info-table'>";

    // 查询网络注册状态
    String resp = sendATCommand("AT+CEREG?", 2000);
    String regStatus = "未知";
    if (resp.indexOf("+CEREG:") >= 0) {
      int idx = resp.indexOf("+CEREG:");
      String tmp = resp.substring(idx + 7);
      int commaIdx = tmp.indexOf(',');
      if (commaIdx >= 0) {
        String stat = tmp.substring(commaIdx + 1, commaIdx + 2);
        int s = stat.toInt();
        switch(s) {
          case 0: regStatus = "未注册，未搜索"; break;
          case 1: regStatus = "已注册，本地网络"; break;
          case 2: regStatus = "未注册，正在搜索"; break;
          case 3: regStatus = "注册被拒绝"; break;
          case 4: regStatus = "未知"; break;
          case 5: regStatus = "已注册，漫游"; break;
          default: regStatus = "状态码: " + stat;
        }
      }
    }
    message += "<tr><td>网络注册</td><td>" + regStatus + "</td></tr>";

    // 查询运营商
    resp = sendATCommand("AT+COPS?", 2000);
    String oper = "未知";
    if (resp.indexOf("+COPS:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        int endIdx = resp.indexOf("\"", idx + 2);
        if (endIdx > idx) {
          oper = resp.substring(idx + 2, endIdx);
        }
      }
    }
    message += "<tr><td>运营商</td><td>" + oper + "</td></tr>";

    // 查询PDP上下文激活状态
    resp = sendATCommand("AT+CGACT?", 2000);
    String pdpStatus = "未激活";
    if (resp.indexOf("+CGACT: 1,1") >= 0) {
      pdpStatus = "已激活";
    } else if (resp.indexOf("+CGACT:") >= 0) {
      pdpStatus = "未激活";
    }
    message += "<tr><td>数据连接</td><td>" + pdpStatus + "</td></tr>";

    // 查询APN
    resp = sendATCommand("AT+CGDCONT?", 2000);
    String apn = "未知";
    if (resp.indexOf("+CGDCONT:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        idx = resp.indexOf(",\"", idx + 2);  // 跳过PDP类型
        if (idx >= 0) {
          int endIdx = resp.indexOf("\"", idx + 2);
          if (endIdx > idx) {
            apn = resp.substring(idx + 2, endIdx);
            if (apn.length() == 0) apn = "(自动)";
          }
        }
      }
    }
    message += "<tr><td>APN</td><td>" + apn + "</td></tr>";

    message += "</table>";
  }
  else if (type == "wifi") {
    // WiFi状态查询
    success = true;
    message = "<table class='info-table'>";

    // WiFi连接状态
    String wifiStatus = WiFi.isConnected() ? "已连接" : "未连接";
    message += "<tr><td>连接状态</td><td>" + wifiStatus + "</td></tr>";

    // SSID
    String ssid = WiFi.SSID();
    if (ssid.length() == 0) ssid = "未知";
    message += "<tr><td>当前SSID</td><td>" + ssid + "</td></tr>";

    // 信号强度 RSSI
    int rssi = WiFi.RSSI();
    String rssiStr = String(rssi) + " dBm";
    if (rssi >= -50) rssiStr += " (信号极好)";
    else if (rssi >= -60) rssiStr += " (信号很好)";
    else if (rssi >= -70) rssiStr += " (信号良好)";
    else if (rssi >= -80) rssiStr += " (信号一般)";
    else if (rssi >= -90) rssiStr += " (信号较弱)";
    else rssiStr += " (信号很差)";
    message += "<tr><td>信号强度 (RSSI)</td><td>" + rssiStr + "</td></tr>";

    // IP地址
    message += "<tr><td>IP地址</td><td>" + WiFi.localIP().toString() + "</td></tr>";

    // 网关
    message += "<tr><td>网关</td><td>" + WiFi.gatewayIP().toString() + "</td></tr>";

    // 子网掩码
    message += "<tr><td>子网掩码</td><td>" + WiFi.subnetMask().toString() + "</td></tr>";

    // DNS
    message += "<tr><td>DNS服务器</td><td>" + WiFi.dnsIP().toString() + "</td></tr>";

    // MAC地址
    message += "<tr><td>MAC地址</td><td>" + WiFi.macAddress() + "</td></tr>";

    // BSSID (路由器MAC)
    message += "<tr><td>路由器BSSID</td><td>" + WiFi.BSSIDstr() + "</td></tr>";

    // 信道
    message += "<tr><td>WiFi信道</td><td>" + String(WiFi.channel()) + "</td></tr>";

    message += "</table>";
  }
  else {
    message = "未知的查询类型";
  }

  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + jsonEscape(message) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

// 处理发送短信请求
void handleSendSms() {
  if (!authRequireCsrf()) return;
  if (!simManagerIsReady() || !simManagerSmsReady()) {
    server.send(409, "application/json",
                "{\"success\":false,\"message\":\"SIM 或短信服务尚未就绪\"}");
    return;
  }
  if (esimIsBusy()) {
    server.send(409, "application/json", "{\"success\":false,\"message\":\"eSIM切换中，无法发送短信\"}");
    return;
  }

  String phone = server.arg("phone");
  String content = server.arg("content");

  phone.trim();
  content.trim();

  bool success = false;
  String resultMsg = "";

  if (phone.length() == 0) {
    resultMsg = "错误：请输入目标号码";
  } else if (content.length() == 0) {
    resultMsg = "错误：请输入短信内容";
  } else {
    logCaptureLn(String("网页端发送短信请求"));
    logCaptureLn(String("目标号码: " + phone));
    logCaptureLn(String("短信内容: " + content));

    success = sendSMS(phone.c_str(), content.c_str());
    resultMsg = success ? "运营商已接收发送请求；这不代表对方已经收到" : "短信发送失败，请检查模组状态";
  }

  String json = "{\"success\":" + String(success ? "true" : "false") +
                ",\"message\":\"" + jsonEscape(resultMsg) + "\"}";
  server.send(200, "application/json", json);
}

// 处理Ping请求
void handlePing() {
  if (!authRequireCsrf()) return;
  if (esimIsBusy()) {
    server.send(409, "application/json", "{\"success\":false,\"message\":\"eSIM切换中，无法执行 Ping\"}");
    return;
  }
  if (!modemSupportsPdpContextControl()) {
    server.send(409, "application/json",
                "{\"success\":false,\"message\":\"当前 ML307Y 固件不安全支持 PDP 上下文开关，已禁用 Ping 流量测试\"}");
    return;
  }
  if (!requireModemRouteReady()) return;

  logCaptureLn(String("网页端发起Ping请求"));
  if (!modemAcquireExclusive()) {
    server.send(409, "application/json",
                "{\"success\":false,\"message\":\"模组正在处理其他通信，请稍后重试\"}");
    return;
  }

  // Pending UART data can contain SMS URCs. Always route it through the
  // shared dispatcher instead of discarding it before a diagnostic command.
  checkSerial1URC();

  // 激活PDP上下文（数据连接）
  logCaptureLn(String("激活数据连接(CGACT)..."));
  String activateResp = sendATCommand("AT+CGACT=1,1", 10000);
  logCaptureLn(String("CGACT响应: " + activateResp));

  // 检查激活是否成功（OK或已激活的情况）
  bool networkActivated = (activateResp.indexOf("OK") >= 0);
  if (!networkActivated) {
    logCaptureLn(String("数据连接激活失败，尝试继续执行..."));
  }

  checkSerial1URC();
  delay(500);  // 等待网络稳定

  // 发送MPING命令，ping 8.8.8.8，超时30秒，ping 1次
  Serial1.println("AT+MPING=\"8.8.8.8\",30,1");

  // 等待响应
  unsigned long start = millis();
  String resp = "";
  bool gotOK = false;
  bool gotError = false;
  bool gotPingResult = false;
  String pingResultMsg = "";

  // 等待最多35秒（30秒超时 + 5秒余量）
  while (millis() - start < 35000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      logCapture(String(c));  // 调试输出
      dispatchSerial1Byte(c, false);

      // 检查是否收到OK
      if (resp.indexOf("OK") >= 0 && !gotOK) {
        gotOK = true;
      }

      // 检查是否收到ERROR
      if (resp.indexOf("+CME ERROR") >= 0 || resp.indexOf("ERROR") >= 0) {
        gotError = true;
        pingResultMsg = "模组返回错误";
        break;
      }

      // 检查是否收到Ping结果URC
      // 成功格式: +MPING: 1,8.8.8.8,32,xxx,xxx
      // 失败格式: +MPING: 2 或其他
      int mpingIdx = resp.indexOf("+MPING:");
      if (mpingIdx >= 0) {
        // 找到换行符确定完整的一行
        int lineEnd = resp.indexOf('\n', mpingIdx);
        if (lineEnd >= 0) {
          String mpingLine = resp.substring(mpingIdx, lineEnd);
          mpingLine.trim();
          logCaptureLn(String("收到MPING结果: " + mpingLine));

          // 解析结果
          // +MPING: <result>[,<ip>,<packet_len>,<time>,<ttl>]
          int colonIdx = mpingLine.indexOf(':');
          if (colonIdx >= 0) {
            String params = mpingLine.substring(colonIdx + 1);
            params.trim();

            // 获取第一个参数（result）
            int commaIdx = params.indexOf(',');
            String resultStr;
            if (commaIdx >= 0) {
              resultStr = params.substring(0, commaIdx);
            } else {
              resultStr = params;
            }
            resultStr.trim();
            int result = resultStr.toInt();

            gotPingResult = true;

            // result=0或1都表示成功（不同模组可能返回不同值）
            // 如果有完整的响应参数（IP、时间等），也视为成功
            bool pingSuccess = (result == 0 || result == 1) || (params.indexOf(',') >= 0 && params.length() > 5);

            if (pingSuccess) {
              // 成功，解析详细信息
              // 格式: 0/1,"8.8.8.8",16,时间,TTL
              int idx1 = params.indexOf(',');
              if (idx1 >= 0) {
                String rest = params.substring(idx1 + 1);
                // 处理IP地址（可能带引号）
                String ip;
                int idx2;
                if (rest.startsWith("\"")) {
                  // 带引号的IP
                  int quoteEnd = rest.indexOf('\"', 1);
                  if (quoteEnd >= 0) {
                    ip = rest.substring(1, quoteEnd);
                    idx2 = rest.indexOf(',', quoteEnd);
                  } else {
                    idx2 = rest.indexOf(',');
                    ip = rest.substring(0, idx2);
                  }
                } else {
                  idx2 = rest.indexOf(',');
                  ip = rest.substring(0, idx2);
                }

                if (idx2 >= 0) {
                  rest = rest.substring(idx2 + 1);
                  int idx3 = rest.indexOf(',');  // packet_len后
                  if (idx3 >= 0) {
                    rest = rest.substring(idx3 + 1);
                    int idx4 = rest.indexOf(',');  // time后
                    String timeStr, ttlStr;
                    if (idx4 >= 0) {
                      timeStr = rest.substring(0, idx4);
                      ttlStr = rest.substring(idx4 + 1);
                    } else {
                      timeStr = rest;
                      ttlStr = "N/A";
                    }
                    timeStr.trim();
                    ttlStr.trim();
                    pingResultMsg = "目标: " + ip + ", 延迟: " + timeStr + "ms, TTL: " + ttlStr;
                  }
                }
              }
              if (pingResultMsg.length() == 0) {
                pingResultMsg = "Ping成功";
              }
            } else {
              // 失败
              pingResultMsg = "Ping超时或目标不可达 (错误码: " + String(result) + ")";
            }
            break;
          }
        }
      }
    }

    if (gotError || gotPingResult) break;
    server.handleClient();
  }

  logCaptureLn(String("\nPing操作完成"));

  // 关闭数据连接以节省流量
  logCaptureLn(String("关闭PDP上下文(CGACT=0)..."));
  String deactivateResp = sendATCommand("AT+CGACT=0,1", 5000);
  logCaptureLn(String("CGACT关闭响应: " + deactivateResp));
  modemReleaseExclusive();

  // 构建JSON响应
  String json = "{";
  if (gotPingResult && pingResultMsg.indexOf("延迟") >= 0) {
    json += "\"success\":true,";
    json += "\"message\":\"" + pingResultMsg + "\"";
  } else if (gotError) {
    json += "\"success\":false,";
    json += "\"message\":\"" + pingResultMsg + "\"";
  } else if (gotPingResult) {
    json += "\"success\":false,";
    json += "\"message\":\"" + pingResultMsg + "\"";
  } else {
    json += "\"success\":false,";
    json += "\"message\":\"操作超时，未收到Ping结果\"";
  }
  json += "}";

  server.send(200, "application/json", json);
}






// 处理日志查询请求 — 返回环形缓冲区中的日志行
void handleLog() {
  if (!checkAuth()) return;

  String json = "[";
  int total = logBufCount;
  int start = total < LOG_BUF_SIZE ? 0 : logBufIdx;
  for (int i = 0; i < total; i++) {
    int pos = (start + i) % LOG_BUF_SIZE;
    if (i > 0) json += ",";
    json += "\"" + jsonEscape(logBuffer[pos]) + "\"";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// 模组控制命令
void handleModem() {
  if (!authRequireCsrf()) return;
  if (esimIsBusy()) {
    server.send(409, "application/json", "{\"success\":false,\"message\":\"eSIM切换中，模组操作已锁定\"}");
    return;
  }
  if (!requireModemRouteReady()) return;

  // 防止重入：modemInit() 内部会调 server.handleClient()，
  // 若浏览器超时重试会导致嵌套调用，最终拖垮 WiFi
  static bool busy = false;
  if (busy) {
    server.send(429, "application/json", "{\"success\":false,\"message\":\"模组正忙，请稍后重试\"}");
    return;
  }
  busy = true;

  String action = server.arg("action");
  if ((action == "restart" || action == "hardreset") && !requirePostMethod()) return;
  String json = "{";
  bool success = false;
  String message = "";

  if (action == "restart") {
    // AT 软重启 — 先响应浏览器再初始化，防止浏览器超时重试
    logCaptureLn(String("网页端请求软重启模组..."));
    server.send(200, "application/json", "{\"success\":true,\"message\":\"正在软重启模组，请等待约 15 秒后刷新页面\"}");
    String resp = sendATCommand("AT+CFUN=1,1", 15000);
    success = (resp.indexOf("OK") >= 0);
    message = success ? "模组软重启成功" : "软重启失败";
    logCaptureLn(String(message + ": " + resp));
    if (success) modemInit();
    busy = false;
    return;
  }
  else if (action == "hardreset") {
    // EN 引脚断电重启（内部已调用 modemInit()）
    logCaptureLn(String("网页端请求硬重启模组..."));
    server.send(200, "application/json", "{\"success\":true,\"message\":\"正在重新上电识别 SIM，请等待约 30–60 秒\"}");
    resetModule();
    busy = false;
    return;
  }
  else if (action == "signal") {
    logCaptureLn(String("网页端查询信号: AT+CSQ"));
    String resp = sendATCommand("AT+CSQ", 3000);
    int csqIdx = resp.indexOf("+CSQ:");
    if (csqIdx >= 0) {
      String csqLine = resp.substring(csqIdx);
      csqLine = csqLine.substring(0, csqLine.indexOf('\n'));
      csqLine.trim();
      int commaIdx = csqLine.indexOf(',');
      if (commaIdx >= 0) {
        int rssi = csqLine.substring(csqLine.indexOf(':') + 1, commaIdx).toInt();
        int ber = csqLine.substring(commaIdx + 1).toInt();
        if (rssi == 99) {
          message = "RSSI: 未知, CSQ: 99, BER: " + String(ber);
        } else {
          int dbm = -113 + rssi * 2;
          String quality;
          if (rssi >= 19) quality = "优秀";
          else if (rssi >= 14) quality = "良好";
          else if (rssi >= 10) quality = "一般";
          else if (rssi >= 5) quality = "较差";
          else quality = "很差";
          message = "RSSI: " + String(dbm) + " dBm (" + quality +
                    "), CSQ: " + String(rssi) + ", BER: " + String(ber);
        }
        success = true;
      }
    }
    if (!success) message = "无法获取信号: " + resp;
  }
  else if (action == "operator") {
    logCaptureLn(String("网页端查询运营商: AT+COPS?"));
    String resp = sendATCommand("AT+COPS?", 5000);
    int copsIdx = resp.indexOf("+COPS:");
    if (copsIdx >= 0) {
      String copsLine = resp.substring(copsIdx);
      copsLine = copsLine.substring(0, copsLine.indexOf('\n'));
      copsLine.trim();
      int q1 = copsLine.indexOf('"');
      int q2 = copsLine.indexOf('"', q1 + 1);
      if (q1 >= 0 && q2 >= 0) {
        message = copsLine.substring(q1 + 1, q2);
        success = true;
      } else {
        message = copsLine;
        success = true;
      }
    }
    if (!success) message = "无法获取运营商: " + resp;
  }
  else if (action == "imei") {
    logCaptureLn(String("网页端查询IMEI: AT+GSN"));
    String resp = sendATCommand("AT+GSN", 3000);
    resp.trim();
    int okIdx = resp.lastIndexOf("OK");
    if (okIdx > 0) resp = resp.substring(0, okIdx);
    int gsnIdx = resp.indexOf("AT+GSN");
    if (gsnIdx >= 0) resp = resp.substring(gsnIdx + 6);
    resp.trim();
    if (resp.length() > 0) {
      message = resp;
      success = true;
    } else {
      message = "无法获取 IMEI";
    }
  }
  else {
    message = "未知操作: " + action;
  }

  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + jsonEscape(message) + "\"";
  json += "}";
  busy = false;
  server.send(200, "application/json", json);
}

// WiFi 重启
void handleWifi() {
  if (!authRequireCsrf()) return;

  static bool busy = false;
  if (busy) {
    server.send(429, "application/json", "{\"success\":false,\"message\":\"WiFi正忙，请稍后重试\"}");
    return;
  }
  busy = true;

  String action = server.arg("action");
  if (action == "restart") {
    logCaptureLn(String("网页端请求重启WiFi..."));
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi 正在重启，请等待约 15 秒后刷新页面\"}");
    if (wifiConnectAll()) {
      logCaptureLn(String("WiFi 重连成功, IP: " + WiFi.localIP().toString()));
    } else {
      logCaptureLn(String("WiFi 重连失败，将在后台持续尝试"));
    }
  } else {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"未知操作\"}");
  }
  busy = false;
}
