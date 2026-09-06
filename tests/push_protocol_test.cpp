#include "push_protocol.h"
#include <ArduinoJson.h>
#include <cassert>
#include <iostream>
#include <limits>

using namespace pushprotocol;

int main() {
  const std::string special = std::string("中文🙂 \"引号\"\\\n\r\t") + char(1) + char(31);
  JsonDocument doc;
  assert(!deserializeJson(doc, "{\"text\":\"" + escapeJson(special) + "\"}"));
  assert(doc["text"].as<std::string>() == special);
  assert(encodeUrl("中文 +/&") == "%E4%B8%AD%E6%96%87%20%2B%2F%26");
  assert(displayTimestampChina("2026-09-06T12:20:02Z") ==
         "2026-09-06 20:20:02（北京时间）");
  assert(displayTimestampChina("2026-12-31T20:20:02Z") ==
         "2027-01-01 04:20:02（北京时间）");
  assert(displayTimestampChina("2024-02-29T20:00:00Z") ==
         "2024-03-01 04:00:00（北京时间）");
  assert(displayTimestampChina("1767225600") ==
         "2026-01-01 08:00:00（北京时间）");
  assert(displayTimestampChina("invalid") == "invalid");

  assert(isWecomWebhook("https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=test-key"));
  assert(!isWecomWebhook("https://qyapi.weixin.qq.com.evil.test/cgi-bin/webhook/send?key=x"));
  assert(!isWecomWebhook("http://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=x"));
  assert(!isWecomWebhook("https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key="));
  assert(!isWecomWebhook("https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=x\n"));

  std::string longText;
  for (int i = 0; i < 1000; ++i) longText += "测🙂试";
  for (size_t boundary : {4U, 5U, 7U, 2048U}) {
    std::string joined;
    for (const auto& part : splitUtf8(longText, boundary)) {
      assert(part.size() <= boundary && !part.empty());
      assert((static_cast<unsigned char>(part.front()) & 0xc0) != 0x80);
      joined += part;
    }
    assert(joined == longText);
  }
  auto parts = wecomPayloads("sender", longText, "2026-09-06T12:20:02Z");
  assert(parts.size() > 1);
  std::string reconstructed;
  for (size_t i = 0; i < parts.size(); ++i) {
    assert(!deserializeJson(doc, parts[i]));
    assert(doc["msgtype"] == "text");
    const auto content = doc["text"]["content"].as<std::string>();
    assert(content.size() <= 2048);
    assert(content.find("2026-09-06 20:20:02（北京时间）") != std::string::npos);
    assert(content.find("分段 " + std::to_string(i + 1) + "/" + std::to_string(parts.size())) != std::string::npos);
    const auto marker = content.find("内容：");
    assert(marker != std::string::npos);
    reconstructed += content.substr(marker + std::string("内容：").size());
  }
  assert(reconstructed == longText);
  assert(!deserializeJson(doc, wecomPayloads("sender", special, "time").front()));
  assert(doc["text"]["content"].as<std::string>().find(special) != std::string::npos);
  assert(wecomPayloads("sender", "", "time").size() == 1);

  assert(evaluateResponse(200, R"({"errcode":0,"errmsg":"ok"})", true).ok);
  const auto rejected = evaluateResponse(200, R"({"errcode":40008,"errmsg":"invalid message type"})", true);
  assert(!rejected.ok && !rejected.retryable);
  assert(rejected.detail.find("40008") != std::string::npos);
  assert(!evaluateResponse(200, "{}", true).ok);
  assert(!evaluateResponse(200, R"({"errcode":false})", true).ok);
  assert(!evaluateResponse(200, R"({"errcode":"0"})", true).ok);
  assert(!evaluateResponse(200, "<html>proxy</html>", true).ok);
  assert(!evaluateResponse(200, R"({"nested":{"errcode":0}})", true).ok);
  assert(evaluateResponse(200, R"({"errcode":45009})", true).retryable);
  assert(evaluateResponse(-1, "", true).retryable);
  assert(evaluateResponse(503, "", true).retryable);
  assert(evaluateResponse(429, "", true).retryable);
  assert(!evaluateResponse(401, "", true).retryable);
  assert(evaluateResponse(204, "", false).ok);
  assert(!evaluateResponse(200, R"({"errcode":40008})", false).ok);

  RetrySchedule schedule;
  const uint32_t now = std::numeric_limits<uint32_t>::max() - 5000U;
  schedule.readyAt = now;
  assert(schedule.ready(now));
  assert(schedule.retry(now));
  assert(!schedule.ready(now + 9999U));
  assert(schedule.ready(now + 10000U));
  assert(schedule.retry(now + 10000U));
  assert(!schedule.ready(now + 39999U));
  assert(schedule.ready(now + 40000U));
  assert(!schedule.retry(now + 40000U));

  std::cout << "PASS: timestamp display, JSON escaping, UTF-8 segmentation, payload limits, webhook validation, API errors and retries\n";
}
