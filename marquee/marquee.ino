/** The MIT License (MIT)
 *  Copyright (c) 2018 David Payne
 *  OpenHAB MQTT LED marquee
 *
 *  OpenHAB / HTTP / Grafana webhook own content. This firmware joins Wi-Fi,
 *  talks MQTT, optionally polls HTTP JSON, and renders text.
 */

#include "Settings.h"

#define VERSION "4.10"
#define HOSTNAME_PREFIX "MARQUEE-"
#define MDNS_NAME "marquee"
#define CONFIG_PATH "/conf.txt"
#define MAX_TEXT_LEN 240
#define HTTP_BODY_MAX 3072

static const unsigned long MQTT_BACKOFF_MS[] = {2000UL, 5000UL, 15000UL, 30000UL};
static const uint8_t MQTT_BACKOFF_COUNT = 4;

const int spacer = 1;
const int width = 5 + spacer;

Max72xxPanel *matrix = nullptr;
WiFiClient wifiClient;
WiFiClient httpNet;
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
bool didScrollOnce = false;
unsigned long settingsDirtyAt = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastHttpPoll = 0;
uint8_t mqttBackoffIndex = 0;

String httpText = "";
String lastError = "";
String lastHttpStatus = "idle";
unsigned long nextHttpDue = 0;

uint16_t fg() { return invertDisplay ? LOW : HIGH; }
uint16_t bg() { return invertDisplay ? HIGH : LOW; }

int8_t getWifiQuality();
void pump();
void readSettings();
void writeSettings();
void initMatrix();
void applyNtp();
void mqttService();
void mqttCallback(char *topic, uint8_t *payload, unsigned int length);
void httpPollService();
void handleRoot();
void handleSave();
void handleDisplayToggle();
void handleForgetWifi();
void handleFactoryReset();
void handleIdentify();
void handleIngest();
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
void applyEffectiveBrightness();
String sanitizeText(const String &in);
String extractJsonString(const String &json, const char *key);
String jsonPathGet(const String &json, const String &path);
String htmlEscape(const String &s);
String clockText(bool blinkColon);
String composeLine();
String displayPayload();
bool mqttConfigured();
bool ntpSynced();
bool inNightWindow();
int parseHHMM(const String &s);
int minutesNow();
void ensureIdentity();
void markSettingsDirty();
void persistIfDue();
String topicPath(const char *leaf);
void subscribeMqtt();
void configModeCallback(WiFiManager *myWiFiManager);
void pauseSeconds(int sec);
String applyTemplate(const String &tmpl, const String *vals, int n);

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

  applyNtp();
  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", 80);

  if (ENABLE_OTA) {
    ArduinoOTA.setHostname(hostname.c_str());
    if (OTA_Password.length() > 0) {
      ArduinoOTA.setPassword(OTA_Password.c_str());
    }
    ArduinoOTA.begin();
  }

  if (WEBSERVER_ENABLED) {
    server.collectHeaders("Authorization");
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/display", HTTP_GET, handleDisplayToggle);
    server.on("/forgetwifi", HTTP_GET, handleForgetWifi);
    server.on("/reset", HTTP_GET, handleFactoryReset);
    server.on("/identify", HTTP_GET, handleIdentify);
    server.on("/ingest", HTTP_POST, handleIngest);
    server.onNotFound(redirectHome);
    serverUpdater.setup(&server, "/update", www_username.c_str(), www_password.c_str());
    server.begin();
    Serial.print(F("Web UI http://"));
    Serial.println(WiFi.localIP());
    Serial.println(F("mDNS http://marquee.local"));
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
  httpPollService();
  persistIfDue();
  applyEffectiveBrightness();

  if (!displayOn || matrix == nullptr) {
    delay(20);
    return;
  }

  const String line = displayPayload();
  const bool hasContent = composeLine().length() > 0;
  if (displayMode == "scroll" && hasContent && !( !scrollLoop && didScrollOnce )) {
    abortScroll = false;
    scrollMessage(line);
    didScrollOnce = true;
    pauseSeconds(scrollPauseSec);
    return;
  }

  matrix->fillScreen(bg());
  centerPrint(line);
  delay(40);
}

void pump() {
  yield();
  MDNS.update();
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

void pauseSeconds(int sec) {
  if (sec <= 0) return;
  unsigned long end = millis() + (unsigned long)sec * 1000UL;
  while ((long)(end - millis()) > 0) {
    pump();
    if (abortScroll) break;
    delay(20);
  }
}

bool mqttConfigured() {
  return MQTT_ENABLED && mqttHost.length() > 0;
}

bool ntpSynced() {
  return time(nullptr) > 1000000000L;
}

int minutesNow() {
  if (!ntpSynced()) return -1;
  time_t nowt = time(nullptr);
  struct tm t;
  localtime_r(&nowt, &t);
  return t.tm_hour * 60 + t.tm_min;
}

int parseHHMM(const String &s) {
  if (s.length() < 4) return -1;
  int colon = s.indexOf(':');
  if (colon < 1) return -1;
  int h = s.substring(0, colon).toInt();
  int m = s.substring(colon + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

bool inNightWindow() {
  int a = parseHHMM(nightStart);
  int b = parseHHMM(nightEnd);
  int now = minutesNow();
  if (a < 0 || b < 0 || now < 0 || a == b) return false;
  if (a < b) return now >= a && now < b;
  return now >= a || now < b;
}

void applyEffectiveBrightness() {
  if (!matrix) return;
  int v = inNightWindow() ? nightIntensity : displayIntensity;
  if (v < 0) v = 0;
  if (v > 15) v = 15;
  matrix->setIntensity(v);
}

String clockText(bool blinkColon) {
  if (!ntpEnabled || !ntpSynced()) {
    return "OH wait";
  }
  time_t nowt = time(nullptr);
  struct tm t;
  localtime_r(&nowt, &t);
  char buf[8];
  char sep = ':';
  if (blinkColon && flashOnSeconds && (t.tm_sec % 2 == 0)) {
    sep = ' ';
  }
  if (IS_24HOUR) {
    snprintf(buf, sizeof(buf), "%02d%c%02d", t.tm_hour, sep, t.tm_min);
  } else {
    int h12 = t.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    snprintf(buf, sizeof(buf), "%d%c%02d", h12, sep, t.tm_min);
  }
  return String(buf);
}

String composeLine() {
  String parts[4];
  int n = 0;
  if (marqueeMessage.length() > 0 && n < 4) parts[n++] = marqueeMessage;
  if (httpText.length() > 0 && n < 4) parts[n++] = httpText;
  if (playlist2.length() > 0 && n < 4) parts[n++] = playlist2;
  if (playlist3.length() > 0 && n < 4) parts[n++] = playlist3;
  if (n == 0) return "";
  String out = parts[0];
  for (int i = 1; i < n; i++) {
    out += "  ";
    out += parts[i];
  }
  if ((int)out.length() > MAX_TEXT_LEN) {
    out = out.substring(0, MAX_TEXT_LEN);
  }
  return out;
}

String displayPayload() {
  if (displayMode == "clock") {
    return clockText(true);
  }
  String line = composeLine();
  if (line.length() == 0) {
    return clockText(displayMode != "scroll");
  }
  return line;
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
  if (httpIntervalSec < 15) httpIntervalSec = 15;
  if (httpIntervalSec > 300) httpIntervalSec = 300;
  if (scrollPauseSec < 0) scrollPauseSec = 0;
  if (scrollPauseSec > 30) scrollPauseSec = 30;
  if (ledRotation < 0) ledRotation = 0;
  if (ledRotation > 3) ledRotation = 3;
}

String topicPath(const char *leaf) {
  return mqttPrefix + "/" + leaf;
}

void initMatrix() {
  if (numberOfHorizontalDisplays < 1) numberOfHorizontalDisplays = 1;
  if (numberOfHorizontalDisplays > 16) numberOfHorizontalDisplays = 16;
  if (displayIntensity < 0) displayIntensity = 0;
  if (displayIntensity > 15) displayIntensity = 15;
  if (nightIntensity < 0) nightIntensity = 0;
  if (nightIntensity > 15) nightIntensity = 15;
  if (displayScrollSpeed < 10) displayScrollSpeed = 10;
  if (displayScrollSpeed > 200) displayScrollSpeed = 200;

  delete matrix;
  matrix = new Max72xxPanel(pinCS, numberOfHorizontalDisplays, numberOfVerticalDisplays);
  const int maxPos = numberOfHorizontalDisplays * numberOfVerticalDisplays;
  for (int i = 0; i < maxPos; i++) {
    matrix->setRotation(i, ledRotation);
    if (reverseChain) {
      matrix->setPosition(i, i, 0);
    } else {
      matrix->setPosition(i, maxPos - i - 1, 0);
    }
  }
  applyEffectiveBrightness();
  matrix->fillScreen(bg());
  matrix->write();
  if (!displayOn) {
    matrix->shutdown(true);
  }
}

void applyNtp() {
  if (!ntpEnabled) return;
  if (posixTZ.length() == 0) posixTZ = "UTC0";
  configTime(posixTZ.c_str(), "pool.ntp.org", "time.nist.gov");
}

void enableDisplay(bool enable) {
  displayOn = enable;
  if (matrix) {
    matrix->shutdown(!enable);
    if (enable) {
      matrix->fillScreen(bg());
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
  if (!mqttConfigured() || WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) {
    mqtt.loop();
    mqttBackoffIndex = 0;
    return;
  }
  unsigned long wait = MQTT_BACKOFF_MS[mqttBackoffIndex < MQTT_BACKOFF_COUNT ? mqttBackoffIndex : (MQTT_BACKOFF_COUNT - 1)];
  if (lastMqttAttempt != 0 && millis() - lastMqttAttempt < wait) return;
  lastMqttAttempt = millis();

  mqttHost.toCharArray(mqttHostBuf, sizeof(mqttHostBuf));
  mqttClientId.toCharArray(mqttClientIdBuf, sizeof(mqttClientIdBuf));
  mqttUser.toCharArray(mqttUserBuf, sizeof(mqttUserBuf));
  mqttPass.toCharArray(mqttPassBuf, sizeof(mqttPassBuf));
  topicPath("status").toCharArray(mqttWillTopicBuf, sizeof(mqttWillTopicBuf));
  mqtt.setServer(mqttHostBuf, mqttPort);

  bool ok;
  if (mqttUser.length() > 0) {
    ok = mqtt.connect(mqttClientIdBuf, mqttUserBuf, mqttPassBuf, mqttWillTopicBuf, 0, true, "offline");
  } else {
    ok = mqtt.connect(mqttClientIdBuf, mqttWillTopicBuf, 0, true, "offline");
  }
  if (ok) {
    mqttBackoffIndex = 0;
    mqtt.publish(mqttWillTopicBuf, "online", true);
    mqtt.publish(topicPath("ip").c_str(), WiFi.localIP().toString().c_str(), true);
    subscribeMqtt();
    lastError = "";
  } else {
    lastError = "MQTT state " + String(mqtt.state());
    if (mqttBackoffIndex < MQTT_BACKOFF_COUNT - 1) mqttBackoffIndex++;
  }
}

void mqttCallback(char *topic, uint8_t *payload, unsigned int length) {
  String t = topic;
  String body;
  body.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) body += (char)payload[i];
  body.trim();
  const String prefix = mqttPrefix + "/";
  if (!t.startsWith(prefix)) return;
  String leaf = t.substring(prefix.length());
  if (leaf == "text" || leaf == "text/set") applyText(body, true);
  else if (leaf == "clear") applyText("", true);
  else if (leaf == "mode") applyMode(body);
  else if (leaf == "brightness") applyBrightness(body);
  else if (leaf == "speed") applySpeed(body);
  else if (leaf == "enable") applyEnable(body);
}

void applyText(const String &raw, bool persistNow) {
  String body = raw;
  body.trim();
  String lower = body;
  lower.toLowerCase();
  if (lower == "null" || lower == "undef" || lower == "undefined") body = "";
  if (body.startsWith("{")) {
    String extracted = extractJsonString(body, "text");
    if (extracted.length() == 0) extracted = extractJsonString(body, "message");
    if (extracted.length() == 0) extracted = extractJsonString(body, "title");
    body = extracted;
  }
  marqueeMessage = sanitizeText(body);
  abortScroll = true;
  didScrollOnce = false;
  if (persistNow) textDirtyFs = true;
}

void applyMode(const String &raw) {
  String m = raw;
  m.trim();
  m.toLowerCase();
  if (m == "scroll" || m == "center" || m == "clock") {
    displayMode = m;
    abortScroll = true;
    didScrollOnce = false;
    markSettingsDirty();
  }
}

void applyBrightness(const String &raw) {
  String b = raw;
  b.trim();
  b.toUpperCase();
  if (b == "ON") displayIntensity = 15;
  else if (b == "OFF") displayIntensity = 0;
  else {
    int v = b.toInt();
    if (v > 15 && v <= 100) v = (v * 15 + 50) / 100;
    if (v < 0) v = 0;
    if (v > 15) v = 15;
    displayIntensity = v;
  }
  applyEffectiveBrightness();
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
  if (e == "on" || e == "1" || e == "true" || e == "yes") {
    enableDisplay(true);
    markSettingsDirty();
  } else if (e == "off" || e == "0" || e == "false" || e == "no") {
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
    if (c == 0xC2 && (c1 == 0xB0 || c1 == 0xBA)) {
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
    } else if (c == 176 || c == 247 || c == 248) {
      out += (char)248;
      i++;
    } else if (c >= 32 && c <= 126) {
      out += (char)c;
      i++;
    } else if (c >= 0xC0) {
      int skip = ((c & 0xF8) == 0xF0) ? 4 : ((c & 0xF0) == 0xE0) ? 3 : 2;
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
  if (k < 0) return "";
  int colon = json.indexOf(':', k + pat.length());
  if (colon < 0) return "";
  int i = colon + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n')) i++;
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
  while (i < (int)json.length() && json[i] != ',' && json[i] != '}' && json[i] != ']' && json[i] != ' ' && json[i] != '\n') {
    val += json[i++];
  }
  return val;
}

static int skipWs(const String &s, int i) {
  while (i < (int)s.length() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
  return i;
}

static int skipJsonValue(const String &s, int i) {
  i = skipWs(s, i);
  if (i >= (int)s.length()) return i;
  char c = s[i];
  if (c == '"') {
    i++;
    while (i < (int)s.length()) {
      if (s[i] == '\\') {
        i += 2;
        continue;
      }
      if (s[i] == '"') return i + 1;
      i++;
    }
    return i;
  }
  if (c == '{' || c == '[') {
    char open = c;
    char close = (c == '{') ? '}' : ']';
    int depth = 0;
    for (; i < (int)s.length(); i++) {
      if (s[i] == '"') {
        i = skipJsonValue(s, i) - 1;
        continue;
      }
      if (s[i] == open) depth++;
      else if (s[i] == close) {
        depth--;
        if (depth == 0) return i + 1;
      }
    }
    return i;
  }
  while (i < (int)s.length() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n') i++;
  return i;
}

static bool isIndexToken(const String &tok) {
  if (tok.length() == 0) return false;
  for (unsigned i = 0; i < tok.length(); i++) {
    if (tok[i] < '0' || tok[i] > '9') return false;
  }
  return true;
}

static String slicePrimitive(const String &json, int from, int to) {
  from = skipWs(json, from);
  while (to > from && (json[to - 1] == ' ' || json[to - 1] == '\n' || json[to - 1] == '\r')) to--;
  if (from < to && json[from] == '"' && json[to - 1] == '"') {
    String v;
    for (int i = from + 1; i < to - 1; i++) {
      if (json[i] == '\\' && i + 1 < to - 1) {
        v += json[i + 1];
        i++;
      } else {
        v += json[i];
      }
    }
    return v;
  }
  return json.substring(from, to);
}

String jsonPathGet(const String &json, const String &path) {
  int from = skipWs(json, 0);
  int to = json.length();
  int start = 0;
  while (start < (int)path.length()) {
    int dot = path.indexOf('.', start);
    if (dot < 0) dot = path.length();
    String tok = path.substring(start, dot);
    tok.trim();
    start = dot + 1;
    from = skipWs(json, from);
    if (tok.length() == 0) continue;
    if (from >= to) return "";
    if (json[from] == '[' && isIndexToken(tok)) {
      int want = tok.toInt();
      int i = from + 1;
      int idx = 0;
      while (i < to) {
        i = skipWs(json, i);
        if (i < to && json[i] == ']') break;
        int vend = skipJsonValue(json, i);
        if (idx == want) {
          from = i;
          to = vend;
          break;
        }
        i = skipWs(json, vend);
        if (i < to && json[i] == ',') i++;
        idx++;
        if (idx > want) return "";
      }
    } else if (json[from] == '{') {
      String key = "\"" + tok + "\"";
      int i = from + 1;
      int depth = 1;
      bool found = false;
      while (i < to && depth > 0) {
        i = skipWs(json, i);
        if (i >= to) break;
        if (depth == 1 && json[i] == '"' && json.substring(i, i + key.length()) == key) {
          int after = i + key.length();
          after = skipWs(json, after);
          if (after < to && json[after] == ':') {
            from = skipWs(json, after + 1);
            to = skipJsonValue(json, from);
            found = true;
            break;
          }
        }
        if (json[i] == '"') {
          i = skipJsonValue(json, i);
          continue;
        }
        if (json[i] == '{' || json[i] == '[') {
          i = skipJsonValue(json, i);
          continue;
        }
        if (json[i] == '}' || json[i] == ']') {
          depth--;
          i++;
          continue;
        }
        i++;
      }
      if (!found) return "";
    } else {
      return "";
    }
  }
  return slicePrimitive(json, from, to);
}

String applyTemplate(const String &tmpl, const String *vals, int n) {
  if (tmpl.length() == 0) {
    String o;
    for (int i = 0; i < n; i++) {
      if (vals[i].length() == 0) continue;
      if (o.length()) o += ' ';
      o += vals[i];
    }
    return o;
  }
  String o = tmpl;
  const char *names[] = {"{value}", "{value2}", "{value3}"};
  for (int i = 0; i < n && i < 3; i++) {
    o.replace(names[i], vals[i]);
  }
  return o;
}

void httpPollService() {
  if (!httpPollEnabled || httpUrl.length() == 0 || WiFi.status() != WL_CONNECTED) return;
  unsigned long interval = (unsigned long)httpIntervalSec * 1000UL;
  if (lastHttpPoll != 0 && millis() - lastHttpPoll < interval) {
    nextHttpDue = lastHttpPoll + interval;
    return;
  }
  lastHttpPoll = millis();
  nextHttpDue = lastHttpPoll + interval;

  if (httpUrl.startsWith("https://")) {
    lastHttpStatus = "HTTPS not supported";
    lastError = lastHttpStatus;
    if (httpOnFail == "wait") httpText = "";
    else if (httpOnFail == "error") httpText = "HTTP err";
    return;
  }

  HTTPClient http;
  http.setTimeout(3000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(httpNet, httpUrl)) {
    lastHttpStatus = "begin failed";
    lastError = lastHttpStatus;
    return;
  }
  if (httpBearer.length() > 0) {
    http.addHeader("Authorization", "Bearer " + httpBearer);
  } else if (httpUser.length() > 0) {
    http.setAuthorization(httpUser.c_str(), httpPass.c_str());
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    lastHttpStatus = "HTTP " + String(code);
    lastError = lastHttpStatus;
    http.end();
    if (httpOnFail == "wait") httpText = "";
    else if (httpOnFail == "error") httpText = "HTTP err";
    abortScroll = true;
    return;
  }

  String body;
  body.reserve(HTTP_BODY_MAX);
  WiFiClient *stream = http.getStreamPtr();
  int len = http.getSize();
  uint8_t buf[128];
  while (http.connected() && (len > 0 || len == -1) && (int)body.length() < HTTP_BODY_MAX) {
    size_t avail = stream->available();
    if (avail) {
      size_t chunk = avail < sizeof(buf) ? avail : sizeof(buf);
      int c = stream->readBytes(buf, chunk);
      for (int i = 0; i < c && (int)body.length() < HTTP_BODY_MAX; i++) body += (char)buf[i];
      if (len > 0) len -= c;
    } else {
      delay(1);
      yield();
    }
  }
  http.end();

  String vals[3];
  int n = 0;
  if (httpJsonPath.length() > 0) {
    String paths = httpJsonPath;
    int start = 0;
    while (start < (int)paths.length() && n < 3) {
      int comma = paths.indexOf(',', start);
      if (comma < 0) comma = paths.length();
      String p = paths.substring(start, comma);
      p.trim();
      start = comma + 1;
      if (p.length() == 0) continue;
      vals[n++] = jsonPathGet(body, p);
    }
  } else if (body.startsWith("{") || body.startsWith("[")) {
    vals[n++] = extractJsonString(body, "text");
    if (vals[0].length() == 0) vals[0] = extractJsonString(body, "value");
  } else {
    int nl = body.indexOf('\n');
    vals[n++] = (nl < 0) ? body : body.substring(0, nl);
    vals[0].trim();
  }

  String rendered = sanitizeText(applyTemplate(httpTemplate, vals, n));
  if (rendered.length() == 0) {
    lastHttpStatus = "empty value";
    lastError = lastHttpStatus;
    if (httpOnFail == "wait") httpText = "";
    else if (httpOnFail == "error") httpText = "HTTP err";
  } else {
    httpText = rendered;
    lastHttpStatus = "ok";
    lastError = "";
    abortScroll = true;
    didScrollOnce = false;
  }
}

static void drawScrollFrame(const String &text, int i) {
  matrix->fillScreen(bg());
  int letter = i / width;
  int x = (matrix->width() - 1) - (i % width);
  int y = (matrix->height() - 8) / 2;
  while (x + width - spacer >= 0 && letter >= 0) {
    if (letter < (int)text.length()) {
      matrix->drawChar(x, y, text[letter], fg(), bg(), 1);
    }
    letter--;
    x -= width;
  }
  matrix->write();
}

void scrollMessage(const String &msg) {
  if (!matrix) return;
  String text = msg + " ";
  const int frames = width * (int)text.length() + matrix->width() - 1 - spacer;
  if (scrollLeft) {
    for (int i = 0; i < frames; i++) {
      pump();
      if (abortScroll || !displayOn) {
        abortScroll = false;
        break;
      }
      drawScrollFrame(text, i);
      delay(displayScrollSpeed);
    }
  } else {
    for (int i = frames - 1; i >= 0; i--) {
      pump();
      if (abortScroll || !displayOn) {
        abortScroll = false;
        break;
      }
      drawScrollFrame(text, i);
      delay(displayScrollSpeed);
    }
  }
}

void centerPrint(const String &msg) {
  if (!matrix) return;
  int x = (matrix->width() - (int)(msg.length() * width)) / 2;
  if (x < 0) x = 0;
  if (!IS_24HOUR && ntpSynced()) {
    time_t nowt = time(nullptr);
    struct tm t;
    localtime_r(&nowt, &t);
    if (t.tm_hour >= 12) {
      matrix->drawPixel(matrix->width() - 1, 6, fg());
    }
  }
  matrix->setTextColor(fg(), bg());
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
  if (matrix) centerPrint("wifi");
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

void readSettings() {
  if (!LittleFS.exists(CONFIG_PATH)) {
    writeSettings();
    return;
  }
  File fr = LittleFS.open(CONFIG_PATH, "r");
  if (!fr) return;
  while (fr.available()) {
    String line = fr.readStringUntil('\n');
    line.trim();
    if (line.startsWith("mqttEnabled=")) MQTT_ENABLED = line.substring(12).toInt();
    else if (line.startsWith("mqttHost=")) { mqttHost = line.substring(9); mqttHost.trim(); }
    else if (line.startsWith("mqttPort=")) { mqttPort = line.substring(9).toInt(); if (mqttPort <= 0) mqttPort = 1883; }
    else if (line.startsWith("mqttUser=")) { mqttUser = line.substring(9); mqttUser.trim(); }
    else if (line.startsWith("mqttPass=")) { mqttPass = line.substring(9); mqttPass.trim(); }
    else if (line.startsWith("mqttClientId=")) { mqttClientId = line.substring(13); mqttClientId.trim(); }
    else if (line.startsWith("mqttPrefix=")) { mqttPrefix = line.substring(11); mqttPrefix.trim(); }
    else if (line.startsWith("ledIntensity=")) displayIntensity = line.substring(13).toInt();
    else if (line.startsWith("nightIntensity=")) nightIntensity = line.substring(15).toInt();
    else if (line.startsWith("nightStart=")) { nightStart = line.substring(11); nightStart.trim(); }
    else if (line.startsWith("nightEnd=")) { nightEnd = line.substring(9); nightEnd.trim(); }
    else if (line.startsWith("scrollSpeed=")) displayScrollSpeed = line.substring(12).toInt();
    else if (line.startsWith("scrollPause=")) scrollPauseSec = line.substring(12).toInt();
    else if (line.startsWith("scrollLeft=")) scrollLeft = line.substring(11).toInt();
    else if (line.startsWith("scrollLoop=")) scrollLoop = line.substring(11).toInt();
    else if (line.startsWith("panelCount=")) numberOfHorizontalDisplays = line.substring(11).toInt();
    else if (line.startsWith("ledRotation=")) ledRotation = line.substring(12).toInt();
    else if (line.startsWith("reverseChain=")) reverseChain = line.substring(13).toInt();
    else if (line.startsWith("invertDisplay=")) invertDisplay = line.substring(14).toInt();
    else if (line.startsWith("is24hour=")) IS_24HOUR = line.substring(9).toInt();
    else if (line.startsWith("flashSeconds=")) flashOnSeconds = line.substring(13).toInt();
    else if (line.startsWith("ntpEnabled=")) ntpEnabled = line.substring(11).toInt();
    else if (line.startsWith("posixTZ=")) { posixTZ = line.substring(8); posixTZ.trim(); }
    else if (line.startsWith("marqueeMessage=")) marqueeMessage = sanitizeText(line.substring(15));
    else if (line.startsWith("playlist2=")) playlist2 = sanitizeText(line.substring(10));
    else if (line.startsWith("playlist3=")) playlist3 = sanitizeText(line.substring(10));
    else if (line.startsWith("displayMode=")) {
      displayMode = line.substring(12); displayMode.trim(); displayMode.toLowerCase();
      if (displayMode != "scroll" && displayMode != "center" && displayMode != "clock") displayMode = "scroll";
    } else if (line.startsWith("displayOn=")) displayOn = line.substring(10).toInt();
    else if (line.startsWith("www_username=")) { www_username = line.substring(13); www_username.trim(); }
    else if (line.startsWith("www_password=")) { www_password = line.substring(13); www_password.trim(); }
    else if (line.startsWith("IS_BASIC_AUTH=")) IS_BASIC_AUTH = line.substring(14).toInt();
    else if (line.startsWith("httpPollEnabled=")) httpPollEnabled = line.substring(16).toInt();
    else if (line.startsWith("httpUrl=")) { httpUrl = line.substring(8); httpUrl.trim(); }
    else if (line.startsWith("httpInterval=")) httpIntervalSec = line.substring(13).toInt();
    else if (line.startsWith("httpJsonPath=")) { httpJsonPath = line.substring(13); httpJsonPath.trim(); }
    else if (line.startsWith("httpTemplate=")) { httpTemplate = line.substring(13); httpTemplate.trim(); }
    else if (line.startsWith("httpUser=")) { httpUser = line.substring(9); httpUser.trim(); }
    else if (line.startsWith("httpPass=")) { httpPass = line.substring(9); httpPass.trim(); }
    else if (line.startsWith("httpBearer=")) { httpBearer = line.substring(11); httpBearer.trim(); }
    else if (line.startsWith("httpOnFail=")) { httpOnFail = line.substring(11); httpOnFail.trim(); }
    else if (line.startsWith("ingestToken=")) { ingestToken = line.substring(12); ingestToken.trim(); }
  }
  fr.close();
}

void writeSettings() {
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return;
  f.println("mqttEnabled=" + String(MQTT_ENABLED ? 1 : 0));
  f.println("mqttHost=" + mqttHost);
  f.println("mqttPort=" + String(mqttPort));
  f.println("mqttUser=" + mqttUser);
  f.println("mqttPass=" + mqttPass);
  f.println("mqttClientId=" + mqttClientId);
  f.println("mqttPrefix=" + mqttPrefix);
  f.println("ledIntensity=" + String(displayIntensity));
  f.println("nightIntensity=" + String(nightIntensity));
  f.println("nightStart=" + nightStart);
  f.println("nightEnd=" + nightEnd);
  f.println("scrollSpeed=" + String(displayScrollSpeed));
  f.println("scrollPause=" + String(scrollPauseSec));
  f.println("scrollLeft=" + String(scrollLeft ? 1 : 0));
  f.println("scrollLoop=" + String(scrollLoop ? 1 : 0));
  f.println("panelCount=" + String(numberOfHorizontalDisplays));
  f.println("ledRotation=" + String(ledRotation));
  f.println("reverseChain=" + String(reverseChain ? 1 : 0));
  f.println("invertDisplay=" + String(invertDisplay ? 1 : 0));
  f.println("is24hour=" + String(IS_24HOUR ? 1 : 0));
  f.println("flashSeconds=" + String(flashOnSeconds ? 1 : 0));
  f.println("ntpEnabled=" + String(ntpEnabled ? 1 : 0));
  f.println("posixTZ=" + posixTZ);
  f.println("marqueeMessage=" + marqueeMessage);
  f.println("playlist2=" + playlist2);
  f.println("playlist3=" + playlist3);
  f.println("displayMode=" + displayMode);
  f.println("displayOn=" + String(displayOn ? 1 : 0));
  f.println("www_username=" + www_username);
  f.println("www_password=" + www_password);
  f.println("IS_BASIC_AUTH=" + String(IS_BASIC_AUTH ? 1 : 0));
  f.println("httpPollEnabled=" + String(httpPollEnabled ? 1 : 0));
  f.println("httpUrl=" + httpUrl);
  f.println("httpInterval=" + String(httpIntervalSec));
  f.println("httpJsonPath=" + httpJsonPath);
  f.println("httpTemplate=" + httpTemplate);
  f.println("httpUser=" + httpUser);
  f.println("httpPass=" + httpPass);
  f.println("httpBearer=" + httpBearer);
  f.println("httpOnFail=" + httpOnFail);
  f.println("ingestToken=" + ingestToken);
  f.close();
}

bool requireAuth() {
  if (!IS_BASIC_AUTH) return true;
  if (server.authenticate(www_username.c_str(), www_password.c_str())) return true;
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

void handleIdentify() {
  if (!requireAuth()) return;
  if (!matrix) {
    redirectHome();
    return;
  }
  matrix->shutdown(false);
  for (int p = 0; p < numberOfHorizontalDisplays; p++) {
    matrix->fillScreen(bg());
    matrix->fillRect(p * 8, 0, 8, 8, fg());
    matrix->write();
    for (int w = 0; w < 18; w++) {
      pump();
      delay(40);
    }
  }
  matrix->fillScreen(bg());
  matrix->write();
  redirectHome();
}

static bool ingestAuthOk() {
  if (ingestToken.length() == 0) return true;
  String tok = server.arg("token");
  if (tok.length() == 0) {
    String auth = server.header("Authorization");
    if (auth.startsWith("Bearer ")) tok = auth.substring(7);
  }
  tok.trim();
  return tok == ingestToken;
}

void handleIngest() {
  if (ingestToken.length() > 0) {
    if (!ingestAuthOk()) {
      server.send(401, "text/plain", "unauthorized");
      return;
    }
  } else if (!requireAuth()) {
    return;
  }
  String body = server.arg("plain");
  if (body.length() == 0 && server.hasArg("text")) body = server.arg("text");
  applyText(body, true);
  if (marqueeMessage.length() == 0 && body.startsWith("{")) {
    String t = extractJsonString(body, "title");
    if (t.length()) applyText(t, true);
  }
  server.send(200, "text/plain", "ok");
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

static void optSel(String &html, const char *val, const String &cur, const char *label) {
  html += F("<option value='");
  html += val;
  html += '\'';
  if (cur == val) html += F(" selected");
  html += '>';
  html += label;
  html += F("</option>");
}

void handleRoot() {
  if (!requireAuth()) return;
  String mqttState = mqtt.connected() ? "online" : (mqttConfigured() ? "offline" : "disabled");
  String ip = WiFi.localIP().toString();
  String line = composeLine();
  if (line.length() == 0) line = displayPayload();
  long nextIn = 0;
  if (httpPollEnabled && nextHttpDue > millis()) nextIn = (long)((nextHttpDue - millis()) / 1000UL);
  int bright = inNightWindow() ? nightIntensity : displayIntensity;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent(F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"));
  server.sendContent(F("<title>OpenHAB MQTT Marquee</title>"));
  server.sendContent(F("<link rel='stylesheet' href='https://www.w3schools.com/w3css/4/w3.css'>"));
  server.sendContent(F("<link rel='stylesheet' href='https://www.w3schools.com/lib/w3-theme-blue-grey.css'></head>"));
  server.sendContent(F("<body class='w3-theme-l5'><header class='w3-container w3-theme'><h2>OpenHAB MQTT Marquee</h2></header>"));
  server.sendContent(F("<div class='w3-container w3-section'>"));

  String st;
  st.reserve(900);
  st += "<p>v" VERSION " &nbsp; <a href='http://";
  st += ip;
  st += "'>";
  st += htmlEscape(ip);
  st += "</a> &nbsp; <code>http://marquee.local</code><br>";
  st += "MQTT <b>";
  st += mqttState;
  st += "</b> &nbsp; HTTP <b>";
  st += htmlEscape(lastHttpStatus);
  st += "</b> &nbsp; RSSI ";
  st += String(getWifiQuality());
  st += "% &nbsp; bright ";
  st += String(bright);
  st += inNightWindow() ? " (night)" : " (day)";
  if (httpPollEnabled) {
    st += " &nbsp; next poll ";
    st += String(nextIn);
    st += "s";
  }
  st += "</p><p>Now: <b>";
  st += htmlEscape(line);
  st += "</b></p>";
  if (lastError.length()) {
    st += "<p class='w3-text-red'>Last error: ";
    st += htmlEscape(lastError);
    st += "</p>";
  }
  server.sendContent(st);

  server.sendContent(F("<p><a class='w3-button w3-theme' href='/display'>"));
  server.sendContent(displayOn ? F("Turn display OFF") : F("Turn display ON"));
  server.sendContent(F("</a> <a class='w3-button w3-blue' href='/identify'>Identify panels</a> "));
  server.sendContent(F("<a class='w3-button w3-grey' href='/update'>Firmware Update</a> "));
  server.sendContent(F("<a class='w3-button w3-orange' href='/forgetwifi' onclick=\"return confirm('Forget Wi-Fi?')\">Forget Wi-Fi</a> "));
  server.sendContent(F("<a class='w3-button w3-red' href='/reset' onclick=\"return confirm('Erase settings?')\">Factory reset</a></p>"));

  server.sendContent(F("<form class='w3-container w3-card w3-white w3-padding' method='POST' action='/save'>"));

  String f;
  f.reserve(3500);
  f += F("<h3>Display / LED</h3>");
  f += "<p><label>Panel count 1–16 (reboot)</label><input class='w3-input w3-border' name='panelCount' type='number' min='1' max='16' value='" + String(numberOfHorizontalDisplays) + "'></p>";
  f += F("<p><label>Rotation</label><select class='w3-select w3-border' name='ledRotation'>");
  optSel(f, "0", String(ledRotation), "0 none");
  optSel(f, "1", String(ledRotation), "1 90 CW");
  optSel(f, "2", String(ledRotation), "2 180");
  optSel(f, "3", String(ledRotation), "3 90 CCW (typical)");
  f += F("</select></p>");
  f += F("<p><input class='w3-check' type='checkbox' name='reverseChain'");
  if (reverseChain) f += F(" checked");
  f += F("> Reverse module order</p>");
  f += F("<p><input class='w3-check' type='checkbox' name='invertDisplay'");
  if (invertDisplay) f += F(" checked");
  f += F("> Invert pixels</p>");
  f += "<p><label>Day brightness 0–15</label><input class='w3-input w3-border' name='ledintensity' type='number' min='0' max='15' value='" + String(displayIntensity) + "'></p>";
  f += "<p><label>Night brightness 0–15</label><input class='w3-input w3-border' name='nightIntensity' type='number' min='0' max='15' value='" + String(nightIntensity) + "'></p>";
  f += "<p><label>Night start</label><input class='w3-input w3-border' name='nightStart' type='time' value='" + htmlEscape(nightStart) + "'></p>";
  f += "<p><label>Night end</label><input class='w3-input w3-border' name='nightEnd' type='time' value='" + htmlEscape(nightEnd) + "'></p>";
  f += "<p><label>Scroll speed ms/pixel 10–200</label><input class='w3-input w3-border' name='scrollspeed' type='number' min='10' max='200' value='" + String(displayScrollSpeed) + "'></p>";
  f += "<p><label>Pause after scroll (seconds 0–30)</label><input class='w3-input w3-border' name='scrollPause' type='number' min='0' max='30' value='" + String(scrollPauseSec) + "'></p>";
  f += F("<p><label>Direction</label><select class='w3-select w3-border' name='scrollDir'>");
  optSel(f, "left", String(scrollLeft ? "left" : "right"), "left");
  optSel(f, "right", String(scrollLeft ? "left" : "right"), "right");
  f += F("</select></p>");
  f += F("<p><input class='w3-check' type='checkbox' name='scrollLoop'");
  if (scrollLoop) f += F(" checked");
  f += F("> Loop scroll (off = once, then clock)</p>");
  f += F("<p><label>Mode</label><select class='w3-select w3-border' name='displayMode'>");
  optSel(f, "scroll", displayMode, "scroll");
  optSel(f, "center", displayMode, "center");
  optSel(f, "clock", displayMode, "clock");
  f += F("</select></p>");
  f += F("<p><input class='w3-check' type='checkbox' name='is24hour'");
  if (IS_24HOUR) f += F(" checked");
  f += F("> 24-hour clock</p>");
  f += F("<p><input class='w3-check' type='checkbox' name='flashSeconds'");
  if (flashOnSeconds) f += F(" checked");
  f += F("> Blink colon</p>");
  f += "<p><label>MQTT / ingest text</label><input class='w3-input w3-border' name='marqueeMsg' value='" + htmlEscape(marqueeMessage) + "' maxlength='240'></p>";
  f += "<p><label>Playlist line 2</label><input class='w3-input w3-border' name='playlist2' value='" + htmlEscape(playlist2) + "' maxlength='80'></p>";
  f += "<p><label>Playlist line 3</label><input class='w3-input w3-border' name='playlist3' value='" + htmlEscape(playlist3) + "' maxlength='80'></p>";
  server.sendContent(f);
  f = "";

  f.reserve(2200);
  f += F("<h3>MQTT</h3>");
  f += F("<p><input class='w3-check' type='checkbox' name='mqttEnabled'");
  if (MQTT_ENABLED) f += F(" checked");
  f += F("> Enabled</p>");
  f += "<p><label>Host</label><input class='w3-input w3-border' name='mqttHost' value='" + htmlEscape(mqttHost) + "' maxlength='79'></p>";
  f += "<p><label>Port</label><input class='w3-input w3-border' name='mqttPort' type='number' value='" + String(mqttPort) + "'></p>";
  f += "<p><label>Username</label><input class='w3-input w3-border' name='mqttUser' value='" + htmlEscape(mqttUser) + "' maxlength='47'></p>";
  f += F("<p><label>Password (blank keeps stored)</label><input class='w3-input w3-border' name='mqttPass' type='password' value='' maxlength='63'></p>");
  f += F("<p><input class='w3-check' type='checkbox' name='mqttAnonymous'> Clear MQTT user/password</p>");
  f += "<p><label>Client ID</label><input class='w3-input w3-border' name='mqttClientId' value='" + htmlEscape(mqttClientId) + "' maxlength='47'></p>";
  f += "<p><label>Topic prefix</label><input class='w3-input w3-border' name='mqttPrefix' value='" + htmlEscape(mqttPrefix) + "' maxlength='79'></p>";
  server.sendContent(f);
  f = "";

  f.reserve(2800);
  f += F("<h3>HTTP / Grafana</h3>");
  f += F("<p class='w3-small'>LAN HTTP only (no HTTPS). Poll Prometheus, Grafana datasource proxy, OpenHAB REST, or any JSON. Push via <code>POST /ingest</code>.</p>");
  f += F("<p><input class='w3-check' type='checkbox' name='httpPollEnabled'");
  if (httpPollEnabled) f += F(" checked");
  f += F("> Enable HTTP poll</p>");
  f += "<p><label>URL</label><input class='w3-input w3-border' name='httpUrl' value='" + htmlEscape(httpUrl) + "' maxlength='200'></p>";
  f += "<p><label>Interval seconds 15–300</label><input class='w3-input w3-border' name='httpInterval' type='number' min='15' max='300' value='" + String(httpIntervalSec) + "'></p>";
  f += "<p><label>JSON paths (comma-separated)</label><input class='w3-input w3-border' name='httpJsonPath' value='" + htmlEscape(httpJsonPath) + "' maxlength='120' placeholder='data.result.0.value.1'></p>";
  f += "<p><label>Template</label><input class='w3-input w3-border' name='httpTemplate' value='" + htmlEscape(httpTemplate) + "' maxlength='80' placeholder='Out {value}C'></p>";
  f += "<p><label>HTTP user</label><input class='w3-input w3-border' name='httpUser' value='" + htmlEscape(httpUser) + "' maxlength='47'></p>";
  f += F("<p><label>HTTP password (blank keeps stored)</label><input class='w3-input w3-border' name='httpPass' type='password' value='' maxlength='63'></p>");
  f += "<p><label>Bearer token (blank keeps stored)</label><input class='w3-input w3-border' name='httpBearer' type='password' value='' maxlength='80'></p>";
  f += F("<p><input class='w3-check' type='checkbox' name='httpClearAuth'> Clear HTTP user/password/bearer</p>");
  f += F("<p><label>On failure</label><select class='w3-select w3-border' name='httpOnFail'>");
  optSel(f, "keep", httpOnFail, "keep last");
  optSel(f, "wait", httpOnFail, "clear (clock / OH wait)");
  optSel(f, "error", httpOnFail, "show HTTP err");
  f += F("</select></p>");
  f += "<p><label>Ingest token (optional, POST /ingest)</label><input class='w3-input w3-border' name='ingestToken' value='" + htmlEscape(ingestToken) + "' maxlength='47'></p>";
  f += "<p>HTTP text now: <code>";
  f += htmlEscape(httpText);
  f += "</code></p>";
  server.sendContent(f);
  f = "";

  f.reserve(1600);
  f += F("<h3>NTP</h3>");
  f += F("<p><input class='w3-check' type='checkbox' name='ntpEnabled'");
  if (ntpEnabled) f += F(" checked");
  f += F("> NTP clock fallback</p>");
  f += "<p><label>POSIX timezone</label><input class='w3-input w3-border' name='posixTZ' value='" + htmlEscape(posixTZ) + "' maxlength='47'></p>";
  f += F("<p class='w3-small'>Example <code>EST5EDT,M3.2.0,M11.1.0</code></p>");
  f += F("<h3>Web security</h3>");
  f += F("<p><input class='w3-check' type='checkbox' name='isBasicAuth'");
  if (IS_BASIC_AUTH) f += F(" checked");
  f += F("> Require basic auth</p>");
  f += "<p><label>User</label><input class='w3-input w3-border' name='userid' value='" + htmlEscape(www_username) + "' maxlength='31'></p>";
  f += "<p><label>Password</label><input class='w3-input w3-border' name='stationpassword' type='password' value='" + htmlEscape(www_password) + "' maxlength='31'></p>";
  f += F("<p><button class='w3-button w3-theme w3-section' type='submit'>Save</button></p></form>");
  f += F("<p class='w3-small'>OpenWeatherMap, OctoPrint, Pi-hole, and News API are not used.</p></div></body></html>");
  server.sendContent(f);
  server.sendContent("");
}

void handleSave() {
  if (!requireAuth()) return;
  int oldPanels = numberOfHorizontalDisplays;

  MQTT_ENABLED = server.hasArg("mqttEnabled");
  mqttHost = server.arg("mqttHost"); mqttHost.trim();
  mqttPort = server.arg("mqttPort").toInt();
  if (mqttPort <= 0) mqttPort = 1883;
  mqttUser = server.arg("mqttUser"); mqttUser.trim();
  if (server.arg("mqttPass").length() > 0) mqttPass = server.arg("mqttPass");
  if (server.hasArg("mqttAnonymous")) { mqttUser = ""; mqttPass = ""; }
  mqttClientId = server.arg("mqttClientId"); mqttClientId.trim();
  mqttPrefix = server.arg("mqttPrefix"); mqttPrefix.trim();

  numberOfHorizontalDisplays = server.arg("panelCount").toInt();
  ledRotation = server.arg("ledRotation").toInt();
  reverseChain = server.hasArg("reverseChain");
  invertDisplay = server.hasArg("invertDisplay");
  displayIntensity = server.arg("ledintensity").toInt();
  nightIntensity = server.arg("nightIntensity").toInt();
  nightStart = server.arg("nightStart"); nightStart.trim();
  nightEnd = server.arg("nightEnd"); nightEnd.trim();
  displayScrollSpeed = server.arg("scrollspeed").toInt();
  scrollPauseSec = server.arg("scrollPause").toInt();
  scrollLeft = (server.arg("scrollDir") != "right");
  scrollLoop = server.hasArg("scrollLoop");
  applyMode(server.arg("displayMode"));
  applyText(server.arg("marqueeMsg"), false);
  playlist2 = sanitizeText(server.arg("playlist2"));
  playlist3 = sanitizeText(server.arg("playlist3"));
  IS_24HOUR = server.hasArg("is24hour");
  flashOnSeconds = server.hasArg("flashSeconds");

  ntpEnabled = server.hasArg("ntpEnabled");
  posixTZ = server.arg("posixTZ"); posixTZ.trim();

  httpPollEnabled = server.hasArg("httpPollEnabled");
  httpUrl = server.arg("httpUrl"); httpUrl.trim();
  httpIntervalSec = server.arg("httpInterval").toInt();
  httpJsonPath = server.arg("httpJsonPath"); httpJsonPath.trim();
  httpTemplate = server.arg("httpTemplate"); httpTemplate.trim();
  httpUser = server.arg("httpUser"); httpUser.trim();
  if (server.arg("httpPass").length() > 0) httpPass = server.arg("httpPass");
  if (server.arg("httpBearer").length() > 0) httpBearer = server.arg("httpBearer");
  if (server.hasArg("httpClearAuth")) {
    httpUser = "";
    httpPass = "";
    httpBearer = "";
  }
  httpOnFail = server.arg("httpOnFail");
  if (httpOnFail != "wait" && httpOnFail != "error") httpOnFail = "keep";
  ingestToken = server.arg("ingestToken"); ingestToken.trim();

  IS_BASIC_AUTH = server.hasArg("isBasicAuth");
  String u = server.arg("userid"); u.trim();
  if (u.length() > 0) www_username = u;
  if (server.arg("stationpassword").length() > 0) www_password = server.arg("stationpassword");

  ensureIdentity();
  writeSettings();
  initMatrix();
  applyNtp();
  lastHttpPoll = 0;
  didScrollOnce = false;
  abortScroll = true;

  if (mqtt.connected()) mqtt.disconnect();
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
