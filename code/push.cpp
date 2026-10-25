#include "push.h"
#include "web_handlers.h"
#include "config.h"
#include "esim_manager.h"
#include "sim_manager.h"
#include "operator_manager.h"
#include "push_protocol.h"
#include <HTTPClient.h>
#include <mbedtls/md.h>
#include <base64.h>
#include <sys/time.h>

namespace {
// HTTPClient handles chunked transfer decoding through writeToStream. Stop at a
// fixed response limit, including when Content-Length is unknown.
class BoundedPushResponse : public Stream {
 public:
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t length) override {
    const size_t room = 4096 - value.length();
    const size_t accepted = length < room ? length : room;
    if (accepted && !value.concat(reinterpret_cast<const char*>(data), accepted)) return 0;
    return accepted;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  String value;
};

String pushResponse(HTTPClient& http) {
  if (http.getSize() > 4096) return "";
  BoundedPushResponse response;
  if (http.writeToStream(&response) < 0) return "";
  return response.value;
}
}  // namespace

// 发送邮件通知函数
bool sendEmailNotification(const char* subject, const char* body) {
  if (config.smtpServer.length() == 0 || config.smtpUser.length() == 0 ||
      config.smtpPass.length() == 0 || config.smtpSendTo.length() == 0) {
    logCaptureLn(String("邮件配置不完整，跳过发送"));
    return false;
  }

  auto statusCallback = [](SMTPStatus status) {
    logCaptureLn(String(status.text));
  };
  smtp.connect(config.smtpServer.c_str(), config.smtpPort, statusCallback);
  if (smtp.isConnected()) {
    if (!smtp.authenticate(config.smtpUser.c_str(), config.smtpPass.c_str(), readymail_auth_password)) {
      logCaptureLn("邮件认证失败");
      return false;
    }

    SMTPMessage msg;
    String from = "sms notify <"; from += config.smtpUser; from += ">";
    msg.headers.add(rfc822_from, from.c_str());
    String to = "your_email <"; to += config.smtpSendTo; to += ">";
    msg.headers.add(rfc822_to, to.c_str());
    msg.headers.add(rfc822_subject, subject);
    msg.text.body(body);
    msg.timestamp = time(nullptr);
    const bool sent = smtp.send(msg);
    logCaptureLn(sent ? "邮件服务器已接收" : "邮件发送失败");
    return sent;
  } else {
    logCaptureLn(String("邮件服务器连接失败"));
    return false;
  }
}

// URL编码辅助函数
String urlEncode(const String& str) {
  return String(pushprotocol::encodeUrl(str.c_str()).c_str());
}

// 钉钉签名函数（时间戳为UTC毫秒级）
String dingtalkSign(const String& secret, int64_t timestamp) {
  String stringToSign = String(timestamp) + "\n" + secret;

  uint8_t hmacResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)secret.c_str(), secret.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)stringToSign.c_str(), stringToSign.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);

  String base64Encoded = base64::encode(hmacResult, 32);
  return urlEncode(base64Encoded);
}

// 获取当前UTC毫秒级时间戳（用于钉钉签名）
int64_t getUtcMillis() {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0) {
    return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
  }
  // 如果获取失败，使用time()函数
  return (int64_t)time(nullptr) * 1000LL;
}

// JSON转义函数
String jsonEscape(const String& str) {
  return String(pushprotocol::escapeJson(str.c_str()).c_str());
}

String notificationDisplayTime(const char* timestamp) {
  return String(pushprotocol::displayTimestampChina(timestamp ? timestamp : "").c_str());
}

pushprotocol::NotificationContext notificationContext(bool complete, bool test) {
  pushprotocol::NotificationContext context;
  // All modem values are cached. Notification formatting never issues AT commands.
  if (simManagerIsPresent()) {
    context.receiver = simManagerPhoneNumber().c_str();
    context.profile = esimActiveProfileLabel().c_str();
    context.simType = esimCapabilityKnown()
        ? (esimIsSupported() ? "eSIM / eUICC" : "实体 SIM") : "SIM";
    context.carrier = operatorCurrentLabel().c_str();
    context.plmn = operatorCurrentNumeric().c_str();
    context.iccidTail = simManagerIccidTail().c_str();
  }
  context.device = config.brandTitle.c_str();
  char id[13];
  snprintf(id, sizeof(id), "%012llX", static_cast<unsigned long long>(ESP.getEfuseMac()));
  context.deviceId = id;
  context.model = detectedModemModel.c_str();
  context.complete = complete;
  context.test = test;
  return context;
}

// 发送单个推送通道
bool sendToChannel(const PushChannel& channel, const char* sender, const char* message,
                   const char* timestamp, const pushprotocol::NotificationContext& context) {
  if (!channel.enabled) return false;

  // 对于某些推送方式，URL可以为空（使用默认URL）
  bool needUrl = (channel.type == PUSH_TYPE_POST_JSON || channel.type == PUSH_TYPE_BARK ||
                  channel.type == PUSH_TYPE_GET || channel.type == PUSH_TYPE_DINGTALK ||
                  channel.type == PUSH_TYPE_CUSTOM || channel.type == PUSH_TYPE_WECOM);
  if (needUrl && channel.url.length() == 0) return false;

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  http.setReuse(false);
  String channelName = channel.name.length() > 0 ? channel.name : ("通道" + String(channel.type));
  logCaptureLn(String("发送到推送通道: " + channelName));

  int httpCode = 0;
  const String title = pushprotocol::notificationTitle(sender, context).c_str();
  const String text = pushprotocol::notificationText(sender, message, timestamp, context).c_str();
  const String titleEscaped = jsonEscape(title);
  const String textEscaped = jsonEscape(text);

  switch (channel.type) {
    case PUSH_TYPE_POST_JSON: {
      // 标准POST JSON格式
      http.begin(channel.url);
      http.addHeader("Content-Type", "application/json");
      String jsonData = pushprotocol::webhookJson(sender, message, timestamp, context).c_str();
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_BARK: {
      // Bark推送格式
      http.begin(channel.url);
      http.addHeader("Content-Type", "application/json");
      String jsonData = "{";
      jsonData += "\"title\":\"" + titleEscaped + "\",";
      jsonData += "\"body\":\"" + textEscaped + "\"";
      jsonData += "}";
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_GET: {
      // GET请求，参数放URL里
      String getUrl = channel.url;
      if (getUrl.indexOf('?') == -1) {
        getUrl += "?";
      } else {
        getUrl += "&";
      }
      getUrl += "sender=" + urlEncode(String(sender));
      getUrl += "&message=" + urlEncode(String(message));
      getUrl += "&timestamp=" + urlEncode(String(timestamp));
      getUrl += "&receiver=" + urlEncode(String(context.receiver.c_str()));
      getUrl += "&profile=" + urlEncode(String(context.profile.c_str()));
      getUrl += "&carrier=" + urlEncode(String(context.carrier.c_str()));
      getUrl += "&plmn=" + urlEncode(String(context.plmn.c_str()));
      getUrl += "&simType=" + urlEncode(String(context.simType.c_str()));
      getUrl += "&iccidTail=" + urlEncode(String(context.iccidTail.c_str()));
      getUrl += "&device=" + urlEncode(String(context.device.c_str()));
      getUrl += "&deviceId=" + urlEncode(String(context.deviceId.c_str()));
      getUrl += "&model=" + urlEncode(String(context.model.c_str()));
      http.begin(getUrl);
      httpCode = http.GET();
      break;
    }

    case PUSH_TYPE_DINGTALK: {
      // 钉钉机器人
      String webhookUrl = channel.url;

      // 如果配置了secret，需要添加签名
      if (channel.key1.length() > 0) {
        // 获取UTC毫秒级时间戳（钉钉要求）
        int64_t ts = getUtcMillis();
        String sign = dingtalkSign(channel.key1, ts);
        if (webhookUrl.indexOf('?') == -1) {
          webhookUrl += "?";
        } else {
          webhookUrl += "&";
        }
        // 使用字符串拼接避免int64_t转换问题
        char tsBuf[21];
        snprintf(tsBuf, sizeof(tsBuf), "%lld", ts);
        webhookUrl += "timestamp=" + String(tsBuf) + "&sign=" + sign;
      }

      http.begin(webhookUrl);
      http.addHeader("Content-Type", "application/json");
      String jsonData = "{\"msgtype\":\"text\",\"text\":{\"content\":\"";
      jsonData += textEscaped;
      jsonData += "\"}}";
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_PUSHPLUS: {
      // PushPlus
      String pushUrl = channel.url.length() > 0 ? channel.url : "http://www.pushplus.plus/send";
      http.begin(pushUrl);
      http.addHeader("Content-Type", "application/json");
      // 发送渠道
      String channelValue = "wechat";
      if (channel.key2.length() > 0) {
          // 仅支持微信公众号（wechat）、浏览器插件（extension）和 PushPlus App（app）三种渠道
          if (channel.key2 == "wechat" || channel.key2 == "extension" || channel.key2 == "app") {
              channelValue = channel.key2;
          } else {
              logCaptureLn(String("Invalid PushPlus channel '" + channel.key2 + "'. Using default 'wechat'."));
          }
      }
      String jsonData = "{";
      jsonData += "\"token\":\"" + jsonEscape(channel.key1) + "\",";
      jsonData += "\"title\":\"" + titleEscaped + "\",";
      jsonData += "\"content\":\"" + String(pushprotocol::escapeJson(pushprotocol::escapeHtml(text.c_str())).c_str()) + "\",";
      jsonData += "\"channel\":\"" + channelValue + "\"";
      jsonData += "}";
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_SERVERCHAN: {
      // Server酱
      String scUrl = channel.url.length() > 0 ? channel.url : ("https://sctapi.ftqq.com/" + channel.key1 + ".send");
      http.begin(scUrl);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      String postData = "title=" + urlEncode(title);
      postData += "&desp=" + urlEncode(text);
      httpCode = http.POST(postData);
      break;
    }

    case PUSH_TYPE_CUSTOM: {
      // 自定义模板
      if (channel.customBody.length() == 0) {
        logCaptureLn(String("自定义模板为空，跳过"));
        return false;
      }
      http.begin(channel.url);
      http.addHeader("Content-Type", "application/json");
      String body = pushprotocol::renderJsonTemplate(channel.customBody.c_str(), sender,
                                                    message, timestamp, context).c_str();
      if (body.isEmpty()) {
        logCaptureLn("自定义通知展开后超过 16 KB，已停止发送");
        return false;
      }
      httpCode = http.POST(body);
      break;
    }

    case PUSH_TYPE_FEISHU: {
      // 飞书机器人
      String webhookUrl = channel.url;
      String jsonData = "{";

      // 如果配置了secret，需要添加签名
      if (channel.key1.length() > 0) {
        // 飞书使用秒级时间戳
        int64_t ts = time(nullptr);
        // 飞书签名: base64(hmac-sha256(timestamp + "\n" + secret, secret))
        String stringToSign = String(ts) + "\n" + channel.key1;
        uint8_t hmacResult[32];
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
        mbedtls_md_hmac_starts(&ctx, (const unsigned char*)channel.key1.c_str(), channel.key1.length());
        mbedtls_md_hmac_update(&ctx, (const unsigned char*)stringToSign.c_str(), stringToSign.length());
        mbedtls_md_hmac_finish(&ctx, hmacResult);
        mbedtls_md_free(&ctx);
        String sign = base64::encode(hmacResult, 32);

        jsonData += "\"timestamp\":\"" + String(ts) + "\",";
        jsonData += "\"sign\":\"" + sign + "\",";
      }

      // 飞书消息体
      jsonData += "\"msg_type\":\"text\",";
      jsonData += "\"content\":{\"text\":\"";
      jsonData += textEscaped;
      jsonData += "\"}}";

      http.begin(webhookUrl);
      http.addHeader("Content-Type", "application/json");
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_WECOM: {
      if (!pushprotocol::isWecomWebhook(channel.url.c_str())) {
        logCaptureLn("企业微信 Webhook 地址无效");
        return false;
      }
      auto payloads = pushprotocol::wecomPayloads(sender, message, timestamp, context);
      if (payloads.empty()) {
        logCaptureLn("企业微信消息编码失败");
        return false;
      }
      for (size_t i = 0; i < payloads.size(); ++i) {
        HTTPClient wecom;
        wecom.setConnectTimeout(5000);
        wecom.setTimeout(5000);
        wecom.setReuse(false);
        if (!wecom.begin(channel.url)) {
          logCaptureLn("企业微信连接初始化失败");
          return false;
        }
        wecom.addHeader("Content-Type", "application/json; charset=utf-8");
        int code = wecom.POST(String(payloads[i].c_str()));
        String response;
        if (code > 0) response = pushResponse(wecom);
        wecom.end();
        auto result = pushprotocol::evaluateResponse(code, response.c_str(), true);
        logCaptureF("[企业微信] 第 %u/%u 段 HTTP %d，%s：%s\n",
                    unsigned(i + 1), unsigned(payloads.size()), code,
                    result.ok ? "投递成功" : "投递失败", result.detail.c_str());
        if (!result.ok) return false;
      }
      return true;
    }

    case PUSH_TYPE_GOTIFY: {
      // Gotify 推送
      String gotifyUrl = channel.url;
      // 确保URL以/结尾
      if (!gotifyUrl.endsWith("/")) gotifyUrl += "/";
      gotifyUrl += "message?token=" + channel.key1;

      http.begin(gotifyUrl);
      http.addHeader("Content-Type", "application/json");
      String jsonData = "{";
      jsonData += "\"title\":\"" + titleEscaped + "\",";
      jsonData += "\"message\":\"" + textEscaped + "\",";
      jsonData += "\"priority\":5";
      jsonData += "}";
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_TELEGRAM: {
      // Telegram Bot 推送
      // channel.key1 是 Chat ID, channel.key2 是 Bot Token
      String tgBaseUrl = channel.url.length() > 0 ? channel.url : "https://api.telegram.org";
      if (tgBaseUrl.endsWith("/")) tgBaseUrl.remove(tgBaseUrl.length() - 1);

      String tgUrl = tgBaseUrl + "/bot" + channel.key2 + "/sendMessage";
      http.begin(tgUrl);
      http.addHeader("Content-Type", "application/json");

      String jsonData = "{";
      jsonData += "\"chat_id\":\"" + jsonEscape(channel.key1) + "\",";
      jsonData += "\"text\":\"" + textEscaped + "\"";
      jsonData += "}";

      httpCode = http.POST(jsonData);
      break;
    }

    default:
      logCaptureLn(String("未知推送类型"));
      return false;
  }

  if (httpCode > 0) {
    logCaptureF("[%s] 响应码: %d\n", channelName.c_str(), httpCode);
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
      logCaptureLn(String("通知平台已响应"));
    }
  } else {
    logCaptureF("[%s] HTTP请求失败: %s\n", channelName.c_str(), http.errorToString(httpCode).c_str());
  }
  const String response = httpCode > 0 ? pushResponse(http) : String("");
  const auto result = pushprotocol::evaluateResponse(httpCode, response.c_str(), false);
  logCaptureF("[%s] %s\n", channelName.c_str(), result.detail.c_str());
  http.end();
  return result.ok;
}

// 发送短信到所有启用的推送通道
void sendSMSToServer(const char* sender, const char* message, const char* timestamp,
                     const pushprotocol::NotificationContext& context) {
  if (WiFi.status() != WL_CONNECTED) {
    logCaptureLn(String("WiFi未连接，跳过推送"));
    return;
  }

  bool hasEnabledChannel = false;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      hasEnabledChannel = true;
      break;
    }
  }

  if (!hasEnabledChannel) {
    logCaptureLn(String("没有启用的推送通道"));
    return;
  }

  logCaptureLn(String("\n=== 开始多通道推送 ==="));
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      sendToChannel(config.pushChannels[i], sender, message, timestamp, context);
      delay(100); // 短暂延迟避免请求过快
    }
  }
  logCaptureLn(String("=== 多通道推送完成 ===\n"));
}
