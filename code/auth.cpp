#include "auth.h"
#include "config.h"
#include "push.h"
#include "web_handlers.h"

namespace {

constexpr uint8_t MAX_SESSIONS = 2;
constexpr unsigned long IDLE_TIMEOUT_MS = 30UL * 60UL * 1000UL;
constexpr unsigned long ABS_TIMEOUT_MS = 8UL * 60UL * 60UL * 1000UL;
constexpr unsigned long LOGIN_LOCK_MS = 30UL * 1000UL;

struct WebSession {
  bool active = false;
  String token;
  IPAddress remoteIp;
  unsigned long createdAt = 0;
  unsigned long lastSeenAt = 0;
};

WebSession sessions[MAX_SESSIONS];
uint8_t failedLogins = 0;
unsigned long lockedUntil = 0;

String randomHex(size_t bytes) {
  static const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; i += 4) {
    uint32_t value = esp_random();
    for (uint8_t j = 0; j < 4 && i + j < bytes; ++j) {
      uint8_t b = (value >> (j * 8)) & 0xff;
      out += hex[b >> 4];
      out += hex[b & 0x0f];
    }
  }
  return out;
}

bool secureEquals(const String &a, const String &b) {
  size_t maxLen = max(a.length(), b.length());
  uint8_t diff = static_cast<uint8_t>(a.length() ^ b.length());
  for (size_t i = 0; i < maxLen; ++i) {
    char ac = i < a.length() ? a[i] : 0;
    char bc = i < b.length() ? b[i] : 0;
    diff |= static_cast<uint8_t>(ac ^ bc);
  }
  return diff == 0;
}


String bearerToken() {
  if (!server.hasHeader("Authorization")) return "";
  String value = server.header("Authorization");
  value.trim();
  const String prefix = "Bearer ";
  if (!value.startsWith(prefix)) return "";
  String token = value.substring(prefix.length());
  token.trim();
  return token;
}

bool isExpired(const WebSession &session) {
  unsigned long now = millis();
  return static_cast<unsigned long>(now - session.lastSeenAt) > IDLE_TIMEOUT_MS ||
         static_cast<unsigned long>(now - session.createdAt) > ABS_TIMEOUT_MS;
}

WebSession *currentSession(bool touch = true) {
  String token = bearerToken();
  if (token.length() != 32) return nullptr;
  IPAddress remote = server.client().remoteIP();
  for (auto &session : sessions) {
    if (!session.active) continue;
    if (isExpired(session)) {
      session.active = false;
      session.token = "";
      continue;
    }
    if (session.remoteIp != remote || !secureEquals(session.token, token)) continue;
    if (touch) session.lastSeenAt = millis();
    return &session;
  }
  return nullptr;
}

WebSession *allocateSession() {
  for (auto &session : sessions) {
    if (!session.active || isExpired(session)) return &session;
  }
  WebSession *oldest = &sessions[0];
  for (auto &session : sessions) {
    if (static_cast<int32_t>(session.lastSeenAt - oldest->lastSeenAt) < 0) {
      oldest = &session;
    }
  }
  return oldest;
}


}  // namespace

void authBegin() {
  static const char *headers[] = {"Authorization"};
  server.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));
}

bool authIsAuthenticated() {
  return currentSession() != nullptr;
}

bool authRequire() {
  String token = bearerToken();
  if (token.length() >= 16) {
    if (config.apiToken.length() > 0 && secureEquals(token, config.apiToken)) return true;
    if (currentSession()) return true;
  }
  sendJsonResponse(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
  return false;
}

bool authRequireCsrf() {
  return authRequire();
}

void authInvalidateAll() {
  for (auto &session : sessions) {
    session.active = false;
    session.token = "";
  }
}

void handleApiSession() {
  WebSession *session = currentSession();
  if (!session) {
    sendJsonResponse(200, "{\"authenticated\":false}");
    return;
  }
  String json = "{\"authenticated\":true,\"user\":\"" + jsonEscape(config.webUser) +
                "\",\"token\":\"" + session->token + "\",\"mustChangePassword\":" +
                String(config.webPass == DEFAULT_WEB_PASS ? "true" : "false") + "}";
  sendJsonResponse(200, json);
}

void handleApiLogin() {
  unsigned long now = millis();
  if (lockedUntil && static_cast<long>(lockedUntil - now) > 0) {
    unsigned long retry = (lockedUntil - now + 999) / 1000;
    sendJsonResponse(429, "{\"ok\":false,\"error\":\"locked\",\"retryAfter\":" + String(retry) + "}");
    return;
  }

  String username = server.arg("username");
  String password = server.arg("password");
  if (username.length() > 64 || password.length() > 128 ||
      !secureEquals(username, config.webUser) || !secureEquals(password, config.webPass)) {
    failedLogins++;
    if (failedLogins >= 5) {
      failedLogins = 0;
      lockedUntil = now + LOGIN_LOCK_MS;
    }
    sendJsonResponse(401, "{\"ok\":false,\"error\":\"invalid_credentials\"}");
    return;
  }

  failedLogins = 0;
  lockedUntil = 0;
  WebSession *session = allocateSession();
  session->active = true;
  session->token = randomHex(16);
  session->remoteIp = server.client().remoteIP();
  session->createdAt = now;
  session->lastSeenAt = now;

  sendJsonResponse(200, "{\"ok\":true,\"token\":\"" + session->token +
                    "\",\"mustChangePassword\":" +
                    String(config.webPass == DEFAULT_WEB_PASS ? "true" : "false") + "}");
}

void handleApiLogout() {
  WebSession *session = currentSession(false);
  if (session) {
    session->active = false;
    session->token = "";
  }
  sendJsonResponse(200, "{\"ok\":true}");
}
