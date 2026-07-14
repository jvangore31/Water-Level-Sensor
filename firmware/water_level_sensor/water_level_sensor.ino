#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

#include "dashboard_assets.h"
#include "provisioning_page.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
#define TRIG_PIN 5
#define ECHO_PIN 18
#define MAX_DISTANCE_CM 450.0f
#define DEFAULT_SAMPLE_INTERVAL_MS 500UL
#define WIFI_CONNECT_TIMEOUT_MS 20000UL
#define WIFI_AP_FALLBACK_MS 30000UL
#define WIFI_SETUP_AP_SHUTDOWN_MS 15000UL
#define FIRMWARE_VERSION "0.3.1"
#define CONFIG_SCHEMA_VERSION 2
#define BATTERY_ADC_PIN 34
#ifndef WATER_LEVEL_BOOTSTRAP_CREDENTIAL
#define WATER_LEVEL_BOOTSTRAP_CREDENTIAL "waterlevel-setup"
#endif

struct AppConfig {
  float containerDepthCm = 120.0f;
  String containerName = "Main Tank";
  float warningThresholdPercent = 35.0f;
  float criticalThresholdPercent = 15.0f;
  String preferredMode = "wifi";
  String calibrationMode = "container_depth";
  float fullDistanceCm = NAN;
  float emptyDistanceCm = NAN;
  float minimumValidDistanceCm = 20.0f;
  float maximumValidDistanceCm = 450.0f;
  uint8_t medianWindowSize = 5;
  float maximumStepCm = 25.0f;
  uint8_t stepConfirmationSamples = 3;
  uint8_t invalidSamplesBeforeFault = 3;
  bool powerSavingEnabled = false;
  float sampleIntervalSeconds = 0.5f;
  uint32_t displayTimeoutSeconds = 0;
  bool scheduledSleepEnabled = false;
  uint32_t awakeWindowSeconds = 30;
  bool batteryMonitoringEnabled = false;
  float batteryLowVoltage = 3.4f;
  float batteryCriticalVoltage = 3.2f;
  float batteryCalibrationMultiplier = 1.0f;
  bool maintenanceApEnabled = true;
  uint32_t maintenanceApDelaySeconds = 30;
  uint32_t maintenanceApIdleTimeoutSeconds = 900;
};

struct Reading {
  float distanceCm = NAN;
  float waterDepthCm = NAN;
  float waterPercent = NAN;
  long rawDurationUs = 0;
  String state = "no_data";
  String timestamp = "";
  float rawDistanceCm = NAN;
  bool outsideCalibrationRange = false;
  uint32_t sampleSequence = 0;
  uint32_t acceptedAt = 0;
};

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
AsyncWebServer server(80);
AsyncWebSocket websocket("/ws");
DNSServer dnsServer;
Preferences preferences;

AppConfig config;
Reading latestReading;
bool displayAvailable = false;
bool apActive = false;
bool webServerStarted = false;
bool mdnsStarted = false;
bool provisioningAttempt = false;
bool hasSavedNetwork = false;
bool stationConnected = false;
String savedSsid;
String savedPassword;
String adminSalt;
String adminVerifier;
String maintenanceApPassword;
String hostname = "water-level";
String apSsid;
String pendingSsid;
String pendingPassword;
String pendingHostname;
String provisioningState = "idle";
String provisioningMessage = "Choose a Wi-Fi network.";
unsigned long provisioningStartedAt = 0;
unsigned long disconnectedAt = 0;
unsigned long lastReconnectAt = 0;
unsigned long reconnectDelayMs = 2000;
unsigned long lastSampleAt = 0;
unsigned long lastDisplayAt = 0;
unsigned long stopApAt = 0;
float validSamples[15] = {0};
size_t validSampleCount = 0;
size_t validSampleIndex = 0;
uint8_t consecutiveInvalidSamples = 0;
uint8_t pendingStepSamples = 0;
float pendingStepDistance = NAN;
uint32_t totalSamples = 0;
uint32_t acceptedSamples = 0;
uint32_t timeoutSamples = 0;
uint32_t rejectedSpikes = 0;
uint32_t wifiReconnectCount = 0;
uint32_t bootCount = 0;
uint32_t watchdogResetCount = 0;
uint32_t brownoutResetCount = 0;
uint32_t minimumFreeHeap = UINT32_MAX;
uint32_t bootStartedAt = 0;
uint32_t lastAuthenticatedActivity = 0;
bool displaySleeping = false;
bool maintenanceApSuppressed = false;
struct AuthSession {
  String token;
  String csrf;
  uint32_t startedAt = 0;
  uint32_t lastSeenAt = 0;
};
AuthSession authSessions[4];
uint8_t loginFailures = 0;
uint32_t loginLockedUntil = 0;

void setup() {
  bootStartedAt = millis();
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BATTERY_ADC_PIN, INPUT);
  esp_task_wdt_config_t watchdogConfig = { .timeout_ms = 8000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_init(&watchdogConfig);
  esp_task_wdt_add(NULL);

  displayAvailable = display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  if (displayAvailable) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Water level ready");
    display.display();
  } else {
    Serial.println("{\"event\":\"display_error\",\"message\":\"display_init_failed\"}");
  }

  loadSettings();
  apSsid = buildApSsid();
  startWiFi();
  configureWebServer();
}

void loop() {
  const unsigned long now = millis();
  esp_task_wdt_reset();
  minimumFreeHeap = min(minimumFreeHeap, ESP.getFreeHeap());
  if (apActive) dnsServer.processNextRequest();
  if (apActive && stopApAt > 0 && (long)(now - stopApAt) >= 0) {
    if (WiFi.status() == WL_CONNECTED) stopProvisioningAp();
    stopApAt = 0;
  }
  if (apActive && adminVerifier.length() && config.maintenanceApIdleTimeoutSeconds > 0 &&
      lastAuthenticatedActivity > 0 && now - lastAuthenticatedActivity >= config.maintenanceApIdleTimeoutSeconds * 1000UL) {
    maintenanceApSuppressed = true;
    stopProvisioningAp();
  }
  websocket.cleanupClients();
  handleWiFiState(now);
  handleSerialCommands();

  const uint32_t sampleIntervalMs = config.scheduledSleepEnabled && config.powerSavingEnabled
    ? DEFAULT_SAMPLE_INTERVAL_MS : (uint32_t)(config.sampleIntervalSeconds * 1000.0f);
  if (now - lastSampleAt >= max(sampleIntervalMs, DEFAULT_SAMPLE_INTERVAL_MS)) {
    lastSampleAt = now;
    sampleSensor();
  }

  if (now - lastDisplayAt >= 1000) {
    lastDisplayAt = now;
    drawDisplay();
  }

  if (config.displayTimeoutSeconds > 0 && !displaySleeping && now - lastAuthenticatedActivity >= config.displayTimeoutSeconds * 1000UL) {
    if (displayAvailable) display.ssd1306_command(SSD1306_DISPLAYOFF);
    displaySleeping = true;
  }

  const uint32_t staleAfter = max((uint32_t)(config.sampleIntervalSeconds * 3000.0f), 5000UL);
  if (latestReading.acceptedAt > 0 && latestReading.state == "ok" && now - latestReading.acceptedAt > staleAfter) {
    latestReading.state = "stale";
    broadcast("reading", readingJson());
    broadcast("status", statusJson());
  }

  if (config.powerSavingEnabled && config.scheduledSleepEnabled &&
      now - bootStartedAt >= config.awakeWindowSeconds * 1000UL && latestReading.sampleSequence > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)(config.sampleIntervalSeconds * 1000000.0f));
    Serial.flush();
    esp_deep_sleep_start();
  }
}

void loadSettings() {
  preferences.begin("water-level", false);
  savedSsid = preferences.getString("wifiSsid", "");
  savedPassword = preferences.getString("wifiPass", "");
  hostname = sanitizeHostname(preferences.getString("hostname", "water-level"));
  config.containerDepthCm = preferences.getFloat("depthCm", 120.0f);
  config.containerName = preferences.getString("tankName", "Main Tank");
  config.warningThresholdPercent = preferences.getFloat("warnPct", 35.0f);
  config.criticalThresholdPercent = preferences.getFloat("criticalPct", 15.0f);
  config.calibrationMode = preferences.getString("calMode", "container_depth");
  config.fullDistanceCm = preferences.getFloat("fullCm", NAN);
  config.emptyDistanceCm = preferences.getFloat("emptyCm", NAN);
  config.minimumValidDistanceCm = preferences.getFloat("minValid", 20.0f);
  config.maximumValidDistanceCm = preferences.getFloat("maxValid", 450.0f);
  config.medianWindowSize = preferences.getUChar("medianN", 5);
  config.maximumStepCm = preferences.getFloat("maxStep", 25.0f);
  config.stepConfirmationSamples = preferences.getUChar("stepN", 3);
  config.invalidSamplesBeforeFault = preferences.getUChar("invalidN", 3);
  config.powerSavingEnabled = preferences.getBool("powerSave", false);
  config.sampleIntervalSeconds = preferences.getFloat("sampleSec", 0.5f);
  config.displayTimeoutSeconds = preferences.getULong("displaySec", 0);
  config.scheduledSleepEnabled = preferences.getBool("sleepEnabled", false);
  config.awakeWindowSeconds = preferences.getULong("awakeSec", 30);
  config.batteryMonitoringEnabled = preferences.getBool("batteryOn", false);
  config.batteryLowVoltage = preferences.getFloat("batteryLow", 3.4f);
  config.batteryCriticalVoltage = preferences.getFloat("batteryCrit", 3.2f);
  config.batteryCalibrationMultiplier = preferences.getFloat("batteryCal", 1.0f);
  config.maintenanceApEnabled = preferences.getBool("apEnabled", true);
  config.maintenanceApDelaySeconds = preferences.getULong("apDelay", 30);
  config.maintenanceApIdleTimeoutSeconds = preferences.getULong("apIdle", 900);
  adminSalt = preferences.getString("adminSalt", "");
  adminVerifier = preferences.getString("adminHash", "");
  maintenanceApPassword = preferences.getString("apPassword", "");
  bootCount = preferences.getUInt("bootCount", 0) + 1;
  watchdogResetCount = preferences.getUInt("wdtCount", 0);
  brownoutResetCount = preferences.getUInt("brownCount", 0);
  const esp_reset_reason_t resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT || resetReason == ESP_RST_INT_WDT) ++watchdogResetCount;
  if (resetReason == ESP_RST_BROWNOUT) ++brownoutResetCount;
  preferences.putUInt("bootCount", bootCount); preferences.putUInt("wdtCount", watchdogResetCount); preferences.putUInt("brownCount", brownoutResetCount);
  hasSavedNetwork = savedSsid.length() > 0;
}

void persistConfig() {
  preferences.putFloat("depthCm", config.containerDepthCm);
  preferences.putString("tankName", config.containerName);
  preferences.putFloat("warnPct", config.warningThresholdPercent);
  preferences.putFloat("criticalPct", config.criticalThresholdPercent);
  preferences.putUInt("schema", CONFIG_SCHEMA_VERSION);
  preferences.putString("calMode", config.calibrationMode);
  if (isfinite(config.fullDistanceCm)) preferences.putFloat("fullCm", config.fullDistanceCm); else preferences.remove("fullCm");
  if (isfinite(config.emptyDistanceCm)) preferences.putFloat("emptyCm", config.emptyDistanceCm); else preferences.remove("emptyCm");
  preferences.putFloat("minValid", config.minimumValidDistanceCm);
  preferences.putFloat("maxValid", config.maximumValidDistanceCm);
  preferences.putUChar("medianN", config.medianWindowSize);
  preferences.putFloat("maxStep", config.maximumStepCm);
  preferences.putUChar("stepN", config.stepConfirmationSamples);
  preferences.putUChar("invalidN", config.invalidSamplesBeforeFault);
  preferences.putBool("powerSave", config.powerSavingEnabled);
  preferences.putFloat("sampleSec", config.sampleIntervalSeconds);
  preferences.putULong("displaySec", config.displayTimeoutSeconds);
  preferences.putBool("sleepEnabled", config.scheduledSleepEnabled);
  preferences.putULong("awakeSec", config.awakeWindowSeconds);
  preferences.putBool("batteryOn", config.batteryMonitoringEnabled);
  preferences.putFloat("batteryLow", config.batteryLowVoltage);
  preferences.putFloat("batteryCrit", config.batteryCriticalVoltage);
  preferences.putFloat("batteryCal", config.batteryCalibrationMultiplier);
  preferences.putBool("apEnabled", config.maintenanceApEnabled);
  preferences.putULong("apDelay", config.maintenanceApDelaySeconds);
  preferences.putULong("apIdle", config.maintenanceApIdleTimeoutSeconds);
}

void startWiFi() {
  WiFi.persistent(false);
  WiFi.mode(hasSavedNetwork ? WIFI_STA : WIFI_AP_STA);
  WiFi.setHostname(hostname.c_str());
  disconnectedAt = millis();

  if (hasSavedNetwork) {
    provisioningState = "connecting";
    provisioningMessage = "Connecting to saved Wi-Fi network…";
    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
  } else {
    startProvisioningAp();
  }
}

void startProvisioningAp() {
  if (apActive) return;
  WiFi.mode(WIFI_AP_STA);
  const String apPassword = maintenanceApPassword.length() >= 8 ? maintenanceApPassword : WATER_LEVEL_BOOTSTRAP_CREDENTIAL;
  WiFi.softAP(apSsid.c_str(), apPassword.c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());
  apActive = true;
  if (adminVerifier.length()) lastAuthenticatedActivity = millis();
  provisioningState = "idle";
  provisioningMessage = "Choose a Wi-Fi network.";
  Serial.printf("{\"event\":\"provisioning\",\"ssid\":\"%s\",\"url\":\"http://192.168.4.1\"}\n", apSsid.c_str());
}

void stopProvisioningAp() {
  if (!apActive) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  Serial.println("{\"event\":\"provisioning_closed\",\"message\":\"setup_access_point_disabled\"}");
}

void handleWiFiState(unsigned long now) {
  if (provisioningAttempt) {
    if (WiFi.status() == WL_CONNECTED) {
      provisioningAttempt = false;
      stationConnected = true;
      savedSsid = pendingSsid;
      savedPassword = pendingPassword;
      hostname = pendingHostname;
      hasSavedNetwork = true;
      preferences.putString("wifiSsid", savedSsid);
      preferences.putString("wifiPass", savedPassword);
      preferences.putString("hostname", hostname);
      provisioningState = "connected";
      provisioningMessage = "Connected. The setup network will close shortly; rejoin your normal Wi-Fi to open the dashboard.";
      if (!adminVerifier.length()) stopApAt = now + WIFI_SETUP_AP_SHUTDOWN_MS;
      onWiFiConnected();
      return;
    }

    if (now - provisioningStartedAt > WIFI_CONNECT_TIMEOUT_MS) {
      provisioningAttempt = false;
      provisioningState = "failed";
      provisioningMessage = "Connection timed out. Check the network name and password.";
      if (hasSavedNetwork) WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!stationConnected) {
      stationConnected = true;
      onWiFiConnected();
    }
    if (apActive && stopApAt == 0) {
      provisioningState = "connected";
      provisioningMessage = "The normal Wi-Fi connection is restored. The setup network will close shortly.";
      if (!adminVerifier.length()) stopApAt = now + WIFI_SETUP_AP_SHUTDOWN_MS;
    }
    disconnectedAt = 0;
    reconnectDelayMs = 2000;
    return;
  }

  if (stationConnected) {
    stationConnected = false;
    if (mdnsStarted) MDNS.end();
    mdnsStarted = false;
  }
  if (disconnectedAt == 0) disconnectedAt = now;
  if (!hasSavedNetwork) {
    startProvisioningAp();
    return;
  }

  if (now - lastReconnectAt >= reconnectDelayMs) {
    lastReconnectAt = now;
    WiFi.reconnect();
    ++wifiReconnectCount;
    reconnectDelayMs = min(reconnectDelayMs * 2, 60000UL);
  }

  if (config.maintenanceApEnabled && !maintenanceApSuppressed && now - disconnectedAt >= config.maintenanceApDelaySeconds * 1000UL) startProvisioningAp();
}

void onWiFiConnected() {
  if (mdnsStarted) MDNS.end();
  mdnsStarted = MDNS.begin(hostname.c_str());
  if (mdnsStarted) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "product", "water-level-sensor");
  }
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.printf("{\"event\":\"wifi_connected\",\"hostname\":\"%s.local\",\"ipAddress\":\"%s\"}\n", hostname.c_str(), WiFi.localIP().toString().c_str());
}

String randomHex(size_t byteCount) {
  static const char* digits = "0123456789abcdef";
  String output;
  output.reserve(byteCount * 2);
  for (size_t index = 0; index < byteCount; ++index) {
    const uint8_t value = (uint8_t)esp_random();
    output += digits[value >> 4];
    output += digits[value & 0x0f];
  }
  return output;
}

String passwordHash(const String& password, const String& salt) {
  String input = salt + ":" + password;
  uint8_t digest[32];
  for (uint16_t round = 0; round < 2048; ++round) {
    mbedtls_sha256((const unsigned char*)input.c_str(), input.length(), digest, 0);
    input = salt;
    for (size_t index = 0; index < sizeof(digest); ++index) {
      char value[3];
      snprintf(value, sizeof(value), "%02x", digest[index]);
      input += value;
    }
    input += password;
  }
  mbedtls_sha256((const unsigned char*)input.c_str(), input.length(), digest, 0);
  String output;
  for (size_t index = 0; index < sizeof(digest); ++index) {
    char value[3]; snprintf(value, sizeof(value), "%02x", digest[index]); output += value;
  }
  return output;
}

String cookieValue(AsyncWebServerRequest* request, const String& name) {
  if (!request->hasHeader("Cookie")) return "";
  const String cookie = request->getHeader("Cookie")->value();
  const String key = name + "=";
  int start = cookie.indexOf(key);
  if (start < 0) return "";
  start += key.length();
  int end = cookie.indexOf(';', start);
  return cookie.substring(start, end < 0 ? cookie.length() : end);
}

AuthSession* requestSession(AsyncWebServerRequest* request) {
  const uint32_t now = millis();
  const String token = cookieValue(request, "wl_session");
  for (AuthSession& session : authSessions) {
    if (session.token.length() && session.token == token && now - session.lastSeenAt <= 1800000UL && now - session.startedAt <= 43200000UL) return &session;
  }
  return nullptr;
}

bool sessionValid(AsyncWebServerRequest* request, bool stateChanging = false) {
  AuthSession* session = requestSession(request);
  if (!session) return false;
  if (stateChanging) {
    if (!request->hasHeader("X-Water-Level-CSRF") || request->getHeader("X-Water-Level-CSRF")->value() != session->csrf) return false;
    if (request->hasHeader("Origin")) {
      const String origin = request->getHeader("Origin")->value();
      if (origin.indexOf(request->host()) < 0) return false;
    }
  }
  session->lastSeenAt = millis();
  lastAuthenticatedActivity = session->lastSeenAt;
  return true;
}

bool requireSession(AsyncWebServerRequest* request, bool stateChanging = false) {
  if (sessionValid(request, stateChanging)) return true;
  request->send(401, "application/json", "{\"message\":\"Authentication required.\"}");
  return false;
}

void startSession(AsyncWebServerRequest* request) {
  AuthSession* selected = nullptr;
  for (AuthSession& session : authSessions) if (!session.token.length()) { selected = &session; break; }
  if (!selected) {
    selected = &authSessions[0];
    for (AuthSession& session : authSessions) if (session.lastSeenAt < selected->lastSeenAt) selected = &session;
  }
  selected->token = randomHex(24);
  selected->csrf = randomHex(16);
  selected->startedAt = selected->lastSeenAt = lastAuthenticatedActivity = millis();
  AsyncWebServerResponse* response = request->beginResponse(200, "application/json",
    "{\"authenticated\":true,\"setupRequired\":" + String(adminVerifier.length() ? "false" : "true") +
    ",\"csrfToken\":\"" + selected->csrf + "\"}");
  response->addHeader("Set-Cookie", "wl_session=" + selected->token + "; HttpOnly; SameSite=Strict; Path=/");
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

void configureWebServer() {
  websocket.handleHandshake([](AsyncWebServerRequest* request) {
    return requestSession(request) != nullptr;
  });
  websocket.onEvent([](AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void*, uint8_t*, size_t) {
    if (type == WS_EVT_CONNECT) sendInitialWebSocketState(client);
  });
  server.addHandler(&websocket);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (apActive && WiFi.status() != WL_CONNECTED && !adminVerifier.length()) sendProvisioningPage(request);
    else sendDashboard(request);
  });
  server.on("/generate_204", HTTP_GET, sendProvisioningPage);
  server.on("/gen_204", HTTP_GET, sendProvisioningPage);
  server.on("/hotspot-detect.html", HTTP_GET, sendProvisioningPage);
  server.on("/connecttest.txt", HTTP_GET, sendProvisioningPage);
  server.on("/ncsi.txt", HTTP_GET, sendProvisioningPage);

  server.on("/api/auth/session", HTTP_GET, [](AsyncWebServerRequest* request) {
    const bool valid = sessionValid(request);
    String output = "{\"authenticated\":" + String(valid ? "true" : "false") +
      ",\"setupRequired\":" + String(adminVerifier.length() ? "false" : "true");
    if (valid) output += ",\"csrfToken\":\"" + requestSession(request)->csrf + "\"";
    output += "}";
    sendJson(request, output);
  });

  auto* loginHandler = new AsyncCallbackJsonWebHandler("/api/auth/login", [](AsyncWebServerRequest* request, JsonVariant& json) {
    if ((long)(millis() - loginLockedUntil) < 0) { request->send(429, "application/json", "{\"message\":\"Too many attempts. Try again later.\"}"); return; }
    const String password = json["password"] | "";
    const bool accepted = adminVerifier.length()
      ? passwordHash(password, adminSalt) == adminVerifier
      : password == WATER_LEVEL_BOOTSTRAP_CREDENTIAL;
    if (!accepted) {
      if (++loginFailures >= 5) { loginFailures = 0; loginLockedUntil = millis() + 900000UL; }
      request->send(401, "application/json", "{\"message\":\"Invalid credentials.\"}");
      return;
    }
    loginFailures = 0;
    startSession(request);
  });
  loginHandler->setMethod(HTTP_POST); loginHandler->setMaxContentLength(256); server.addHandler(loginHandler);

  server.on("/api/auth/logout", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!requireSession(request, true)) return;
    AuthSession* session = requestSession(request);
    if (session) session->token = "";
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", "{\"message\":\"Logged out.\"}");
    response->addHeader("Set-Cookie", "wl_session=; Max-Age=0; HttpOnly; SameSite=Strict; Path=/");
    request->send(response);
  });

  auto* passwordHandler = new AsyncCallbackJsonWebHandler("/api/auth/password", [](AsyncWebServerRequest* request, JsonVariant& json) {
    if (!requireSession(request, true)) return;
    const String password = json["newPassword"] | "";
    const String apPassword = json["maintenanceApPassword"] | "";
    if (password.length() < 10 || apPassword.length() < 8 || apPassword == password || apPassword == WATER_LEVEL_BOOTSTRAP_CREDENTIAL) {
      request->send(400, "application/json", "{\"message\":\"Use an administrator password of at least 10 characters and a different AP password of at least 8 characters.\"}"); return;
    }
    adminSalt = randomHex(16); adminVerifier = passwordHash(password, adminSalt); maintenanceApPassword = apPassword;
    preferences.putString("adminSalt", adminSalt); preferences.putString("adminHash", adminVerifier); preferences.putString("apPassword", maintenanceApPassword);
    request->send(200, "application/json", "{\"message\":\"Credentials saved. The maintenance AP password applies after restart.\"}");
  });
  passwordHandler->setMethod(HTTP_PUT); passwordHandler->setMaxContentLength(384); server.addHandler(passwordHandler);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) { if (requireSession(request)) sendJson(request, statusJson()); });
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* request) { if (requireSession(request)) sendJson(request, configJson()); });
  server.on("/api/reading", HTTP_GET, [](AsyncWebServerRequest* request) { if (requireSession(request)) sendJson(request, readingJson()); });
  server.on("/api/network", HTTP_GET, [](AsyncWebServerRequest* request) { if (requireSession(request)) sendJson(request, networkJson()); });
  server.on("/api/diagnostics", HTTP_GET, [](AsyncWebServerRequest* request) { if (requireSession(request)) sendJson(request, diagnosticsJson()); });
  server.on("/api/display/wake", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!requireSession(request, true)) return;
    displaySleeping = false; lastAuthenticatedActivity = millis();
    if (displayAvailable) display.ssd1306_command(SSD1306_DISPLAYON);
    request->send(200, "application/json", "{\"message\":\"Display awake.\"}");
  });
  server.on("/api/calibration/capture-full", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!requireSession(request, true)) return;
    if (!isfinite(latestReading.distanceCm) || latestReading.state != "ok") { request->send(409, "application/json", "{\"message\":\"A stable valid reading is required.\"}"); return; }
    config.fullDistanceCm = latestReading.distanceCm;
    if (isfinite(config.emptyDistanceCm) && config.emptyDistanceCm - config.fullDistanceCm >= 5.0f) config.calibrationMode = "full_empty";
    persistConfig(); sendJson(request, configJson());
  });
  server.on("/api/calibration/capture-empty", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!requireSession(request, true)) return;
    if (!isfinite(latestReading.distanceCm) || latestReading.state != "ok") { request->send(409, "application/json", "{\"message\":\"A stable valid reading is required.\"}"); return; }
    if (isfinite(config.fullDistanceCm) && latestReading.distanceCm - config.fullDistanceCm < 5.0f) { request->send(400, "application/json", "{\"message\":\"Empty point must be at least 5 cm beyond the full point.\"}"); return; }
    config.emptyDistanceCm = latestReading.distanceCm;
    if (isfinite(config.fullDistanceCm)) config.calibrationMode = "full_empty";
    persistConfig(); recalculateReading(); sendJson(request, configJson());
  });
  server.on("/api/setup/status", HTTP_GET, [](AsyncWebServerRequest* request) { sendJson(request, setupStatusJson()); });
  server.on("/api/setup/networks", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!adminVerifier.length() || requireSession(request)) handleNetworkScan(request);
  });

  auto* configHandler = new AsyncCallbackJsonWebHandler("/api/config", [](AsyncWebServerRequest* request, JsonVariant& json) {
    if (!requireSession(request, true)) return;
    handleConfigUpdate(request, json);
  });
  configHandler->setMethod(HTTP_PUT);
  configHandler->setMaxContentLength(2048);
  server.addHandler(configHandler);

  auto* connectHandler = new AsyncCallbackJsonWebHandler("/api/setup/connect", [](AsyncWebServerRequest* request, JsonVariant& json) {
    if (adminVerifier.length() && !requireSession(request, true)) return;
    handleProvisioningRequest(request, json);
  });
  connectHandler->setMethod(HTTP_POST);
  connectHandler->setMaxContentLength(512);
  server.addHandler(connectHandler);

  auto* resetHandler = new AsyncCallbackJsonWebHandler("/api/network/reset", [](AsyncWebServerRequest* request, JsonVariant& json) {
    if (!requireSession(request, true)) return;
    if (json["confirm"] != "RESET_WIFI") {
      request->send(400, "application/json", "{\"message\":\"Confirmation value must be RESET_WIFI.\"}");
      return;
    }
    preferences.remove("wifiSsid");
    preferences.remove("wifiPass");
    request->send(202, "application/json", "{\"message\":\"Wi-Fi settings cleared. Restarting in setup mode.\"}");
    delay(250);
    ESP.restart();
  });
  resetHandler->setMethod(HTTP_POST);
  resetHandler->setMaxContentLength(128);
  server.addHandler(resetHandler);

  auto* factoryResetHandler = new AsyncCallbackJsonWebHandler("/api/factory-reset", [](AsyncWebServerRequest* request, JsonVariant& json) {
    if (!requireSession(request, true)) return;
    if (json["confirm"] != "FACTORY_RESET") { request->send(400, "application/json", "{\"message\":\"Confirmation value must be FACTORY_RESET.\"}"); return; }
    preferences.clear();
    request->send(202, "application/json", "{\"message\":\"All user settings cleared. Restarting.\"}");
    delay(250); ESP.restart();
  });
  factoryResetHandler->setMethod(HTTP_POST); factoryResetHandler->setMaxContentLength(128); server.addHandler(factoryResetHandler);

  server.onNotFound([](AsyncWebServerRequest* request) {
    if (apActive && !adminVerifier.length()) sendProvisioningPage(request);
    else if (!request->url().startsWith("/api/")) sendDashboard(request);
    else request->send(404, "application/json", "{\"message\":\"Not found.\"}");
  });
  server.begin();
  webServerStarted = true;
}

void sendDashboard(AsyncWebServerRequest* request) {
  AsyncWebServerResponse* response = request->beginResponse_P(200, "text/html", DASHBOARD_HTML_GZ, DASHBOARD_HTML_GZ_LEN);
  response->addHeader("Content-Encoding", "gzip");
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
}

void sendProvisioningPage(AsyncWebServerRequest* request) {
  request->send_P(200, "text/html", PROVISIONING_HTML);
}

void handleNetworkScan(AsyncWebServerRequest* request) {
  const int count = WiFi.scanNetworks(false, true);
  JsonDocument document;
  JsonArray networks = document["networks"].to<JsonArray>();
  String seen;
  for (int index = 0; index < count; ++index) {
    String ssid = WiFi.SSID(index);
    if (ssid.length() == 0 || seen.indexOf("\n" + ssid + "\n") >= 0) continue;
    seen += "\n" + ssid + "\n";
    JsonObject network = networks.add<JsonObject>();
    network["ssid"] = ssid;
    network["rssi"] = WiFi.RSSI(index);
    network["secured"] = WiFi.encryptionType(index) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  String output;
  serializeJson(document, output);
  sendJson(request, output);
}

void handleProvisioningRequest(AsyncWebServerRequest* request, JsonVariant& json) {
  const String ssid = json["ssid"] | "";
  const String password = json["password"] | "";
  const String requestedHostname = sanitizeHostname(String(json["hostname"] | "water-level"));
  if (ssid.length() == 0 || ssid.length() > 32) {
    request->send(400, "application/json", "{\"message\":\"Enter a valid network name.\"}");
    return;
  }
  if (password.length() > 64) {
    request->send(400, "application/json", "{\"message\":\"Wi-Fi password is too long.\"}");
    return;
  }
  pendingSsid = ssid;
  pendingPassword = password;
  pendingHostname = requestedHostname;
  provisioningAttempt = true;
  provisioningStartedAt = millis();
  provisioningState = "connecting";
  provisioningMessage = "Connecting to " + ssid + "…";
  WiFi.disconnect();
  WiFi.setHostname(pendingHostname.c_str());
  WiFi.begin(pendingSsid.c_str(), pendingPassword.c_str());
  request->send(202, "application/json", "{\"message\":\"Connection started.\"}");
}

void handleConfigUpdate(AsyncWebServerRequest* request, JsonVariant& json) {
  const float depth = json["containerDepthCm"] | 0.0f;
  const float warning = json["warningThresholdPercent"] | -1.0f;
  const float critical = json["criticalThresholdPercent"] | -1.0f;
  const String name = json["containerName"] | "";
  JsonVariant measurement = json["measurement"];
  JsonVariant power = json["power"];
  JsonVariant network = json["network"];
  const String calibrationMode = measurement["calibrationMode"] | config.calibrationMode;
  const float full = measurement.isNull() ? config.fullDistanceCm : measurement["fullDistanceCm"].isNull() ? NAN : measurement["fullDistanceCm"].as<float>();
  const float empty = measurement.isNull() ? config.emptyDistanceCm : measurement["emptyDistanceCm"].isNull() ? NAN : measurement["emptyDistanceCm"].as<float>();
  const float minimumValid = measurement["minimumValidDistanceCm"] | config.minimumValidDistanceCm;
  const float maximumValid = measurement["maximumValidDistanceCm"] | config.maximumValidDistanceCm;
  const uint8_t medianWindow = measurement["medianWindowSize"] | config.medianWindowSize;
  const float maximumStep = measurement["maximumStepCm"] | config.maximumStepCm;
  const uint8_t stepSamples = measurement["stepConfirmationSamples"] | config.stepConfirmationSamples;
  const uint8_t invalidSamples = measurement["invalidSamplesBeforeFault"] | config.invalidSamplesBeforeFault;
  const bool powerSaving = power["powerSavingEnabled"] | config.powerSavingEnabled;
  const float sampleSeconds = power["sampleIntervalSeconds"] | config.sampleIntervalSeconds;
  const uint32_t displaySeconds = power["displayTimeoutSeconds"] | config.displayTimeoutSeconds;
  const bool sleepEnabled = power["scheduledSleepEnabled"] | config.scheduledSleepEnabled;
  const uint32_t awakeSeconds = power["awakeWindowSeconds"] | config.awakeWindowSeconds;
  const bool batteryEnabled = power["batteryMonitoringEnabled"] | config.batteryMonitoringEnabled;
  const float batteryLow = power["batteryLowVoltage"] | config.batteryLowVoltage;
  const float batteryCritical = power["batteryCriticalVoltage"] | config.batteryCriticalVoltage;
  const float batteryCalibration = power["batteryCalibrationMultiplier"] | config.batteryCalibrationMultiplier;
  const bool apEnabled = network["maintenanceApEnabled"] | config.maintenanceApEnabled;
  const uint32_t apDelay = network["maintenanceApDelaySeconds"] | config.maintenanceApDelaySeconds;
  const uint32_t apIdle = network["maintenanceApIdleTimeoutSeconds"] | config.maintenanceApIdleTimeoutSeconds;
  const bool calibrationValid = calibrationMode == "container_depth" ||
    (calibrationMode == "full_empty" && isfinite(full) && isfinite(empty) && full >= minimumValid && empty <= maximumValid && empty - full >= 5.0f);
  if (depth <= 0 || depth > 10000 || warning < 0 || warning > 100 || critical < 0 || critical > warning || name.length() > 64 ||
      !calibrationValid || minimumValid < 2 || maximumValid > MAX_DISTANCE_CM || maximumValid <= minimumValid ||
      medianWindow < 3 || medianWindow > 15 || medianWindow % 2 == 0 || maximumStep < 0 || stepSamples < 1 || stepSamples > 10 || invalidSamples < 1 || invalidSamples > 20 ||
      sampleSeconds < 0.5f || sampleSeconds > 3600 || (displaySeconds > 0 && (displaySeconds < 10 || displaySeconds > 86400)) ||
      awakeSeconds < 15 || awakeSeconds > 600 || batteryCritical >= batteryLow || batteryCalibration <= 0 || apDelay > 600 || (apIdle > 0 && apIdle < 60)) {
    request->send(400, "application/json", "{\"message\":\"Invalid container configuration.\"}");
    return;
  }
  config.containerDepthCm = depth;
  config.containerName = name;
  config.warningThresholdPercent = warning;
  config.criticalThresholdPercent = critical;
  config.calibrationMode = calibrationMode; config.fullDistanceCm = full; config.emptyDistanceCm = empty;
  config.minimumValidDistanceCm = minimumValid; config.maximumValidDistanceCm = maximumValid;
  config.medianWindowSize = medianWindow; config.maximumStepCm = maximumStep; config.stepConfirmationSamples = stepSamples; config.invalidSamplesBeforeFault = invalidSamples;
  config.powerSavingEnabled = powerSaving; config.sampleIntervalSeconds = sampleSeconds; config.displayTimeoutSeconds = displaySeconds;
  config.scheduledSleepEnabled = sleepEnabled; config.awakeWindowSeconds = awakeSeconds; config.batteryMonitoringEnabled = batteryEnabled;
  config.batteryLowVoltage = batteryLow; config.batteryCriticalVoltage = batteryCritical; config.batteryCalibrationMultiplier = batteryCalibration;
  config.maintenanceApEnabled = apEnabled; config.maintenanceApDelaySeconds = apDelay; config.maintenanceApIdleTimeoutSeconds = apIdle;
  validSampleCount = 0; validSampleIndex = 0; pendingStepSamples = 0;
  persistConfig();
  recalculateReading();
  broadcast("config", configJson());
  broadcast("reading", readingJson());
  sendJson(request, configJson());
}

void sampleSensor() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  const long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  latestReading.rawDurationUs = duration;
  ++totalSamples;

  const float rawDistance = duration > 0 ? duration * 0.034f / 2.0f : NAN;
  latestReading.rawDistanceCm = rawDistance;
  if (duration <= 0 || rawDistance > config.maximumValidDistanceCm || rawDistance < config.minimumValidDistanceCm) {
    ++consecutiveInvalidSamples;
    if (duration <= 0) ++timeoutSamples;
    if (consecutiveInvalidSamples >= config.invalidSamplesBeforeFault) {
      latestReading.state = duration <= 0 || rawDistance > config.maximumValidDistanceCm ? "out_of_range" : "too_close";
      latestReading.distanceCm = NAN; latestReading.waterDepthCm = NAN; latestReading.waterPercent = NAN;
      validSampleCount = 0; validSampleIndex = 0;
    }
  } else {
    validSamples[validSampleIndex] = rawDistance;
    validSampleIndex = (validSampleIndex + 1) % config.medianWindowSize;
    if (validSampleCount < config.medianWindowSize) ++validSampleCount;
    const float filtered = medianSample();
    if (isfinite(latestReading.distanceCm) && config.maximumStepCm > 0 && abs(filtered - latestReading.distanceCm) > config.maximumStepCm) {
      if (!isfinite(pendingStepDistance) || abs(filtered - pendingStepDistance) > config.maximumStepCm / 2.0f) { pendingStepDistance = filtered; pendingStepSamples = 1; }
      else ++pendingStepSamples;
      if (pendingStepSamples < config.stepConfirmationSamples) { latestReading.state = "pending_confirmation"; ++rejectedSpikes; }
      else { latestReading.distanceCm = filtered; pendingStepSamples = 0; pendingStepDistance = NAN; latestReading.state = "ok"; }
    } else { latestReading.distanceCm = filtered; pendingStepSamples = 0; pendingStepDistance = NAN; latestReading.state = "ok"; }
    if (latestReading.state == "ok") {
      consecutiveInvalidSamples = 0; ++acceptedSamples; latestReading.acceptedAt = millis(); latestReading.timestamp = isoTimestamp(); ++latestReading.sampleSequence; recalculateReading();
    }
  }

  const String reading = readingJson();
  Serial.println(serialReadingJson());
  broadcast("reading", reading);
  broadcast("status", statusJson());
}

void recalculateReading() {
  if (!isfinite(latestReading.distanceCm) || config.containerDepthCm <= 0) {
    latestReading.waterDepthCm = NAN;
    latestReading.waterPercent = NAN;
    if (latestReading.state == "ok") latestReading.state = "invalid";
    return;
  }
  if (config.calibrationMode == "full_empty" && isfinite(config.fullDistanceCm) && isfinite(config.emptyDistanceCm)) {
    latestReading.waterPercent = constrain((config.emptyDistanceCm - latestReading.distanceCm) / (config.emptyDistanceCm - config.fullDistanceCm) * 100.0f, 0.0f, 100.0f);
    latestReading.waterDepthCm = (latestReading.waterPercent / 100.0f) * (config.emptyDistanceCm - config.fullDistanceCm);
    latestReading.outsideCalibrationRange = latestReading.distanceCm < config.fullDistanceCm || latestReading.distanceCm > config.emptyDistanceCm;
  } else {
    latestReading.waterDepthCm = max(config.containerDepthCm - latestReading.distanceCm, 0.0f);
    latestReading.waterPercent = constrain((latestReading.waterDepthCm / config.containerDepthCm) * 100.0f, 0.0f, 100.0f);
    latestReading.outsideCalibrationRange = latestReading.distanceCm > config.containerDepthCm;
  }
}

float medianSample() {
  float values[15];
  for (size_t index = 0; index < validSampleCount; ++index) values[index] = validSamples[index];
  for (size_t left = 0; left < validSampleCount; ++left) {
    for (size_t right = left + 1; right < validSampleCount; ++right) {
      if (values[right] < values[left]) {
        const float temporary = values[left]; values[left] = values[right]; values[right] = temporary;
      }
    }
  }
  return values[validSampleCount / 2];
}

String readingJson() {
  JsonDocument document;
  if (isfinite(latestReading.distanceCm)) document["distanceCm"] = serialized(String(latestReading.distanceCm, 1)); else document["distanceCm"] = nullptr;
  if (isfinite(latestReading.waterDepthCm)) document["waterDepthCm"] = serialized(String(latestReading.waterDepthCm, 1)); else document["waterDepthCm"] = nullptr;
  if (isfinite(latestReading.waterPercent)) document["waterPercent"] = serialized(String(latestReading.waterPercent, 1)); else document["waterPercent"] = nullptr;
  document["rawDurationUs"] = latestReading.rawDurationUs;
  document["readingState"] = latestReading.state;
  if (latestReading.timestamp.length()) document["timestamp"] = latestReading.timestamp; else document["timestamp"] = nullptr;
  document["source"] = "json";
  if (isfinite(latestReading.rawDistanceCm)) document["rawDistanceCm"] = serialized(String(latestReading.rawDistanceCm, 1)); else document["rawDistanceCm"] = nullptr;
  document["outsideCalibrationRange"] = latestReading.outsideCalibrationRange;
  document["consecutiveInvalidSamples"] = consecutiveInvalidSamples;
  document["sampleSequence"] = latestReading.sampleSequence;
  document["uptimeMilliseconds"] = millis();
  String output; serializeJson(document, output); return output;
}

String serialReadingJson() {
  JsonDocument document;
  if (isfinite(latestReading.distanceCm)) document["distanceCm"] = serialized(String(latestReading.distanceCm, 1)); else document["distanceCm"] = nullptr;
  document["rawDurationUs"] = latestReading.rawDurationUs;
  document["readingState"] = latestReading.state;
  if (latestReading.timestamp.length()) document["timestamp"] = latestReading.timestamp;
  String output; serializeJson(document, output); return output;
}

String configJson() {
  JsonDocument document;
  document["schemaVersion"] = CONFIG_SCHEMA_VERSION;
  document["containerDepthCm"] = config.containerDepthCm;
  document["containerName"] = config.containerName;
  document["warningThresholdPercent"] = config.warningThresholdPercent;
  document["criticalThresholdPercent"] = config.criticalThresholdPercent;
  document["preferredMode"] = "wifi";
  JsonObject measurement = document["measurement"].to<JsonObject>();
  measurement["calibrationMode"] = config.calibrationMode;
  if (isfinite(config.fullDistanceCm)) measurement["fullDistanceCm"] = config.fullDistanceCm; else measurement["fullDistanceCm"] = nullptr;
  if (isfinite(config.emptyDistanceCm)) measurement["emptyDistanceCm"] = config.emptyDistanceCm; else measurement["emptyDistanceCm"] = nullptr;
  measurement["minimumValidDistanceCm"] = config.minimumValidDistanceCm;
  measurement["maximumValidDistanceCm"] = config.maximumValidDistanceCm;
  measurement["medianWindowSize"] = config.medianWindowSize;
  measurement["maximumStepCm"] = config.maximumStepCm;
  measurement["stepConfirmationSamples"] = config.stepConfirmationSamples;
  measurement["invalidSamplesBeforeFault"] = config.invalidSamplesBeforeFault;
  JsonObject power = document["power"].to<JsonObject>();
  power["powerSavingEnabled"] = config.powerSavingEnabled;
  power["sampleIntervalSeconds"] = config.sampleIntervalSeconds;
  power["displayTimeoutSeconds"] = config.displayTimeoutSeconds;
  power["scheduledSleepEnabled"] = config.scheduledSleepEnabled;
  power["awakeWindowSeconds"] = config.awakeWindowSeconds;
  power["batteryMonitoringEnabled"] = config.batteryMonitoringEnabled;
  power["batteryLowVoltage"] = config.batteryLowVoltage;
  power["batteryCriticalVoltage"] = config.batteryCriticalVoltage;
  power["batteryCalibrationMultiplier"] = config.batteryCalibrationMultiplier;
  JsonObject network = document["network"].to<JsonObject>();
  network["maintenanceApEnabled"] = config.maintenanceApEnabled;
  network["maintenanceApDelaySeconds"] = config.maintenanceApDelaySeconds;
  network["maintenanceApIdleTimeoutSeconds"] = config.maintenanceApIdleTimeoutSeconds;
  String output; serializeJson(document, output); return output;
}

String statusJson() {
  JsonDocument document;
  document["mode"] = "wifi";
  document["deviceConnected"] = WiFi.status() == WL_CONNECTED;
  document["sensorHealthy"] = latestReading.state == "ok";
  document["firmwareVersion"] = FIRMWARE_VERSION;
  document["serialPort"] = nullptr;
  if (WiFi.status() == WL_CONNECTED) document["ipAddress"] = WiFi.localIP().toString(); else document["ipAddress"] = nullptr;
  document["hostname"] = hostname + ".local";
  document["wifiConnected"] = WiFi.status() == WL_CONNECTED;
  document["wifiSignalDbm"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  document["uptimeSeconds"] = millis() / 1000;
  document["status"] = latestReading.state == "ok" ? "reading" : (WiFi.status() == WL_CONNECTED ? "error" : "disconnected");
  document["message"] = latestReading.state == "ok" ? "Receiving sensor readings." : "Waiting for a valid sensor reading.";
  document["maintenanceApActive"] = apActive;
  document["scheduledSleepEnabled"] = config.powerSavingEnabled && config.scheduledSleepEnabled;
  document["clockSynchronized"] = time(nullptr) >= 1700000000;
  document["authenticationRequired"] = true;
  if (config.batteryMonitoringEnabled) {
    const float voltage = analogReadMilliVolts(BATTERY_ADC_PIN) / 1000.0f * 2.0f * config.batteryCalibrationMultiplier;
    document["batteryVoltage"] = voltage;
    document["batteryState"] = voltage <= config.batteryCriticalVoltage ? "critical" : voltage <= config.batteryLowVoltage ? "low" : "normal";
  } else { document["batteryVoltage"] = nullptr; document["batteryState"] = "disabled"; }
  String output; serializeJson(document, output); return output;
}

String networkJson() {
  JsonDocument document;
  document["configured"] = hasSavedNetwork;
  document["connected"] = WiFi.status() == WL_CONNECTED;
  document["ssid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : savedSsid;
  document["hostname"] = hostname;
  if (WiFi.status() == WL_CONNECTED) document["ipAddress"] = WiFi.localIP().toString(); else document["ipAddress"] = nullptr;
  document["signalDbm"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  String output; serializeJson(document, output); return output;
}

String diagnosticsJson() {
  JsonDocument document;
  document["firmwareVersion"] = FIRMWARE_VERSION;
  document["build"] = __DATE__ " " __TIME__;
  document["uptimeSeconds"] = millis() / 1000;
  document["resetReason"] = (int)esp_reset_reason();
  document["bootCount"] = bootCount;
  document["watchdogResetCount"] = watchdogResetCount;
  document["brownoutResetCount"] = brownoutResetCount;
  document["freeHeap"] = ESP.getFreeHeap();
  document["minimumFreeHeap"] = minimumFreeHeap;
  document["totalSamples"] = totalSamples;
  document["acceptedSamples"] = acceptedSamples;
  document["timeouts"] = timeoutSamples;
  document["rejectedSpikes"] = rejectedSpikes;
  document["consecutiveFailures"] = consecutiveInvalidSamples;
  document["lastAcceptedSampleUptimeMs"] = latestReading.acceptedAt;
  document["wifiReconnectCount"] = wifiReconnectCount;
  document["wifiSignalDbm"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  document["schemaVersion"] = CONFIG_SCHEMA_VERSION;
  document["sketchSize"] = ESP.getSketchSize();
  document["freeSketchSpace"] = ESP.getFreeSketchSpace();
  String output; serializeJson(document, output); return output;
}

String setupStatusJson() {
  JsonDocument document;
  document["state"] = provisioningState;
  document["message"] = provisioningMessage;
  document["apSsid"] = apSsid;
  document["setupAccessPoint"] = true;
  document["setupAccessPointActive"] = apActive;
  const long shutdownRemainingMs = stopApAt > 0 ? (long)(stopApAt - millis()) : 0;
  document["setupAccessPointShutdownSeconds"] = shutdownRemainingMs > 0 ? (shutdownRemainingMs + 999) / 1000 : 0;
  document["normalNetworkSsid"] = provisioningState == "connected" ? savedSsid : "";
  document["hostname"] = provisioningState == "connected" ? hostname : pendingHostname.length() ? pendingHostname : hostname;
  if (WiFi.status() == WL_CONNECTED) document["ipAddress"] = WiFi.localIP().toString(); else document["ipAddress"] = nullptr;
  String output; serializeJson(document, output); return output;
}

void sendInitialWebSocketState(AsyncWebSocketClient* client) {
  client->text(eventJson("config", configJson()));
  client->text(eventJson("status", statusJson()));
  client->text(eventJson("reading", readingJson()));
}

void broadcast(const String& type, const String& data) { websocket.textAll(eventJson(type, data)); }
String eventJson(const String& type, const String& data) { return "{\"type\":\"" + type + "\",\"data\":" + data + "}"; }
void sendJson(AsyncWebServerRequest* request, const String& data) { request->send(200, "application/json", data); }

String sanitizeHostname(String value) {
  value.toLowerCase();
  String clean;
  for (size_t index = 0; index < value.length() && clean.length() < 32; ++index) {
    const char character = value[index];
    if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-') clean += character;
  }
  while (clean.startsWith("-")) clean.remove(0, 1);
  while (clean.endsWith("-")) clean.remove(clean.length() - 1);
  return clean.length() ? clean : "water-level";
}

String buildApSsid() {
  const uint64_t chipId = ESP.getEfuseMac();
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", (uint16_t)(chipId & 0xFFFF));
  return "WaterLevel-" + String(suffix);
}

String isoTimestamp() {
  const time_t now = time(nullptr);
  if (now < 1700000000) return "";
  struct tm utc;
  gmtime_r(&now, &utc);
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String(buffer);
}

void handleSerialCommands() {
  static String line;
  while (Serial.available()) {
    const char character = Serial.read();
    if (character == '\n') {
      line.trim();
      if (line == "WIFI_RESET " + apSsid) {
        preferences.remove("wifiSsid");
        preferences.remove("wifiPass");
        Serial.println("{\"event\":\"wifi_reset\",\"message\":\"restarting\"}");
        delay(100);
        ESP.restart();
      }
      if (line == "FACTORY_RESET " + apSsid) {
        preferences.clear();
        Serial.println("{\"event\":\"factory_reset\",\"message\":\"restarting\"}");
        delay(100); ESP.restart();
      }
      line = "";
    } else if (line.length() < 128) line += character;
  }
}

void drawDisplay() {
  if (!displayAvailable) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(config.containerName.substring(0, 20));
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 17);
  if (isfinite(latestReading.waterPercent) && latestReading.state == "ok") {
    display.print(latestReading.waterPercent, 1);
    display.println(" %");
  } else display.println("No Read");
  display.setTextSize(1);
  display.setCursor(0, 43);
  if (WiFi.status() == WL_CONNECTED) {
    display.println((hostname + ".local").substring(0, 21));
    display.println(WiFi.localIP());
  } else if (apActive) {
    display.println(apSsid.substring(0, 21));
    display.println("192.168.4.1");
  } else display.println("Wi-Fi connecting...");
  display.display();
}
