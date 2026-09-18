#include "ota_manager.h"

#include "globals.h"
#include "ota_policy.h"
#include "push.h"
#include "web_handlers.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

extern "C" {
extern const uint8_t ota_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t ota_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");
}

// Arduino marks an OTA image valid before setup() by default. Defer that step
// until NVS, the HTTP server and either station or provisioning Wi-Fi are live.
extern "C" bool verifyRollbackLater() {
  return true;
}

namespace {

constexpr char RELEASE_API[] =
    "https://api.github.com/repos/sukiyra/sms-forwarder-board/releases/latest";
constexpr char RELEASE_ASSET_PREFIX[] =
    "https://github.com/sukiyra/sms-forwarder-board/releases/download/";
constexpr unsigned long QUEUE_DELAY_MS = 350;
constexpr unsigned long METADATA_MAX_AGE_MS = 10UL * 60UL * 1000UL;
constexpr unsigned long DOWNLOAD_IDLE_TIMEOUT_MS = 20000;
constexpr size_t DOWNLOAD_BUFFER_SIZE = 4096;
constexpr size_t DOWNLOAD_SLICE_BYTES = 16 * 1024;
constexpr unsigned long DOWNLOAD_SLICE_MS = 12;

enum OtaState {
  OTA_IDLE,
  OTA_CHECK_QUEUED,
  OTA_CHECKING,
  OTA_UP_TO_DATE,
  OTA_AVAILABLE,
  OTA_INSTALL_QUEUED,
  OTA_CONNECTING,
  OTA_DOWNLOADING,
  OTA_VERIFYING,
  OTA_REBOOTING,
  OTA_FAILED
};

struct OtaJob {
  OtaState state = OTA_IDLE;
  String message = "尚未检查更新";
  String latestVersion;
  String assetUrl;
  String digest;
  size_t assetSize = 0;
  size_t bytesReceived = 0;
  size_t partitionSize = 0;
  unsigned int progress = 0;
  unsigned long queuedAt = 0;
  unsigned long checkedAt = 0;
  unsigned long restartAt = 0;
  bool supported = false;
  bool rollbackPending = false;
};

OtaJob job;
NetworkClientSecure downloadClient;
HTTPClient downloadHttp;
NetworkClient *downloadStream = nullptr;
uint8_t *downloadBuffer = nullptr;
mbedtls_sha256_context downloadSha;
bool downloadHttpOpen = false;
bool downloadShaReady = false;
unsigned long downloadLastDataAt = 0;

const char *stateName(OtaState state) {
  switch (state) {
    case OTA_CHECK_QUEUED: return "check_queued";
    case OTA_CHECKING: return "checking";
    case OTA_UP_TO_DATE: return "up_to_date";
    case OTA_AVAILABLE: return "available";
    case OTA_INSTALL_QUEUED: return "install_queued";
    case OTA_CONNECTING: return "connecting";
    case OTA_DOWNLOADING: return "downloading";
    case OTA_VERIFYING: return "verifying";
    case OTA_REBOOTING: return "rebooting";
    case OTA_FAILED: return "failed";
    default: return "idle";
  }
}

bool busyState(OtaState state) {
  return state == OTA_CHECK_QUEUED || state == OTA_CHECKING ||
         state == OTA_INSTALL_QUEUED || state == OTA_DOWNLOADING ||
         state == OTA_CONNECTING ||
         state == OTA_VERIFYING || state == OTA_REBOOTING;
}

void closeDownload(bool abortUpdate) {
  if (abortUpdate) Update.abort();
  if (downloadShaReady) {
    mbedtls_sha256_free(&downloadSha);
    downloadShaReady = false;
  }
  if (downloadBuffer) {
    free(downloadBuffer);
    downloadBuffer = nullptr;
  }
  if (downloadHttpOpen) {
    downloadHttp.end();
    downloadHttpOpen = false;
  }
  downloadStream = nullptr;
}

void fail(const String &message) {
  closeDownload(true);
  job.state = OTA_FAILED;
  job.restartAt = 0;
  job.message = message;
  logCaptureLn("OTA 失败：" + message);
}

void configureSecureClient(NetworkClientSecure &client) {
  const size_t bundleSize = static_cast<size_t>(ota_crt_bundle_end - ota_crt_bundle_start);
  client.setCACertBundle(ota_crt_bundle_start, bundleSize);
  client.setHandshakeTimeout(15);
}

void configureHttp(HTTPClient &http) {
  http.setConnectTimeout(12000);
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("sms-forwarder-board/" FIRMWARE_VERSION);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
}

String sha256Hex(const uint8_t digest[32]) {
  static const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(64);
  for (size_t i = 0; i < 32; ++i) {
    out += hex[digest[i] >> 4];
    out += hex[digest[i] & 0x0f];
  }
  return out;
}

void checkLatestRelease() {
  job.state = OTA_CHECKING;
  job.progress = 8;
  job.message = "正在安全连接 GitHub Releases";
  job.latestVersion = "";
  job.assetUrl = "";
  job.digest = "";
  job.assetSize = 0;
  job.bytesReceived = 0;

  if (!job.supported) {
    fail("当前分区表没有备用 OTA 槽，请先通过 USB 烧录 OTA 版本");
    return;
  }
  if (!WiFi.isConnected()) {
    fail("WiFi 未连接，无法检查更新");
    return;
  }
  if (time(nullptr) < 1700000000) {
    fail("系统时间尚未同步，无法安全验证 HTTPS 证书");
    return;
  }

  NetworkClientSecure client;
  configureSecureClient(client);
  HTTPClient http;
  if (!http.begin(client, RELEASE_API)) {
    fail("无法创建 GitHub 更新请求");
    return;
  }
  configureHttp(http);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    String reason = code > 0 ? String("GitHub 返回 HTTP ") + code : http.errorToString(code);
    http.end();
    fail(reason);
    return;
  }

  // Reading ArduinoJson directly from HTTPClient's TLS stream can see a
  // temporary empty socket buffer as EOF and report IncompleteInput. GitHub's
  // release document is small, so read the complete, length-bounded response
  // first and only then parse it.
  int metadataSize = http.getSize();
  if (metadataSize <= 0 || metadataSize > 64 * 1024) {
    http.end();
    fail("GitHub 版本信息大小无效");
    return;
  }
  String payload = http.getString();
  http.end();
  if (payload.length() != static_cast<size_t>(metadataSize)) {
    fail("GitHub 版本信息下载不完整");
    return;
  }

  JsonDocument filter;
  filter["tag_name"] = true;
  filter["draft"] = true;
  filter["prerelease"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["digest"] = true;
  filter["assets"][0]["size"] = true;
  JsonDocument doc;
  DeserializationError jsonError =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (jsonError) {
    fail("GitHub 版本信息解析失败：" + String(jsonError.c_str()));
    return;
  }
  if (doc["draft"] | false || doc["prerelease"] | false) {
    fail("GitHub 最新版本不是正式发布版");
    return;
  }

  String tag = doc["tag_name"] | "";
  int comparison = otapolicy::compareVersions(tag.c_str(), FIRMWARE_VERSION);
  if (comparison == -2) {
    fail("GitHub 版本号格式无效");
    return;
  }
  job.latestVersion = tag;
  job.checkedAt = millis();
  if (comparison <= 0) {
    job.state = OTA_UP_TO_DATE;
    job.progress = 100;
    job.message = comparison == 0 ? "当前已是最新版本" : "服务器版本较旧，已拒绝降级";
    return;
  }

  for (JsonObject asset : doc["assets"].as<JsonArray>()) {
    String name = asset["name"] | "";
    String url = asset["browser_download_url"] | "";
    String digest = asset["digest"] | "";
    size_t size = asset["size"] | 0;
    if (!url.startsWith(RELEASE_ASSET_PREFIX)) continue;
    if (!otapolicy::validOtaAsset(tag.c_str(), name.c_str(), digest.c_str(), size,
                                  job.partitionSize)) {
      continue;
    }
    job.assetUrl = url;
    job.digest = digest;
    job.assetSize = size;
    break;
  }
  if (!job.assetUrl.length()) {
    fail("最新版本缺少名称、大小和 SHA-256 均有效的 OTA 固件");
    return;
  }
  job.state = OTA_AVAILABLE;
  job.progress = 100;
  job.message = "发现新版本 " + tag + "，可开始在线升级";
}

void beginInstall() {
  closeDownload(false);
  job.state = OTA_CONNECTING;
  job.progress = 2;
  job.bytesReceived = 0;
  job.message = "正在连接固件服务器";

  if (!WiFi.isConnected()) {
    fail("WiFi 已断开，未写入新固件");
    return;
  }
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (!next || next->size < job.assetSize) {
    fail("备用 OTA 分区空间不足");
    return;
  }

  configureSecureClient(downloadClient);
  if (!downloadHttp.begin(downloadClient, job.assetUrl)) {
    fail("无法创建固件下载请求");
    return;
  }
  downloadHttpOpen = true;
  configureHttp(downloadHttp);
  int code = downloadHttp.GET();
  if (code != HTTP_CODE_OK) {
    String reason = code > 0 ? String("固件服务器返回 HTTP ") + code
                             : downloadHttp.errorToString(code);
    fail(reason);
    return;
  }
  int declaredSize = downloadHttp.getSize();
  if (declaredSize > 0 && static_cast<size_t>(declaredSize) != job.assetSize) {
    fail("下载大小与 GitHub 元数据不一致");
    return;
  }
  if (!Update.begin(job.assetSize, U_FLASH)) {
    String reason = String("无法打开备用 OTA 分区：") + Update.errorString();
    fail(reason);
    return;
  }

  downloadBuffer = static_cast<uint8_t *>(malloc(DOWNLOAD_BUFFER_SIZE));
  if (!downloadBuffer) {
    fail("内存不足，OTA 已安全取消");
    return;
  }
  mbedtls_sha256_init(&downloadSha);
  downloadShaReady = true;
  if (mbedtls_sha256_starts(&downloadSha, 0) != 0) {
    fail("SHA-256 校验器初始化失败");
    return;
  }

  downloadStream = downloadHttp.getStreamPtr();
  downloadLastDataAt = millis();
  job.state = OTA_DOWNLOADING;
  job.progress = 4;
  job.message = "正在下载固件，期间请勿断电";
}

void continueInstall() {
  if (!downloadStream || !downloadBuffer || !downloadShaReady) {
    fail("OTA 下载上下文丢失，新固件未启用");
    return;
  }
  const unsigned long sliceStartedAt = millis();
  size_t sliceBytes = 0;
  while (job.bytesReceived < job.assetSize && sliceBytes < DOWNLOAD_SLICE_BYTES &&
         millis() - sliceStartedAt < DOWNLOAD_SLICE_MS) {
    size_t available = downloadStream->available();
    if (available) {
      size_t wanted = min(DOWNLOAD_BUFFER_SIZE,
                          min(available, job.assetSize - job.bytesReceived));
      int count = downloadStream->read(downloadBuffer, wanted);
      if (count <= 0) break;
      size_t written = static_cast<size_t>(count);
      if (Update.write(downloadBuffer, written) != written ||
          mbedtls_sha256_update(&downloadSha, downloadBuffer, written) != 0) {
        fail("Flash 写入失败，新固件未启用");
        return;
      }
      job.bytesReceived += written;
      sliceBytes += written;
      downloadLastDataAt = millis();
      job.progress = min(94U, 4U + static_cast<unsigned int>(
          (job.bytesReceived * 90ULL) / job.assetSize));
      job.message = "正在下载固件 " + String(job.bytesReceived / 1024) + " / " +
                    String((job.assetSize + 1023) / 1024) + " KB";
    } else {
      if (!downloadHttp.connected() ||
          millis() - downloadLastDataAt > DOWNLOAD_IDLE_TIMEOUT_MS) {
        fail("固件下载中断，新固件未启用");
        return;
      }
      break;
    }
  }
  if (job.bytesReceived < job.assetSize) return;

  uint8_t digestBytes[32] = {};
  job.state = OTA_VERIFYING;
  job.progress = 96;
  job.message = "正在校验固件 SHA-256";
  bool shaOk = mbedtls_sha256_finish(&downloadSha, digestBytes) == 0;
  closeDownload(false);
  String actual = sha256Hex(digestBytes);
  String expected = job.digest.substring(7);
  expected.toLowerCase();
  if (!shaOk || actual != expected) {
    fail("SHA-256 不匹配，已拒绝启用下载的固件");
    return;
  }
  if (!Update.end(false) || !Update.isFinished()) {
    fail(String("固件完整性检查失败：") + Update.errorString());
    return;
  }

  job.state = OTA_REBOOTING;
  job.progress = 100;
  job.message = "升级写入成功，设备即将重启；启动自检失败会自动回滚";
  job.restartAt = millis() + 2500;
  logCaptureLn("OTA 写入完成，将重启到 " + job.latestVersion);
}

}  // namespace

void otaManagerBegin() {
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  job.supported = next != nullptr;
  job.partitionSize = next ? next->size : 0;
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  job.rollbackPending = running &&
      esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY;
  if (job.supported) {
    logCaptureLn("OTA 双分区已就绪，备用槽 " + String(job.partitionSize) + " bytes");
  } else {
    logCaptureLn("当前分区表不支持 OTA，需先通过 USB 完整烧录");
  }
}

void otaManagerConfirmBoot() {
  if (!job.rollbackPending) return;
  esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
  if (result == ESP_OK) {
    job.rollbackPending = false;
    logCaptureLn("OTA 启动自检通过，已确认当前固件");
  } else {
    logCaptureLn("OTA 启动确认失败，错误码 " + String(result));
  }
}

bool otaManagerBusy() {
  return busyState(job.state);
}

bool otaManagerSupported() {
  return job.supported;
}

size_t otaManagerPartitionSize() {
  return job.partitionSize;
}

String otaManagerRunningPartition() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  return running && running->label ? String(running->label) : String("unknown");
}

bool otaManagerQueueCheck(String &error) {
  if (otaManagerBusy()) {
    error = "已有 OTA 任务正在执行";
    return false;
  }
  job.state = OTA_CHECK_QUEUED;
  job.progress = 0;
  job.bytesReceived = 0;
  job.message = "已提交版本检查";
  job.queuedAt = millis();
  job.restartAt = 0;
  return true;
}

bool otaManagerQueueInstall(String &error) {
  if (otaManagerBusy()) {
    error = "已有 OTA 任务正在执行";
    return false;
  }
  if (!job.supported) {
    error = "当前分区表不支持在线升级";
    return false;
  }
  if (job.state != OTA_AVAILABLE || !job.assetUrl.length() ||
      millis() - job.checkedAt > METADATA_MAX_AGE_MS) {
    error = "更新信息不存在或已过期，请重新检查版本";
    return false;
  }
  job.state = OTA_INSTALL_QUEUED;
  job.progress = 0;
  job.bytesReceived = 0;
  job.message = "升级请求已接受，正在准备下载";
  job.queuedAt = millis();
  return true;
}

String otaManagerStatusJson() {
  String json;
  json.reserve(820);
  json = "{\"ok\":true,\"currentVersion\":\"" FIRMWARE_VERSION "\",\"supported\":" +
         String(job.supported ? "true" : "false") + ",\"partitionSize\":" +
         String(job.partitionSize) + ",\"runningPartition\":\"" +
         jsonEscape(otaManagerRunningPartition()) +
         "\",\"rollbackPending\":" + String(job.rollbackPending ? "true" : "false") +
         ",\"state\":\"" + stateName(job.state) + "\",\"busy\":" +
         String(otaManagerBusy() ? "true" : "false") + ",\"progress\":" +
         String(job.progress) + ",\"latestVersion\":\"" + jsonEscape(job.latestVersion) +
         "\",\"assetSize\":" + String(job.assetSize) + ",\"bytesReceived\":" +
         String(job.bytesReceived) + ",\"message\":\"" +
         jsonEscape(job.message) + "\"}";
  return json;
}

void otaManagerLoop() {
  if (job.state == OTA_REBOOTING && job.restartAt &&
      static_cast<long>(millis() - job.restartAt) >= 0) {
    delay(50);
    ESP.restart();
  }
  if (job.state == OTA_CHECK_QUEUED && millis() - job.queuedAt >= QUEUE_DELAY_MS) {
    checkLatestRelease();
  } else if (job.state == OTA_INSTALL_QUEUED && millis() - job.queuedAt >= QUEUE_DELAY_MS) {
    beginInstall();
  } else if (job.state == OTA_DOWNLOADING) {
    continueInstall();
  }
}
