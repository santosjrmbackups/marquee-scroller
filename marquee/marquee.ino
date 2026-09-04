/** The MIT License (MIT)
 *  Copyright (c) 2018 David Payne
 *  OpenHAB MQTT LED marquee conversion
 *
 *  OpenHAB owns all content. This firmware joins Wi-Fi, talks MQTT, and
 *  renders text. OpenWeatherMap, OctoPrint, Pi-hole, News API, and
 *  TimeZoneDB are not used.
 */

#include "Settings.h"

#define VERSION "4.00"
#define HOSTNAME_PREFIX "MARQUEE-"
#define CONFIG_PATH "/conf.txt"
#define MAX_TEXT_LEN 240

static const unsigned long MQTT_BACKOFF_MS[] = {2000UL, 5000UL, 15000UL, 30000UL};
static const uint8_t MQTT_BACKOFF_COUNT = 4;

const int spacer = 1;
const int width = 5 + spacer;

Max72xxPanel *matrix = nullptr;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
ESP8266WebServer server(WEBSERVER_PORT);
ESP8266HTTPUpdateServer serverUpdater;

char mqttHostBuf[80];
char mqttClientIdBuf[48];
char mqttUserBuf[48];
char mqttPassBuf[64];
char mqttWillTopicBuf[96];

bool abortScroll = false;
bool textDirtyFs = false;
bool settingsDirtyFs = false;
unsigned long settingsDirtyAt = 0;
unsigned long lastMqttAttempt = 0;
uint8_t mqttBackoffIndex = 0;

int8_t getWifiQuality();
void pump();
void readSettings();
void writeSettings();
void initMatrix();
void applyNtp();
void mqttService();
void mqttCallback(char *topic, uint8_t *payload, unsigned int length);
void handleRoot();
void handleSave();
void handleDisplayToggle();
void handleForgetWifi();
void handleFactoryReset();
void redirectHome();
bool requireAuth();
void scrollMessage(const String &msg);
void centerPrint(const String &msg);
void enableDisplay(bool enable);
void applyText(const String &raw, bool persistNow);
void applyMode(const String &raw);
void applyBrightness(const String &raw);
void applySpeed(const String &raw);
void applyEnable(const String &raw);
String sanitizeText(const String &in);
String extractJsonString(const String &json, const char *key);
String htmlEscape(const String &s);
String clockText(bool blinkColon);
String displayPayload();
bool mqttConfigured();
bool ntpSynced();
void ensureIdentity();
void markSettingsDirty();
void persistIfDue();
String topicPath(const char *leaf);
void subscribeMqtt();
void configModeCallback(WiFiManager *myWiFiManager);

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("OpenHAB MQTT Marquee " VERSION));

  if (!LittleFS.begin()) {
    Serial.println(F("LittleFS mount failed, formatting"));
    LittleFS.format();
    LittleFS.begin();
  }

  readSettings();
  ensureIdentity();
  initMatrix();

  if (matrix) {
    centerPrint("hello");
    delay(400);
  }

  WiFiManager wifiManager;
  wifiManager.setAPCallback(configModeCallback);
  String hostname = String(HOSTNAME_PREFIX) + String(ESP.getChipId(), HEX);
  hostname.toUpperCase();
  WiFi.hostname(hostname);
  if (!wifiManager.autoConnect(hostname.c_str())) {
    delay(2000);
    ESP.reset();
  }

  Serial.print(F("Wi-Fi RSSI "));
  Serial.print(getWifiQuality());
  Serial.println('%');

  applyNtp();

  if (ENABLE_OTA) {
    ArduinoOTA.setHostname(hostname.c_str());
    if (OTA_Password.length() > 0) {
      ArduinoOTA.setPassword(OTA_Password.c_str());
    }
    ArduinoOTA.begin();
  }

  if (WEBSERVER_ENABLED) {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/display", HTTP_GET, handleDisplayToggle);
    server.on("/forgetwifi", HTTP_GET, handleForgetWifi);
    server.on("/reset", HTTP_GET, handleFactoryReset);
    server.onNotFound(redirectHome);
    serverUpdater.setup(&server, "/update", www_username.c_str(), www_password.c_str());
    server.begin();
    Serial.print(F("Web UI http://"));
    Serial.println(WiFi.localIP());
  }

  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(20);
  mqtt.setSocketTimeout(2);

  if (matrix) {
    String boot = String("v") + VERSION + " IP:" + WiFi.localIP().toString();
    scrollMessage(boot);
  }
}

void loop() {
  pump();
  mqttService();
  persistIfDue();

  if (!displayOn || matrix == nullptr) {
    delay(20);
    return;
  }

  const String payload = displayPayload();
  if (displayMode == "scroll" && mqttConfigured() && marqueeMessage.length() > 0) {
    abortScroll = false;
    scrollMessage(marqueeMessage);
    return;
  }

  matrix->fillScreen(LOW);
  centerPrint(payload);
  delay(40);
}

void pump() {
  yield();
  if (WEBSERVER_ENABLED) {
    server.handleClient();
  }
  if (ENABLE_OTA) {
    ArduinoOTA.handle();
  }
  if (mqtt.connected()) {
    mqtt.loop();
  }
}

bool mqttConfigured() {
  return MQTT_ENABLED && mqttHost.length() > 0;
}

bool ntpSynced() {
  return time(nullptr) > 1000000000L;
}

String clockText(bool blinkColon) {
  if (!ntpEnabled || !ntpSynced()) {
    return "OH wait";
  }
  time_t nowt = time(nullptr);
  struct tm t;
  localtime_r(&nowt, &t);
  int h12 = t.tm_hour % 12;
  if (h12 == 0) {
    h12 = 12;
  }
  char buf[8];
  char sep = ':';
  if (blinkColon && (t.tm_sec % 2 == 0)) {
    sep = ' ';
  }
  snprintf(buf, sizeof(buf), "%d%c%02d", h12, sep, t.tm_min);
  return String(buf);
}

String displayPayload() {
  if (!mqttConfigured() || displayMode == "clock" || marqueeMessage.length() == 0) {
    return clockText(displayMode != "scroll");
  }
  return marqueeMessage;
}

void ensureIdentity() {
  if (mqttClientId.length() == 0) {
    mqttClientId = String("marquee-") + String(ESP.getChipId(), HEX);
  }
  if (mqttPrefix.length() == 0) {
    mqttPrefix = "openhab/marquee";
  }
  while (mqttPrefix.endsWith("/")) {
    mqttPrefix.remove(mqttPrefix.length() - 1);
  }
}

String topicPath(const char *leaf) {
  return mqttPrefix + "/" + leaf;
}

void initMatrix() {
  if (numberOfHorizontalDisplays < 1) numberOfHorizontalDisplays = 1;
  if (numberOfHorizontalDisplays > 16) numberOfHorizontalDisplays = 16;
  if (displayIntensity < 0) displayIntensity = 0;
  if (displayIntensity > 15) displayIntensity = 15;
  if (displayScrollSpeed < 10) displayScrollSpeed = 10;
  if (displayScrollSpeed > 200) displayScrollSpeed = 200;

  delete matrix;
  matrix = new Max72xxPanel(pinCS, numberOfHorizontalDisplays, numberOfVerticalDisplays);
  const int maxPos = numberOfHorizontalDisplays * numberOfVerticalDisplays;
  for (int i = 0; i < maxPos; i++) {
    matrix->setRotation(i, ledRotation);
    matrix->setPosition(i, maxPos - i - 1, 0);
  }
  matrix->setIntensity(displayIntensity);
  matrix->fillScreen(LOW);
  matrix->write();
  if (!displayOn) {
    matrix->shutdown(true);
  }
}

void applyNtp() {
  if (!ntpEnabled) {
    return;
  }
  if (posixTZ.length() == 0) {
    posixTZ = "UTC0";
  }
  configTime(posixTZ.c_str(), "pool.ntp.org", "time.nist.gov");
}

void enableDisplay(bool enable) {
  displayOn = enable;
  if (matrix) {
    matrix->shutdown(!enable);
    if (enable) {
      matrix->fillScreen(LOW);
      matrix->write();
    }
  }
}

void markSettingsDirty() {
  settingsDirtyFs = true;
  settingsDirtyAt = millis();
}

void persistIfDue() {
  if (textDirtyFs) {
    textDirtyFs = false;
    writeSettings();
    settingsDirtyFs = false;
    return;
  }
  if (settingsDirtyFs && (millis() - settingsDirtyAt > 2000UL)) {
    settingsDirtyFs = false;
    writeSettings();
  }
}

void subscribeMqtt() {
  mqtt.subscribe(topicPath("text").c_str());
  mqtt.subscribe(topicPath("text/set").c_str());
  mqtt.subscribe(topicPath("clear").c_str());
  mqtt.subscribe(topicPath("mode").c_str());
  mqtt.subscribe(topicPath("brightness").c_str());
  mqtt.subscribe(topicPath("speed").c_str());
  mqtt.subscribe(topicPath("enable").c_str());
}

void mqttService() {
  if (!mqttConfigured() || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (mqtt.connected()) {
    mqtt.loop();
    mqttBackoffIndex = 0;
    return;
  }

  unsigned long wait = MQTT_BACKOFF_MS[mqttBackoffIndex < MQTT_BACKOFF_COUNT ? mqttBackoffIndex : (MQTT_BACKOFF_COUNT - 1)];
  if (lastMqttAttempt != 0 && millis() - lastMqttAttempt < wait) {
    return;
  }
  lastMqttAttempt = millis();

  mqttHost.toCharArray(mqttHostBuf, sizeof(mqttHostBuf));
  mqttClientId.toCharArray(mqttClientIdBuf, sizeof(mqttClientIdBuf));
  mqttUser.toCharArray(mqttUserBuf, sizeof(mqttUserBuf));
  mqttPass.toCharArray(mqttPassBuf, sizeof(mqttPassBuf));
  topicPath("status").toCharArray(mqttWillTopicBuf, sizeof(mqttWillTopicBuf));

  mqtt.setServer(mqttHostBuf, mqttPort);

  Serial.print(F("MQTT connect "));
  Serial.print(mqttHostBuf);
  Serial.print(':');
  Serial.println(mqttPort);

  bool ok;
  if (mqttUser.length() > 0) {
    ok = mqtt.connect(mqttClientIdBuf, mqttUserBuf, mqttPassBuf, mqttWillTopicBuf, 0, true, "offline");
  } else {
    ok = mqtt.connect(mqttClientIdBuf, mqttWillTopicBuf, 0, true, "offline");
  }

  if (ok) {
    Serial.println(F("MQTT connected"));
    mqttBackoffIndex = 0;
    mqtt.publish(mqttWillTopicBuf, "online", true);
    mqtt.publish(topicPath("ip").c_str(), WiFi.localIP().toString().c_str(), true);
    subscribeMqtt();
  } else {
    Serial.print(F("MQTT failed, state="));
    Serial.println(mqtt.state());
    if (mqttBackoffIndex < MQTT_BACKOFF_COUNT - 1) {
      mqttBackoffIndex++;
    }
  }
}

void mqttCallback(char *topic, uint8_t *payload, unsigned int length) {
  String t = topic;
  String body;
  body.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    body += (char)payload[i];
  }
  body.trim();

  const String prefix = mqttPrefix + "/";
  if (!t.startsWith(prefix)) {
    return;
  }
  String leaf = t.substring(prefix.length());

  if (leaf == "text" || leaf == "text/set") {
    applyText(body, true);
  } else if (leaf == "clear") {
    applyText("", true);
  } else if (leaf == "mode") {
    applyMode(body);
  } else if (leaf == "brightness") {
    applyBrightness(body);
  } else if (leaf == "speed") {
    applySpeed(body);
  } else if (leaf == "enable") {
    applyEnable(body);
  }
}

void applyText(const String &raw, bool persistNow) {
  String body = raw;
  body.trim();
  String lower = body;
  lower.toLowerCase();
  if (lower == "null" || lower == "undef" || lower == "undefined") {
    body = "";
  }
  if (body.startsWith("{")) {
    String extracted = extractJsonString(body, "message");
    if (extracted.length() == 0) {
      extracted = extractJsonString(body, "text");
    }
    body = extracted;
  }
  marqueeMessage = sanitizeText(body);
  abortScroll = true;
  if (persistNow) {
    textDirtyFs = true;
  }
  Serial.print(F("Text: "));
  Serial.println(marqueeMessage);
}

void applyMode(const String &raw) {
  String m = raw;
  m.trim();
  m.toLowerCase();
  if (m == "scroll" || m == "center" || m == "clock") {
    displayMode = m;
    abortScroll = true;
    markSettingsDirty();
  }
}

void applyBrightness(const String &raw) {
  String b = raw;
  b.trim();
  b.toUpperCase();
  if (b == "ON") {
    displayIntensity = 15;
  } else if (b == "OFF") {
    displayIntensity = 0;
  } else {
    int v = b.toInt();
    if (v > 15 && v <= 100) {
      v = (v * 15 + 50) / 100;
    }
    if (v < 0) v = 0;
    if (v > 15) v = 15;
    displayIntensity = v;
  }
  if (matrix) {
    matrix->setIntensity(displayIntensity);
  }
  markSettingsDirty();
}

void applySpeed(const String &raw) {
  int v = raw.toInt();
  if (v < 10) v = 10;
  if (v > 200) v = 200;
  displayScrollSpeed = v;
  markSettingsDirty();
}

void applyEnable(const String &raw) {
  String e = raw;
  e.trim();
  e.toLowerCase();
  bool on = (e == "on" || e == "1" || e == "true" || e == "yes");
  bool off = (e == "off" || e == "0" || e == "false" || e == "no");
  if (on) {
    enableDisplay(true);
    markSettingsDirty();
  } else if (off) {
    enableDisplay(false);
    markSettingsDirty();
  }
}

String sanitizeText(const String &in) {
  String out;
  out.reserve(min((int)in.length(), MAX_TEXT_LEN));
  unsigned i = 0;
  while (i < in.length() && out.length() < MAX_TEXT_LEN) {
    unsigned char c = in[i];
    unsigned char c1 = (i + 1 < in.length()) ? (unsigned char)in[i + 1] : 0;
    unsigned char c2 = (i + 2 < in.length()) ? (unsigned char)in[i + 2] : 0;

    if (c == 0xC2 && c1 == 0xB0) {
      out += (char)248;
      i += 2;
    } else if (c == 0xC2 && c1 == 0xBA) {
      out += (char)248;
      i += 2;
    } else if (c == 0xE2 && c1 == 0x80 && (c2 == 0x93 || c2 == 0x94)) {
      out += '-';
      i += 3;
    } else if (c == 0xE2 && c1 == 0x80 && (c2 == 0x98 || c2 == 0x99)) {
      out += '\'';
      i += 3;
    } else if (c == 0xE2 && c1 == 0x80 && (c2 == 0x9C || c2 == 0x9D)) {
      out += '"';
      i += 3;
    } else if (c == 176 || c == 247) {
      out += (char)248;
      i++;
    } else if (c >= 32 && c <= 126) {
      out += (char)c;
      i++;
    } else if (c >= 0xC0) {
      int skip = 1;
      if ((c & 0xF8) == 0xF0) skip = 4;
      else if ((c & 0xF0) == 0xE0) skip = 3;
      else if ((c & 0xE0) == 0xC0) skip = 2;
      out += '?';
      i += skip;
    } else {
      out += '?';
      i++;
    }
  }
  return out;
}

String extractJsonString(const String &json, const char *key) {
  String pat = String("\"") + key + "\"";
  int k = json.indexOf(pat);
  if (k < 0) {
    return "";
  }
  int colon = json.indexOf(':', k + pat.length());
  if (colon < 0) {
    return "";
  }
  int i = colon + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t')) {
    i++;
  }
  if (i < (int)json.length() && json[i] == '"') {
    i++;
    String val;
    while (i < (int)json.length() && json[i] != '"') {
      if (json[i] == '\\' && i + 1 < (int)json.length()) {
        val += json[i + 1];
        i += 2;
      } else {
        val += json[i++];
      }
    }
    return val;
  }
  String val;
  while (i < (int)json.length() && json[i] != ',' && json[i] != '}' && json[i] != ' ') {
    val += json[i++];
  }
  return val;
}

void scrollMessage(const String &msg) {
  if (!matrix) {
    return;
  }
  String text = msg + " ";
  const int frames = width * text.length() + matrix->width() - 1 - spacer;
  for (int i = 0; i < frames; i++) {
    pump();
    if (abortScroll) {
      abortScroll = false;
      break;
    }
    if (!displayOn) {
      break;
    }
    matrix->fillScreen(LOW);
    int letter = i / width;
    int x = (matrix->width() - 1) - (i % width);
    int y = (matrix->height() - 8) / 2;
    while (x + width - spacer >= 0 && letter >= 0) {
      if (letter < (int)text.length()) {
        matrix->drawChar(x, y, text[letter], HIGH, LOW, 1);
      }
      letter--;
      x -= width;
    }
    matrix->write();
    delay(displayScrollSpeed);
  }
}

void centerPrint(const String &msg) {
  if (!matrix) {
    return;
  }
  int x = (matrix->width() - (int)(msg.length() * width)) / 2;
  if (x < 0) {
    x = 0;
  }
  matrix->setCursor(x, 0);
  matrix->print(msg);
  matrix->write();
}

int8_t getWifiQuality() {
  int32_t dbm = WiFi.RSSI();
  if (dbm <= -100) return 0;
  if (dbm >= -50) return 100;
  return 2 * (dbm + 100);
}

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println(F("Config portal"));
  Serial.println(myWiFiManager->getConfigPortalSSID());
  if (matrix) {
    centerPrint("wifi");
  }
}

void readSettings() {
  if (!LittleFS.exists(CONFIG_PATH)) {
    Serial.println(F("No config yet, writing defaults"));
    writeSettings();
    return;
  }
  File fr = LittleFS.open(CONFIG_PATH, "r");
  if (!fr) {
    Serial.println(F("Config open failed"));
    return;
  }
  while (fr.available()) {
    String line = fr.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (line.startsWith("mqttEnabled=")) {
      MQTT_ENABLED = line.substring(12).toInt();
    } else if (line.startsWith("mqttHost=")) {
      mqttHost = line.substring(9);
      mqttHost.trim();
    } else if (line.startsWith("mqttPort=")) {
      mqttPort = line.substring(9).toInt();
      if (mqttPort <= 0) mqttPort = 1883;
    } else if (line.startsWith("mqttUser=")) {
      mqttUser = line.substring(9);
      mqttUser.trim();
    } else if (line.startsWith("mqttPass=")) {
      mqttPass = line.substring(9);
      mqttPass.trim();
    } else if (line.startsWith("mqttClientId=")) {
      mqttClientId = line.substring(13);
      mqttClientId.trim();
    } else if (line.startsWith("mqttPrefix=")) {
      mqttPrefix = line.substring(11);
      mqttPrefix.trim();
    } else if (line.startsWith("ledIntensity=")) {
      displayIntensity = line.substring(13).toInt();
    } else if (line.startsWith("scrollSpeed=")) {
      displayScrollSpeed = line.substring(12).toInt();
    } else if (line.startsWith("panelCount=")) {
      numberOfHorizontalDisplays = line.substring(11).toInt();
    } else if (line.startsWith("ntpEnabled=")) {
      ntpEnabled = line.substring(11).toInt();
    } else if (line.startsWith("posixTZ=")) {
      posixTZ = line.substring(8);
      posixTZ.trim();
    } else if (line.startsWith("marqueeMessage=")) {
      marqueeMessage = sanitizeText(line.substring(15));
    } else if (line.startsWith("displayMode=")) {
      displayMode = line.substring(12);
      displayMode.trim();
      displayMode.toLowerCase();
      if (displayMode != "scroll" && displayMode != "center" && displayMode != "clock") {
        displayMode = "scroll";
      }
    } else if (line.startsWith("displayOn=")) {
      displayOn = line.substring(10).toInt();
    } else if (line.startsWith("www_username=")) {
      www_username = line.substring(13);
      www_username.trim();
    } else if (line.startsWith("www_password=")) {
      www_password = line.substring(13);
      www_password.trim();
    } else if (line.startsWith("IS_BASIC_AUTH=")) {
      IS_BASIC_AUTH = line.substring(14).toInt();
    }
  }
  fr.close();
}

void writeSettings() {
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) {
    Serial.println(F("Config write failed"));
    return;
  }
  f.println("mqttEnabled=" + String(MQTT_ENABLED ? 1 : 0));
  f.println("mqttHost=" + mqttHost);
  f.println("mqttPort=" + String(mqttPort));
  f.println("mqttUser=" + mqttUser);
  f.println("mqttPass=" + mqttPass);
  f.println("mqttClientId=" + mqttClientId);
  f.println("mqttPrefix=" + mqttPrefix);
  f.println("ledIntensity=" + String(displayIntensity));
  f.println("scrollSpeed=" + String(displayScrollSpeed));
  f.println("panelCount=" + String(numberOfHorizontalDisplays));
  f.println("ntpEnabled=" + String(ntpEnabled ? 1 : 0));
  f.println("posixTZ=" + posixTZ);
  f.println("marqueeMessage=" + marqueeMessage);
  f.println("displayMode=" + displayMode);
  f.println("displayOn=" + String(displayOn ? 1 : 0));
  f.println("www_username=" + www_username);
  f.println("www_password=" + www_password);
  f.println("IS_BASIC_AUTH=" + String(IS_BASIC_AUTH ? 1 : 0));
  f.close();
  Serial.println(F("Settings saved"));
}

bool requireAuth() {
  if (!IS_BASIC_AUTH) {
    return true;
  }
  if (server.authenticate(www_username.c_str(), www_password.c_str())) {
    return true;
  }
  server.requestAuthentication();
  return false;
}

void redirectHome() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleDisplayToggle() {
  if (!requireAuth()) return;
  enableDisplay(!displayOn);
  markSettingsDirty();
  redirectHome();
}

void handleForgetWifi() {
  if (!requireAuth()) return;
  redirectHome();
  delay(200);
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  ESP.restart();
}

void handleFactoryReset() {
  if (!requireAuth()) return;
  LittleFS.remove(CONFIG_PATH);
  redirectHome();
  delay(200);
  ESP.restart();
}

String htmlEscape(const String &s) {
  String o;
  o.reserve(s.length());
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += F("&amp;");
    else if (c == '<') o += F("&lt;");
    else if (c == '>') o += F("&gt;");
    else if (c == '"') o += F("&quot;");
    else o += c;
  }
  return o;
}

void handleRoot() {
  if (!requireAuth()) return;

  String mqttState = mqtt.connected() ? "online" : (mqttConfigured() ? "offline" : "disabled");
  String ip = WiFi.localIP().toString();

  String html;
  html.reserve(4200);
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>OpenHAB MQTT Marquee</title>");
  html += F("<link rel='stylesheet' href='https://www.w3schools.com/w3css/4/w3.css'>");
  html += F("<link rel='stylesheet' href='https://www.w3schools.com/lib/w3-theme-blue-grey.css'>");
  html += F("</head><body class='w3-theme-l5'>");
  html += F("<header class='w3-container w3-theme'><h2>OpenHAB MQTT Marquee</h2></header>");
  html += F("<div class='w3-container w3-section'>");
  html += "<p>Version " VERSION " &nbsp; IP " + htmlEscape(ip) +
          " &nbsp; MQTT <b>" + mqttState + "</b> &nbsp; RSSI " + String(getWifiQuality()) + "%</p>";
  html += F("<p><a class='w3-button w3-theme' href='/display'>");
  html += displayOn ? F("Turn display OFF") : F("Turn display ON");
  html += F("</a> <a class='w3-button w3-grey' href='/update'>Firmware Update</a> ");
  html += F("<a class='w3-button w3-orange' href='/forgetwifi' onclick=\"return confirm('Forget Wi-Fi and reboot into AP?')\">Forget Wi-Fi</a> ");
  html += F("<a class='w3-button w3-red' href='/reset' onclick=\"return confirm('Erase settings and reboot?')\">Factory reset</a></p>");

  html += F("<form class='w3-container w3-card w3-white w3-padding' method='POST' action='/save'>");
  html += F("<h3>MQTT</h3>");
  html += F("<p><input class='w3-check' type='checkbox' name='mqttEnabled'");
  if (MQTT_ENABLED) html += F(" checked");
  html += F("> Enabled</p>");
  html += "<p><label>Host</label><input class='w3-input w3-border' name='mqttHost' value='" + htmlEscape(mqttHost) + "' maxlength='79'></p>";
  html += "<p><label>Port</label><input class='w3-input w3-border' name='mqttPort' type='number' value='" + String(mqttPort) + "'></p>";
  html += "<p><label>Username (empty = anonymous)</label><input class='w3-input w3-border' name='mqttUser' value='" + htmlEscape(mqttUser) + "' maxlength='47'></p>";
  html += F("<p><label>Password (leave blank to keep stored)</label><input class='w3-input w3-border' name='mqttPass' type='password' value='' maxlength='63'></p>");
  html += F("<p><input class='w3-check' type='checkbox' name='mqttAnonymous'> Clear user/password (anonymous)</p>");
  html += "<p><label>Client ID</label><input class='w3-input w3-border' name='mqttClientId' value='" + htmlEscape(mqttClientId) + "' maxlength='47'></p>";
  html += "<p><label>Topic prefix</label><input class='w3-input w3-border' name='mqttPrefix' value='" + htmlEscape(mqttPrefix) + "' maxlength='79'></p>";

  html += F("<h3>Display</h3>");
  html += "<p><label>Panel count (1–16, reboot on change)</label><input class='w3-input w3-border' name='panelCount' type='number' min='1' max='16' value='" + String(numberOfHorizontalDisplays) + "'></p>";
  html += "<p><label>Brightness 0–15</label><input class='w3-input w3-border' name='ledintensity' type='number' min='0' max='15' value='" + String(displayIntensity) + "'></p>";
  html += "<p><label>Scroll speed (ms/pixel, 10–200)</label><input class='w3-input w3-border' name='scrollspeed' type='number' min='10' max='200' value='" + String(displayScrollSpeed) + "'></p>";
  html += F("<p><label>Mode</label><select class='w3-select w3-border' name='displayMode'>");
  html += F("<option value='scroll'");
  if (displayMode == "scroll") html += F(" selected");
  html += F(">scroll</option><option value='center'");
  if (displayMode == "center") html += F(" selected");
  html += F(">center</option><option value='clock'");
  if (displayMode == "clock") html += F(" selected");
  html += F(">clock</option></select></p>");
  html += "<p><label>Current text (test)</label><input class='w3-input w3-border' name='marqueeMsg' value='" + htmlEscape(marqueeMessage) + "' maxlength='240'></p>";

  html += F("<h3>NTP</h3>");
  html += F("<p><input class='w3-check' type='checkbox' name='ntpEnabled'");
  if (ntpEnabled) html += F(" checked");
  html += F("> NTP clock fallback</p>");
  html += "<p><label>POSIX timezone</label><input class='w3-input w3-border' name='posixTZ' value='" + htmlEscape(posixTZ) + "' maxlength='47'></p>";
  html += F("<p class='w3-small'>Idle clock only. Send preformatted time in MQTT text for OpenHAB local time. Example: <code>EST5EDT,M3.2.0,M11.1.0</code></p>");

  html += F("<h3>Web security</h3>");
  html += F("<p><input class='w3-check' type='checkbox' name='isBasicAuth'");
  if (IS_BASIC_AUTH) html += F(" checked");
  html += F("> Require basic auth</p>");
  html += "<p><label>User</label><input class='w3-input w3-border' name='userid' value='" + htmlEscape(www_username) + "' maxlength='31'></p>";
  html += "<p><label>Password</label><input class='w3-input w3-border' name='stationpassword' type='password' value='" + htmlEscape(www_password) + "' maxlength='31'></p>";
  html += F("<p><button class='w3-button w3-theme w3-section' type='submit'>Save</button></p>");
  html += F("</form>");
  html += F("<p class='w3-small'>OpenWeatherMap, OctoPrint, Pi-hole, and News API were removed. OpenHAB owns the content.</p>");
  html += F("</div></body></html>");
  server.send(200, "text/html", html);
}

void handleSave() {
  if (!requireAuth()) return;

  int oldPanels = numberOfHorizontalDisplays;

  MQTT_ENABLED = server.hasArg("mqttEnabled");
  mqttHost = server.arg("mqttHost");
  mqttHost.trim();
  mqttPort = server.arg("mqttPort").toInt();
  if (mqttPort <= 0) mqttPort = 1883;
  mqttUser = server.arg("mqttUser");
  mqttUser.trim();
  String newPass = server.arg("mqttPass");
  if (newPass.length() > 0) {
    mqttPass = newPass;
  }
  if (server.hasArg("mqttAnonymous")) {
    mqttUser = "";
    mqttPass = "";
  }
  mqttClientId = server.arg("mqttClientId");
  mqttClientId.trim();
  mqttPrefix = server.arg("mqttPrefix");
  mqttPrefix.trim();

  numberOfHorizontalDisplays = server.arg("panelCount").toInt();
  displayIntensity = server.arg("ledintensity").toInt();
  displayScrollSpeed = server.arg("scrollspeed").toInt();
  applyMode(server.arg("displayMode"));
  applyText(server.arg("marqueeMsg"), false);

  ntpEnabled = server.hasArg("ntpEnabled");
  posixTZ = server.arg("posixTZ");
  posixTZ.trim();

  IS_BASIC_AUTH = server.hasArg("isBasicAuth");
  String u = server.arg("userid");
  u.trim();
  if (u.length() > 0) www_username = u;
  String p = server.arg("stationpassword");
  if (p.length() > 0) www_password = p;

  ensureIdentity();
  if (displayIntensity < 0) displayIntensity = 0;
  if (displayIntensity > 15) displayIntensity = 15;
  if (displayScrollSpeed < 10) displayScrollSpeed = 10;
  if (displayScrollSpeed > 200) displayScrollSpeed = 200;
  if (numberOfHorizontalDisplays < 1) numberOfHorizontalDisplays = 1;
  if (numberOfHorizontalDisplays > 16) numberOfHorizontalDisplays = 16;

  writeSettings();
  if (matrix) {
    matrix->setIntensity(displayIntensity);
  }
  applyNtp();

  if (mqtt.connected()) {
    mqtt.disconnect();
  }
  mqttBackoffIndex = 0;
  lastMqttAttempt = 0;

  if (oldPanels != numberOfHorizontalDisplays) {
    server.send(200, "text/html", F("<html><body>Panel count changed, rebooting…</body></html>"));
    delay(500);
    ESP.restart();
    return;
  }

  redirectHome();
}
