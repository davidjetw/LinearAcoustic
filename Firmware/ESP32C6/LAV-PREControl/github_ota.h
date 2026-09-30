#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <atomic>
#include <time.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include "ota_config.h"
#include "ota_version.h"

#if !defined(CONFIG_IDF_TARGET_ESP32C6)
#error This firmware is intended for ESP32-C6 only.
#endif
static_assert(AudioVersion::key(AUDIO_FW_VERSION) != 0, "Invalid calendar firmware version");

// HTTPS belongs to the worker. WebServer and ArduinoOTA belong to networkTask.
// Audio GPIO/NVS quiescing belongs ONLY to loop(), via these atomic flags.
namespace AudioOTA {
static const char firmwareRecord[] = "LA_FW_VERSION:" AUDIO_FW_VERSION;
static std::atomic<bool> holdRequested{false}, holdReady{false}, busy{false};
static std::atomic<bool> rebootRequested{false};
static SemaphoreHandle_t stateMutex = nullptr;
static QueueHandle_t jobs = nullptr;
static TaskHandle_t workerHandle = nullptr;
static bool servicesStarted = false;
static char token[33];
static char otaPassword[65] = {};
struct Manifest {
  char version[24] = {};
  char url[320] = {};
  char sha256[65] = {};
  uint32_t size = 0;
};
struct State {
  char phase[24] = "idle";
  char message[180] = "尚未檢查更新";
  unsigned progress = 0;
  bool available = false;
  uint32_t checkedAt = 0;
  Manifest release;
};
static State state;
enum class Job : uint8_t { Check, Install };

static State snapshot() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  State result = state;
  xSemaphoreGive(stateMutex);
  return result;
}
static void report(const char *phase, const char *message, unsigned progress = 0) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  strlcpy(state.phase, phase, sizeof(state.phase));
  strlcpy(state.message, message, sizeof(state.message));
  state.progress = progress;
  xSemaphoreGive(stateMutex);
}
static void progress(unsigned percent) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  state.progress = percent;
  xSemaphoreGive(stateMutex);
}
static bool configured() { return AUDIO_OTA_USER[0] && otaPassword[0]; }
static void initPassword() {
  Preferences credentials;
  if (!credentials.begin("ota_auth", false)) return;
  String saved = credentials.getString("password", "");
  if (saved.length() >= 16 && saved.length() < sizeof(otaPassword)) {
    strlcpy(otaPassword, saved.c_str(), sizeof(otaPassword));
  } else {
    snprintf(otaPassword, sizeof(otaPassword), "%08lx%08lx%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random());
    if (credentials.putString("password", otaPassword) != strlen(otaPassword)) otaPassword[0] = '\0';
  }
  credentials.end();
  Serial.println("[OTA] USB serial command: OTA PASSWORD (115200 baud, newline).");
}
static void serialHelp() {
  static char line[40];
  static unsigned used = 0;
  static bool overflow = false;
  // Bound work per loop; do not affect physical control timing.
  for (unsigned count=0; count<40 && Serial.available(); ++count) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      line[used] = '\0';
      if (!overflow && strcmp(line, "OTA PASSWORD") == 0) {
        if (configured()) Serial.printf("[OTA] user: %s  password: %s\n", AUDIO_OTA_USER, otaPassword);
        else Serial.println("[OTA] Credential storage failed; OTA disabled.");
      }
      used = 0; overflow = false;
    } else if (used < sizeof(line)-1) line[used++] = c;
    else overflow = true;
  }
}
static void releaseAudio() {
  holdRequested.store(false);
  holdReady.store(false);
}
static bool holdAudio() {
  holdReady.store(false);
  holdRequested.store(true);
  uint32_t started = millis();
  while (!holdReady.load()) {
    if (millis() - started > 5000) {
      releaseAudio();
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return true;
}
static void fail(const char *message) {
  report("error", message);
  releaseAudio();
  busy.store(false);
}
static bool httpsReady(String &error) {
  if (WiFi.status() != WL_CONNECTED) { error = "尚未連上 Wi-Fi"; return false; }
  // Certificate verification needs a valid clock; never fall back to setInsecure.
  if (time(nullptr) < 1704067200) {
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");
    uint32_t started = millis();
    while (time(nullptr) < 1704067200) {
      if (millis() - started > 15000) { error = "時間同步失敗，無法驗證 HTTPS 憑證"; return false; }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
  return true;
}
static void secureClient(NetworkClientSecure &client) {
  // Arduino-ESP32 3.3.12 built-in CA trust bundle, including hostname checks.
  client.useBuiltinCACertBundle();
  client.setConnectionTimeout(5000);
  client.setHandshakeTimeout(15);
  client.setTimeout(10000);
}
static void configureHTTP(HTTPClient &http) {
  http.setConnectTimeout(5000);
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setReuse(false);
  http.useHTTP10(true); // Require a bounded non-chunked body.
}
static bool validURL(const char *url) {
  String s(url);
  if (!s.startsWith(AUDIO_OTA_DOWNLOAD_PREFIX) || !s.endsWith(".bin")) return false;
  if (s.indexOf("..") >= 0 || s.indexOf('?') >= 0 || s.indexOf('#') >= 0) return false;
  for (unsigned i = 0; i < s.length(); ++i) if ((uint8_t)s[i] <= 32 || s[i] == '\\') return false;
  return true;
}
static bool readManifest(Manifest &out, String &error) {
  if (!httpsReady(error)) return false;
  NetworkClientSecure client;
  secureClient(client);
  HTTPClient http;
  configureHTTP(http);
  if (!http.begin(client, AUDIO_OTA_MANIFEST_URL)) { error = "無法開啟版本資訊網址"; return false; }
  http.addHeader("Cache-Control", "no-cache");
  const int code = http.GET();
  if (code != HTTP_CODE_OK) { error = "版本資訊下載失敗，HTTP " + String(code); http.end(); return false; }
  const int size = http.getSize();
  if (size <= 0 || size > 4096) { error = "版本資訊大小不合法或缺少 Content-Length"; http.end(); return false; }
  char body[4097];
  const size_t received = http.getStreamPtr()->readBytes(body, size);
  http.end();
  if (received != (size_t)size) { error = "版本資訊下載不完整"; return false; }
  body[size] = '\0';
  JsonDocument doc;
  if (deserializeJson(doc, body, size, DeserializationOption::NestingLimit(4))) {
    error = "版本資訊 JSON 格式錯誤"; return false;
  }
  if (!doc["schema"].is<int>() || doc["schema"].as<int>() != 1 ||
      !doc["size"].is<uint32_t>() || !doc["version"].is<const char *>() ||
      !doc["sha256"].is<const char *>() || !doc["url"].is<const char *>()) {
    error = "GitHub 尚未發布具備完整校驗資訊的新韌體；舊測試檔不可安裝"; return false;
  }
  if (strcmp(doc["board"] | "", AUDIO_OTA_BOARD) ||
      strcmp(doc["product"] | "", AUDIO_OTA_PRODUCT)) {
    error = "韌體板型或產品不符合此裝置"; return false;
  }
  const char *ver = doc["version"];
  const char *url = doc["url"];
  const char *sha = doc["sha256"];
  const uint32_t bytes = doc["size"];
  if (!AudioVersion::key(ver) || strlen(ver) >= sizeof(out.version) ||
      strlen(url) >= sizeof(out.url) || !validURL(url) || strlen(sha) != 64) {
    error = "版本號、下載網址或 SHA-256 格式不正確"; return false;
  }
  for (unsigned i = 0; i < 64; ++i) if (!isxdigit((unsigned char)sha[i])) {
    error = "SHA-256 必須是 64 位十六進位字串"; return false;
  }
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (!next || bytes < 32 || bytes > next->size) {
    error = "韌體超過 OTA 分割區容量，需先經 USB 調整分割區"; return false;
  }
  strlcpy(out.version, ver, sizeof(out.version));
  strlcpy(out.url, url, sizeof(out.url));
  strlcpy(out.sha256, sha, sizeof(out.sha256));
  for (char *p = out.sha256; *p; ++p) *p = tolower((unsigned char)*p);
  out.size = bytes;
  return true;
}

static bool download(const Manifest &m, String &error) {
  if (!httpsReady(error)) return false;
  NetworkClientSecure client;
  secureClient(client);
  HTTPClient http;
  configureHTTP(http);
  if (!http.begin(client, m.url)) { error = "無法開啟韌體網址"; return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK || http.getSize() != (int)m.size) {
    error = "韌體 HTTP 狀態或大小不符，未安裝"; http.end(); return false;
  }
  if (!Update.begin(m.size, U_FLASH)) { error = "無法開始寫入 OTA 分割區"; http.end(); return false; }
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  bool ok = mbedtls_sha256_starts(&hash, 0) == 0;
  uint32_t total = 0, lastData = millis(), started = millis();
  uint8_t buffer[2048];
  auto *stream = http.getStreamPtr();
  while (ok && total < m.size) {
    int available = stream->available();
    if (available > 0) {
      size_t want = min(sizeof(buffer), (size_t)(m.size - total));
      want = min(want, (size_t)available);
      int n = stream->read(buffer, want);
      if (n <= 0) { error = "韌體讀取失敗"; ok = false; break; }
      if (mbedtls_sha256_update(&hash, buffer, n) != 0 || Update.write(buffer, n) != (size_t)n) {
        error = "韌體寫入或校驗計算失敗"; ok = false; break;
      }
      total += n;
      lastData = millis();
      progress((uint64_t)total * 99 / m.size);
    } else if (!http.connected() || millis() - lastData > 15000) {
      error = "韌體下載中斷或逾時"; ok = false; break;
    }
    if (millis() - started > 180000) { error = "韌體下載超過三分鐘"; ok = false; break; }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  unsigned char digest[32];
  char hex[65];
  if (ok) {
    ok = mbedtls_sha256_finish(&hash, digest) == 0;
    if (ok) {
      for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
      ok = strcmp(hex, m.sha256) == 0;
    }
    if (!ok) error = "SHA-256 不符，已拒絕安裝";
  } else if (!error.length()) error = "校驗初始化失敗";
  mbedtls_sha256_free(&hash);
  http.end();
  // SHA and length are checked BEFORE marking the partition bootable.
  if (ok) {
    ok = Update.end(false);
    if (!ok) error = "韌體映像驗證失敗，請確認是 ESP32-C6 應用程式 bin";
  }
  if (!ok && Update.isRunning()) Update.abort();
  return ok;
}

static void worker(void *) {
  Job job;
  for (;;) {
    if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) continue;
    String error;
    if (job == Job::Check) {
      Manifest m;
      const bool ok = readManifest(m, error);
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      state.available = false;
      state.checkedAt = 0;
      state.release = Manifest{};
      if (ok) {
        state.release = m;
        state.checkedAt = millis();
        state.available = AudioVersion::key(m.version) > AudioVersion::key(AUDIO_FW_VERSION);
      }
      xSemaphoreGive(stateMutex);
      if (!ok) report("error", error.c_str());
      else report("checked", snapshot().available ? "有新版本，可按安裝更新" : "目前已是相同或較新的版本");
      busy.store(false);
    } else {
      const State approved = snapshot();
      report("preparing", "正在停止音量馬達並依原時序進入待機");
      if (!holdAudio()) { fail("音響未能進入待機，已取消更新"); continue; }
      report("downloading", "下載與校驗韌體中，請勿斷電");
      if (!download(approved.release, error)) { fail(error.c_str()); continue; }
      report("success", "更新成功，即將重新啟動", 100);
      vTaskDelay(pdMS_TO_TICKS(1500));
      ESP.restart();
    }
  }
}

static bool auth(WebServer &server) {
  if (!configured()) {
    server.send(503, "text/plain; charset=utf-8", "OTA 密碼儲存失敗，更新功能已停用，請檢查序列埠訊息。");
    return false;
  }
  if (!server.authenticate(AUDIO_OTA_USER, otaPassword)) {
    server.requestAuthentication(DIGEST_AUTH, "Linear Acoustic OTA");
    return false;
  }
  return true;
}
static bool csrf(WebServer &server) {
  if (!server.hasArg("token") || server.arg("token") != token) {
    server.send(403, "text/plain; charset=utf-8", "頁面已失效，請重新開啟更新頁面。"); return false;
  }
  return true;
}
static void sendState(WebServer &server) {
  const State s = snapshot();
  JsonDocument doc;
  doc["current"] = firmwareRecord + sizeof("LA_FW_VERSION:") - 1;
  doc["latest"] = s.release.version;
  doc["phase"] = s.phase;
  doc["message"] = s.message;
  doc["progress"] = s.progress;
  doc["busy"] = busy.load();
  doc["available"] = s.available && millis() - s.checkedAt < 600000;
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  doc["capacity"] = next ? next->size : 0;
  String json;
  serializeJson(doc, json);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", json);
}

static const char PAGE[] PROGMEM = R"OTAHTML(<!doctype html><html lang="zh-Hant"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Linear Acoustic 更新</title>
<style>body{font:16px system-ui;background:#121214;color:#eee;max-width:540px;margin:32px auto;padding:0 20px;line-height:1.7}button,a{font:inherit}button{padding:10px 16px;margin:8px 8px 8px 0;border:0;border-radius:12px;background:#03dac6;color:#111}button:disabled{opacity:.4}a{color:#03dac6}small{color:#aaa}progress{width:100%}input{margin:12px 0;max-width:100%}</style>
<h2>韌體更新</h2><p>目前版本：<b id="current"></b><br>GitHub 最新版本：<b id="latest">尚未檢查</b></p>
<p id="message" role="status">載入中…</p><progress id="progress" max="100" value="0"></progress>
<button id="check">檢查更新</button><button id="install" disabled>安裝更新</button>
<p><small>只有按下安裝才會更新。安裝前音響會自動進入待機；失敗時維持待機，成功後重新啟動。</small></p>
<details><summary>手動上傳 .bin</summary><form id="manual" action="/update" method="post" enctype="multipart/form-data">
<input name="token" type="hidden" value="%%TOKEN%%"><input id="file" type="file" name="update" accept=".bin" required>
<button id="upload" type="submit">上傳並更新</button></form><small>請選擇本機型的應用程式 .bin，不要選 merged.bin、bootloader 或 partitions。</small></details>
<p><a href="/">返回音響控制</a></p><script>
const token='%%TOKEN%%'; let latest='', pending=false, active=false;
const $=id=>document.getElementById(id);
async function refresh(){try{const r=await fetch('/ota/status',{cache:'no-store'});if(!r.ok)throw Error('無法取得更新狀態，請重新登入');const s=await r.json();latest=s.latest;active=s.busy;$('current').textContent=s.current;$('latest').textContent=s.latest||'尚未取得可安裝版本';$('message').textContent=s.message;$('progress').value=s.progress;$('check').disabled=s.busy||pending;$('install').disabled=s.busy||pending||!s.available;$('upload').disabled=s.busy||pending;}catch(e){$('message').textContent='連線中斷或裝置正在重啟，稍後將重試';$('install').disabled=true;$('check').disabled=true;$('upload').disabled=true;}}
async function command(path){if(pending||active)return;pending=true;$('check').disabled=true;$('install').disabled=true;try{const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({token,version:latest})});const text=await r.text();if(!r.ok)throw Error(text);}catch(e){alert(e.message);}finally{pending=false;await refresh();}}
$('check').onclick=()=>command('/ota/check');$('install').onclick=()=>{if(confirm('安裝 '+latest+'？音響將先進入待機，更新期間請勿斷電。'))command('/ota/install');};
$('manual').onsubmit=e=>{if(active||pending||!confirm('上傳韌體並更新？音響將先進入待機。'))e.preventDefault();};
refresh();setInterval(refresh,1500);
</script></html>)OTAHTML";

static bool manualOwned = false, manualEnded = false, manualFailed = false;
static uint32_t manualLastData = 0;
static void clearManual() {
  if (manualOwned && Update.isRunning()) Update.abort();
  manualOwned = manualEnded = manualFailed = false;
}
static void begin(WebServer &server) {
  initPassword();
  stateMutex = xSemaphoreCreateMutex();
  if (!stateMutex) { Serial.println("[OTA] mutex allocation failed"); return; }
  snprintf(token, sizeof(token), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(),
           (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
  if (configured()) {
    jobs = xQueueCreate(1, sizeof(Job));
    if (!jobs || xTaskCreate(worker, "GitHubOTA", 12288, nullptr, 1, &workerHandle) != pdPASS)
      report("error", "更新背景任務啟動失敗，請重啟裝置");
  }
  server.on("/ota", HTTP_GET, [&server]() {
    if (!auth(server)) return;
    String page = FPSTR(PAGE);
    page.replace("%%TOKEN%%", token);
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Frame-Options", "DENY");
    server.send(200, "text/html; charset=utf-8", page);
  });
  server.on("/ota/status", HTTP_GET, [&server]() { if (auth(server)) sendState(server); });
  server.on("/ota/check", HTTP_POST, [&server]() {
    if (!auth(server) || !csrf(server)) return;
    if (!workerHandle) { server.send(503, "text/plain", "OTA worker unavailable"); return; }
    if (busy.exchange(true)) { server.send(409, "text/plain", "Update operation is busy"); return; }
    report("checking", "正在檢查 GitHub 版本資訊");
    Job job = Job::Check;
    if (xQueueSend(jobs, &job, 0) != pdTRUE) { fail("更新佇列忙碌，請重試"); server.send(503, "text/plain", "Queue busy"); return; }
    server.send(202, "text/plain", "Checking");
  });
  server.on("/ota/install", HTTP_POST, [&server]() {
    if (!auth(server) || !csrf(server)) return;
    if (!workerHandle) { server.send(503, "text/plain", "OTA worker unavailable"); return; }
    if (busy.exchange(true)) { server.send(409, "text/plain", "Update operation is busy"); return; }
    State s = snapshot();
    if (!s.available || millis() - s.checkedAt >= 600000 || server.arg("version") != s.release.version) {
      busy.store(false); server.send(409, "text/plain; charset=utf-8", "請重新檢查版本後再安裝"); return;
    }
    report("queued", "更新已排程");
    Job job = Job::Install;
    if (xQueueSend(jobs, &job, 0) != pdTRUE) { fail("更新佇列忙碌，請重試"); server.send(503, "text/plain", "Queue busy"); return; }
    server.send(202, "text/plain", "Installing");
  });
  server.on("/update", HTTP_POST, [&server]() {
    if (!auth(server) || !csrf(server)) {
      if (manualOwned) { clearManual(); fail("手動上傳驗證失敗，已取消更新"); }
      return;
    }
    if (!manualOwned) { server.send(409, "text/plain", "Upload rejected or busy; open /ota again."); return; }
    bool ok = !manualFailed && manualEnded && Update.end(true);
    clearManual();
    if (!ok) { fail("手動更新失敗，音響維持待機"); server.send(400, "text/plain", "Update failed; no reboot."); return; }
    report("success", "更新成功，即將重新啟動", 100);
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain; charset=utf-8", "更新成功，即將重新啟動。稍後請重新開啟音響網頁。");
    rebootRequested.store(true);
  }, [&server]() {
    HTTPUpload &u = server.upload();
    manualLastData = millis();
    if (u.status == UPLOAD_FILE_START) {
      if (manualOwned) { manualFailed = true; if (Update.isRunning()) Update.abort(); return; }
      if (!configured() || !server.authenticate(AUDIO_OTA_USER, otaPassword) ||
          !server.hasArg("token") || server.arg("token") != token) return;
      if (busy.exchange(true)) return;
      manualOwned = true; manualEnded = manualFailed = false;
      report("uploading", "進入待機並接收手動韌體");
      if (!holdAudio() || !Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) manualFailed = true;
    } else if (u.status == UPLOAD_FILE_WRITE && manualOwned && !manualFailed) {
      if (Update.write(u.buf, u.currentSize) != u.currentSize) { manualFailed = true; Update.abort(); }
    } else if (u.status == UPLOAD_FILE_END && manualOwned) {
      manualEnded = u.totalSize > 0;
    } else if (u.status == UPLOAD_FILE_ABORTED && manualOwned) {
      clearManual(); fail("上傳已中止，音響維持待機");
    }
  });
}
static void startArduinoOTA() {
  if (servicesStarted || !configured() || !stateMutex) return;
  servicesStarted = true;
  ArduinoOTA.setPassword(otaPassword);
  ArduinoOTA.setRebootOnSuccess(false);
  ArduinoOTA.onStart([]() {
    busy.store(true);
    report("uploading", "Arduino IDE 韌體更新中");
    if (ArduinoOTA.getCommand() != U_FLASH || !holdAudio()) Update.abort();
  });
  ArduinoOTA.onProgress([](unsigned done, unsigned total) { if (total) progress((uint64_t)done * 99 / total); });
  ArduinoOTA.onError([](ota_error_t) { fail("Arduino IDE 更新失敗，音響維持待機"); });
  ArduinoOTA.onEnd([]() { report("success", "更新成功，即將重新啟動", 100); rebootRequested.store(true); });
  ArduinoOTA.begin();
}
static void networkLoop() {
  if (rebootRequested.load()) { delay(1000); ESP.restart(); }
  if (manualOwned && millis() - manualLastData > 30000) {
    clearManual(); fail("手動上傳逾時，已取消更新");
  }
  if (servicesStarted && !busy.load()) ArduinoOTA.handle();
}
} // namespace AudioOTA
