#include "sms_process.h"
#include "web_handlers.h"
#include "modem.h"
#include "push.h"
#include "sms_store.h"
#include "esim_manager.h"
#include "operator_manager.h"
#include "sim_manager.h"

// 初始化长短信缓存
void initConcatBuffer() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    concatBuffer[i].inUse = false;
    concatBuffer[i].receivedParts = 0;
    for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
      concatBuffer[i].parts[j].valid = false;
      concatBuffer[i].parts[j].text = "";
    }
  }
}

// 查找或创建长短信缓存槽位
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts) {
  // 先查找是否已存在
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse &&
        concatBuffer[i].refNumber == refNumber &&
        concatBuffer[i].sender.equals(sender)) {
      return i;
    }
  }

  // 查找空闲槽位
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (!concatBuffer[i].inUse) {
      concatBuffer[i].inUse = true;
      concatBuffer[i].refNumber = refNumber;
      concatBuffer[i].sender = String(sender);
      concatBuffer[i].totalParts = totalParts;
      concatBuffer[i].receivedParts = 0;
      concatBuffer[i].firstPartTime = millis();
      for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
        concatBuffer[i].parts[j].valid = false;
        concatBuffer[i].parts[j].text = "";
      }
      return i;
    }
  }

  // 没有空闲槽位，查找最老的槽位覆盖
  int oldestSlot = 0;
  unsigned long oldestTime = concatBuffer[0].firstPartTime;
  for (int i = 1; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].firstPartTime < oldestTime) {
      oldestTime = concatBuffer[i].firstPartTime;
      oldestSlot = i;
    }
  }

  // 覆盖最老的槽位
  logCaptureLn(String("⚠️ 长短信缓存已满，覆盖最老的槽位"));
  concatBuffer[oldestSlot].inUse = true;
  concatBuffer[oldestSlot].refNumber = refNumber;
  concatBuffer[oldestSlot].sender = String(sender);
  concatBuffer[oldestSlot].totalParts = totalParts;
  concatBuffer[oldestSlot].receivedParts = 0;
  concatBuffer[oldestSlot].firstPartTime = millis();
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[oldestSlot].parts[j].valid = false;
    concatBuffer[oldestSlot].parts[j].text = "";
  }
  return oldestSlot;
}

// 合并长短信各分段
String assembleConcatSms(int slot) {
  String result = "";
  int supportedParts = min(concatBuffer[slot].totalParts, MAX_CONCAT_PARTS);
  for (int i = 0; i < supportedParts; i++) {
    if (concatBuffer[slot].parts[i].valid) {
      result += concatBuffer[slot].parts[i].text;
    } else {
      result += "[缺失分段" + String(i + 1) + "]";
    }
  }
  if (concatBuffer[slot].totalParts > MAX_CONCAT_PARTS) {
    result += "[短信分段超过设备上限，后续内容未保存]";
  }
  return result;
}

// 清空长短信槽位
void clearConcatSlot(int slot) {
  concatBuffer[slot].inUse = false;
  concatBuffer[slot].receivedParts = 0;
  concatBuffer[slot].sender = "";
  concatBuffer[slot].timestamp = "";
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[slot].parts[j].valid = false;
    concatBuffer[slot].parts[j].text = "";
  }
}

// 检查长短信超时并转发
void checkConcatTimeout() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse) {
      if (now - concatBuffer[i].firstPartTime >= CONCAT_TIMEOUT_MS) {
        logCaptureLn(String("⏰ 长短信超时，强制转发不完整消息"));
        logCaptureF("  参考号: %d, 已收到: %d/%d\n",
                      concatBuffer[i].refNumber,
                      concatBuffer[i].receivedParts,
                      concatBuffer[i].totalParts);

        // 合并已收到的分段
        String fullText = assembleConcatSms(i);

        // 处理短信内容
        if (processSmsContent(concatBuffer[i].sender.c_str(),
                              fullText.c_str(),
                              concatBuffer[i].timestamp.c_str())) {
          clearConcatSlot(i);
        } else {
          // Keep the assembled message in RAM and retry later instead of
          // silently losing it when persistent inbox storage is unavailable.
          concatBuffer[i].firstPartTime = now;
        }
      }
    }
  }
}

// 读取串口一行（含回车换行），返回行字符串，无新行时返回空
String readSerialLine(HardwareSerial& port) {
  static char lineBuf[SERIAL_BUFFER_SIZE];
  static int linePos = 0;

  while (port.available()) {
    char c = port.read();
    if (c == '\n') {
      lineBuf[linePos] = 0;
      String res = String(lineBuf);
      linePos = 0;
      return res;
    } else if (c != '\r') {  // 跳过\r
      if (linePos < SERIAL_BUFFER_SIZE - 1)
        lineBuf[linePos++] = c;
      else
        linePos = 0;  //超长报错保护，重头计
    }
  }
  return "";
}

// 检查字符串是否为有效的十六进制PDU数据
bool isHexString(const String& str) {
  if (str.length() == 0) return false;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// 检查发送者是否在号码黑名单中
bool isInNumberBlackList(const char* sender) {
  if (config.numberBlackList.length() == 0) return false;

  String originalSender = String(sender);
  bool has86 = originalSender.startsWith("+86");
  String strippedSender = has86 ? originalSender.substring(3) : "";

  int listLen = (int)config.numberBlackList.length();

  int start = 0;
  while (start <= listLen) {
    int end = config.numberBlackList.indexOf('\n', start);
    if (end == -1) end = listLen;

    String line = config.numberBlackList.substring(start, end);
    line.trim();

    if (line.length() > 0 && (line.equals(originalSender) || (has86 && line.equals(strippedSender)))) {
      return true;
    }

    start = end + 1;
  }

  return false;
}

// 检查发送者是否为管理员
bool isAdmin(const char* sender) {
  if (config.adminPhone.length() == 0) return false;

  // 去除可能的国际区号前缀进行比较
  String senderStr = String(sender);
  String adminStr = config.adminPhone;

  // 去除+86前缀
  if (senderStr.startsWith("+86")) {
    senderStr = senderStr.substring(3);
  }
  if (adminStr.startsWith("+86")) {
    adminStr = adminStr.substring(3);
  }

  return senderStr.equals(adminStr);
}

// 处理管理员命令
void processAdminCommand(const char* sender, const char* text) {
  String cmd = String(text);
  cmd.trim();

  logCaptureLn(String("处理管理员命令: " + cmd));

  // 处理 SMS:号码:内容 命令
  if (cmd.startsWith("SMS:")) {
    int firstColon = cmd.indexOf(':');
    int secondColon = cmd.indexOf(':', firstColon + 1);

    if (secondColon > firstColon + 1) {
      String targetPhone = cmd.substring(firstColon + 1, secondColon);
      String smsContent = cmd.substring(secondColon + 1);

      targetPhone.trim();
      smsContent.trim();

      logCaptureLn(String("目标号码: " + targetPhone));
      logCaptureLn(String("短信内容: " + smsContent));

      bool success = sendSMS(targetPhone.c_str(), smsContent.c_str());

      // 发送邮件通知结果
      String subject = success ? "运营商已接收短信发送请求" : "短信发送失败";
      String body = "管理员命令执行结果:\n";
      body += "命令: " + cmd + "\n";
      body += "目标号码: " + targetPhone + "\n";
      body += "短信内容: " + smsContent + "\n";
      body += "执行结果: " + String(success ? "运营商已接收发送请求（不代表对方已收到）" : "失败");

      sendEmailNotification(subject.c_str(), body.c_str());
    } else {
      logCaptureLn(String("SMS命令格式错误"));
      sendEmailNotification("命令执行失败", "SMS命令格式错误，正确格式: SMS:号码:内容");
    }
  }
  // 处理 RESET 命令
  else if (cmd.equals("RESET")) {
    logCaptureLn(String("执行RESET命令"));

    // 先发送邮件通知（因为重启后就发不了了）
    sendEmailNotification("重启命令已执行", "收到RESET命令，即将重启模组和ESP32...");

    // 重启模组
    resetModule();

    // 重启ESP32
    logCaptureLn(String("正在重启ESP32..."));
    delay(1000);
    ESP.restart();
  }
  else {
    logCaptureLn(String("未知命令: " + cmd));
  }
}

// 处理最终的短信内容（管理员命令检查和转发）
bool processSmsContent(const char* sender, const char* text, const char* timestamp) {
  // 检查是否在号码黑名单中
  if (isInNumberBlackList(sender)) {
    logCaptureLn(String("发送者在号码黑名单中，忽略该短信"));
    return true;
  }

  // 检查是否为管理员命令
  if (isAdmin(sender)) {
    logCaptureLn(String("收到管理员短信，检查命令..."));
    String smsText = String(text);
    smsText.trim();

    // 检查是否为命令格式
    if (smsText.startsWith("SMS:") || smsText.equals("RESET")) {
      processAdminCommand(sender, text);
      // 命令已处理，不再发送普通通知邮件
      return true;
    }
  }

  String smsText = String(text);
  String activeProfile = esimActiveProfileSmsLabel();
  bool complete = smsText.indexOf("[缺失分段") < 0 &&
                  smsText.indexOf("[短信分段超过设备上限") < 0;
  uint32_t storedId = smsStoreAdd(sender, text, timestamp, activeProfile.c_str(), complete);
  if (!storedId) {
    logCaptureLn(String("⚠️ 短信持久化失败，未执行通知转发"));
    return false;
  }
  logCaptureF("短信已接收，id=%lu，长度=%u%s\n", storedId,
              static_cast<unsigned>(smsText.length()), complete ? "" : "，分段不完整");

  // 发送通知http（推送到所有启用的通道）
  sendSMSToServer(sender, text, timestamp);
  // 发送通知邮件
  String subject = ""; subject+="短信";subject+=sender;subject+=",";subject+=text;
  String body = ""; body+="来自：";body+=sender;body+="，时间：";body+=notificationDisplayTime(timestamp);body+="，内容：";body+=text;
  sendEmailNotification(subject.c_str(), body.c_str());
  return true;
}

namespace {

enum SmsRxState { SMS_RX_IDLE, SMS_RX_WAIT_PDU };
SmsRxState smsRxState = SMS_RX_IDLE;
String serial1LineBuffer;
unsigned long smsRxStartedAt = 0;
constexpr unsigned long SMS_PDU_WAIT_TIMEOUT_MS = 3000UL;

constexpr uint8_t STORED_SMS_QUEUE_CAPACITY = 12;
constexpr size_t STORED_SMS_RESPONSE_CAPACITY = 1024;
constexpr unsigned long STORED_SMS_COMMAND_TIMEOUT_MS = 10000UL;
constexpr unsigned long STORED_SMS_ABORT_TIMEOUT_MS = 10000UL;
constexpr unsigned long STORED_SMS_MAX_RETRY_MS = 60000UL;

enum StoredSmsStage {
  STORED_SMS_IDLE,
  STORED_SMS_CPMS,
  STORED_SMS_CMGR,
  STORED_SMS_CMGD,
  STORED_SMS_ABORTING
};

struct StoredSmsEntry {
  bool used;
  char memory[3];
  uint16_t index;
  // Once a PDU has been handed to the normal processing path, never read it
  // again just because CMGD failed. This prevents duplicate inbox records and
  // duplicate notifications while deletion is retried.
  bool processed;
  bool verifyBeforeDelete;
  uint16_t pduLength;
  uint32_t pduFingerprint;
  uint8_t failures;
  unsigned long nextAttemptAt;
  bool fromBackfill;
};

StoredSmsEntry storedSmsQueue[STORED_SMS_QUEUE_CAPACITY] = {};
StoredSmsStage storedSmsStage = STORED_SMS_IDLE;
int8_t storedSmsActiveSlot = -1;
uint8_t storedSmsCursor = 0;
bool storedSmsOwnsExclusive = false;
bool storedSmsPduLinePending = false;
bool directCmtAfterCmgrHeader = false;
bool preserveStoredSmsQueueForTransportReset = false;
bool storedSmsBackfillActive = false;
char storedSmsBackfillMemory[3] = {'S', 'M', '\0'};
uint16_t storedSmsBackfillNextIndex = 1;
uint16_t storedSmsBackfillLastIndex = 0;
unsigned long storedSmsCommandStartedAt = 0;
char storedSmsResponse[STORED_SMS_RESPONSE_CAPACITY] = {};
size_t storedSmsResponseLength = 0;

bool deadlineReached(unsigned long now, unsigned long deadline) {
  return static_cast<long>(now - deadline) >= 0;
}

bool supportedSmsMemory(const String& memory) {
  // These are the standard read/delete stores that may be reported by +CMTI.
  // A strict allow-list also keeps the value safe to place in an AT command.
  return memory == "SM" || memory == "ME" || memory == "MT";
}

bool parseCmti(const String& line, String& memory, uint16_t& index) {
  int firstQuote = line.indexOf('"');
  int secondQuote = firstQuote < 0 ? -1 : line.indexOf('"', firstQuote + 1);
  if (firstQuote < 0 || secondQuote != firstQuote + 3) return false;
  memory = line.substring(firstQuote + 1, secondQuote);
  if (!supportedSmsMemory(memory)) return false;

  int comma = line.indexOf(',', secondQuote + 1);
  if (comma < 0) return false;
  String value = line.substring(comma + 1);
  value.trim();
  if (!value.length() || value.length() > 5) return false;
  uint32_t parsed = 0;
  for (unsigned int i = 0; i < value.length(); ++i) {
    char c = value.charAt(i);
    if (c < '0' || c > '9') return false;
    parsed = parsed * 10U + static_cast<uint32_t>(c - '0');
    if (parsed > 65535U) return false;
  }
  index = static_cast<uint16_t>(parsed);
  return true;
}

void enqueueStoredSms(const String& memory, uint16_t index, bool fromBackfill = false) {
  int freeSlot = -1;
  for (uint8_t i = 0; i < STORED_SMS_QUEUE_CAPACITY; ++i) {
    if (storedSmsQueue[i].used) {
      if (storedSmsQueue[i].index == index &&
          memory.charAt(0) == storedSmsQueue[i].memory[0] &&
          memory.charAt(1) == storedSmsQueue[i].memory[1]) {
        return;
      }
    } else if (freeSlot < 0) {
      freeSlot = i;
    }
  }
  if (freeSlot < 0) {
    logCaptureLn(String("⚠️ 已存短信读取队列已满，保留短信在模组中"));
    return;
  }

  StoredSmsEntry& entry = storedSmsQueue[freeSlot];
  entry.used = true;
  entry.memory[0] = memory.charAt(0);
  entry.memory[1] = memory.charAt(1);
  entry.memory[2] = '\0';
  entry.index = index;
  entry.processed = false;
  entry.verifyBeforeDelete = false;
  entry.pduLength = 0;
  entry.pduFingerprint = 0;
  entry.failures = 0;
  entry.nextAttemptAt = millis();
  entry.fromBackfill = fromBackfill;
  if (!fromBackfill) {
    logCaptureF("检测到模组已存短信：%s/%u，已加入读取队列\n",
                entry.memory, static_cast<unsigned>(entry.index));
  }
}

void observeCmtiLine(const String& line) {
  String memory;
  uint16_t index = 0;
  if (!parseCmti(line, memory, index)) {
    logCaptureLn(String("⚠️ 收到无法识别的 +CMTI 上报，未执行读取"));
    return;
  }
  enqueueStoredSms(memory, index);
}

bool isInterleavedControlLine(const String& line) {
  return line == "OK" || line == "ERROR" || line.startsWith("AT") ||
         line.startsWith("+") || line.startsWith("NO CARRIER");
}

void observeRegistrationLine(const String& line) {
  if (!line.startsWith("+CEREG:")) return;
  String fields = line.substring(7);
  fields.trim();
  int firstComma = fields.indexOf(',');
  int status = -1;
  if (firstComma < 0) {
    status = fields.toInt();
  } else {
    String first = fields.substring(0, firstComma);
    String remainder = fields.substring(firstComma + 1);
    first.trim();
    remainder.trim();
    // Query responses are <n>,<stat>; unsolicited reports are <stat>,"<tac>"...
    status = remainder.startsWith("\"") ? first.toInt() : remainder.toInt();
  }
  if (status >= 0 && status <= 5) {
    modemReady = (status == 1 || status == 5) && simManagerIsReady();
  }
}

String currentSmsArrivalTimestamp(const char* fallbackScts) {
  // Store the time at which this device processed the first received PDU. UTC
  // ISO-8601 is unambiguous, survives browser time zones, and fits the existing
  // 25-byte timestamp field without changing the persistent store layout.
  time_t now = time(nullptr);
  if (now >= 1577836800) {  // 2020-01-01; rejects an unsynchronised 1970 clock.
    struct tm utc = {};
    gmtime_r(&now, &utc);
    char formatted[21] = {};
    if (strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%SZ", &utc) == 20) {
      return String(formatted);
    }
  }
  // If NTP is unavailable, retain the SMS service-centre timestamp. The web UI
  // recognises this legacy/SCTS form and labels it honestly instead of showing
  // 1970 or treating it as a Unix integer.
  return fallbackScts ? String(fallbackScts) : String();
}

bool handleDecodedPdu() {
  int* concatInfo = pdu.getConcatInfo();
  int refNumber = concatInfo ? concatInfo[0] : 0;
  int partNumber = concatInfo ? concatInfo[1] : 0;
  int totalParts = concatInfo ? concatInfo[2] : 0;
  logCaptureF("短信PDU已解析：发送者=%s，字符数=%u，分段=%d/%d\n",
              pdu.getSender(), static_cast<unsigned>(String(pdu.getText()).length()),
              partNumber, totalParts);

  if (totalParts > 1 && partNumber > 0) {
    int slot = findOrCreateConcatSlot(refNumber, pdu.getSender(), totalParts);
    int partIndex = partNumber - 1;
    if (partIndex >= 0 && partIndex < MAX_CONCAT_PARTS &&
        !concatBuffer[slot].parts[partIndex].valid) {
      concatBuffer[slot].parts[partIndex].valid = true;
      concatBuffer[slot].parts[partIndex].text = String(pdu.getText());
      concatBuffer[slot].receivedParts++;
      if (concatBuffer[slot].receivedParts == 1) {
        concatBuffer[slot].timestamp = currentSmsArrivalTimestamp(pdu.getTimeStamp());
      }
    }
    // Messages beyond the supported part count intentionally wait for timeout,
    // which records them as incomplete without indexing outside the fixed array.
    if (totalParts <= MAX_CONCAT_PARTS &&
        concatBuffer[slot].receivedParts >= totalParts) {
      String fullText = assembleConcatSms(slot);
      bool handled = processSmsContent(concatBuffer[slot].sender.c_str(), fullText.c_str(),
                                       concatBuffer[slot].timestamp.c_str());
      if (handled) {
        clearConcatSlot(slot);
      } else {
        concatBuffer[slot].firstPartTime = millis();
      }
      return handled;
    }
    return true;
  } else {
    String receivedAt = currentSmsArrivalTimestamp(pdu.getTimeStamp());
    return processSmsContent(pdu.getSender(), pdu.getText(), receivedAt.c_str());
  }
}

bool storedSmsLineEquals(const char* expected) {
  size_t start = 0;
  size_t expectedLength = strlen(expected);
  while (start < storedSmsResponseLength) {
    size_t end = start;
    while (end < storedSmsResponseLength && storedSmsResponse[end] != '\n') ++end;
    while (start < end &&
           (storedSmsResponse[start] == '\r' || storedSmsResponse[start] == ' ')) ++start;
    while (end > start &&
           (storedSmsResponse[end - 1] == '\r' || storedSmsResponse[end - 1] == ' ')) --end;
    if (end - start == expectedLength &&
        memcmp(storedSmsResponse + start, expected, expectedLength) == 0) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

bool storedSmsResponseHas(const char* value) {
  return strstr(storedSmsResponse, value) != nullptr;
}

bool storedSmsResponseError() {
  return storedSmsLineEquals("ERROR") || storedSmsResponseHas("+CME ERROR:") ||
         storedSmsResponseHas("+CMS ERROR:");
}

bool storedSmsIndexMissingError() {
  const char* marker = strstr(storedSmsResponse, "+CMS ERROR:");
  if (!marker) marker = strstr(storedSmsResponse, "+CME ERROR:");
  if (!marker) return false;
  marker = strchr(marker, ':');
  if (!marker) return false;
  while (*(++marker) == ' ') {}
  return atoi(marker) == 321;
}

void resetStoredSmsResponse() {
  storedSmsResponseLength = 0;
  storedSmsResponse[0] = '\0';
}

void releaseStoredSmsWire() {
  if (storedSmsOwnsExclusive) modemReleaseExclusive();
  storedSmsOwnsExclusive = false;
  storedSmsPduLinePending = false;
  directCmtAfterCmgrHeader = false;
  storedSmsStage = STORED_SMS_IDLE;
  storedSmsActiveSlot = -1;
  resetStoredSmsResponse();
}

void clearStoredSmsEntry(uint8_t slot) {
  storedSmsQueue[slot].used = false;
  storedSmsQueue[slot].memory[0] = '\0';
  storedSmsQueue[slot].memory[1] = '\0';
  storedSmsQueue[slot].memory[2] = '\0';
  storedSmsQueue[slot].index = 0;
  storedSmsQueue[slot].processed = false;
  storedSmsQueue[slot].verifyBeforeDelete = false;
  storedSmsQueue[slot].pduLength = 0;
  storedSmsQueue[slot].pduFingerprint = 0;
  storedSmsQueue[slot].failures = 0;
  storedSmsQueue[slot].nextAttemptAt = 0;
  storedSmsQueue[slot].fromBackfill = false;
}

unsigned long storedSmsRetryDelay(uint8_t failures) {
  uint8_t shift = failures > 6 ? 6 : failures;
  unsigned long delayMs = 1000UL << shift;
  return delayMs > STORED_SMS_MAX_RETRY_MS ? STORED_SMS_MAX_RETRY_MS : delayMs;
}

void markStoredSmsRetry(const char* reason) {
  int8_t slot = storedSmsActiveSlot;
  if (slot >= 0 && slot < STORED_SMS_QUEUE_CAPACITY && storedSmsQueue[slot].used) {
    StoredSmsEntry& entry = storedSmsQueue[slot];
    if (storedSmsStage == STORED_SMS_CMGD && entry.processed) {
      // CMGD may have succeeded even if its terminal response was lost. Read
      // and fingerprint the slot before any retry, so a newly reused index can
      // never be deleted as though it were the already processed message.
      entry.verifyBeforeDelete = true;
    }
    if (entry.failures < 255) ++entry.failures;
    entry.nextAttemptAt = millis() + storedSmsRetryDelay(entry.failures);
    logCaptureF("⚠️ 已存短信%s失败（%s/%u），稍后重试；原短信未删除：%s\n",
                reason, entry.memory, static_cast<unsigned>(entry.index),
                entry.processed ? "将先核对内容再重试删除，不会重复入库" : "等待重新读取");
  }
}

void scheduleStoredSmsRetry(const char* reason) {
  markStoredSmsRetry(reason);
  releaseStoredSmsWire();
}

void beginStoredSmsAbort(const char* reason) {
  markStoredSmsRetry(reason);
  resetStoredSmsResponse();
  storedSmsStage = STORED_SMS_ABORTING;
  storedSmsCommandStartedAt = millis();
  // A unique read-only response proves that the timed-out command has reached
  // a terminal boundary. Keep exclusive ownership until that proof arrives.
  Serial1.println("AT+CMEE?");
}

bool storedSmsAbortSynchronized() {
  bool sawProbe = false;
  size_t start = 0;
  while (start < storedSmsResponseLength) {
    size_t end = start;
    while (end < storedSmsResponseLength && storedSmsResponse[end] != '\n') ++end;
    while (start < end &&
           (storedSmsResponse[start] == '\r' || storedSmsResponse[start] == ' ')) ++start;
    while (end > start &&
           (storedSmsResponse[end - 1] == '\r' || storedSmsResponse[end - 1] == ' ')) --end;
    String line = String(storedSmsResponse).substring(start, end);
    if (line.startsWith("+CMEE:")) {
      sawProbe = true;
    } else if (sawProbe && (line == "OK" || line == "ERROR")) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

void sendStoredSmsCommand(const String& command, StoredSmsStage stage) {
  resetStoredSmsResponse();
  storedSmsPduLinePending = false;
  directCmtAfterCmgrHeader = false;
  storedSmsStage = stage;
  storedSmsCommandStartedAt = millis();
  Serial1.println(command);
}

int8_t nextStoredSmsSlot() {
  unsigned long now = millis();
  // Unprocessed messages take priority over deletion retries, so one stubborn
  // CMGD error cannot prevent later verification codes from being ingested.
  for (uint8_t pass = 0; pass < 2; ++pass) {
    for (uint8_t offset = 0; offset < STORED_SMS_QUEUE_CAPACITY; ++offset) {
      uint8_t slot = (storedSmsCursor + offset) % STORED_SMS_QUEUE_CAPACITY;
      StoredSmsEntry& entry = storedSmsQueue[slot];
      if (!entry.used || !deadlineReached(now, entry.nextAttemptAt)) continue;
      if ((pass == 0 && entry.processed) || (pass == 1 && !entry.processed)) continue;
      storedSmsCursor = (slot + 1) % STORED_SMS_QUEUE_CAPACITY;
      return static_cast<int8_t>(slot);
    }
  }
  return -1;
}

bool extractCmgrPdu(String& pduLine) {
  String response(storedSmsResponse);
  bool foundHeader = false;
  int start = 0;
  while (start <= response.length()) {
    int end = response.indexOf('\n', start);
    if (end < 0) end = response.length();
    String line = response.substring(start, end);
    line.trim();
    if (!foundHeader) {
      if (line.startsWith("+CMGR:")) foundHeader = true;
    } else if (line.length()) {
      // The PDU is the first non-empty line after +CMGR. Refuse to scan past
      // another URC, because doing so could decode an interleaved +CMT payload
      // as the stored message.
      if ((line.length() & 1U) != 0 || !isHexString(line)) return false;
      pduLine = line;
      return true;
    }
    if (end >= response.length()) break;
    start = end + 1;
  }
  return false;
}

uint32_t storedPduFingerprint(const String& value) {
  uint32_t hash = 2166136261UL;
  for (unsigned int i = 0; i < value.length(); ++i) {
    char c = value.charAt(i);
    if (c >= 'a' && c <= 'f') c -= ('a' - 'A');
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619UL;
  }
  return hash;
}

void startStoredSmsAttempt(int8_t slot) {
  if (slot < 0 || slot >= STORED_SMS_QUEUE_CAPACITY ||
      !storedSmsQueue[slot].used) return;
  if (!simManagerIsReady() || !simManagerSmsReady() || modemIsBooting() ||
      esimIsBusy() || operatorManagerIsBusy() || simManagerIsBusy() ||
      smsReceiverAwaitingPdu()) return;
  if (!modemAcquireExclusive()) return;
  storedSmsOwnsExclusive = true;

  // Drain complete URCs before issuing CPMS. A +CMT header without its PDU
  // must finish first, otherwise a CMGR PDU could be mistaken for that message.
  checkSerial1URC();
  if (smsReceiverAwaitingPdu() || !storedSmsQueue[slot].used) {
    releaseStoredSmsWire();
    return;
  }

  storedSmsActiveSlot = slot;
  String command = "AT+CPMS=\"";
  command += storedSmsQueue[slot].memory;
  command += "\"";
  sendStoredSmsCommand(command, STORED_SMS_CPMS);
}

void finishStoredSmsRead() {
  int8_t slot = storedSmsActiveSlot;
  if (slot < 0 || slot >= STORED_SMS_QUEUE_CAPACITY ||
      !storedSmsQueue[slot].used) {
    releaseStoredSmsWire();
    return;
  }

  String pduLine;
  if (!extractCmgrPdu(pduLine)) {
    scheduleStoredSmsRetry("解析");
    return;
  }

  StoredSmsEntry& current = storedSmsQueue[slot];
  uint16_t pduLength = static_cast<uint16_t>(pduLine.length());
  uint32_t fingerprint = storedPduFingerprint(pduLine);
  if (current.processed && current.verifyBeforeDelete &&
      current.pduLength == pduLength && current.pduFingerprint == fingerprint) {
    current.verifyBeforeDelete = false;
    current.failures = 0;
    String command = "AT+CMGD=";
    command += String(current.index);
    sendStoredSmsCommand(command, STORED_SMS_CMGD);
    return;
  }
  if (current.processed && current.verifyBeforeDelete) {
    // The previous CMGD succeeded but its response was lost, and the modem has
    // since reused this index for a different SMS. Process the new PDU normally.
    current.processed = false;
    current.verifyBeforeDelete = false;
  }
  if (!pdu.decodePDU(pduLine.c_str())) {
    scheduleStoredSmsRetry("解析");
    return;
  }

  char memory0 = storedSmsQueue[slot].memory[0];
  char memory1 = storedSmsQueue[slot].memory[1];
  uint16_t index = storedSmsQueue[slot].index;
  // CMGR has completed, so release the AT lock before normal SMS processing.
  // This preserves administrator SMS commands that may themselves send SMS.
  releaseStoredSmsWire();
  bool handled = handleDecodedPdu();

  StoredSmsEntry& entry = storedSmsQueue[slot];
  if (!entry.used || entry.memory[0] != memory0 || entry.memory[1] != memory1 ||
      entry.index != index) {
    return;  // SIM state changed while notification delivery was in progress.
  }
  if (!handled) {
    if (entry.failures < 255) ++entry.failures;
    entry.nextAttemptAt = millis() + storedSmsRetryDelay(entry.failures);
    return;
  }
  entry.processed = true;
  entry.verifyBeforeDelete = false;
  entry.pduLength = pduLength;
  entry.pduFingerprint = fingerprint;
  entry.failures = 0;
  entry.nextAttemptAt = millis();
}

void handleStoredSmsTerminal() {
  int8_t slot = storedSmsActiveSlot;
  if (slot < 0 || slot >= STORED_SMS_QUEUE_CAPACITY ||
      !storedSmsQueue[slot].used) {
    releaseStoredSmsWire();
    return;
  }

  if (storedSmsStage == STORED_SMS_CPMS) {
    bool readFirst = !storedSmsQueue[slot].processed ||
                     storedSmsQueue[slot].verifyBeforeDelete;
    String command = readFirst ? "AT+CMGR=" : "AT+CMGD=";
    command += String(storedSmsQueue[slot].index);
    sendStoredSmsCommand(command, readFirst ? STORED_SMS_CMGR : STORED_SMS_CMGD);
    return;
  }
  if (storedSmsStage == STORED_SMS_CMGR) {
    finishStoredSmsRead();
    return;
  }
  if (storedSmsStage == STORED_SMS_CMGD) {
    char memory[3] = {storedSmsQueue[slot].memory[0], storedSmsQueue[slot].memory[1], '\0'};
    uint16_t index = storedSmsQueue[slot].index;
    clearStoredSmsEntry(slot);
    releaseStoredSmsWire();
    logCaptureF("模组已存短信已处理并删除：%s/%u\n", memory,
                static_cast<unsigned>(index));
  }
}

void drainStoredSmsWire() {
  while (Serial1.available()) {
    char value = static_cast<char>(Serial1.read());
    dispatchSerial1Byte(value, false);
    if (storedSmsResponseLength + 1 < STORED_SMS_RESPONSE_CAPACITY) {
      storedSmsResponse[storedSmsResponseLength++] = value;
      storedSmsResponse[storedSmsResponseLength] = '\0';
    } else if (storedSmsStage == STORED_SMS_ABORTING) {
      // Keep the tail while waiting for the unique +CMEE probe response.
      constexpr size_t keep = STORED_SMS_RESPONSE_CAPACITY / 2;
      memmove(storedSmsResponse, storedSmsResponse + storedSmsResponseLength - keep, keep);
      storedSmsResponseLength = keep;
      storedSmsResponse[storedSmsResponseLength++] = value;
      storedSmsResponse[storedSmsResponseLength] = '\0';
    } else {
      beginStoredSmsAbort("响应过长");
      return;
    }
  }

  if (storedSmsStage == STORED_SMS_ABORTING) {
    if (storedSmsAbortSynchronized()) {
      logCaptureLn(String("已存短信 AT 通道已重新同步"));
      releaseStoredSmsWire();
      return;
    }
    if (millis() - storedSmsCommandStartedAt >= STORED_SMS_ABORT_TIMEOUT_MS) {
      logCaptureLn(String("⚠️ 已存短信 AT 通道无法重新同步，正在恢复模组"));
      // This is a transport recovery of the same card, not a confirmed SIM
      // replacement. Keep queued indexes/fingerprints across both invalidate
      // calls inside resetModule(); the next real SIM observation may still
      // clear them through the normal smsResetForSimChange() path.
      preserveStoredSmsQueueForTransportReset = true;
      resetModule();
      preserveStoredSmsQueueForTransportReset = false;
    }
    return;
  }

  if (storedSmsResponseError()) {
    if (storedSmsStage == STORED_SMS_CMGR && storedSmsActiveSlot >= 0 &&
        storedSmsActiveSlot < STORED_SMS_QUEUE_CAPACITY) {
      StoredSmsEntry& entry = storedSmsQueue[storedSmsActiveSlot];
      if (entry.used && entry.fromBackfill && !entry.processed) {
        // A range scan intentionally probes empty SIM indexes. Any failed read
        // is skipped and will be checked again after the next boot/PLMN restore.
        clearStoredSmsEntry(storedSmsActiveSlot);
        releaseStoredSmsWire();
        return;
      }
      if (entry.used && storedSmsIndexMissingError()) {
        char memory[3] = {entry.memory[0], entry.memory[1], '\0'};
        uint16_t index = entry.index;
        clearStoredSmsEntry(storedSmsActiveSlot);
        releaseStoredSmsWire();
        logCaptureF("模组短信索引已不在存储区：%s/%u\n", memory,
                    static_cast<unsigned>(index));
        return;
      }
    }
    scheduleStoredSmsRetry(storedSmsStage == STORED_SMS_CMGD ? "删除" : "读取");
    return;
  }
  if (storedSmsLineEquals("OK")) {
    handleStoredSmsTerminal();
    return;
  }
  if (millis() - storedSmsCommandStartedAt >= STORED_SMS_COMMAND_TIMEOUT_MS) {
    beginStoredSmsAbort(storedSmsStage == STORED_SMS_CMGD ? "删除超时" : "读取超时");
  }
}

void processSerial1Line(String line, bool debugLog) {
  line.trim();
  if (!line.length()) return;
  observeRegistrationLine(line);
  if (debugLog && !line.startsWith("+CMT:")) {
    if (line.startsWith("+ICCID:")) {
      logCaptureLn(String("Modem> +ICCID: [已脱敏]"));
    } else {
      logCaptureLn(String("Modem> " + line));
    }
  }

  if (line.startsWith("+CMTI:")) {
    observeCmtiLine(line);
    if (smsRxState == SMS_RX_IDLE) return;
  }

  if (storedSmsStage == STORED_SMS_CMGR && line.startsWith("+CMGR:")) {
    storedSmsPduLinePending = true;
    directCmtAfterCmgrHeader = false;
  }
  if (storedSmsPduLinePending && line.startsWith("+CMT:")) {
    // The direct-delivery URC interrupted CMGR after its header. Its PDU comes
    // first; the stored CMGR PDU remains pending after the direct PDU.
    directCmtAfterCmgrHeader = true;
  }

  if (smsRxState == SMS_RX_IDLE) {
    if (line.startsWith("+CMT:")) {
      smsRxState = SMS_RX_WAIT_PDU;
      smsRxStartedAt = millis();
      logCaptureLn(String("检测到短信上报，等待PDU"));
    } else if (storedSmsPduLinePending && isHexString(line)) {
      storedSmsPduLinePending = false;
    }
    return;
  }

  if (isHexString(line)) {
    if (storedSmsPduLinePending && !directCmtAfterCmgrHeader) {
      // +CMT began before the CMGR response. Keep waiting for that direct PDU
      // and leave this hex line exclusively to the CMGR response parser.
      storedSmsPduLinePending = false;
      return;
    }
    if (!pdu.decodePDU(line.c_str())) {
      logCaptureLn(String("⚠️ 短信PDU解析失败"));
    } else {
      handleDecodedPdu();
    }
    directCmtAfterCmgrHeader = false;
  } else if (line.startsWith("+CMT:")) {
    // A second header means the first PDU was lost; keep waiting for the newest.
    logCaptureLn(String("⚠️ 短信PDU缺失，已同步到下一条上报"));
    smsRxStartedAt = millis();
    return;
  } else if (isInterleavedControlLine(line)) {
    // AT responses and other URCs may be interleaved between +CMT and its PDU.
    // Keep the receive state until the bounded timeout instead of dropping SMS.
    return;
  } else {
    logCaptureLn(String("⚠️ 短信上报后未收到有效PDU"));
  }
  smsRxState = SMS_RX_IDLE;
  smsRxStartedAt = 0;
}

}  // namespace

void dispatchSerial1Byte(char value, bool debugLog) {
  if (value == '\n') {
    String line = serial1LineBuffer;
    serial1LineBuffer = "";
    processSerial1Line(line, debugLog);
    return;
  }
  if (value == '\r') return;
  if (serial1LineBuffer.length() < SERIAL_BUFFER_SIZE - 1) {
    serial1LineBuffer += value;
  } else {
    serial1LineBuffer = "";
    smsRxState = SMS_RX_IDLE;
    logCaptureLn(String("⚠️ 模组串口行过长，已丢弃"));
  }
}

// All Serial1 consumers feed the same line dispatcher, so +CMT URCs are not
// discarded when they arrive during an AT command.
void checkSerial1URC() {
  while (Serial1.available()) {
    dispatchSerial1Byte(static_cast<char>(Serial1.read()), true);
  }
}

void checkSmsReceiveTimeout() {
  if (smsRxState != SMS_RX_WAIT_PDU || !smsRxStartedAt) return;
  if (millis() - smsRxStartedAt < SMS_PDU_WAIT_TIMEOUT_MS) return;
  smsRxState = SMS_RX_IDLE;
  smsRxStartedAt = 0;
  logCaptureLn(String("⚠️ 短信PDU等待超时，接收状态已复位"));
}

bool smsReceiverAwaitingPdu() {
  checkSmsReceiveTimeout();
  return smsRxState == SMS_RX_WAIT_PDU;
}

void smsStoredMessageLoop() {
  if (storedSmsStage != STORED_SMS_IDLE) {
    drainStoredSmsWire();
    return;
  }
  int8_t slot = nextStoredSmsSlot();
  if (slot < 0 && storedSmsBackfillActive) {
    if (storedSmsBackfillNextIndex <= storedSmsBackfillLastIndex) {
      enqueueStoredSms(String(storedSmsBackfillMemory), storedSmsBackfillNextIndex++, true);
      slot = nextStoredSmsSlot();
    } else {
      storedSmsBackfillActive = false;
      logCaptureLn(String("模组积压短信扫描完成"));
    }
  }
  if (slot >= 0) startStoredSmsAttempt(slot);
}

bool smsStoredMessageIsBusy() {
  return storedSmsStage != STORED_SMS_IDLE || storedSmsOwnsExclusive;
}

void smsScanStoredMessages(const char* memory, uint16_t lastIndex) {
  String requested = memory ? String(memory) : String();
  if (!supportedSmsMemory(requested) || lastIndex == 0) return;
  storedSmsBackfillMemory[0] = requested.charAt(0);
  storedSmsBackfillMemory[1] = requested.charAt(1);
  storedSmsBackfillMemory[2] = '\0';
  storedSmsBackfillNextIndex = 1;
  storedSmsBackfillLastIndex = lastIndex;
  storedSmsBackfillActive = true;
  logCaptureF("开始扫描模组积压短信：%s/1-%u\n", storedSmsBackfillMemory,
              static_cast<unsigned>(lastIndex));
}

void smsResetForSimChange() {
  bool clearStoredQueue = !preserveStoredSmsQueueForTransportReset;
  if (storedSmsOwnsExclusive) modemReleaseExclusive();
  storedSmsOwnsExclusive = false;
  storedSmsPduLinePending = false;
  directCmtAfterCmgrHeader = false;
  storedSmsStage = STORED_SMS_IDLE;
  storedSmsActiveSlot = -1;
  storedSmsCursor = 0;
  resetStoredSmsResponse();
  if (clearStoredQueue) {
    for (uint8_t i = 0; i < STORED_SMS_QUEUE_CAPACITY; ++i) {
      clearStoredSmsEntry(i);
    }
    storedSmsBackfillActive = false;
    storedSmsBackfillNextIndex = 1;
    storedSmsBackfillLastIndex = 0;
  }
  smsRxState = SMS_RX_IDLE;
  smsRxStartedAt = 0;
  serial1LineBuffer = "";
  initConcatBuffer();
}
