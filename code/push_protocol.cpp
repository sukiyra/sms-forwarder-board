#include "push_protocol.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace pushprotocol {

std::string escapeJson(const std::string& text) {
  static const char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(text.size() + 16);
  for (unsigned char c : text) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (c < 0x20) {
          result += "\\u00";
          result += hex[c >> 4];
          result += hex[c & 15];
        } else result += static_cast<char>(c);
    }
  }
  return result;
}

std::string encodeUrl(const std::string& text) {
  static const char hex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : text) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      result += static_cast<char>(c);
    } else {
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 15];
    }
  }
  return result;
}

namespace {

bool allDigits(const std::string& value) {
  if (value.empty()) return false;
  for (char c : value) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

bool leapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int monthDays(int year, int month) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && leapYear(year) ? 29 : days[month - 1];
}

std::string beijingText(int year, int month, int day, int hour, int minute, int second) {
  char value[64] = {};
  std::snprintf(value, sizeof(value), "%04d-%02d-%02d %02d:%02d:%02d（北京时间）",
                year, month, day, hour, minute, second);
  return value;
}

}  // namespace

std::string displayTimestampChina(const std::string& timestamp) {
  // New SMS records use fixed UTC ISO-8601. Keep that representation in the
  // store/API, but render human notifications in the product's China timezone.
  if (timestamp.size() == 20 && timestamp[4] == '-' && timestamp[7] == '-' &&
      timestamp[10] == 'T' && timestamp[13] == ':' && timestamp[16] == ':' &&
      timestamp[19] == 'Z') {
    const std::string digits = timestamp.substr(0, 4) + timestamp.substr(5, 2) +
                               timestamp.substr(8, 2) + timestamp.substr(11, 2) +
                               timestamp.substr(14, 2) + timestamp.substr(17, 2);
    if (!allDigits(digits)) return timestamp;
    int year = std::atoi(timestamp.substr(0, 4).c_str());
    int month = std::atoi(timestamp.substr(5, 2).c_str());
    int day = std::atoi(timestamp.substr(8, 2).c_str());
    int hour = std::atoi(timestamp.substr(11, 2).c_str());
    int minute = std::atoi(timestamp.substr(14, 2).c_str());
    int second = std::atoi(timestamp.substr(17, 2).c_str());
    if (year < 2020 || month < 1 || month > 12 || day < 1 ||
        day > monthDays(year, month) || hour > 23 || minute > 59 || second > 59) {
      return timestamp;
    }
    hour += 8;
    if (hour >= 24) {
      hour -= 24;
      if (++day > monthDays(year, month)) {
        day = 1;
        if (++month > 12) {
          month = 1;
          ++year;
        }
      }
    }
    return beijingText(year, month, day, hour, minute, second);
  }

  // Push-channel test messages historically pass a Unix timestamp. Format it
  // consistently too, while retaining unknown/SMSC timestamp forms verbatim.
  if ((timestamp.size() == 10 || timestamp.size() == 13) && allDigits(timestamp)) {
    long long seconds = std::strtoll(timestamp.c_str(), nullptr, 10);
    if (timestamp.size() == 13) seconds /= 1000;
    if (seconds >= 1577836800LL) {
      std::time_t adjusted = static_cast<std::time_t>(seconds + 8LL * 3600LL);
      const std::tm* local = std::gmtime(&adjusted);
      if (local) {
        return beijingText(local->tm_year + 1900, local->tm_mon + 1, local->tm_mday,
                           local->tm_hour, local->tm_min, local->tm_sec);
      }
    }
  }
  return timestamp;
}

std::vector<std::string> splitUtf8(const std::string& text, size_t maxBytes) {
  std::vector<std::string> parts;
  if (maxBytes < 4) return parts;
  if (text.empty()) return {""};
  for (size_t start = 0; start < text.size();) {
    size_t end = std::min(start + maxBytes, text.size());
    while (end < text.size() && end > start &&
           (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    if (end == start) return {};  // Invalid UTF-8 input: never spin forever.
    parts.push_back(text.substr(start, end - start));
    start = end;
  }
  return parts;
}

bool isWecomWebhook(const std::string& url) {
  const std::string prefix = "https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=";
  return url.compare(0, prefix.size(), prefix) == 0 && url.size() > prefix.size() &&
         url.find_first_of(" \r\n\t#") == std::string::npos;
}

std::vector<std::string> wecomPayloads(const std::string& sender,
                                     const std::string& message,
                                     const std::string& timestamp) {
  const std::string heading = "短信通知\n发送者：" + sender + "\n时间：" +
                              displayTimestampChina(timestamp) + "\n";
  if (heading.size() > 512) return {};
  // Leave room for part numbering. WeCom's text.content limit is in UTF-8 bytes.
  auto chunks = splitUtf8(message, 2048 - heading.size() - 48);
  std::vector<std::string> payloads;
  for (size_t i = 0; i < chunks.size(); ++i) {
    std::string content = heading;
    if (chunks.size() > 1) content += "分段 " + std::to_string(i + 1) + "/" + std::to_string(chunks.size()) + "\n";
    content += "内容：" + chunks[i];
    payloads.push_back("{\"msgtype\":\"text\",\"text\":{\"content\":\"" + escapeJson(content) + "\"}}");
  }
  return payloads;
}

DeliveryResult evaluateResponse(int httpCode, const std::string& body, bool requireWecomResult) {
  if (httpCode <= 0) return {false, true, "网络请求失败（" + std::to_string(httpCode) + "）"};
  if (httpCode < 200 || httpCode >= 300) {
    return {false, httpCode == 408 || httpCode == 429 || httpCode >= 500,
            "HTTP " + std::to_string(httpCode)};
  }
  JsonDocument doc;
  const auto error = deserializeJson(doc, body);
  if (error) {
    return requireWecomResult ? DeliveryResult{false, false, "企业微信返回无效 JSON"}
                             : DeliveryResult{true, false, "HTTP " + std::to_string(httpCode)};
  }
  const auto code = doc["errcode"];
  if (requireWecomResult || !code.isNull()) {
    if (!code.is<int>()) return {false, false, "响应缺少有效的 errcode"};
    const int value = code.as<int>();
    if (value != 0) {
      std::string detail = "接口拒绝（" + std::to_string(value) + "）";
      const char* message = doc["errmsg"] | "";
      auto parts = splitUtf8(message, 160);
      if (*message && !parts.empty()) detail += ": " + parts.front();
      return {false, value == -1 || value == 45009 || value == 45011, detail};
    }
    return {true, false, "企业微信已接收（errcode=0）"};
  }
  return {true, false, "HTTP " + std::to_string(httpCode)};
}

}  // namespace pushprotocol
