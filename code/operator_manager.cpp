#include "operator_manager.h"

#include "esim_manager.h"
#include "globals.h"
#include "modem.h"
#include "sim_manager.h"
#include "sms_process.h"
#include "web_handlers.h"

namespace {

constexpr uint8_t MAX_NETWORKS = 20;
constexpr unsigned long SCAN_TIMEOUT_MS = 180000UL;
constexpr unsigned long SELECT_TIMEOUT_MS = 120000UL;
constexpr unsigned long ESIM_AUTO_TIMEOUT_MS = 180000UL;
constexpr unsigned long QUERY_TIMEOUT_MS = 10000UL;
constexpr unsigned long SCAN_CACHE_MS = 15UL * 60UL * 1000UL;
constexpr size_t MAX_RESPONSE_BYTES = 8192;

enum TaskType { TASK_NONE, TASK_QUERY, TASK_SCAN, TASK_SELECT, TASK_AUTO };
enum WireStage {
  STAGE_NONE,
  STAGE_PRIMARY,
  STAGE_REG_WAIT,
  STAGE_CEREG,
  STAGE_AUTO_RECOVERY,
  STAGE_AUTO_RECOVERY_WAIT,
  STAGE_AUTO_RECOVERY_CEREG,
  STAGE_AUTO_RECOVERY_FORMAT,
  STAGE_AUTO_RECOVERY_QUERY,
  STAGE_FORMAT,
  STAGE_QUERY,
  STAGE_ABORTING,
  STAGE_RECOVER_POWER_OFF,
  STAGE_RECOVER_BOOT
};

struct OperatorNetwork {
  uint8_t status = 0;
  String longName;
  String shortName;
  String numeric;
  int act = -1;
};

struct CurrentOperator {
  int mode = -1;
  int format = -1;
  String numeric;
  String name;
  int act = -1;
  bool known = false;
};

struct OperatorJob {
  uint32_t id = 0;
  bool active = false;
  bool done = false;
  bool ok = false;
  bool warning = false;
  TaskType type = TASK_NONE;
  WireStage stage = STAGE_NONE;
  String phase = "idle";
  String message;
  String error;
  String targetNumeric;
  int targetAct = -1;
  bool fromEsim = false;
  bool recoveryRebooted = false;
  unsigned long startedAt = 0;
  unsigned long commandStartedAt = 0;
  unsigned long phaseAt = 0;
  unsigned long recoveryStartedAt = 0;
};

OperatorNetwork networks[MAX_NETWORKS];
uint8_t networkCount = 0;
unsigned long networksUpdatedAt = 0;
CurrentOperator currentOperator;
OperatorJob job;
String responseBuffer;
bool initialQueryPending = false;
bool invalidatePending = false;
String recoveryMessage;
String recoveryError;

void clearOperatorCache() {
  networkCount = 0;
  networksUpdatedAt = 0;
  currentOperator = CurrentOperator();
  initialQueryPending = true;
  invalidatePending = false;
}

String jsonEscapeOperator(const String& input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    unsigned char c = input[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char encoded[7];
          snprintf(encoded, sizeof(encoded), "\\u%04x", c);
          out += encoded;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

const char* taskName(TaskType type) {
  switch (type) {
    case TASK_QUERY: return "query";
    case TASK_SCAN: return "scan";
    case TASK_SELECT: return "select";
    case TASK_AUTO: return "auto";
    default: return "idle";
  }
}

const char* actName(int act) {
  switch (act) {
    case 0: return "GSM";
    case 1: return "不支持";
    case 2: return "UTRAN";
    case 3: return "EDGE";
    case 4: return "HSDPA";
    case 5: return "HSUPA";
    case 6: return "HSPA";
    case 7: return "LTE";
    case 8: return "HSPA+";
    case 9: return "不支持";
    case 10: return "LTE/5GCN";
    case 11: return "不支持";
    case 12: return "NG-RAN";
    case 13: return "不支持";
    default: return "未知";
  }
}

String carrierName(const String& numeric) {
  if (!numeric.startsWith("460") || numeric.length() < 5) return "";
  String mnc = numeric.substring(3);
  if (mnc == "00" || mnc == "02" || mnc == "04" || mnc == "07" ||
      mnc == "08" || mnc == "13" || mnc == "20") return "中国移动";
  if (mnc == "01" || mnc == "06" || mnc == "09" || mnc == "10") return "中国联通";
  if (mnc == "03" || mnc == "05" || mnc == "11" || mnc == "12") return "中国电信";
  return "中国运营商";
}

bool numericPlmn(const String& value) {
  if (value.length() != 5 && value.length() != 6) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isDigit(value[i])) return false;
  }
  return true;
}

String unquote(String value) {
  value.trim();
  if (value.length() >= 2 && value[0] == '"' && value[value.length() - 1] == '"') {
    value = value.substring(1, value.length() - 1);
  }
  return value;
}

uint8_t splitFields(const String& input, String* fields, uint8_t capacity) {
  bool quoted = false;
  size_t start = 0;
  uint8_t count = 0;
  for (size_t i = 0; i <= input.length() && count < capacity; ++i) {
    char c = i < input.length() ? input[i] : ',';
    if (c == '"') quoted = !quoted;
    if (c == ',' && !quoted) {
      fields[count] = input.substring(start, i);
      fields[count].trim();
      ++count;
      start = i + 1;
    }
  }
  return count;
}

bool sameNetwork(const OperatorNetwork& network, const String& numeric, int act) {
  return network.numeric == numeric && network.act == act;
}

void updateCurrentFromScan() {
  for (uint8_t i = 0; i < networkCount; ++i) {
    if (networks[i].status != 2) continue;
    currentOperator.numeric = networks[i].numeric;
    currentOperator.name = networks[i].longName.length() ? networks[i].longName : networks[i].shortName;
    currentOperator.act = networks[i].act;
    currentOperator.known = true;
    return;
  }
}

bool parseScanResponse(const String& response, String& error) {
  int marker = response.indexOf("+COPS:");
  if (marker < 0) {
    error = "模组没有返回运营商列表";
    return false;
  }

  networkCount = 0;
  size_t pos = marker + 6;
  while (pos < response.length() && networkCount < MAX_NETWORKS) {
    int open = response.indexOf('(', pos);
    if (open < 0) break;
    bool quoted = false;
    int close = -1;
    for (size_t i = open + 1; i < response.length(); ++i) {
      char c = response[i];
      if (c == '"') quoted = !quoted;
      if (c == ')' && !quoted) {
        close = static_cast<int>(i);
        break;
      }
    }
    if (close < 0) break;
    String tuple = response.substring(open + 1, close);
    String fields[5];
    uint8_t count = splitFields(tuple, fields, 5);
    if (count >= 4) {
      String statusText = unquote(fields[0]);
      String numeric = unquote(fields[3]);
      int status = statusText.toInt();
      int act = count >= 5 ? unquote(fields[4]).toInt() : -1;
      if (status >= 0 && status <= 3 && numericPlmn(numeric)) {
        bool duplicate = false;
        for (uint8_t i = 0; i < networkCount; ++i) {
          if (sameNetwork(networks[i], numeric, act)) {
            duplicate = true;
            if (status == 2 || networks[i].status == 0) networks[i].status = status;
            break;
          }
        }
        if (!duplicate) {
          OperatorNetwork& network = networks[networkCount++];
          network.status = static_cast<uint8_t>(status);
          network.longName = unquote(fields[1]);
          network.shortName = unquote(fields[2]);
          network.numeric = numeric;
          network.act = act;
        }
      }
    }
    pos = close + 1;
  }

  if (!networkCount) {
    error = "扫描完成，但没有解析到可用网络";
    return false;
  }
  networksUpdatedAt = millis();
  updateCurrentFromScan();
  return true;
}

bool parseCurrentResponse(const String& response) {
  int marker = response.indexOf("+COPS:");
  if (marker < 0) return false;
  int lineEnd = response.indexOf('\n', marker);
  if (lineEnd < 0) lineEnd = response.length();
  String values = response.substring(marker + 6, lineEnd);
  values.trim();
  String fields[4];
  uint8_t count = splitFields(values, fields, 4);
  if (!count) return false;
  CurrentOperator next;
  next.mode = unquote(fields[0]).toInt();
  next.format = count > 1 ? unquote(fields[1]).toInt() : -1;
  String oper = count > 2 ? unquote(fields[2]) : "";
  next.act = count > 3 ? unquote(fields[3]).toInt() : -1;
  if (numericPlmn(oper)) {
    next.numeric = oper;
    next.name = carrierName(oper);
  } else {
    next.name = oper;
  }
  next.known = oper.length() > 0 && next.mode != 2;
  currentOperator = next;

  bool markedCurrent = false;
  for (uint8_t i = 0; i < networkCount; ++i) {
    if (networks[i].status == 2) networks[i].status = 1;
  }
  for (uint8_t i = 0; i < networkCount; ++i) {
    if (markedCurrent || networks[i].numeric != currentOperator.numeric) continue;
    if (currentOperator.act >= 0 && networks[i].act != currentOperator.act) continue;
    networks[i].status = 2;
    markedCurrent = true;
  }
  return true;
}

int parseCeregStatus(const String& response) {
  int queryStatus = -1;
  int urcStatus = -1;
  int searchFrom = 0;
  while (searchFrom < response.length()) {
    int marker = response.indexOf("+CEREG:", searchFrom);
    if (marker < 0) break;
    int lineEnd = response.indexOf('\n', marker);
    if (lineEnd < 0) lineEnd = response.length();
    String fields = response.substring(marker + 7, lineEnd);
    fields.trim();
    int comma = fields.indexOf(',');
    if (comma < 0) {
      int status = fields.toInt();
      if (status >= 0 && status <= 5) urcStatus = status;
    } else {
      String first = fields.substring(0, comma);
      String remainder = fields.substring(comma + 1);
      first.trim();
      remainder.trim();
      if (remainder.startsWith("\"")) {
        int status = first.toInt();
        if (status >= 0 && status <= 5) urcStatus = status;
      } else {
        int status = remainder.toInt();
        if (status >= 0 && status <= 5) queryStatus = status;
      }
    }
    searchFrom = lineEnd + 1;
  }
  return queryStatus >= 0 ? queryStatus : urcStatus;
}

bool terminalLine(const String& response, const char* expected) {
  int start = 0;
  while (start < response.length()) {
    int end = response.indexOf('\n', start);
    if (end < 0) end = response.length();
    String line = response.substring(start, end);
    line.trim();
    if (line == expected) return true;
    start = end + 1;
  }
  return false;
}

bool responseError(const String& response) {
  return terminalLine(response, "ERROR") || response.indexOf("+CME ERROR:") >= 0 ||
         response.indexOf("+CMS ERROR:") >= 0;
}

void sendWireCommand(const String& command, WireStage stage, const String& phase,
                     const String& message) {
  responseBuffer = "";
  responseBuffer.reserve(stage == STAGE_PRIMARY && job.type == TASK_SCAN ? 4096 : 256);
  job.stage = stage;
  job.phase = phase;
  job.message = message;
  job.commandStartedAt = millis();
  job.phaseAt = millis();
  Serial1.println(command);
}

void beginAutoRecovery(const String& message, const String& error) {
  recoveryMessage = message;
  recoveryError = error;
  job.recoveryStartedAt = millis();
  sendWireCommand("AT+COPS=0", STAGE_AUTO_RECOVERY, "recovering",
                  "目标网络不可用，正在恢复自动选网");
}

void releaseExclusive() {
  modemReleaseExclusive();
}

void finishJob(bool ok, const String& message, const String& error = "", bool warning = false) {
  const bool restoreSms = job.type == TASK_SCAN || job.type == TASK_SELECT || job.type == TASK_AUTO;
  job.active = false;
  job.done = true;
  job.ok = ok;
  job.warning = warning;
  job.phase = ok ? "complete" : "failed";
  job.message = message;
  job.error = error;
  job.stage = STAGE_NONE;
  releaseExclusive();
  if (invalidatePending) clearOperatorCache();
  if (restoreSms) simManagerRestoreSmsConfiguration();
  if (job.type != TASK_QUERY) {
    logCaptureLn(String("运营商任务") + (ok ? "完成：" : "失败：") + (error.length() ? error : message));
  }
}

void beginRecovery(const String& message, const String& error) {
  recoveryMessage = message;
  recoveryError = error;
  responseBuffer = "";
  responseBuffer.reserve(256);
  job.stage = STAGE_ABORTING;
  job.phase = "recovering";
  job.message = "正在中止未完成的模组命令";
  job.commandStartedAt = millis();
  job.phaseAt = millis();
  // V.250-style best-effort abort. Do not enqueue a new AT command until a
  // terminal response arrives or the modem has been power-cycled.
  Serial1.write('\r');
}

bool beginTask(TaskType type, String& message, bool fromEsim = false) {
  if (!simManagerIsReady() && !fromEsim) {
    message = "SIM 尚未就绪";
    return false;
  }
  if (job.active) {
    message = "已有运营商任务正在执行";
    return false;
  }
  if (esimIsBusy() && !fromEsim) {
    message = "eSIM 正在切换，请稍后操作运营商";
    return false;
  }
  if (!modemAcquireExclusive()) {
    message = "模组正在处理其他通信";
    return false;
  }
  checkSerial1URC();
  job = OperatorJob();
  job.id = esp_random();
  job.active = true;
  job.type = type;
  job.fromEsim = fromEsim;
  job.startedAt = millis();
  message = "任务已创建";
  return true;
}

bool startQuery(bool initial) {
  String ignored;
  if (!beginTask(TASK_QUERY, ignored)) return false;
  initialQueryPending = false;
  sendWireCommand("AT+COPS=3,2", STAGE_FORMAT, "querying", initial ? "正在读取当前运营商" : "正在核对运营商");
  return true;
}

unsigned long taskTimeout() {
  if (job.type == TASK_SCAN) return SCAN_TIMEOUT_MS;
  if (job.type == TASK_AUTO && job.fromEsim) return ESIM_AUTO_TIMEOUT_MS;
  if (job.type == TASK_SELECT || job.type == TASK_AUTO) return SELECT_TIMEOUT_MS;
  return QUERY_TIMEOUT_MS;
}

void handleSuccessfulResponse() {
  if (job.type == TASK_SCAN && job.stage == STAGE_PRIMARY) {
    String error;
    if (!parseScanResponse(responseBuffer, error)) {
      finishJob(false, "运营商扫描结果不可用", error);
      return;
    }
    finishJob(true, "扫描完成，发现 " + String(networkCount) + " 个网络");
    return;
  }

  if (job.stage == STAGE_AUTO_RECOVERY) {
    job.stage = STAGE_AUTO_RECOVERY_WAIT;
    job.phase = "recovering";
    job.message = "自动选网命令已接受，等待网络恢复";
    job.phaseAt = millis();
    return;
  }

  if (job.stage == STAGE_AUTO_RECOVERY_FORMAT) {
    sendWireCommand("AT+COPS?", STAGE_AUTO_RECOVERY_QUERY, "recovering",
                    "网络已恢复，正在确认当前运营商");
    return;
  }

  if (job.stage == STAGE_AUTO_RECOVERY_QUERY) {
    bool parsed = parseCurrentResponse(responseBuffer);
    String detail = recoveryError;
    if (parsed && currentOperator.known) {
      String actual = operatorCurrentLabel();
      if (actual.length()) detail += "；已自动恢复到 " + actual;
    } else {
      currentOperator.mode = 0;
      detail += "；已恢复自动选网，运营商信息稍后刷新";
    }
    finishJob(false, recoveryMessage, detail);
    return;
  }

  if (job.stage == STAGE_FORMAT) {
    sendWireCommand("AT+COPS?", STAGE_QUERY, "querying", "正在读取当前运营商");
    return;
  }

  if (job.stage == STAGE_QUERY) {
    bool parsed = parseCurrentResponse(responseBuffer);
    if (job.type == TASK_QUERY) {
      finishJob(parsed, parsed ? "当前运营商已更新" : "当前运营商暂不可读",
                parsed ? "" : "AT+COPS? 未返回有效状态", !parsed);
    } else if (!parsed || !currentOperator.known) {
      if (job.type == TASK_SELECT) {
        beginAutoRecovery("目标网络未能完成注册", "模组没有返回目标运营商");
      } else {
        finishJob(false, "无法确认当前驻留网络", "AT+COPS? 未返回有效状态");
      }
    } else if (job.type == TASK_SELECT && currentOperator.numeric != job.targetNumeric) {
      String actual = operatorCurrentLabel();
      beginAutoRecovery("目标网络未生效",
                        actual.length() ? "当前仍驻留在 " + actual : "当前驻留网络与目标 PLMN 不一致");
    } else if (job.type == TASK_AUTO && currentOperator.mode != 0) {
      finishJob(false, "自动选网模式未生效", "AT+COPS? 返回的模式不是自动选网");
    } else {
      finishJob(true, "选网命令已生效，当前网络已更新");
    }
    return;
  }

  if (job.stage == STAGE_CEREG) {
    int registration = parseCeregStatus(responseBuffer);
    if (registration == 1 || registration == 5) {
      modemReady = true;
      sendWireCommand("AT+COPS=3,2", STAGE_FORMAT, "verifying", "网络已注册，正在核对当前运营商");
      return;
    }
    modemReady = false;
    if (registration == 3) {
      beginAutoRecovery("目标网络拒绝注册", "当前 SIM 或 Profile 没有该运营商的注册权限");
      return;
    }
    job.stage = STAGE_REG_WAIT;
    job.phase = "registering";
    job.message = registration == 2 ? "正在搜索目标网络" : "等待蜂窝网络完成注册";
    job.phaseAt = millis();
    return;
  }

  if (job.stage == STAGE_AUTO_RECOVERY_CEREG) {
    int registration = parseCeregStatus(responseBuffer);
    if (registration == 1 || registration == 5) {
      modemReady = true;
      sendWireCommand("AT+COPS=3,2", STAGE_AUTO_RECOVERY_FORMAT, "recovering",
                      "网络已恢复，正在核对运营商");
      return;
    }
    modemReady = false;
    job.stage = STAGE_AUTO_RECOVERY_WAIT;
    job.phase = "recovering";
    job.message = registration == 2 ? "自动选网正在搜索可用网络" : "等待自动选网恢复注册";
    job.phaseAt = millis();
    return;
  }

  if ((job.type == TASK_SELECT || job.type == TASK_AUTO) && job.stage == STAGE_PRIMARY) {
    job.stage = STAGE_REG_WAIT;
    job.phase = "registering";
    job.message = "选网命令已接受，等待网络注册";
    job.phaseAt = millis();
  }
}

int jobProgress() {
  if (!job.active) return job.ok && job.done ? 100 : 0;
  unsigned long elapsed = millis() - job.startedAt;
  if (job.type == TASK_SCAN) return 5 + min(85UL, elapsed * 85UL / 120000UL);
  if (job.stage == STAGE_REG_WAIT || job.stage == STAGE_CEREG) return 72;
  if (job.stage == STAGE_AUTO_RECOVERY || job.stage == STAGE_AUTO_RECOVERY_WAIT ||
      job.stage == STAGE_AUTO_RECOVERY_CEREG || job.stage == STAGE_AUTO_RECOVERY_FORMAT ||
      job.stage == STAGE_AUTO_RECOVERY_QUERY) return 88;
  if (job.stage == STAGE_ABORTING || job.stage == STAGE_RECOVER_POWER_OFF ||
      job.stage == STAGE_RECOVER_BOOT) return 94;
  if (job.stage == STAGE_FORMAT || job.stage == STAGE_QUERY) return job.type == TASK_QUERY ? 55 : 88;
  return job.type == TASK_QUERY ? 20 : 24;
}

String networkJson(const OperatorNetwork& network) {
  String carrier = carrierName(network.numeric);
  String json = "{\"status\":" + String(network.status) + ",\"longName\":\"" +
                jsonEscapeOperator(network.longName) + "\",\"shortName\":\"" +
                jsonEscapeOperator(network.shortName) + "\",\"numeric\":\"" +
                network.numeric + "\",\"act\":" + String(network.act) +
                ",\"actName\":\"" + actName(network.act) + "\",\"carrier\":\"" +
                jsonEscapeOperator(carrier) + "\",\"selectable\":" +
                String(network.status == 1 ? "true" : "false") + "}";
  return json;
}

}  // namespace

void operatorManagerBegin() {
  initialQueryPending = true;
}

void operatorManagerLoop() {
  if (!job.active) {
    if (initialQueryPending && millis() > 2500 && simManagerIsReady() && modemReady &&
        !esimIsBusy() && !modemIsBusy()) {
      startQuery(true);
    }
    return;
  }

  if (job.stage == STAGE_ABORTING) {
    while (Serial1.available()) {
      char c = static_cast<char>(Serial1.read());
      dispatchSerial1Byte(c, false);
      responseBuffer += c;
      if (responseBuffer.length() > 512) {
        responseBuffer.remove(0, responseBuffer.length() - 512);
      }
    }
    if (terminalLine(responseBuffer, "OK") || responseError(responseBuffer)) {
      bool refreshOperator = job.type == TASK_SELECT || job.type == TASK_AUTO;
      finishJob(false, recoveryMessage, recoveryError);
      if (refreshOperator) operatorManagerInvalidate();
      return;
    }
    if (millis() - job.phaseAt >= 10000) {
      modemReady = false;
      pinMode(MODEM_EN_PIN, OUTPUT);
      digitalWrite(MODEM_EN_PIN, LOW);
      job.stage = STAGE_RECOVER_POWER_OFF;
      job.phase = "recovering";
      job.message = "中止未获确认，正在安全重启模组";
      job.phaseAt = millis();
    }
    return;
  }

  if (job.stage == STAGE_RECOVER_POWER_OFF) {
    if (millis() - job.phaseAt >= 1500) {
      digitalWrite(MODEM_EN_PIN, HIGH);
      job.stage = STAGE_RECOVER_BOOT;
      job.message = "模组重新上电，等待短信服务恢复";
      job.phaseAt = millis();
    }
    return;
  }

  if (job.stage == STAGE_RECOVER_BOOT) {
    if (millis() - job.phaseAt >= 6500) {
      modemInit();
      if ((job.type == TASK_SELECT || job.type == TASK_AUTO) && !job.recoveryRebooted) {
        job.recoveryRebooted = true;
        beginAutoRecovery(recoveryMessage, recoveryError + "；模组已重新初始化");
      } else {
        finishJob(false, recoveryMessage, recoveryError + "；模组已重新初始化");
        operatorManagerInvalidate();
      }
    }
    return;
  }

  if (job.stage == STAGE_REG_WAIT) {
    while (Serial1.available()) {
      dispatchSerial1Byte(static_cast<char>(Serial1.read()), false);
    }
    if (millis() - job.startedAt > taskTimeout()) {
      // The primary COPS command already returned a terminal OK before this
      // wait state, so there is no in-flight AT command to abort.
      if (job.type == TASK_SELECT) {
        beginAutoRecovery("目标网络注册超时", "目标运营商在 120 秒内未完成注册");
      } else {
        finishJob(false, "选网后网络注册超时", "请重新扫描后再试");
        operatorManagerInvalidate();
      }
      return;
    }
    if (millis() - job.phaseAt >= 2500) {
      sendWireCommand("AT+CEREG?", STAGE_CEREG, "registering", "正在检查网络注册状态");
    }
    return;
  }

  if (job.stage == STAGE_AUTO_RECOVERY_WAIT) {
    while (Serial1.available()) {
      dispatchSerial1Byte(static_cast<char>(Serial1.read()), false);
    }
    if (millis() - job.recoveryStartedAt > SELECT_TIMEOUT_MS) {
      beginRecovery("自动选网恢复超时", recoveryError + "；模组将安全重启");
      return;
    }
    if (millis() - job.phaseAt >= 2500) {
      sendWireCommand("AT+CEREG?", STAGE_AUTO_RECOVERY_CEREG, "recovering",
                      "正在检查自动选网恢复状态");
    }
    return;
  }

  while (Serial1.available()) {
    char c = static_cast<char>(Serial1.read());
    dispatchSerial1Byte(c, false);
    if (responseBuffer.length() < MAX_RESPONSE_BYTES) {
      responseBuffer += c;
    } else {
      beginRecovery("模组响应过长", "运营商响应超过设备解析上限");
      return;
    }
  }

  if (responseError(responseBuffer)) {
    const bool autoRecovery = job.stage == STAGE_AUTO_RECOVERY ||
                              job.stage == STAGE_AUTO_RECOVERY_CEREG ||
                              job.stage == STAGE_AUTO_RECOVERY_FORMAT ||
                              job.stage == STAGE_AUTO_RECOVERY_QUERY;
    if (job.type == TASK_SELECT && !autoRecovery) {
      beginAutoRecovery("目标网络拒绝选网命令", "当前 SIM 或 Profile 无法注册所选网络");
      return;
    }
    if (autoRecovery) {
      beginRecovery("自动选网恢复失败", recoveryError + "；模组将安全重启");
      return;
    }
    String detail = job.type == TASK_SCAN ? "当前 Profile 不允许扫描或模组扫描失败" : "运营商拒绝了选网命令";
    finishJob(false, "运营商操作失败", detail);
    return;
  }
  if (terminalLine(responseBuffer, "OK")) {
    handleSuccessfulResponse();
    return;
  }
  const bool autoRecovery = job.stage == STAGE_AUTO_RECOVERY ||
                            job.stage == STAGE_AUTO_RECOVERY_CEREG ||
                            job.stage == STAGE_AUTO_RECOVERY_FORMAT ||
                            job.stage == STAGE_AUTO_RECOVERY_QUERY;
  if ((job.type == TASK_SELECT || job.type == TASK_AUTO) && !autoRecovery &&
      millis() - job.startedAt > taskTimeout()) {
    if (job.type == TASK_SELECT) {
      beginRecovery("手动选网命令超时", "模组未结束选网命令，将安全重启并恢复服务");
    } else {
      beginRecovery("选网后网络注册超时", "请重新扫描后再试");
    }
    return;
  }
  if (autoRecovery && millis() - job.recoveryStartedAt > SELECT_TIMEOUT_MS) {
    beginRecovery("自动选网恢复超时", recoveryError + "；模组将安全重启");
    return;
  }
  if (millis() - job.commandStartedAt > taskTimeout()) {
    beginRecovery("运营商操作超时", job.type == TASK_SCAN ? "扫描超过 180 秒" : "模组未在时限内响应");
  }
}

bool operatorManagerIsBusy() {
  return job.active;
}

bool operatorManagerStartScan(String& message) {
  if (!beginTask(TASK_SCAN, message)) return false;
  networkCount = 0;
  networksUpdatedAt = 0;
  sendWireCommand("AT+COPS=?", STAGE_PRIMARY, "scanning", "正在扫描可用运营商，期间蜂窝网络可能短暂中断");
  message = "运营商扫描已开始";
  return true;
}

bool operatorManagerStartSelect(const String& numeric, int act, String& message) {
  if (!numericPlmn(numeric) || act < 0 || act > 13) {
    message = "运营商参数无效";
    return false;
  }
  if (!networksUpdatedAt || millis() - networksUpdatedAt > SCAN_CACHE_MS) {
    message = "请先重新扫描运营商";
    return false;
  }
  const OperatorNetwork* target = nullptr;
  for (uint8_t i = 0; i < networkCount; ++i) {
    if (sameNetwork(networks[i], numeric, act)) target = &networks[i];
  }
  if (!target || target->status != 1) {
    message = "目标网络不在本次可选扫描结果中";
    return false;
  }
  if (currentOperator.numeric == numeric) {
    message = "当前已经在该网络上";
    return false;
  }
  if (!beginTask(TASK_SELECT, message)) return false;
  job.targetNumeric = numeric;
  job.targetAct = act;
  // Mode 1 performs a real manual selection. If registration is rejected or
  // times out, the state machine sends COPS=0 and verifies service recovery.
  // AcT is omitted so ML307A/C/R/Y variants may choose a supported LTE mode.
  String command = "AT+COPS=1,2,\"" + numeric + "\"";
  sendWireCommand(command, STAGE_PRIMARY, "selecting", "正在手动选择 " + (carrierName(numeric).length() ? carrierName(numeric) : numeric));
  message = "手动选网任务已开始";
  return true;
}

bool operatorManagerStartAuto(String& message) {
  if (!beginTask(TASK_AUTO, message)) return false;
  sendWireCommand("AT+COPS=0", STAGE_PRIMARY, "selecting", "正在恢复自动选网");
  message = "自动选网任务已开始";
  return true;
}

bool operatorManagerStartAutoForEsim(String& message) {
  if (!beginTask(TASK_AUTO, message, true)) return false;
  sendWireCommand("AT+COPS=0", STAGE_PRIMARY, "selecting",
                  "新 Profile 已生效，正在恢复自动选网");
  message = "Profile 切换后的自动选网已开始";
  return true;
}

bool operatorManagerLastJobSucceeded() {
  return job.done && job.ok;
}

String operatorManagerLastJobMessage() {
  if (job.error.length()) return job.error;
  return job.message;
}

bool operatorManagerIsAutomaticSelection() {
  return currentOperator.mode == 0;
}

String operatorCurrentLabel() {
  if (currentOperator.name.length()) return currentOperator.name;
  if (currentOperator.numeric.length()) return carrierName(currentOperator.numeric).length()
                                               ? carrierName(currentOperator.numeric)
                                               : currentOperator.numeric;
  return "";
}

String operatorCurrentNumeric() {
  return currentOperator.numeric;
}

int operatorCurrentAct() {
  return currentOperator.act;
}

String operatorCurrentActName() {
  return actName(currentOperator.act);
}

String operatorManagerJson() {
  bool cacheFresh = networksUpdatedAt && millis() - networksUpdatedAt <= SCAN_CACHE_MS;
  String json;
  json.reserve(1800 + networkCount * 180);
  json = "{\"ok\":true,\"current\":{\"known\":" + String(currentOperator.known ? "true" : "false") +
         ",\"mode\":" + String(currentOperator.mode) + ",\"numeric\":\"" +
         jsonEscapeOperator(currentOperator.numeric) + "\",\"name\":\"" +
         jsonEscapeOperator(currentOperator.name) + "\",\"carrier\":\"" +
         jsonEscapeOperator(carrierName(currentOperator.numeric)) + "\",\"act\":" +
         String(currentOperator.act) + ",\"actName\":\"" + actName(currentOperator.act) +
         "\",\"registered\":" + String(modemReady ? "true" : "false") +
         "},\"cacheFresh\":" + String(cacheFresh ? "true" : "false") +
         ",\"updatedAt\":" + String(networksUpdatedAt) + ",\"networks\":[";
  for (uint8_t i = 0; i < networkCount; ++i) {
    if (i) json += ',';
    json += networkJson(networks[i]);
  }
  json += "],\"job\":{\"id\":" + String(job.id) + ",\"active\":" +
          String(job.active ? "true" : "false") + ",\"running\":" +
          String(job.active ? "true" : "false") + ",\"done\":" +
          String(job.done ? "true" : "false") + ",\"ok\":" +
          String(job.ok ? "true" : "false") + ",\"warning\":" +
          String(job.warning ? "true" : "false") + ",\"type\":\"" + taskName(job.type) +
          "\",\"phase\":\"" + jsonEscapeOperator(job.phase) + "\",\"progress\":" +
          String(jobProgress()) + ",\"message\":\"" + jsonEscapeOperator(job.message) +
          "\",\"error\":\"" + jsonEscapeOperator(job.error) + "\",\"targetNumeric\":\"" +
          jsonEscapeOperator(job.targetNumeric) + "\",\"targetAct\":" +
          String(job.targetAct) + "}}";
  return json;
}

void operatorManagerInvalidate() {
  if (job.active) {
    invalidatePending = true;
    return;
  }
  clearOperatorCache();
}
