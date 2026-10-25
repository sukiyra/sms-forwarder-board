#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pushprotocol {

struct DeliveryResult {
  bool ok;
  bool retryable;
  std::string detail;
};

struct NotificationContext {
  std::string receiver;
  std::string profile;
  std::string simType;
  std::string carrier;
  std::string plmn;
  std::string iccidTail;
  std::string device;
  std::string deviceId;
  std::string model;
  bool complete = true;
  bool test = false;
};

std::string escapeJson(const std::string& text);
std::string encodeUrl(const std::string& text);
std::string escapeHtml(const std::string& text);
std::string displayTimestampChina(const std::string& timestamp);
std::string notificationHeading(const std::string& sender,
                                const std::string& timestamp,
                                const NotificationContext& context);
std::string notificationText(const std::string& sender, const std::string& message,
                             const std::string& timestamp,
                             const NotificationContext& context);
std::string notificationTitle(const std::string& sender,
                              const NotificationContext& context);
std::string webhookJson(const std::string& sender, const std::string& message,
                        const std::string& timestamp,
                        const NotificationContext& context);
std::string renderJsonTemplate(const std::string& body, const std::string& sender,
                               const std::string& message,
                               const std::string& timestamp,
                               const NotificationContext& context);
std::vector<std::string> splitUtf8(const std::string& text, size_t maxBytes);
std::vector<std::string> wecomPayloads(const std::string& sender,
                                     const std::string& message,
                                     const std::string& timestamp,
                                     const NotificationContext& context = {});
bool isWecomWebhook(const std::string& url);
DeliveryResult evaluateResponse(int httpCode, const std::string& body,
                                bool requireWecomResult);

struct RetrySchedule {
  uint8_t failures = 0;
  uint32_t readyAt = 0;
  bool ready(uint32_t now) const { return static_cast<int32_t>(now - readyAt) >= 0; }
  bool retry(uint32_t now) {
    if (++failures >= 3) return false;
    readyAt = now + (failures == 1 ? 10000U : 30000U);
    return true;
  }
};

}  // namespace pushprotocol
