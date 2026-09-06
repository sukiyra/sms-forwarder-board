#include "globals.h"
#include "config.h"
#include "web_handlers.h"
#include <HTTPClient.h>
#include "modem.h"
#include "push.h"
#include "sms_process.h"
#include "auth.h"
#include "api_handlers.h"
#include "sms_store.h"
#include "esim_manager.h"
#include "operator_manager.h"
#include "sim_manager.h"
#include "factory_serial.h"

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.begin(115200);
  // 缩短初始化延时，WiFi连接会处理自己的超时
  delay(200);
  Serial1.setRxBufferSize(MODEM_RX_BUFFER_SIZE);
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);
  while (Serial1.available()) Serial1.read();
  modemPowerCycle();
  while (Serial1.available()) Serial1.read();
  initConcatBuffer();
  loadConfig();
  configValid = isConfigValid();
  if (!smsStoreBegin()) {
    logCaptureLn(String("⚠️ 短信持久化存储初始化失败"));
  }

  // ---- HTTP 服务先行启动,保证任意 WiFi 状态下都可访问管理台 ----
  // WebServer 依赖 WiFi 协议栈的事件队列,必须先初始化 WiFi 再起服务
  // persistent(false): 凭据只由本固件 NVS 管理,防止 esp 自动重连旧网络干扰热点
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  authBegin();
  server.on("/", HTTP_GET, handleRoot);
  server.on("/tools", HTTP_GET, handleRoot);
  server.on("/sms", HTTP_GET, handleRoot);
  server.on("/query", handleQuery);
  server.on("/at", HTTP_POST, handleATCommand);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/modem", handleModem);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/ping", HTTP_POST, handlePing);
  registerApiRoutes();
  server.begin();
  logCaptureLn(String("HTTP服务器已启动"));

  // ---- WiFi:依次尝试主备网络,全部失败则开启配置热点 ----
  if (wifiConnectAll()) {
    logCaptureLn(String("wifi已连接"));
    logCapture(String("IP地址: "));
    logCaptureLn(WiFi.localIP().toString());
    logCapture(String("信号强度(RSSI): "));
    logCaptureLn(String(WiFi.RSSI()) + " dBm");
  } else {
    wifiStartAp();
  }

  // ---- NTP 时间同步 ----
  logCaptureLn(String("正在同步NTP时间..."));
  configTime(0, 0, "ntp.ntsc.ac.cn", "ntp.aliyun.com", "pool.ntp.org");
  unsigned long ntpStartedAt = millis();
  while (time(nullptr) < 100000 && millis() - ntpStartedAt < 5000) {
    delay(25);
    server.handleClient();
  }
  if (time(nullptr) >= 100000) {
    logCaptureLn(String("NTP时间同步成功"));
    time_t now = time(nullptr);
    logCapture(String("当前UTC时间戳: "));
    logCaptureLn(String(now));
  } else {
    logCaptureLn(String("NTP时间同步失败，将使用设备时间"));
  }

  ssl_client.setInsecure();
  digitalWrite(LED_BUILTIN, LOW);

  // ---- 启动通知（网页已可用，发邮件不会影响用户访问） ----
  if (configValid) {
    logCaptureLn(String("配置有效，发送启动通知..."));
    String subject = "短信转发器已启动";
    String body = "设备已启动\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }

  // ---- 模组初始化（较慢，但网页已可访问） ----
  modemInit();
  esimManagerBegin();
  simManagerBegin();
  operatorManagerBegin();
}

void loop() {
  // Drain long COPS responses before serving HTTP so the UART ring cannot
  // overflow while a network scan is returning several kilobytes at once.
  operatorManagerLoop();
  // Only drain an already active SIM transaction before HTTP. New periodic
  // probes are started after serving HTTP so the 3-second status poll cannot
  // repeatedly observe a short CPIN transaction as a permanently busy modem.
  if (simManagerIsBusy()) simManagerLoop();
  if (smsStoredMessageIsBusy()) smsStoredMessageLoop();
  server.handleClient();
  static bool configWarningPrinted = false;
  if (!configValid && !configWarningPrinted) {
    configWarningPrinted = true;
    logCaptureLn(String("⚠️ 请访问 " + getDeviceUrl() + " 配置系统参数"));
  }
  checkConcatTimeout();
  checkSmsReceiveTimeout();
  operatorManagerLoop();
  simManagerLoop();
  smsStoredMessageLoop();
  if (!operatorManagerIsBusy() && !simManagerIsBusy() && !smsStoredMessageIsBusy()) {
    esimManagerLoop();
  }
  if (!esimIsBusy() && !operatorManagerIsBusy() && !simManagerIsBusy() &&
      !smsStoredMessageIsBusy()) {
    factorySerialLoop();
    checkSerial1URC();
  }
  // Web requests only enqueue outbound SMS so the browser gets an immediate
  // response. Run the modem transaction after URCs and SIM work are drained.
  processPendingWebSms();
  if (schedulerTickDue() && !pendingWebSmsBusy()) {
    checkCustomTasks();
  }
  wifiMaintenanceLoop();
  delay(1);
}

// 自定义任务:按小时间隔定时执行 重启/AT/HTTP
static void runCustomTask(int i) {
  const CustomTask &t = config.tasks[i];
  if (t.type == TASK_RESTART) {
    logCaptureLn(String("自定义任务") + String(i + 1) + ": 定时重启设备");
    configRecordTaskRun(i, time(nullptr));
    delay(500);
    ESP.restart();
  } else if (t.type == TASK_AT) {
    logCaptureLn(String("自定义任务") + String(i + 1) + ": 执行 AT: " + t.param);
    String resp = sendATCommand(t.param.c_str(), 8000);
    logCaptureLn(String("任务AT响应: ") + resp.substring(0, 180));
  } else if (t.type == TASK_HTTP) {
    logCaptureLn(String("自定义任务") + String(i + 1) + ": " + String(t.httpMethod == 1 ? "POST " : "GET ") + t.param);
    if (WiFi.status() != WL_CONNECTED) {
      logCaptureLn(String("任务HTTP跳过: WiFi未连接"));
      return;
    }
    HTTPClient http;
    http.begin(t.param.c_str());
    http.setTimeout(15000);
    int from = 0;
    while (from >= 0 && from < (int)t.headers.length()) {
      int eol = t.headers.indexOf('\n', from);
      String line = t.headers.substring(from, eol < 0 ? t.headers.length() : eol);
      line.trim();
      int colon = line.indexOf(':');
      if (colon > 0) {
        String name = line.substring(0, colon);
        name.trim();
        String value = line.substring(colon + 1);
        value.trim();
        http.addHeader(name.c_str(), value.c_str(), false);
      }
      from = eol < 0 ? t.headers.length() : eol + 1;
    }
    int code = t.httpMethod == 1 ? http.POST(t.body) : http.GET();
    logCaptureLn(String("任务HTTP响应码: ") + String(code));
    http.end();
  }
}

// 日历模式按本地时区 UTC+8 计算,与网页端展示一致
constexpr long kLocalTzOffsetSec = 8 * 3600L;

static uint8_t daysInMonth(int year, int mon) {
  static const uint8_t d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (mon == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  return d[mon];
}

// 返回最近一个已到达的计划时刻(本地帧);非日历模式返回 0
static time_t lastScheduledAt(const CustomTask &t, time_t nowLocal, struct tm &lt) {
  time_t dayStart = nowLocal - (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
  if (t.mode == 1) return dayStart + t.hour * 3600 + t.minute * 60;
  if (t.mode == 2) {
    int back = (lt.tm_wday - t.weekday + 7) % 7;
    return dayStart - (time_t)back * 86400 + t.hour * 3600 + t.minute * 60;
  }
  if (t.mode == 3) {
    uint8_t dom = t.dayOfMonth < 1 ? 1 : t.dayOfMonth;
    uint8_t dim = daysInMonth(lt.tm_year + 1900, lt.tm_mon);
    if (dom > dim) dom = dim;
    time_t monthStart = dayStart - (time_t)(lt.tm_mday - 1) * 86400;
    return monthStart + (time_t)(dom - 1) * 86400 + t.hour * 3600 + t.minute * 60;
  }
  return 0;
}

static void checkCustomTasks() {
  time_t now = time(nullptr);
  if (now < 100000) return;
  for (int i = 0; i < MAX_CUSTOM_TASKS; i++) {
    const CustomTask &t = config.tasks[i];
    if (t.type == TASK_NONE) continue;
    if (t.mode == 0 && t.intervalSeconds == 0) continue;
    if (t.type != TASK_HTTP) {
      if (esimIsBusy() || operatorManagerIsBusy() || simManagerIsBusy() ||
          smsStoredMessageIsBusy() || modemIsBusy()) {
        continue;
      }
    }
    uint32_t last = configTaskLastRun(i);
    bool due = false;
    if (t.mode == 0) {
      due = (last == 0) || ((uint32_t)now - last >= t.intervalSeconds);
    } else {
      time_t local = now + kLocalTzOffsetSec;
      struct tm lt;
      gmtime_r(&local, &lt);
      time_t at = lastScheduledAt(t, local, lt);
      if (at > 0) due = (local >= at) && (last == 0 || (time_t)last + kLocalTzOffsetSec < at);
    }
    if (!due) continue;
    configRecordTaskRun(i, (uint32_t)now);
    runCustomTask(i);
  }
}

static unsigned long schedulerLastTick = 0;
static bool schedulerTickDue() {
  if (millis() - schedulerLastTick < 60000) return false;
  schedulerLastTick = millis();
  return true;
}
