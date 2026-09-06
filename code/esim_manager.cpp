#include "esim_manager.h"
#include "push.h"
#include "modem.h"
#include "operator_manager.h"
#include "sim_manager.h"
#include "web_handlers.h"

namespace {

constexpr const char *ISDR_AID = "A0000005591010FFFFFFFF8900000100";
constexpr uint8_t MAX_ESIM_PROFILES = 8;
constexpr unsigned long SWITCH_TOTAL_TIMEOUT_MS = 480000UL;
constexpr unsigned long LATE_REGISTRATION_TIMEOUT_MS = 120000UL;

struct EsimProfile {
  String id;
  String iccid;
  String provider;
  String name;
  bool enabled = false;
};

enum SwitchState {
  SWITCH_IDLE,
  SWITCH_POWER_OFF,
  SWITCH_BOOT_WAIT,
  SWITCH_WAIT_AT,
  SWITCH_CONFIGURE,
  SWITCH_VERIFY,
  SWITCH_AUTO_SELECT,
  SWITCH_WAIT_NETWORK,
  SWITCH_RECOVER_VERIFY
};

struct SwitchJob {
  uint32_t id = 0;
  bool active = false;
  bool done = false;
  bool ok = false;
  bool warning = false;
  bool smsConfigWarning = false;
  bool autoSelectWarning = false;
  bool networkWarning = false;
  bool profileConfirmed = false;
  String phase = "idle";
  String message;
  String targetId;
  unsigned long startedAt = 0;
  unsigned long phaseAt = 0;
  unsigned long lastAttemptAt = 0;
};

EsimProfile profiles[MAX_ESIM_PROFILES];
uint8_t profileCount = 0;
unsigned long profilesUpdatedAt = 0;
String cachedEid;
bool eidValid = false;
SwitchState switchState = SWITCH_IDLE;
SwitchJob job;
Preferences esimPrefs;
bool profileIoBusy = false;
uint8_t configureAttempts = 0;


String hexByte(uint8_t value) {
  const char *hex = "0123456789ABCDEF";
  String out;
  out += hex[value >> 4];
  out += hex[value & 0x0f];
  return out;
}

uint8_t channelCla(int channel) {
  if (channel >= 0 && channel <= 3) return 0x80 | channel;
  if (channel >= 4 && channel <= 19) return 0xC0 | (channel - 4);
  return 0x80;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool readHexByte(const String &hex, size_t byteOffset, uint8_t &value) {
  size_t pos = byteOffset * 2;
  if (pos + 1 >= hex.length()) return false;
  int high = hexNibble(hex[pos]);
  int low = hexNibble(hex[pos + 1]);
  if (high < 0 || low < 0) return false;
  value = static_cast<uint8_t>((high << 4) | low);
  return true;
}

struct Tlv {
  uint32_t tag = 0;
  size_t valueOffset = 0;
  size_t length = 0;
  size_t nextOffset = 0;
};

bool readTlv(const String &hex, size_t offset, size_t limit, Tlv &tlv) {
  uint8_t first;
  if (!readHexByte(hex, offset++, first)) return false;
  tlv.tag = first;
  if ((first & 0x1f) == 0x1f) {
    uint8_t next;
    do {
      if (!readHexByte(hex, offset++, next)) return false;
      tlv.tag = (tlv.tag << 8) | next;
    } while (next & 0x80);
  }

  uint8_t lenByte;
  if (!readHexByte(hex, offset++, lenByte)) return false;
  if ((lenByte & 0x80) == 0) {
    tlv.length = lenByte;
  } else {
    uint8_t lengthBytes = lenByte & 0x7f;
    if (lengthBytes == 0 || lengthBytes > 2) return false;
    tlv.length = 0;
    for (uint8_t i = 0; i < lengthBytes; ++i) {
      uint8_t part;
      if (!readHexByte(hex, offset++, part)) return false;
      tlv.length = (tlv.length << 8) | part;
    }
  }
  tlv.valueOffset = offset;
  tlv.nextOffset = offset + tlv.length;
  return tlv.nextOffset <= limit && tlv.nextOffset * 2 <= hex.length();
}

String hexSlice(const String &hex, size_t offset, size_t length) {
  return hex.substring(offset * 2, (offset + length) * 2);
}

String hexToUtf8(const String &hex, size_t offset, size_t length) {
  String value;
  value.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    uint8_t b;
    if (!readHexByte(hex, offset + i, b)) break;
    value += static_cast<char>(b);
  }
  return value;
}

String decodeIccid(const String &hex, size_t offset, size_t length) {
  String out;
  out.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    uint8_t b;
    if (!readHexByte(hex, offset + i, b)) break;
    uint8_t low = b & 0x0f;
    uint8_t high = b >> 4;
    if (low <= 9) out += static_cast<char>('0' + low);
    if (high <= 9) out += static_cast<char>('0' + high);
  }
  return out;
}

int parseChannel(const String &response) {
  int start = 0;
  while (start < response.length()) {
    int end = response.indexOf('\n', start);
    if (end < 0) end = response.length();
    String line = response.substring(start, end);
    line.trim();
    if (line.startsWith("+CCHO:")) {
      line = line.substring(6);
      line.trim();
    }
    bool digits = line.length() > 0;
    for (size_t i = 0; i < line.length(); ++i) digits &= isDigit(line[i]);
    if (digits) {
      int value = line.toInt();
      if (value > 0 && value < 20) return value;
    }
    start = end + 1;
  }
  return -1;
}

enum IsdrTransport {
  TRANSPORT_CGLA,         // 原生 AT+CCHO/CGLA
  TRANSPORT_CSIM_LOGICAL, // CSIM 手动开逻辑通道
  TRANSPORT_CSIM_BASIC    // CSIM 基础通道直连 ISD-R
};
uint8_t isdrTransport = TRANSPORT_CGLA;

enum EsimCapability {
  ESIM_CAPABILITY_UNKNOWN,
  ESIM_CAPABILITY_SUPPORTED,
  ESIM_CAPABILITY_UNSUPPORTED
};

EsimCapability esimCapability = ESIM_CAPABILITY_UNKNOWN;
unsigned long capabilityCheckedAt = 0;
bool profileListLoaded = false;

void markEsimSupported() {
  esimCapability = ESIM_CAPABILITY_SUPPORTED;
  capabilityCheckedAt = millis();
}

void markPhysicalSim() {
  if (esimCapability != ESIM_CAPABILITY_UNSUPPORTED) {
    logCaptureLn("未检测到 eUICC 卡功能，已切换为实体 SIM 模式");
  }
  esimCapability = ESIM_CAPABILITY_UNSUPPORTED;
  capabilityCheckedAt = millis();
  profileListLoaded = false;
}

// AT+CSIM 透传:响应引号内为 <data><SW>
bool csimExchange(const String &apdu, String &data, uint16_t &status, String &error) {
  data = "";
  status = 0;
  String cmd = "AT+CSIM=" + String(apdu.length()) + ",\"" + apdu + "\"";
  String response = sendATCommand(cmd.c_str(), 6000);
  int marker = response.indexOf("+CSIM:");
  int firstQuote = marker >= 0 ? response.indexOf('"', marker) : -1;
  int secondQuote = firstQuote >= 0 ? response.indexOf('"', firstQuote + 1) : -1;
  if (firstQuote < 0 || secondQuote <= firstQuote + 4) {
    error = "eUICC APDU 无响应";
    return false;
  }
  String payload = response.substring(firstQuote + 1, secondQuote);
  payload.trim();
  if (payload.length() < 4 || payload.length() % 2 != 0) {
    error = "eUICC APDU 响应格式错误";
    return false;
  }
  status = static_cast<uint16_t>(strtoul(payload.substring(payload.length() - 4).c_str(), nullptr, 16));
  data = payload.substring(0, payload.length() - 4);
  return true;
}

bool openIsdr(int &channel, String &error) {
  if (esimCapability == ESIM_CAPABILITY_UNSUPPORTED) {
    error = "当前为实体 SIM，不支持 eSIM 卡功能";
    return false;
  }
  String response = sendATCommand(("AT+CCHO=\"" + String(ISDR_AID) + "\"").c_str(), 5000);
  channel = parseChannel(response);
  if (channel > 0) {
    isdrTransport = TRANSPORT_CGLA;
    markEsimSupported();
    return true;
  }

  String data;
  uint16_t status = 0;
  if (csimExchange("0070000001", data, status, error) && status == 0x9000 && data.length() >= 2) {
    int ch = static_cast<int>(strtoul(data.substring(0, 2).c_str(), nullptr, 16));
    if (ch > 0 && ch < 20) {
      String cla = hexByte(channelCla(ch));
      if (csimExchange(cla + "A4040010" + String(ISDR_AID), data, status, error)) {
        if ((status >> 8) == 0x61) {
          csimExchange(cla + "C00000" + hexByte(status & 0xff), data, status, error);
          status = 0x9000;
        }
        if (status == 0x9000) {
          channel = ch;
          isdrTransport = TRANSPORT_CSIM_LOGICAL;
          markEsimSupported();
          return true;
        }
      }
      String closeData;
      uint16_t closeStatus = 0;
      String closeError;
      csimExchange("00708000" + hexByte(ch), closeData, closeStatus, closeError);
    }
  }
  if (csimExchange("00A4040010" + String(ISDR_AID), data, status, error)) {
    if ((status >> 8) == 0x61) {
      csimExchange("00C00000" + hexByte(status & 0xff), data, status, error);
      status = 0x9000;
    }
    if (status == 0x9000) {
      channel = 0;
      isdrTransport = TRANSPORT_CSIM_BASIC;
      markEsimSupported();
      return true;
    }
  }
  if (esimCapability == ESIM_CAPABILITY_UNKNOWN) markPhysicalSim();
  error = esimCapability == ESIM_CAPABILITY_UNSUPPORTED
              ? "当前为实体 SIM，不支持 eSIM 卡功能"
              : "eUICC 暂时无响应，请稍后重试";
  return false;
}

void closeChannel(int channel) {
  if (channel <= 0) return;
  if (isdrTransport == TRANSPORT_CGLA) {
    String cmd = "AT+CCHC=" + String(channel);
    sendATCommand(cmd.c_str(), 2000);
  } else if (isdrTransport == TRANSPORT_CSIM_LOGICAL) {
    String data;
    uint16_t status = 0;
    String error;
    csimExchange("00708000" + hexByte(channel), data, status, error);
  }
}

bool transmitApdu(int channel, const String &apdu, String &data, uint16_t &status, String &error) {
  data = "";
  status = 0;
  String current = apdu;
  for (uint8_t round = 0; round < 8; ++round) {
    String cmd;
    const char *marker;
    if (isdrTransport == TRANSPORT_CGLA) {
      cmd = "AT+CGLA=" + String(channel) + "," + String(current.length()) + ",\"" + current + "\"";
      marker = "+CGLA:";
    } else {
      cmd = "AT+CSIM=" + String(current.length()) + ",\"" + current + "\"";
      marker = "+CSIM:";
    }
    String response = sendATCommand(cmd.c_str(), 6000);
    int m = response.indexOf(marker);
    int firstQuote = m >= 0 ? response.indexOf('"', m) : -1;
    int secondQuote = firstQuote >= 0 ? response.indexOf('"', firstQuote + 1) : -1;
    if (firstQuote < 0 || secondQuote <= firstQuote + 4) {
      error = "eUICC APDU 无响应";
      return false;
    }
    String payload = response.substring(firstQuote + 1, secondQuote);
    payload.trim();
    if (payload.length() < 4 || payload.length() % 2 != 0) {
      error = "eUICC APDU 响应格式错误";
      return false;
    }
    String swHex = payload.substring(payload.length() - 4);
    status = static_cast<uint16_t>(strtoul(swHex.c_str(), nullptr, 16));
    data += payload.substring(0, payload.length() - 4);
    uint8_t sw1 = status >> 8;
    uint8_t sw2 = status & 0xff;
    if (sw1 == 0x61) {
      String grCla = (isdrTransport == TRANSPORT_CSIM_BASIC) ? String("00") : hexByte(channelCla(channel));
      current = grCla + "C00000" + hexByte(sw2);
      continue;
    }
    return true;
  }
  error = "eUICC APDU 分段响应过多";
  return false;
}

bool parseProfiles(const String &hex, String &error) {
  profileCount = 0;
  Tlv outer;
  size_t totalBytes = hex.length() / 2;
  if (!readTlv(hex, 0, totalBytes, outer) || outer.tag != 0xBF2D) {
    error = "Profile 列表格式无效";
    return false;
  }
  Tlv list;
  if (!readTlv(hex, outer.valueOffset, outer.nextOffset, list) || list.tag != 0xA0) {
    error = "Profile 列表为空或不可读";
    return false;
  }

  size_t pos = list.valueOffset;
  while (pos < list.nextOffset && profileCount < MAX_ESIM_PROFILES) {
    Tlv record;
    if (!readTlv(hex, pos, list.nextOffset, record)) break;
    pos = record.nextOffset;
    if (record.tag != 0xE3) continue;

    EsimProfile profile;
    size_t fieldPos = record.valueOffset;
    while (fieldPos < record.nextOffset) {
      Tlv field;
      if (!readTlv(hex, fieldPos, record.nextOffset, field)) break;
      fieldPos = field.nextOffset;
      switch (field.tag) {
        case 0x5A: profile.iccid = decodeIccid(hex, field.valueOffset, field.length); break;
        case 0x4F: profile.id = hexSlice(hex, field.valueOffset, field.length); break;
        case 0x9F70: {
          uint8_t value = 0;
          if (field.length && readHexByte(hex, field.valueOffset, value)) profile.enabled = value == 1;
          break;
        }
        case 0x91: profile.provider = hexToUtf8(hex, field.valueOffset, field.length); break;
        case 0x92: profile.name = hexToUtf8(hex, field.valueOffset, field.length); break;
      }
    }
    if (profile.id.length() == 32) profiles[profileCount++] = profile;
  }
  profilesUpdatedAt = millis();
  return true;
}

bool readProfiles(String &error) {
  if (profileIoBusy) {
    error = "eUICC 正在读取";
    return false;
  }
  profileIoBusy = true;
  int channel = -1;
  if (!openIsdr(channel, error)) {
    profileIoBusy = false;
    return false;
  }
  String data;
  uint16_t status = 0;
  String apdu = hexByte(channelCla(channel)) + "E2910003BF2D0000";
  bool ok = transmitApdu(channel, apdu, data, status, error);
  closeChannel(channel);
  if (!ok) {
    profileIoBusy = false;
    return false;
  }
  if (status != 0x9000) {
    error = "Profile 列表读取失败 (SW=" + hexByte(status >> 8) + hexByte(status & 0xff) + ")";
    profileIoBusy = false;
    return false;
  }
  bool parsed = parseProfiles(data, error);
  if (parsed) profileListLoaded = true;
  profileIoBusy = false;
  return parsed;
}

int profileOpResult(const String &hex, uint32_t tag) {
  Tlv outer;
  size_t total = hex.length() / 2;
  if (!readTlv(hex, 0, total, outer) || outer.tag != tag) return -1;
  size_t pos = outer.valueOffset;
  while (pos < outer.nextOffset) {
    Tlv field;
    if (!readTlv(hex, pos, outer.nextOffset, field)) break;
    pos = field.nextOffset;
    if ((field.tag == 0x80 || field.tag == 0x02) && field.length == 1) {
      uint8_t value;
      if (readHexByte(hex, field.valueOffset, value)) return value;
    }
  }
  return -1;
}

void savePending(bool pending, const String &target, const String &phase) {
  esimPrefs.begin("esim_state", false);
  esimPrefs.putBool("pending", pending);
  esimPrefs.putString("target", target);
  esimPrefs.putString("phase", phase);
  esimPrefs.end();
}

void setPhase(const String &phase, const String &message) {
  job.phase = phase;
  job.message = message;
  job.phaseAt = millis();
  job.lastAttemptAt = 0;
}

void finishJob(bool ok, const String &message, bool warning = false) {
  job.active = false;
  job.done = true;
  job.ok = ok;
  job.warning = warning;
  job.message = message;
  job.phase = ok ? "complete" : "failed";
  switchState = SWITCH_IDLE;
  savePending(false, "", job.phase);
}

void reconcileDelayedSuccess() {
  if (!job.done || !job.ok || !job.warning) return;
  if (job.smsConfigWarning && simManagerSmsReady()) job.smsConfigWarning = false;
  if (job.networkWarning && modemReady) job.networkWarning = false;
  if (job.autoSelectWarning && operatorManagerIsAutomaticSelection()) {
    job.autoSelectWarning = false;
  }
  job.warning = job.smsConfigWarning || job.autoSelectWarning || job.networkWarning;
  if (!job.warning) {
    job.message = "Profile 切换完成，网络与短信服务已恢复";
    job.phaseAt = millis();
  } else if (job.smsConfigWarning) {
    job.message = "Profile 已切换，短信服务仍在恢复";
  } else if (job.autoSelectWarning) {
    job.message = "Profile 已切换并注册网络，仍在确认自动选网模式";
  } else {
    job.message = "Profile 已切换，仍在等待网络注册";
  }
}

bool targetIsEnabled() {
  for (uint8_t i = 0; i < profileCount; ++i) {
    if (profiles[i].id.equalsIgnoreCase(job.targetId)) return profiles[i].enabled;
  }
  return false;
}

}  // namespace

void esimManagerBegin() {
  esimPrefs.begin("esim_state", true);
  bool pending = esimPrefs.getBool("pending", false);
  String target = esimPrefs.getString("target", "");
  esimPrefs.end();
  if (pending && target.length() == 32) {
    job.id = esp_random();
    job.active = true;
    job.targetId = target;
    job.startedAt = millis();
    setPhase("verifying", "正在恢复并核对上次切换结果");
    switchState = SWITCH_RECOVER_VERIFY;
  }
}

String esimGetEid(String &error) {
  if (eidValid) return cachedEid;
  if (esimCapability == ESIM_CAPABILITY_UNSUPPORTED) {
    error = "实体 SIM 没有 EID";
    return "";
  }
  if (!simManagerIsReady()) {
    error = "SIM 尚未就绪";
    return "";
  }
  if (esimIsBusy() || profileIoBusy) {
    error = "eUICC 忙";
    return "";
  }
  profileIoBusy = true;
  int channel = -1;
  if (!openIsdr(channel, error)) {
    profileIoBusy = false;
    return "";
  }
  String data;
  uint16_t status = 0;
  String apdu = hexByte(channelCla(channel)) + "E2910006BF3E035C015A00";
  bool ok = transmitApdu(channel, apdu, data, status, error);
  closeChannel(channel);
  profileIoBusy = false;
  if (!ok) return "";
  if (status != 0x9000) {
    error = "EID 读取失败 (SW=" + hexByte(status >> 8) + hexByte(status & 0xff) + ")";
    return "";
  }
  Tlv top;
  size_t totalBytes = data.length() / 2;
  String eid;
  if (readTlv(data, 0, totalBytes, top) && top.tag == 0xBF3E) {
    size_t pos = top.valueOffset;
    while (pos < top.nextOffset) {
      Tlv field;
      if (!readTlv(data, pos, top.nextOffset, field)) break;
      pos = field.nextOffset;
      if (field.tag == 0x5A) {
        eid = hexSlice(data, field.valueOffset, field.length);
        break;
      }
    }
  }
  if (eid.length() == 0) {
    error = "EID 响应格式无效";
    return "";
  }
  cachedEid = eid;
  eidValid = true;
  return cachedEid;
}

bool esimDeleteProfile(const String &profileId, String &message) {
  if (esimCapability == ESIM_CAPABILITY_UNSUPPORTED) {
    message = "实体 SIM 没有可删除的 eSIM Profile";
    return false;
  }
  if (!simManagerIsReady()) {
    message = "SIM 尚未就绪";
    return false;
  }
  if (esimIsBusy()) {
    message = "eSIM 切换任务正在执行";
    return false;
  }
  String normalized = profileId;
  normalized.toUpperCase();
  bool found = false;
  bool enabled = false;
  for (uint8_t i = 0; i < profileCount; ++i) {
    if (profiles[i].id.equalsIgnoreCase(normalized)) {
      found = true;
      enabled = profiles[i].enabled;
    }
  }
  if (!found) {
    String error;
    if (!readProfiles(error)) {
      message = error;
      return false;
    }
    for (uint8_t i = 0; i < profileCount; ++i) {
      if (profiles[i].id == normalized) {
        found = true;
        enabled = profiles[i].enabled;
      }
    }
  }
  if (!found) {
    message = "目标 Profile 不在当前 eUICC 列表中";
    return false;
  }
  if (enabled) {
    message = "不能删除正在使用的 Profile，请先切换到其他卡功能";
    return false;
  }
  profileIoBusy = true;
  int channel = -1;
  String error;
  if (!openIsdr(channel, error)) {
    profileIoBusy = false;
    message = error;
    return false;
  }
  String request = "BF33124F10" + normalized;
  String apdu = hexByte(channelCla(channel)) + "E2910015" + request + "00";
  String data;
  uint16_t status = 0;
  bool transmitted = transmitApdu(channel, apdu, data, status, error);
  closeChannel(channel);
  profileIoBusy = false;
  if (!transmitted) {
    message = error;
    return false;
  }
  if (status != 0x9000) {
    message = "eUICC 拒绝删除请求 (SW=" + hexByte(status >> 8) + hexByte(status & 0xff) + ")";
    return false;
  }
  int result = data.length() ? profileOpResult(data, 0xBF33) : -1;
  if (result != 0) {
    message = result > 0 ? "删除失败，eUICC 返回码 " + String(result)
                         : "删除响应无法解析";
    return false;
  }
  esimManagerInvalidateProfiles();
  return true;
}

bool esimRefreshProfiles(String &error) {
  if (!simManagerIsReady()) {
    error = "SIM 尚未就绪";
    return false;
  }
  if (esimIsBusy() && switchState != SWITCH_VERIFY && switchState != SWITCH_RECOVER_VERIFY) {
    error = "eSIM 正在切换";
    return false;
  }
  if (esimCapability == ESIM_CAPABILITY_UNSUPPORTED) {
    error = "当前为实体 SIM，不支持 eSIM 卡功能";
    return false;
  }
  return readProfiles(error);
}

String esimProfilesJson() {
  String activeId;
  for (uint8_t i = 0; i < profileCount; ++i) {
    if (profiles[i].enabled) activeId = profiles[i].id;
  }
  String supported = esimCapability == ESIM_CAPABILITY_SUPPORTED
                         ? "true"
                         : (esimCapability == ESIM_CAPABILITY_UNSUPPORTED ? "false" : "null");
  String message = esimCapability == ESIM_CAPABILITY_UNSUPPORTED
                       ? "实体 SIM 已识别，短信收发与运营商选网可正常使用"
                       : (esimCapability == ESIM_CAPABILITY_SUPPORTED
                              ? "eSIM 卡功能已识别"
                              : "等待识别 SIM 卡类型");
  String json = "{\"ok\":true,\"supported\":" + supported + ",\"mode\":\"" +
                esimModeName() + "\",\"message\":\"" + jsonEscape(message) +
                "\",\"iccidTail\":\"" + jsonEscape(simManagerIccidTail()) +
                "\",\"capabilityCheckedAt\":" + String(capabilityCheckedAt) +
                ",\"updatedAt\":" + String(profilesUpdatedAt) +
                ",\"activeId\":\"" + activeId + "\",\"switching\":" +
                String(job.active ? "true" : "false") + ",\"profiles\":[";
  for (uint8_t i = 0; i < profileCount; ++i) {
    if (i) json += ',';
    json += "{\"id\":\"" + profiles[i].id + "\",\"iccid\":\"" + jsonEscape(profiles[i].iccid) +
            "\",\"provider\":\"" + jsonEscape(profiles[i].provider) + "\",\"operator\":\"" +
            jsonEscape(profiles[i].provider) + "\",\"name\":\"" + jsonEscape(profiles[i].name) +
            "\",\"type\":\"eSIM\",\"status\":\"" + (profiles[i].enabled ? "当前使用中" : "可切换") +
            "\",\"enabled\":" + (profiles[i].enabled ? "true" : "false") +
            ",\"active\":" + (profiles[i].enabled ? "true" : "false") +
            ",\"available\":" + (profiles[i].enabled ? "false" : "true") + "}";
  }
  json += "],\"job\":" + esimJobJson() + "}";
  return json;
}

bool esimProfilesLoaded() {
  return esimCapability == ESIM_CAPABILITY_UNSUPPORTED || profileListLoaded;
}

void esimManagerInvalidateProfiles() {
  if (profileIoBusy) return;
  for (uint8_t i = 0; i < MAX_ESIM_PROFILES; ++i) profiles[i] = EsimProfile();
  profileCount = 0;
  profilesUpdatedAt = 0;
  profileListLoaded = false;
  cachedEid = "";
  eidValid = false;
}

void esimManagerResetCapability() {
  if (profileIoBusy || esimIsBusy()) return;
  esimManagerInvalidateProfiles();
  esimCapability = ESIM_CAPABILITY_UNKNOWN;
  capabilityCheckedAt = 0;
}

bool esimCapabilityKnown() {
  return esimCapability != ESIM_CAPABILITY_UNKNOWN;
}

bool esimIsSupported() {
  return esimCapability == ESIM_CAPABILITY_SUPPORTED;
}

const char* esimModeName() {
  if (esimCapability == ESIM_CAPABILITY_SUPPORTED) return "esim";
  if (esimCapability == ESIM_CAPABILITY_UNSUPPORTED) return "physical";
  return "unknown";
}

bool esimStartSwitch(const String &profileId, String &message) {
  if (esimCapability == ESIM_CAPABILITY_UNSUPPORTED) {
    message = "实体 SIM 没有可切换的 eSIM Profile";
    return false;
  }
  if (!simManagerIsReady()) {
    message = "SIM 尚未就绪";
    return false;
  }
  if (esimIsBusy()) {
    message = "已有 eSIM 切换任务正在执行";
    return false;
  }
  String normalized = profileId;
  normalized.toUpperCase();
  EsimProfile *target = nullptr;
  for (uint8_t i = 0; i < profileCount; ++i) {
    if (profiles[i].id == normalized) target = &profiles[i];
  }
  if (!target) {
    String error;
    if (!readProfiles(error)) {
      message = error;
      return false;
    }
    for (uint8_t i = 0; i < profileCount; ++i) {
      if (profiles[i].id == normalized) target = &profiles[i];
    }
  }
  if (!target) {
    message = "目标 Profile 不在当前 eUICC 列表中";
    return false;
  }
  if (target->enabled) {
    message = "目标 Profile 已经启用";
    return false;
  }

  job = SwitchJob();
  configureAttempts = 0;
  operatorManagerInvalidate();
  job.id = esp_random();
  job.active = true;
  job.targetId = target->id;
  job.startedAt = millis();
  setPhase("enable_sent", "正在提交 Profile 原子切换请求");
  savePending(true, job.targetId, job.phase);

  int channel = -1;
  String error;
  bool opened = openIsdr(channel, error);
  String data;
  uint16_t status = 0;
  bool transmitted = false;
  if (!opened) {
    String detail = error.length() ? error : "无法打开 eUICC 通道";
    finishJob(false, detail);
    message = detail;
    return false;
  }
  String request = "BF3117A0124F10" + target->id + "8101FF";
  String apdu = hexByte(channelCla(channel)) + "E291001A" + request + "00";
  transmitted = transmitApdu(channel, apdu, data, status, error);
  closeChannel(channel);

  if (transmitted && status == 0x9000 && data.length()) {
    int result = profileOpResult(data, 0xBF31);
    if (result > 0) {
      static const char *errors[] = {"", "目标不存在", "目标不是停用状态", "策略禁止切换",
                                     "不允许重新启用该 Profile", "SIM Toolkit 忙"};
      String detail = result < 6 ? errors[result] : "eUICC 返回未知错误 " + String(result);
      finishJob(false, detail);
      message = detail;
      return false;
    }
  }
  if (transmitted && status != 0x9000 && (status >> 8) != 0x91) {
    String detail = "eUICC 拒绝切换请求 (SW=" + hexByte(status >> 8) +
                    hexByte(status & 0xff) + ")";
    finishJob(false, detail);
    message = detail;
    return false;
  }
  // 超时、91xx 或无结果对象都可能表示命令已执行；绝不重发，重置后以列表状态为准。
  modemReady = false;
  // Withdraw the old Profile and ICCID before power-cycling. Any SMS received
  // during recovery must never inherit receiver metadata from the old Profile.
  simManagerInvalidate();
  pinMode(MODEM_EN_PIN, OUTPUT);
  digitalWrite(MODEM_EN_PIN, LOW);
  setPhase("radio_off", transmitted ? "请求已接受，正在重置 SIM 与模组" : "响应不确定，正在重置后核对实际状态");
  savePending(true, job.targetId, job.phase);
  switchState = SWITCH_POWER_OFF;
  message = "切换任务已创建";
  return true;
}

void esimManagerLoop() {
  if (switchState == SWITCH_IDLE) {
    reconcileDelayedSuccess();
    return;
  }
  unsigned long now = millis();

  switch (switchState) {
    case SWITCH_POWER_OFF:
      if (now - job.phaseAt >= 1500) {
        digitalWrite(MODEM_EN_PIN, HIGH);
        setPhase("sim_reset", "模组已重新上电，等待 SIM 初始化");
        switchState = SWITCH_BOOT_WAIT;
      }
      break;
    case SWITCH_BOOT_WAIT:
      if (now - job.phaseAt >= 6500) {
        setPhase("waiting_sim", "等待模组 AT 端口就绪");
        switchState = SWITCH_WAIT_AT;
      }
      break;
    case SWITCH_WAIT_AT:
      if (!job.lastAttemptAt || now - job.lastAttemptAt >= 1200) {
        job.lastAttemptAt = now;
        if (sendATandWaitOK("AT", 800)) {
          configureAttempts = 0;
          setPhase("configuring", "正在恢复短信上报配置");
          switchState = SWITCH_CONFIGURE;
        }
      }
      break;
    case SWITCH_CONFIGURE: {
      if (job.lastAttemptAt && now - job.lastAttemptAt < 1500) break;
      job.lastAttemptAt = now;
      ++configureAttempts;
      sendATandWaitOK("AT+CMEE=1", 1200);
      if (modemSupportsPdpContextControl()) {
        sendATandWaitOK("AT+CGACT=0,1", 3000);
      }
      String iccidResponse = sendATCommand("AT+ICCID", 1800);
      simManagerCaptureIccid(iccidResponse);
      bool cnmi = sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1500);
      bool pdu = sendATandWaitOK("AT+CMGF=0", 1500);
      if (!cnmi || !pdu) {
        if (configureAttempts < 3) {
          job.message = "短信配置恢复失败，正在重试 " + String(configureAttempts) + "/3";
          break;
        }
        // The eUICC command may already have succeeded. Keep the pending marker
        // until BF2D confirms the active profile, even when SMS setup failed.
        job.smsConfigWarning = true;
        job.warning = true;
        setPhase("verifying", "短信配置暂未恢复，继续核对目标 Profile");
        switchState = SWITCH_VERIFY;
        break;
      }
      sendATandWaitOK("AT+CEREG=2", 1200);
      setPhase("verifying", "正在核对目标 Profile 是否生效");
      switchState = SWITCH_VERIFY;
      break;
    }
    case SWITCH_VERIFY:
    case SWITCH_RECOVER_VERIFY: {
      String error;
      if (!readProfiles(error)) {
        if (now - job.phaseAt > 20000) finishJob(false, "无法核对 Profile：" + error);
        break;
      }
      if (!targetIsEnabled()) {
        finishJob(false, "eUICC 未启用目标 Profile，已停止切换");
        break;
      }
      job.profileConfirmed = true;
      savePending(true, job.targetId, "auto_select");
      setPhase("auto_select", "Profile 已切换，正在恢复自动选网");
      switchState = SWITCH_AUTO_SELECT;
      String autoMessage;
      if (!operatorManagerStartAutoForEsim(autoMessage)) {
        job.autoSelectWarning = true;
        job.warning = true;
        setPhase("registering", "自动选网暂未启动，继续等待蜂窝网络注册");
        switchState = SWITCH_WAIT_NETWORK;
      }
      break;
    }
    case SWITCH_AUTO_SELECT:
      if (operatorManagerIsBusy()) break;
      if (operatorManagerLastJobSucceeded()) {
        modemReady = true;
        job.autoSelectWarning = false;
        job.networkWarning = false;
        job.warning = job.smsConfigWarning;
        finishJob(true,
                  job.smsConfigWarning ? "Profile 已切换并注册网络，但短信上报配置仍在恢复"
                                       : "Profile 切换完成，已恢复自动选网并注册网络",
                  job.smsConfigWarning);
      } else {
        job.autoSelectWarning = true;
        job.warning = true;
        String detail = operatorManagerLastJobMessage();
        setPhase("registering", detail.length()
                                    ? "自动选网未完成（" + detail + "），继续等待网络注册"
                                    : "自动选网未完成，继续等待网络注册");
        switchState = SWITCH_WAIT_NETWORK;
      }
      break;
    case SWITCH_WAIT_NETWORK:
      if (!job.lastAttemptAt || now - job.lastAttemptAt >= 3000) {
        job.lastAttemptAt = now;
        if (waitCEREG()) {
          modemReady = true;
          job.networkWarning = false;
          job.warning = job.smsConfigWarning || job.autoSelectWarning;
          String registeredMessage = job.smsConfigWarning
                                         ? "Profile 已切换并注册网络，但短信上报配置恢复失败，请重启模组"
                                         : (job.autoSelectWarning
                                                ? "Profile 已切换并注册网络，但自动选网模式尚未确认"
                                                : "Profile 切换完成，网络已注册");
          finishJob(true,
                    registeredMessage, job.smsConfigWarning || job.autoSelectWarning);
        } else if (now - job.phaseAt > LATE_REGISTRATION_TIMEOUT_MS) {
          job.networkWarning = true;
          String timeoutMessage = job.smsConfigWarning
                                      ? "Profile 已切换；短信配置恢复失败且网络注册超时"
                                      : (job.autoSelectWarning
                                             ? "Profile 已切换，但自动选网与网络注册未完成"
                                             : "Profile 已切换，但网络注册超时");
          finishJob(true,
                    timeoutMessage, true);
        }
      }
      break;
    default: break;
  }

  if (job.active && !operatorManagerIsBusy() && now - job.startedAt > SWITCH_TOTAL_TIMEOUT_MS) {
    if (job.profileConfirmed) {
      job.networkWarning = true;
      finishJob(true, "Profile 已切换，但自动选网与网络恢复超时", true);
    } else {
      finishJob(false, "切换超时，请检查 SIM 和网络状态");
    }
  }
}

bool esimIsBusy() {
  return job.active || switchState != SWITCH_IDLE;
}

String esimActiveProfileLabel() {
  for (uint8_t i = 0; i < profileCount; ++i) {
    if (!profiles[i].enabled) continue;
    if (profiles[i].name.length()) return profiles[i].name;
    if (profiles[i].provider.length()) return profiles[i].provider;
  }
  if (esimCapability == ESIM_CAPABILITY_UNSUPPORTED && simManagerIsReady()) return "实体 SIM";
  return "";
}

String esimActiveProfileSmsLabel() {
  for (uint8_t i = 0; i < profileCount; ++i) {
    if (!profiles[i].enabled) continue;

    String label = profiles[i].name.length() ? profiles[i].name : profiles[i].provider;
    if (!label.length()) label = "eSIM";
    String tail = profiles[i].iccid.length() >= 4
                      ? profiles[i].iccid.substring(profiles[i].iccid.length() - 4)
                      : simManagerIccidTail();
    if (tail.length() != 4) return label;

    const String suffix = " · 尾号 " + tail;
    // StoredSms::profile has room for 48 UTF-8 bytes. Preserve the unique tail
    // instead of silently truncating it when a provider supplies a long name.
    if (label.length() + suffix.length() <= 48) return label + suffix;
    return String("eSIM") + suffix;
  }
  String tail = simManagerIccidTail();
  if (tail.length() == 4) return String("SIM · 尾号 ") + tail;
  if (esimCapability == ESIM_CAPABILITY_UNSUPPORTED) return "实体 SIM";
  return "接收卡未识别";
}

String esimJobJson() {
  int progress = 0;
  if (job.phase == "enable_sent") progress = 12;
  else if (job.phase == "radio_off") progress = 24;
  else if (job.phase == "sim_reset") progress = 38;
  else if (job.phase == "waiting_sim") progress = 52;
  else if (job.phase == "configuring") progress = 64;
  else if (job.phase == "verifying") progress = 78;
  else if (job.phase == "auto_select") progress = 86;
  else if (job.phase == "registering") progress = 94;
  else if (job.phase == "complete") progress = 100;
  String json = "{\"id\":" + String(job.id) + ",\"active\":" + (job.active ? "true" : "false") +
                ",\"running\":" + (job.active ? "true" : "false") + ",\"progress\":" + String(progress) +
                ",\"done\":" + (job.done ? "true" : "false") + ",\"ok\":" + (job.ok ? "true" : "false") +
                ",\"warning\":" + (job.warning ? "true" : "false") + ",\"phase\":\"" +
                jsonEscape(job.phase) + "\",\"targetId\":\"" + jsonEscape(job.targetId) +
                "\",\"message\":\"" + jsonEscape(job.message) + "\",\"elapsed\":" +
                String(job.startedAt ? (millis() - job.startedAt) / 1000 : 0) + "}";
  return json;
}
