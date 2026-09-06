#include "push_protocol.h"
#include <ArduinoJson.h>
#include <algorithm>

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
  const std::string heading = "短信通知\n发送者：" + sender + "\n时间：" + timestamp + "\n";
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
