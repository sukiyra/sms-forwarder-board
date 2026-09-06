#include "modem.h"
#include "web_handlers.h"
#include "sms_process.h"
#include "operator_manager.h"
#include "sim_manager.h"
#include "modem_profile.h"

namespace {
uint8_t modemTransactionDepth = 0;
bool modemBooting = false;
bool modemExclusive = false;
bool pdpContextControlSupported = false;
bool detectedModelSupported = false;
String smsDeliveryMode = "unconfigured";

void beginModemTransaction() {
  if (modemTransactionDepth < 255) ++modemTransactionDepth;
}

void endModemTransaction() {
  if (modemTransactionDepth > 0) --modemTransactionDepth;
}
}  // namespace

bool modemIsBusy() {
  return modemBooting || modemTransactionDepth > 0 || modemExclusive;
}

bool modemIsBooting() {
  return modemBooting;
}

bool modemSupportsPdpContextControl() {
  return pdpContextControlSupported;
}

bool modemModelSupported() {
  return detectedModelSupported;
}

const char* modemSmsDeliveryMode() {
  return smsDeliveryMode.c_str();
}

void modemSetSmsDeliveryMode(const char* mode) {
  smsDeliveryMode = mode ? mode : "unconfigured";
}

bool modemAcquireExclusive() {
  if (modemIsBusy()) return false;
  modemExclusive = true;
  return true;
}

void modemReleaseExclusive() {
  modemExclusive = false;
}

// 发送AT命令并获取响应
String sendATCommand(const char* cmd, unsigned long timeout) {
  checkSerial1URC();
  beginModemTransaction();
  Serial1.println(cmd);

  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    if (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      dispatchSerial1Byte(c, false);
      if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) {
        // 读取剩余数据（最多 50ms）
        unsigned long t = millis();
        while (millis() - t < 50) {
          if (Serial1.available()) {
            char trailing = static_cast<char>(Serial1.read());
            resp += trailing;
            dispatchSerial1Byte(trailing, false);
          }
          server.handleClient();
        }
        endModemTransaction();
        return resp;
      }
    }
    server.handleClient();
  }
  endModemTransaction();
  return resp;
}

// 新增"模组断电重启"函数
void modemPowerCycle() {
  pinMode(MODEM_EN_PIN, OUTPUT);

  logCaptureLn(String("EN 拉低：关闭模组"));
  digitalWrite(MODEM_EN_PIN, LOW);
  delay(1200);  // 关机时间给够

  logCaptureLn(String("EN 拉高：开启模组"));
  digitalWrite(MODEM_EN_PIN, HIGH);
  delay(6000);  // 等模组完全启动再发AT（关键）
}

// 重启模组（EN引脚断电重启 + 重新初始化）
void resetModule() {
  modemBooting = true;
  modemReady = false;
  // Withdraw the old card state before the fixed power-cycle delay so every
  // client immediately stops using stale SIM/Profile/operator information.
  simManagerInvalidate();
  logCaptureLn(String("正在硬重启模组（EN 断电重启）..."));
  modemPowerCycle();
  modemInit();
}

// 模组 AT 初始化流程（setup 中调用，resetModule 后也调用）
void modemInit() {
  simManagerInvalidate();
  operatorManagerInvalidate();
  modemBooting = true;
  modemReady = false;
  modemSetSmsDeliveryMode("unconfigured");
  checkSerial1URC();

  bool atReady = false;
  for (uint8_t attempt = 0; attempt < 12; ++attempt) {
    if (sendATandWaitOK("AT", 1000)) {
      atReady = true;
      break;
    }
    logCaptureLn(String("AT未响应，重试..."));
    blink_short(250);
  }
  if (!atReady) {
    logCaptureLn(String("⚠️ 模组在限定次数内未响应，网页保持可用"));
    modemBooting = false;
    return;
  }
  logCaptureLn(String("模组AT响应正常"));
  if (!sendATandWaitOK("AT+CMEE=1", 1200)) {
    logCaptureLn(String("⚠️ 无法启用数值 CME 错误，SIM 检测将继续重试"));
  }

  // Different ML307 generations format ATI differently. Query the standardized
  // identity commands as well, then classify by the complete model string.
  detectedModemManufacturer = "未知";
  detectedModemModel = "ML307";
  detectedModemFirmware = "未知";
  detectedModemFamily = "未知";
  detectedModelSupported = false;
  pdpContextControlSupported = false;
  String resp = sendATCommand("ATI", 2000);
  logCaptureLn(String("ATI响应: " + resp));
  String atiModel = modemIdentityValue(resp, "ATI", "", true);
  String manufacturerResponse = sendATCommand("AT+CGMI", 1600);
  String modelResponse = sendATCommand("AT+CGMM", 1600);
  String firmwareResponse = sendATCommand("AT+CGMR", 1600);
  String manufacturer = modemIdentityValue(manufacturerResponse, "AT+CGMI", "+CGMI:", false);
  String model = modemIdentityValue(modelResponse, "AT+CGMM", "+CGMM:", true);
  String version = modemIdentityValue(firmwareResponse, "AT+CGMR", "+CGMR:", false);
  if (!manufacturer.length()) manufacturer = modemIdentityValue(resp, "ATI", "", false);
  if (!model.length()) model = atiModel;
  if (manufacturer.length()) detectedModemManufacturer = manufacturer;
  if (model.length()) detectedModemModel = model;
  if (version.length()) detectedModemFirmware = version;

  ModemProfile profile = modemClassify(detectedModemModel + "\n" + atiModel);
  detectedModemFamily = profile.familyName;
  detectedModelSupported = profile.supported;
  if (profile.supported) {
    logCaptureLn(String("模组自动识别：") + profile.familyName + " / " + detectedModemModel);
  } else {
    logCaptureLn(String("⚠️ 未识别的模组料号：") + detectedModemModel +
                 "；将仅尝试标准短信指令");
  }

  if (profile.mayProbePdpContext) {
    String capability = sendATCommand("AT+CGACT=?", 1800);
    pdpContextControlSupported = capability.indexOf("OK") >= 0 &&
                                 capability.indexOf("ERROR") < 0;
  }

  if (pdpContextControlSupported) {
    bool dataDisabled = false;
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
      if (sendATandWaitOK("AT+CGACT=0,1", 5000)) {
        dataDisabled = true;
        break;
      }
      logCaptureLn(String("设置CGACT失败，重试..."));
      blink_short(250);
    }
    logCaptureLn(dataDisabled ? String("已禁用数据连接(AT+CGACT=0,1)，防止流量消耗")
                              : String("⚠️ 无法确认数据连接已禁用"));
  } else {
    logCaptureLn(String("兼容模式：跳过 AT+CGACT=0,1，短信功能不受影响"));
  }
  String iccidResponse = sendATCommand("AT+ICCID", 2000);
  simManagerCaptureIccid(iccidResponse);
  String iccidTail = simManagerIccidTail();
  logCaptureLn(iccidTail.length() == 4
                   ? String("接收卡已识别（ICCID 尾号 ") + iccidTail + ")"
                   : String("⚠️ 暂未读取到接收卡 ICCID，短信来源将使用 Profile 回退标签"));
  bool cnmiReady = false;
  for (uint8_t attempt = 0; attempt < 5; ++attempt) {
    if (sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1200)) {
      cnmiReady = true;
      modemSetSmsDeliveryMode("direct");
      break;
    }
    blink_short(200);
  }
  if (!cnmiReady) {
    // Some A/C firmware only accepts stored-message notifications. The +CMTI
    // reader selects the memory named by the URC before reading and deleting it.
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
      if (sendATandWaitOK("AT+CNMI=2,1,0,0,0", 1200)) {
        cnmiReady = true;
        modemSetSmsDeliveryMode("stored");
        break;
      }
      blink_short(200);
    }
  }
  bool pduReady = false;
  for (uint8_t attempt = 0; attempt < 5; ++attempt) {
    if (sendATandWaitOK("AT+CMGF=0", 1200)) {
      pduReady = true;
      break;
    }
    blink_short(200);
  }
  if (!cnmiReady || !pduReady) {
    logCaptureLn(String("⚠️ 短信上报配置失败，稍后可从诊断中心重启模组重试"));
    modemBooting = false;
    return;
  }
  logCaptureLn(String("短信PDU上报配置完成，模式：") + modemSmsDeliveryMode());
  sendATandWaitOK("AT+CEREG=2", 1200);  // 启用网络注册状态变化上报
  logCaptureLn(String("PDU模式设置完成"));
  int ceregRetry = 0;
  while (!waitCEREG() && ceregRetry < 20) {
    logCaptureLn(String("等待网络注册..."));
    ceregRetry++;
    blink_short(250);
  }
  if (ceregRetry < 20) {
    logCaptureLn(String("网络已注册"));
    modemReady = true;
  } else {
    logCaptureLn(String("⚠️ 网络注册超时（无SIM卡或信号差），模组功能不可用"));
    modemReady = false;
  }
  modemBooting = false;
}

void blink_short(unsigned long gap_time) {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(gap_time);
}

bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  String resp = sendATCommand(cmd, timeout);
  return resp.indexOf("OK") >= 0 && resp.indexOf("ERROR") < 0;
}

// 检测网络注册状态（LTE/4G）
// CEREG状态: 1=已注册本地, 5=已注册漫游
bool waitCEREG() {
  String resp = sendATCommand("AT+CEREG?", 2000);
  int queryStatus = -1;
  int urcStatus = -1;
  int searchFrom = 0;
  while (searchFrom < resp.length()) {
    int marker = resp.indexOf("+CEREG:", searchFrom);
    if (marker < 0) break;
    int lineEnd = resp.indexOf('\n', marker);
    if (lineEnd < 0) lineEnd = resp.length();
    String fields = resp.substring(marker + 7, lineEnd);
    fields.trim();
    int comma = fields.indexOf(',');
    if (comma < 0) {
      int value = fields.toInt();
      if (value >= 0 && value <= 5) urcStatus = value;
    } else {
      String first = fields.substring(0, comma);
      String remainder = fields.substring(comma + 1);
      first.trim();
      remainder.trim();
      int value = remainder.startsWith("\"") ? first.toInt() : remainder.toInt();
      if (value >= 0 && value <= 5) {
        if (remainder.startsWith("\"")) urcStatus = value;
        else queryStatus = value;
      }
    }
    searchFrom = lineEnd + 1;
  }
  int status = queryStatus >= 0 ? queryStatus : urcStatus;
  return status == 1 || status == 5;
}

// 发送短信（PDU模式）
bool sendSMS(const char* phoneNumber, const char* message) {
  if (!simManagerIsReady() || !simManagerSmsReady()) {
    logCaptureLn(String("SIM 或短信服务尚未就绪，暂不能发送短信"));
    return false;
  }
  if (modemIsBusy()) {
    logCaptureLn(String("模组正忙，暂不能发送短信"));
    return false;
  }
  String normalizedPhone(phoneNumber);
  // Mainland mobile numbers are commonly entered without a country code.
  // Always use international format so roaming SIM SMSCs route them correctly.
  if (normalizedPhone.length() == 11 && normalizedPhone.charAt(0) == '1') {
    normalizedPhone = "+86" + normalizedPhone;
  }
  logCaptureLn(String("准备发送短信..."));
  String maskedPhone(normalizedPhone);
  if (maskedPhone.length() > 4) maskedPhone = "****" + maskedPhone.substring(maskedPhone.length() - 4);
  logCapture(String("目标号码: ")); logCaptureLn(maskedPhone);
  logCapture(String("短信内容长度: ")); logCaptureLn(String(strlen(message)) + " 字节");

  // 使用pdulib编码PDU
  pdu.setSCAnumber();  // 使用默认短信中心
  int pduLen = pdu.encodePDU(normalizedPhone.c_str(), message);

  if (pduLen < 0) {
    logCapture(String("PDU编码失败，错误码: "));
    logCaptureLn(String(pduLen));
    return false;
  }

  logCapture(String("PDU长度: ")); logCaptureLn(String(pduLen));

  // 发送AT+CMGS命令
  String cmgsCmd = "AT+CMGS=";
  cmgsCmd += pduLen;

  checkSerial1URC();
  beginModemTransaction();
  Serial1.println(cmgsCmd);

  // 等待 > 提示符
  unsigned long start = millis();
  bool gotPrompt = false;
  while (millis() - start < 5000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      logCapture(String(c));
      dispatchSerial1Byte(c, false);
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
    server.handleClient();
    delay(1);
  }

  if (!gotPrompt) {
    logCaptureLn(String("未收到>提示符"));
    endModemTransaction();
    return false;
  }

  // 发送PDU数据
  Serial1.print(pdu.getSMS());
  Serial1.write(0x1A);  // Ctrl+Z 结束

  // 等待响应
  start = millis();
  String resp = "";
  while (millis() - start < 30000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      logCapture(String(c));
      dispatchSerial1Byte(c, false);
      if (resp.indexOf("OK") >= 0) {
        logCaptureLn(String("\n运营商已接收短信发送请求"));
        endModemTransaction();
        return true;
      }
      if (resp.indexOf("ERROR") >= 0) {
        logCaptureLn(String("\n短信发送失败"));
        endModemTransaction();
        return false;
      }
    }
    server.handleClient();
    delay(1);
  }
  logCaptureLn(String("短信发送超时"));
  endModemTransaction();
  return false;
}
