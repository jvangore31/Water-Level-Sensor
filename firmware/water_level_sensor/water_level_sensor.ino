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

#include "dashboard_assets.h"
#include "provisioning_page.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
#define TRIG_PIN 5
#define ECHO_PIN 18
#define MAX_DISTANCE_CM 450.0f
#define SAMPLE_INTERVAL_MS 500UL
#define WIFI_CONNECT_TIMEOUT_MS 20000UL
#define WIFI_AP_FALLBACK_MS 600000UL
#define WIFI_SETUP_AP_SHUTDOWN_MS 15000UL
#define FIRMWARE_VERSION "0.2.0"

struct AppConfig {
  float containerDepthCm = 120.0f;
  String containerName = "Main Tank";
  float warningThresholdPercent = 35.0f;
  float criticalThresholdPercent = 15.0f;
  String preferredMode = "wifi";
};

struct Reading {
  float distanceCm = NAN;
  float waterDepthCm = NAN;
  float waterPercent = NAN;
  long rawDurationUs = 0;
  String state = "no_data";
  String timestamp = "";
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
float validSamples[5] = {0};
size_t validSampleCount = 0;
size_t validSampleIndex = 0;

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

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
  if (apActive) dnsServer.processNextRequest();
  if (apActive && stopApAt > 0 && (long)(now - stopApAt) >= 0) {
    if (WiFi.status() == WL_CONNECTED) stopProvisioningAp();
    stopApAt = 0;
  }
  websocket.cleanupClients();
  handleWiFiState(now);
  handleSerialCommands();

  if (now - lastSampleAt >= SAMPLE_INTERVAL_MS) {
    lastSampleAt = now;
    sampleSensor();
  }

  if (now - lastDisplayAt >= 1000) {
    lastDisplayAt = now;
    drawDisplay();
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
  hasSavedNetwork = savedSsid.length() > 0;
}

void persistConfig() {
  preferences.putFloat("depthCm", config.containerDepthCm);
  preferences.putString("tankName", config.containerName);
  preferences.putFloat("warnPct", config.warningThresholdPercent);
  preferences.putFloat("criticalPct", config.criticalThresholdPercent);
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
  WiFi.softAP(apSsid.c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());
  apActive = true;
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
      stopApAt = now + WIFI_SETUP_AP_SHUTDOWN_MS;
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
      stopApAt = now + WIFI_SETUP_AP_SHUTDOWN_MS;
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
    reconnectDelayMs = min(reconnectDelayMs * 2, 60000UL);
  }

  if (now - disconnectedAt >= WIFI_AP_FALLBACK_MS) startProvisioningAp();
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

void configureWebServer() {
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  websocket.onEvent([](AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void*, uint8_t*, size_t) {
    if (type == WS_EVT_CONNECT) sendInitialWebSocketState(client);
  });
  server.addHandler(&websocket);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (apActive && WiFi.status() != WL_CONNECTED) sendProvisioningPage(request);
    else sendDashboard(request);
  });
  server.on("/generate_204", HTTP_GET, sendProvisioningPage);
  server.on("/gen_204", HTTP_GET, sendProvisioningPage);
  server.on("/hotspot-detect.html", HTTP_GET, sendProvisioningPage);
  server.on("/connecttest.txt", HTTP_GET, sendProvisioningPage);
  server.on("/ncsi.txt", HTTP_GET, sendProvisioningPage);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) { sendJson(request, statusJson()); });
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* request) { sendJson(request, configJson()); });
  server.on("/api/reading", HTTP_GET, [](AsyncWebServerRequest* request) { sendJson(request, readingJson()); });
  server.on("/api/network", HTTP_GET, [](AsyncWebServerRequest* request) { sendJson(request, networkJson()); });
  server.on("/api/setup/status", HTTP_GET, [](AsyncWebServerRequest* request) { sendJson(request, setupStatusJson()); });
  server.on("/api/setup/networks", HTTP_GET, handleNetworkScan);

  auto* configHandler = new AsyncCallbackJsonWebHandler("/api/config", [](AsyncWebServerRequest* request, JsonVariant& json) {
    handleConfigUpdate(request, json);
  });
  configHandler->setMethod(HTTP_PUT);
  configHandler->setMaxContentLength(1024);
  server.addHandler(configHandler);

  auto* connectHandler = new AsyncCallbackJsonWebHandler("/api/setup/connect", [](AsyncWebServerRequest* request, JsonVariant& json) {
    handleProvisioningRequest(request, json);
  });
  connectHandler->setMethod(HTTP_POST);
  connectHandler->setMaxContentLength(512);
  server.addHandler(connectHandler);

  auto* resetHandler = new AsyncCallbackJsonWebHandler("/api/network/reset", [](AsyncWebServerRequest* request, JsonVariant& json) {
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

  server.onNotFound([](AsyncWebServerRequest* request) {
    if (apActive) sendProvisioningPage(request);
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
  if (depth <= 0 || depth > 10000 || warning < 0 || warning > 100 || critical < 0 || critical > warning || name.length() > 64) {
    request->send(400, "application/json", "{\"message\":\"Invalid container configuration.\"}");
    return;
  }
  config.containerDepthCm = depth;
  config.containerName = name;
  config.warningThresholdPercent = warning;
  config.criticalThresholdPercent = critical;
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
  latestReading.timestamp = isoTimestamp();

  const float rawDistance = duration > 0 ? duration * 0.034f / 2.0f : NAN;
  if (duration <= 0 || rawDistance > MAX_DISTANCE_CM) {
    latestReading.distanceCm = NAN;
    latestReading.waterDepthCm = NAN;
    latestReading.waterPercent = NAN;
    latestReading.state = "out_of_range";
  } else if (rawDistance < 0) {
    latestReading.distanceCm = NAN;
    latestReading.state = "invalid";
  } else {
    validSamples[validSampleIndex] = rawDistance;
    validSampleIndex = (validSampleIndex + 1) % 5;
    if (validSampleCount < 5) ++validSampleCount;
    latestReading.distanceCm = medianSample();
    latestReading.state = "ok";
    recalculateReading();
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
  latestReading.waterDepthCm = max(config.containerDepthCm - latestReading.distanceCm, 0.0f);
  latestReading.waterPercent = constrain((latestReading.waterDepthCm / config.containerDepthCm) * 100.0f, 0.0f, 100.0f);
  if (latestReading.distanceCm > config.containerDepthCm) latestReading.state = "out_of_range";
}

float medianSample() {
  float values[5];
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
  document["containerDepthCm"] = config.containerDepthCm;
  document["containerName"] = config.containerName;
  document["warningThresholdPercent"] = config.warningThresholdPercent;
  document["criticalThresholdPercent"] = config.criticalThresholdPercent;
  document["preferredMode"] = "wifi";
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
      if (line == "WIFI_RESET") {
        preferences.remove("wifiSsid");
        preferences.remove("wifiPass");
        Serial.println("{\"event\":\"wifi_reset\",\"message\":\"restarting\"}");
        delay(100);
        ESP.restart();
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
