#include <DNSServer.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <time.h>

#include "alert.h"
#include "ca_roots.h"
// The public server; a self-hosted bridge builds with -DALERT_URL='"https://host/peace-stick"'.
#ifndef ALERT_URL
#define ALERT_URL "https://62.238.111.55.nip.io/peace-stick"
#endif
// Optional: src/secrets.h with WIFI_SSID and WIFI_PASS seeds the first network. Without it the stick opens phone setup.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

const unsigned long POLL_MS = 3000, POLL_SAVER_MS = 6000;
const unsigned long WIFI_RETRY_MS = 15000, WIFI_JOIN_MS = 12000;
const unsigned long HISTORY_EVERY_MS = 60000;
const unsigned long FLASH_MS = 150, FLASH_FAST_MS = 80, DRAW_MS = 250;
const unsigned long TRIAL_MS = 300000;    // a new firmware that reaches no server in this time gives way to the old one
const int TRIAL_BOOTS = 3;
const uint8_t BRIGHT_LEVELS[] = {30, 70, 140}, VOLUME_LEVELS[] = {90, 170, 255};
const char* const LEVEL_NAMES[] = {"Low", "Medium", "High"};
const unsigned long CLEAR_SCREEN_MS = 5000, TEST_MS = 5000, BATTERY_EVERY_MS = 10000;
const unsigned long BACK_HOME_MS = 60000, SCREEN_OFF_MS = 30000, PORTAL_MS = 300000;
const unsigned long FLIP_HOLD_MS = 1000;  // long enough that bouncing in a bag does not silence an alarm
const float FACE_DOWN_G = -0.8f;
const int BEEP_CYCLES = 6;                // drones do not ring forever, a missile alert does
const int LOW_BATTERY = 10;
const int MAX_NETS = 8;
const uint8_t LED_PIN = 19;               // the red LED on the Plus2, shared with the IR transmitter
const uint8_t BRIGHT_SAVER = 40, BRIGHT_ACTIVE = 160, BRIGHT_ALARM = 255;
const char* const AP_NAME = "Vartovyi";

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
const uint16_t C_BG = rgb(13, 17, 23), C_PANEL = rgb(28, 34, 43), C_LINE = rgb(52, 60, 72);
const uint16_t C_TEXT = rgb(236, 240, 245), C_MUTED = rgb(138, 148, 162);
const uint16_t C_GREEN = rgb(52, 211, 153), C_DEEP_GREEN = rgb(6, 95, 70), C_AMBER = rgb(251, 191, 36), C_BLUE = rgb(96, 165, 250);
const uint16_t C_ORANGE = rgb(249, 115, 22), C_RED = rgb(239, 40, 40), C_BLACK = rgb(0, 0, 0), C_WHITE = rgb(255, 255, 255);

struct FetchResult {
  int code;
  Feed feed;
};
struct HistoryResult {
  int code;
  History history;
};
struct Step {
  uint16_t hz, ms;  // hz 0 is a pause
};
// The buzzer is loudest around 2.5-4 kHz.
const Step SIREN[] = {{2400, 280}, {3400, 280}};
const Step BEEPS[] = {{3000, 150}, {0, 120}, {3000, 150}, {0, 120}, {3000, 150}, {0, 900}};
const Step LINK_LOST[] = {{1600, 200}, {0, 1800}};
const Step CHIME[] = {{2000, 150}, {0, 40}, {2600, 150}, {0, 40}, {3300, 450}};
const Step SOFT[] = {{2600, 120}, {0, 80}};
const Step BALLISTIC[] = {{3700, 110}, {2300, 110}};

enum Page { P_HOME, P_MAP, P_DETAILS, P_HISTORY, P_STATS, P_SETTINGS, PAGE_COUNT };
const char* const PAGE_NAMES[] = {"VARTOVYI", "MAP", "ACTIVE", "HISTORY", "STATS", "SETTINGS"};
enum Menu { M_NONE, M_MAIN, M_ZONES };
enum MenuItem { MI_ZONES, MI_NIGHT, MI_FLIP, MI_POWER, MI_BRIGHT, MI_VOLUME, MI_TEST, MI_SETUP, MI_UPDATE, MI_NETWORK, MI_DELAY, MI_BACK, MI_COUNT };
enum PowerMode { POWER_AUTO, POWER_NORMAL, POWER_SAVER };

struct Settings {
  unsigned zones = DEFAULT_ZONES;
  bool nightOn = true, flip = true;
  int nightStart = 23, nightEnd = 7;
  int power = POWER_AUTO;
  int bright = 1, volume = 2;  // indexes into BRIGHT_LEVELS and VOLUME_LEVELS
};
struct Net {
  String ssid, pass;
};

M5Canvas canvas(&M5.Display);  // draw off screen, so a fast flash does not tear
QueueHandle_t results, historyResults;
SemaphoreHandle_t poolLock;
Preferences prefs;
WebServer server(80);
DNSServer dns;

Settings cfg;
Net pool[MAX_NETS];
int poolCount = 0;

static Feed feed;
static History history;
View view{};
Ringer ringer = {NEVER_ACKED, false};
bool hasData = false, chimePending = false, historyLoaded = false, historyFailed = false;
bool usb = true, batteryWarned = false, screenOff = false;
int battery = -1, batteryMv = 0;
unsigned long lastGood = 0, clearUntil = 0, testUntil = 0, lastBattery = 0, lastInput = 0, lastDraw = 0, lastHistory = 0;
unsigned long portalUntil = 0, flipSince = 0, lastFlipCheck = 0;
volatile bool historyWanted = false, portalOn = false, saverNow = false;
volatile unsigned maskNow = DEFAULT_ZONES;
volatile bool updateWanted = false;
volatile int updatePercent = -1;
const char* volatile updateStatus = "";
bool remindPending = false, trial = false;
unsigned long lastRing = 0, lastNightBeep = 0;
long lastDelay = -1;

Page page = P_HOME;
Menu menu = M_NONE;
int menuIndex = 0, scrollPos = 0;

String apPass, apQr;
String nearby[12];
int nearbyCount = 0;
bool portalStopRequested = false;

Sound playing = S_NONE;
int soundStep = 0, beepCycles = 0;
unsigned long nextStepAt = 0;

bool timeIsSynced(long long now) { return now > 1700000000; }  // TLS cannot check a certificate before NTP answers
bool due(unsigned long until, unsigned long t) { return until && (long)(until - t) > 0; }
bool isAlarm(Sound s) { return s == S_SIREN || s == S_BALLISTIC || s == S_BEEPS || s == S_LINK; }

// ---------- settings ----------

void savePool() {
  prefs.putInt("nets", poolCount);
  for (int i = 0; i < poolCount; i++) {
    prefs.putString(("s" + String(i)).c_str(), pool[i].ssid);
    prefs.putString(("p" + String(i)).c_str(), pool[i].pass);
  }
}

void saveSettings() {
  prefs.putUInt("zones", cfg.zones);
  prefs.putBool("night", cfg.nightOn);
  prefs.putBool("flip", cfg.flip);
  prefs.putInt("nStart", cfg.nightStart);
  prefs.putInt("nEnd", cfg.nightEnd);
  prefs.putInt("power", cfg.power);
  prefs.putInt("bright", cfg.bright);
  prefs.putInt("volume", cfg.volume);
  maskNow = cfg.zones;
}

void loadSettings() {
  prefs.begin("peace", false);
  cfg.zones = prefs.getUInt("zones", DEFAULT_ZONES);
  cfg.nightOn = prefs.getBool("night", true);
  cfg.flip = prefs.getBool("flip", true);
  cfg.nightStart = prefs.getInt("nStart", 23);
  cfg.nightEnd = prefs.getInt("nEnd", 7);
  cfg.power = prefs.getInt("power", POWER_AUTO);
  cfg.bright = constrain(prefs.getInt("bright", 1), 0, 2);
  cfg.volume = constrain(prefs.getInt("volume", 2), 0, 2);
  maskNow = cfg.zones;
  poolCount = min(prefs.getInt("nets", 0), MAX_NETS);
  for (int i = 0; i < poolCount; i++) {
    pool[i].ssid = prefs.getString(("s" + String(i)).c_str(), "");
    pool[i].pass = prefs.getString(("p" + String(i)).c_str(), "");
  }
#ifdef WIFI_SSID
  if (!poolCount) {  // first start: the network this firmware was built with
    pool[0] = {WIFI_SSID, WIFI_PASS};
    poolCount = 1;
    savePool();
  }
#endif
}

// ---------- network ----------

// Joins the first saved network that is in range: the list is in order of preference, so home wins over the hotspot.
// shortcut: a hidden network never appears in a scan, make it visible or a list with hidden names will need a direct try.
void connectStep() {
  static unsigned long lastTry = 0;
  if (WiFi.status() == WL_CONNECTED || portalOn || !poolCount) return;  // no scan while setup scans
  if (lastTry && millis() - lastTry < WIFI_RETRY_MS) return;
  int found = WiFi.scanNetworks();
  Net saved[MAX_NETS];
  xSemaphoreTake(poolLock, portMAX_DELAY);
  int count = poolCount;
  for (int i = 0; i < count; i++) saved[i] = pool[i];
  xSemaphoreGive(poolLock);
  for (int i = 0; i < count && WiFi.status() != WL_CONNECTED && !portalOn; i++) {
    bool inRange = false;
    for (int j = 0; j < found && !inRange; j++) inRange = WiFi.SSID(j) == saved[i].ssid;
    if (!inRange) continue;
    Serial.printf("joining network %d of %d\n", i + 1, count);
    WiFi.begin(saved[i].ssid.c_str(), saved[i].pass.c_str());
    for (unsigned long start = millis(); WiFi.status() != WL_CONNECTED && millis() - start < WIFI_JOIN_MS;) vTaskDelay(pdMS_TO_TICKS(200));
    if (WiFi.status() != WL_CONNECTED) WiFi.disconnect();
  }
  WiFi.scanDelete();
  lastTry = millis();
}

WiFiClientSecure tls;
HTTPClient http;

// Returns the HTTP code, or a negative error. Keeps the connection: a new TLS handshake takes seconds.
int fetchText(const char* url, String& body) {
  static bool configured = false;
  if (!configured) {
    tls.setCACert(CA_ROOTS);
    tls.setHandshakeTimeout(10);
    http.setTimeout(8000);
    http.setReuse(true);
    configured = true;
  }
  if (!http.begin(tls, url)) return -1;
  int code = http.GET();
  body = code == 200 ? http.getString() : "";
  http.end();
  return code;
}

// Runs in the fetch task, on request only. The MD5 the server publishes is checked over the whole download.
void runUpdate() {
  updatePercent = -1;
  String md5;
  updateStatus = "Checking";
  int code = fetchText(ALERT_URL "/version", md5);
  md5.trim();
  if (code != 200 || md5.length() != 32) {
    updateStatus = "None published";
    return;
  }
  if (md5 == ESP.getSketchMD5()) {
    updateStatus = "Up to date";
    return;
  }
  updateStatus = "Downloading";
  if (!http.begin(tls, ALERT_URL "/firmware")) {
    updateStatus = "Failed";
    return;
  }
  code = http.GET();
  int size = http.getSize();
  bool ok = code == 200 && size > 0 && Update.begin(size);
  if (ok) {
    Update.setMD5(md5.c_str());
    Update.onProgress([](size_t done, size_t total) { updatePercent = done * 100 / total; });
    ok = Update.writeStream(*http.getStreamPtr()) == (size_t)size && Update.end();
    if (!ok) Update.abort();
  }
  http.end();
  tls.stop();
  if (!ok) {
    Serial.printf("update failed: http %d, %s\n", code, Update.errorString());
    updateStatus = "Failed";
    updatePercent = -1;
    return;
  }
  prefs.putBool("trial", true);
  prefs.putInt("tries", 0);
  Serial.println("update written, restarting");
  delay(300);
  ESP.restart();
}

void rollBack(const char* why) {
  Serial.printf("new firmware gives way to the old one: %s\n", why);
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  prefs.putBool("trial", false);
  if (other && esp_ota_set_boot_partition(other) == ESP_OK) ESP.restart();
}

// A separate task: a slow network must not block the button or the alarm.
void fetchTask(void*) {
  static FetchResult r;
  static HistoryResult h;
  for (;;) {
    unsigned long start = millis();
    connectStep();
    if (WiFi.status() == WL_CONNECTED && timeIsSynced(time(nullptr))) {
      String body;
      r.code = fetchText(ALERT_URL, body);
      if (r.code == 200 && !parseFeed(body.c_str(), r.feed)) r.code = -100;  // answered, but not our file
      xQueueOverwrite(results, &r);
      if (updateWanted) {
        updateWanted = false;
        runUpdate();
      }
      if (historyWanted) {
        historyWanted = false;
        h.code = fetchText(ALERT_URL "/history", body);
        if (h.code == 200 && !parseHistory(body.c_str(), maskNow, h.history)) h.code = -100;
        xQueueOverwrite(historyResults, &h);
      }
    }
    unsigned long wait = saverNow ? POLL_SAVER_MS : POLL_MS, took = millis() - start;
    vTaskDelay(pdMS_TO_TICKS(took < wait ? wait - took : 100));
  }
}

// ---------- phone setup page ----------

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (char c : s) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

String setupPage(const String& message) {
  String h;
  h.reserve(5000);
  h += F("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Vartovyi</title><style>"
         "body{margin:0;background:#0d1117;color:#ecf0f5;font:16px -apple-system,system-ui,sans-serif;padding:16px;max-width:560px}"
         "h1{font-size:22px;margin:4px 0 16px}h2{font-size:15px;color:#8a94a2;text-transform:uppercase;letter-spacing:.06em;margin:0 0 10px}"
         "section{background:#1c222b;border-radius:14px;padding:14px;margin:0 0 14px}"
         ".row{display:flex;align-items:center;gap:8px;padding:8px 0;border-top:1px solid #343c48}.row:first-of-type{border-top:0}.row span{flex:1;overflow-wrap:anywhere}"
         "input[type=text],input[type=password],input[type=number],select{width:100%;box-sizing:border-box;background:#0d1117;color:#ecf0f5;border:1px solid #343c48;border-radius:10px;padding:11px;font-size:16px;margin:0 0 8px}"
         "button{background:#34d399;color:#06281e;border:0;border-radius:10px;padding:11px 16px;font-size:16px;font-weight:600}"
         "button.s{background:#343c48;color:#ecf0f5;padding:7px 11px;font-size:14px;font-weight:500}label{display:flex;gap:10px;align-items:center;padding:7px 0}"
         "input[type=checkbox]{width:20px;height:20px}.tip,.msg{font-size:14px;color:#8a94a2}.msg{color:#34d399;margin:0 0 12px}.times{display:flex;gap:8px}"
         "</style></head><body><h1>Vartovyi</h1>");
  if (message.length()) h += "<p class=msg>" + htmlEscape(message) + "</p>";

  h += F("<section><h2>Wi-Fi networks</h2><p class=tip>The stick joins the first one in range, top to bottom.</p>");
  xSemaphoreTake(poolLock, portMAX_DELAY);
  for (int i = 0; i < poolCount; i++) {
    h += "<form class=row method=post action=/net><input type=hidden name=i value=" + String(i) + "><span>" + htmlEscape(pool[i].ssid) + "</span>";
    if (i) h += F("<button class=s name=do value=up>Up</button>");
    if (poolCount > 1) h += F("<button class=s name=do value=del>Remove</button>");
    h += F("</form>");
  }
  bool full = poolCount >= MAX_NETS;
  xSemaphoreGive(poolLock);
  if (full) {
    h += F("<p class=tip>The list is full. Remove a network to add another.</p>");
  } else {
    h += F("<form method=post action=/add style='margin-top:12px'><input type=text name=ssid maxlength=32 list=near placeholder='Network name' autocapitalize=off autocorrect=off required>"
           "<datalist id=near>");
    for (int i = 0; i < nearbyCount; i++) h += "<option value=\"" + htmlEscape(nearby[i]) + "\">";
    h += F("</datalist><input type=password name=pass maxlength=63 placeholder='Password'><button>Add network</button></form>"
           "<p class=tip>iPhone hotspot: turn on Maximize Compatibility, and keep the Personal Hotspot screen open while the stick connects.</p>");
  }
  h += F("</section><section><h2>Alert zones</h2><p class=tip>Zones heading for one you watch ring too.</p><form method=post action=/zones>");
  for (int i = 0; i < ZONE_COUNT; i++) {
    h += "<label><input type=checkbox name=z" + String(i) + (cfg.zones & (1u << i) ? " checked>" : ">") + KNOWN_ZONES[i].name + "</label>";
  }
  h += F("<p class=tip>Watching any raion also includes alerts for the whole oblast.</p><button>Save zones</button></form></section>"
         "<section><h2>Night mode</h2><form method=post action=/night><label><input type=checkbox name=on");
  if (cfg.nightOn) h += F(" checked");
  h += F(">Drones beep once at night. Missiles always ring.</label><div class=times><input type=number name=start min=0 max=23 aria-label='Start hour' value=");
  h += String(cfg.nightStart);
  h += F("><input type=number name=end min=0 max=23 aria-label='End hour' value=");
  h += String(cfg.nightEnd);
  h += F("></div><button>Save night mode</button></form></section>"
         "<section><h2>Display and sound</h2><form method=post action=/display>");
  const char* const NAMES[] = {"bright", "volume"};
  const char* const LABELS[] = {"Screen brightness", "Volume of drones and all-clear (missiles are always at maximum)"};
  const int VALUES[] = {cfg.bright, cfg.volume};
  for (int k = 0; k < 2; k++) {
    h += String("<label for=") + NAMES[k] + ">" + LABELS[k] + "</label><select id=" + NAMES[k] + " name=" + NAMES[k] + ">";
    for (int i = 0; i < 3; i++) h += "<option value=" + String(i) + (VALUES[k] == i ? " selected>" : ">") + LEVEL_NAMES[i] + "</option>";
    h += F("</select>");
  }
  h += F("<button>Save</button></form></section>"
         "<form method=post action=/done><button class=s>Close setup</button></form></body></html>");
  return h;
}

void showPage(const String& message) {
  server.send(200, "text/html; charset=utf-8", setupPage(message));
  portalUntil = millis() + PORTAL_MS;
}

void addNetwork() {
  String ssid = server.arg("ssid"), pass = server.arg("pass");
  ssid.trim();
  if (ssid.isEmpty() || ssid.length() > 32) return showPage("Network name must be 1-32 characters.");
  if (pass.length() && (pass.length() < 8 || pass.length() > 63)) return showPage("Password must be 8-63 characters, or empty for an open network.");
  xSemaphoreTake(poolLock, portMAX_DELAY);
  int at = -1;
  for (int i = 0; i < poolCount; i++)
    if (pool[i].ssid == ssid) at = i;
  bool room = at >= 0 || poolCount < MAX_NETS;
  if (room) {
    if (at < 0) at = poolCount++;
    pool[at] = {ssid, pass};
    savePool();
  }
  xSemaphoreGive(poolLock);
  showPage(room ? "Saved " + ssid + "." : "The list is full.");
}

void changeNetwork() {
  int i = server.arg("i").toInt();
  String action = server.arg("do");
  xSemaphoreTake(poolLock, portMAX_DELAY);
  bool valid = server.hasArg("i") && i >= 0 && i < poolCount;
  if (valid && action == "up" && i > 0) {
    std::swap(pool[i], pool[i - 1]);
  } else if (valid && action == "del" && poolCount > 1) {  // the last one stays: no network would lock the stick out
    for (int k = i; k < poolCount - 1; k++) pool[k] = pool[k + 1];
    poolCount--;
    pool[poolCount] = Net{};
    prefs.remove(("s" + String(poolCount)).c_str());
    prefs.remove(("p" + String(poolCount)).c_str());
  }
  if (valid) savePool();
  xSemaphoreGive(poolLock);
  showPage("Network list updated.");
}

void saveZones() {
  unsigned mask = 0;
  for (int i = 0; i < ZONE_COUNT; i++)
    if (server.hasArg("z" + String(i))) mask |= 1u << i;
  if (!mask) return showPage("Keep at least one zone, or the stick would never ring.");
  cfg.zones = mask;
  saveSettings();
  historyLoaded = false;
  showPage("Zones saved.");
}

void saveNight() {
  int start = server.arg("start").toInt(), end = server.arg("end").toInt();
  if (start < 0 || start > 23 || end < 0 || end > 23) return showPage("Hours must be 0-23.");
  cfg.nightOn = server.hasArg("on");
  cfg.nightStart = start;
  cfg.nightEnd = end;
  saveSettings();
  showPage("Night mode saved.");
}

void saveDisplay() {
  int bright = server.arg("bright").toInt(), volume = server.arg("volume").toInt();
  if (bright < 0 || bright > 2 || volume < 0 || volume > 2) return showPage("Unknown value.");
  cfg.bright = bright;
  cfg.volume = volume;
  saveSettings();
  showPage("Display and sound saved.");
}

void setupRoutes() {
  server.on("/display", HTTP_POST, saveDisplay);
  server.on("/", HTTP_GET, [] { showPage(""); });
  server.on("/add", HTTP_POST, addNetwork);
  server.on("/net", HTTP_POST, changeNetwork);
  server.on("/zones", HTTP_POST, saveZones);
  server.on("/night", HTTP_POST, saveNight);
  server.on("/done", HTTP_POST, [] {
    server.send(200, "text/html; charset=utf-8", "<!doctype html><meta name=viewport content='width=device-width'><body style='background:#0d1117;color:#ecf0f5;font:18px system-ui;padding:24px'>Setup closed. You can leave this network.");
    portalStopRequested = true;
  });
  // Any other address, like the iPhone's captive portal check, leads to the page, so it opens by itself.
  server.onNotFound([] {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
  });
}

void startPortal() {
  portalOn = true;
  int found = WiFi.scanNetworks();
  nearbyCount = 0;
  for (int i = 0; i < found && nearbyCount < 12; i++) {
    String name = WiFi.SSID(i);
    bool repeat = name.isEmpty();
    for (int k = 0; k < nearbyCount && !repeat; k++) repeat = nearby[k] == name;
    if (!repeat) nearby[nearbyCount++] = name;
  }
  WiFi.scanDelete();
  apPass = "";
  const char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";  // nothing easy to misread on a small screen
  for (int i = 0; i < 10; i++) apPass += ALPHABET[esp_random() % (sizeof ALPHABET - 1)];
  apQr = String("WIFI:T:WPA;S:") + AP_NAME + ";P:" + apPass + ";;";
  WiFi.mode(WIFI_AP_STA);  // keeps the station up: alerts keep arriving during setup
  WiFi.softAP(AP_NAME, apPass.c_str());
  dns.start(53, "*", WiFi.softAPIP());
  server.begin();
  portalUntil = millis() + PORTAL_MS;
  Serial.println("phone setup started");
}

void stopPortal() {
  server.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalStopRequested = false;
  portalUntil = 0;
  apPass = apQr = "";
  portalOn = false;
  Serial.println("phone setup closed");
}

// ---------- drawing ----------

void drawBattery(int right, int y, uint16_t fg) {
  if (battery < 0) return;
  auto& d = canvas;
  const int W = 22, H = 11;
  int x = right - W - 3;
  uint16_t fill = fg != C_MUTED ? fg : battery <= 15 ? C_RED : battery <= 35 ? C_AMBER : C_GREEN;
  d.drawRoundRect(x, y, W, H, 3, fg);
  d.fillRect(x + W, y + 3, 2, H - 6, fg);
  int inner = constrain(battery, 0, 100) * (W - 4) / 100;
  if (inner > 0) d.fillRect(x + 2, y + 2, inner, H - 4, fill);
  char text[8];
  snprintf(text, sizeof text, "%d%%", battery);
  d.setFont(&fonts::Font2);
  d.setTextDatum(middle_right);
  d.setTextColor(fg);
  d.drawString(text, x - 4, y + H / 2);
}

// The biggest font that fits the screen width.
void drawFitted(const char* text, int y, uint16_t color) {
  static const lgfx::IFont* const FONTS[] = {&fonts::FreeSansBold24pt7b, &fonts::FreeSansBold18pt7b, &fonts::FreeSansBold12pt7b,
                                             &fonts::FreeSansBold9pt7b, &fonts::Font2};
  auto& d = canvas;
  for (auto font : FONTS) {
    d.setFont(font);
    if (d.textWidth(text) <= d.width() - 12 || font == FONTS[4]) break;
  }
  d.setTextDatum(middle_center);
  d.setTextColor(color);
  d.drawString(text, d.width() / 2, y);
}

void drawSmall(const char* text, int x, int y, uint8_t datum, uint16_t color) {
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(datum);
  canvas.setTextColor(color);
  canvas.drawString(text, x, y);
}

void drawClipped(const char* text, int x, int y, int width, uint16_t color) {
  canvas.setClipRect(x, y - 9, width, 18);
  drawSmall(text, x, y, middle_left, color);
  canvas.clearClipRect();
}

uint16_t levelColor(Level l) {
  switch (l) {
    case L_NONE: return C_GREEN;
    case L_LOST: return C_BLUE;
    case L_ALERT: return C_ORANGE;
    case L_YELLOW: return C_AMBER;
    default: return C_RED;
  }
}
uint16_t onColor(uint16_t color) { return color == C_RED ? C_WHITE : C_BLACK; }

const char* linkTitle(Link l) {
  switch (l) {
    case LINK_NOWIFI: return "NO WI-FI";
    case LINK_NOANSWER: return "SERVER SILENT";
    case LINK_STALE: return "BRIDGE DOWN";
    case LINK_BOT: return "API SILENT";
    default: return "";
  }
}

bool nightNow(long long now) {
  if (!cfg.nightOn || !timeIsSynced(now)) return false;
  time_t raw = now;
  struct tm local;
  return localtime_r(&raw, &local) && isNightHour(local.tm_hour, cfg.nightStart, cfg.nightEnd);
}

// Alternates a solid colour with black, readable from across the room.
void drawAlarm(const char* title, const char* line1, const char* line2, const char* hint, uint16_t color, bool on) {
  auto& d = canvas;
  uint16_t bg = on ? color : C_BLACK;
  uint16_t fg = on ? onColor(color) : color;
  d.fillScreen(bg);
  drawBattery(d.width() - 6, 5, fg);
  drawFitted(title, 42, fg);
  drawFitted(line1, 80, fg);
  if (line2[0]) drawSmall(line2, d.width() / 2, 103, top_center, fg);
  drawSmall(hint, d.width() / 2, 118, top_center, fg);
}

// Every normal page: its name, dots for where you are, the battery.
void drawHeader(uint16_t dot) {
  auto& d = canvas;
  d.fillScreen(C_BG);
  d.fillCircle(9, 10, 4, dot);
  drawSmall(PAGE_NAMES[page], 18, 10, middle_left, C_TEXT);
  for (int i = 0; i < PAGE_COUNT; i++) {
    int x = 100 + i * 9;
    if (i == page) d.fillCircle(x, 10, 3, C_TEXT);
    else d.fillCircle(x, 10, 2, C_LINE);
  }
  drawBattery(d.width() - 5, 4, C_MUTED);
  d.drawFastHLine(0, 21, d.width(), C_PANEL);
}

uint16_t linkDot(Link link) { return link == LINK_WAIT ? C_MUTED : linkIsBad(link) ? C_AMBER : C_GREEN; }

void drawHome(Link link, long long now) {
  auto& d = canvas;
  drawHeader(linkDot(link));
  if (view.level != L_NONE) {  // an alert the user silenced: calm, and impossible to mistake for all clear
    uint16_t color = levelColor(view.level);
    d.fillRect(0, 22, d.width(), 34, color);
    drawFitted(viewTitle(view), 39, onColor(color));
    char timer[16] = "--:--:--";
    if (view.since && timeIsSynced(now)) formatTimer(now - view.since, timer, sizeof timer);
    d.setFont(&fonts::FreeSansBold24pt7b);
    d.setTextDatum(middle_center);
    d.setTextColor(C_TEXT);
    d.drawString(timer, d.width() / 2, 80);
    char line[64];
    if (linkIsBad(link)) snprintf(line, sizeof line, "%s - last known state", linkTitle(link));
    else if (view.level == L_LOST) snprintf(line, sizeof line, "%s: may still be active", view.zone);
    else snprintf(line, sizeof line, "%s  %s", view.zone, view.threats);
    drawSmall(line, d.width() / 2, 112, top_center, linkIsBad(link) ? C_AMBER : C_MUTED);
    return;
  }
  if (link == LINK_WAIT) {
    drawFitted("Connecting...", 66, C_TEXT);
    drawSmall(WiFi.status() == WL_CONNECTED ? "Reaching the server" : "Looking for Wi-Fi", d.width() / 2, 90, top_center, C_MUTED);
    return;
  }
  if (linkIsBad(link)) {
    drawFitted(linkTitle(link), 60, C_AMBER);
    drawSmall("Alerts may be missed!", d.width() / 2, 84, top_center, C_TEXT);
    return;
  }
  char clock[6] = "--:--";
  time_t raw = now;
  struct tm local;
  if (timeIsSynced(now) && localtime_r(&raw, &local)) strftime(clock, sizeof clock, "%H:%M", &local);
  d.setFont(&fonts::Font7);
  d.setTextDatum(middle_center);
  d.setTextColor(C_TEXT);
  d.drawString(clock, d.width() / 2, 52);
  drawSmall("ALL CLEAR", d.width() / 2, 84, middle_center, C_GREEN);
  int watched = 0;
  for (int i = 0; i < ZONE_COUNT; i++) watched += (cfg.zones >> i) & 1;
  char line[36];
  if (watched == 1) {
    for (int i = 0; i < ZONE_COUNT; i++)
      if (cfg.zones & (1u << i)) snprintf(line, sizeof line, "Watching %s", KNOWN_ZONES[i].name);
  } else {
    snprintf(line, sizeof line, "Watching %d zones", watched);
  }
  d.fillRoundRect(6, 100, d.width() - 12, 30, 7, C_PANEL);
  drawClipped(line, 13, 115, 124, C_TEXT);
  char right[32];
  snprintf(right, sizeof right, "%s%s", nightNow(now) ? "Night  " : "", saverNow ? "Saver" : (WiFi.isConnected() ? WiFi.SSID().substring(0, 14).c_str() : ""));
  drawSmall(right, d.width() - 13, 115, middle_right, C_MUTED);
}

const ZoneAlert* zoneAlert(int index) {
  for (int i = 0; i < feed.count; i++)
    if (feed.zones[i].level != L_NONE && zoneIndex(feed.zones[i].name) == index) return &feed.zones[i];
  return nullptr;
}

// The oblast drawn as tiles in roughly their real places. Watched zones fill with the alert's colour,
// the others only get an outline.
void drawMap(Link link) {
  static const struct { int zone, col, row, span; } TILES[] = {
      {1, 0, 0, 2}, {8, 2, 0, 1}, {2, 0, 1, 1}, {0, 1, 1, 1}, {3, 2, 1, 1},
      {4, 0, 2, 1}, {5, 1, 2, 1}, {6, 2, 2, 1}, {7, 0, 3, 2},
  };
  auto& d = canvas;
  drawHeader(linkDot(link));
  const int X = 5, Y = 25, TW = 74, TH = 25, GAP = 4;
  for (auto& tile : TILES) {
    int x = X + tile.col * (TW + GAP), y = Y + tile.row * (TH + GAP), w = tile.span * TW + (tile.span - 1) * GAP;
    const ZoneAlert* a = zoneAlert(tile.zone);
    bool wanted = zoneWanted(tile.zone, cfg.zones);
    uint16_t text = wanted ? C_TEXT : C_MUTED;
    if (a && wanted) {
      d.fillRoundRect(x, y, w, TH, 5, levelColor(a->level));
      text = onColor(levelColor(a->level));
    } else {
      d.fillRoundRect(x, y, w, TH, 5, C_PANEL);
      if (a) d.drawRoundRect(x, y, w, TH, 5, levelColor(a->level));
      else if (wanted) d.drawRoundRect(x, y, w, TH, 5, C_LINE);
    }
    drawSmall(KNOWN_ZONES[tile.zone].label, x + w / 2, y + TH / 2, middle_center, text);
  }
  char line[20];
  snprintf(line, sizeof line, feed.count == 1 ? "1 zone active" : "%d zones active", feed.count);
  drawSmall(line, X + 2 * (TW + GAP) + TW / 2, Y + 3 * (TH + GAP) + TH / 2, middle_center, feed.count ? C_AMBER : C_MUTED);
}

void drawDetails(Link link, long long now) {
  auto& d = canvas;
  drawHeader(linkDot(link));
  const ZoneAlert* shown[MAX_ZONES];
  int count = 0;
  for (int i = 0; i < feed.count; i++)
    if (zoneMatters(feed.zones[i], cfg.zones)) shown[count++] = &feed.zones[i];
  if (!count) {
    drawFitted("No active alerts", 72, C_MUTED);
    return;
  }
  int first = count > 3 ? scrollPos % count : 0;
  for (int r = 0; r < 3 && first + r < count; r++) {
    const ZoneAlert& z = *shown[first + r];
    int y = 24 + r * 37;
    d.fillRoundRect(3, y, 5, 33, 2, levelColor(z.level));
    drawClipped(z.name, 13, y + 8, 120, C_TEXT);
    char timer[16] = "";
    if (z.since && timeIsSynced(now)) formatTimer(now - z.since, timer, sizeof timer);
    drawSmall(timer, d.width() - 5, y + 8, middle_right, C_TEXT);
    char line[80];
    if (z.conf) snprintf(line, sizeof line, "%s%s%d%%%s%s", z.threats, z.threats[0] ? "  " : "", z.conf, z.direction[0] ? "  to " : "", z.direction);
    else snprintf(line, sizeof line, "%s", z.threats[0] ? z.threats : "Threat not specified");
    drawClipped(line, 13, y + 25, d.width() - 18, C_MUTED);
  }
}

void drawHistory(Link link, long long now) {
  auto& d = canvas;
  drawHeader(linkDot(link));
  if (!historyLoaded || !history.eventCount) {
    drawFitted(historyFailed ? "Not available" : historyLoaded ? "No events yet" : "Loading...", 72, C_MUTED);
    return;
  }
  const int ROWS = 5;
  int first = history.eventCount > ROWS ? scrollPos * ROWS % history.eventCount : 0;
  struct tm today {}, local {};
  time_t raw = now;
  localtime_r(&raw, &today);
  for (int r = 0; r < ROWS && first + r < history.eventCount; r++) {
    const HistoryEvent& e = history.events[first + r];
    int y = 34 + r * 21;
    raw = e.ts;
    localtime_r(&raw, &local);
    char when[14];
    strftime(when, sizeof when, local.tm_yday == today.tm_yday ? "%H:%M" : "%d.%m %H:%M", &local);
    d.fillCircle(8, y, 4, levelColor(e.level));
    drawClipped(when, 17, y, 80, C_MUTED);
    drawClipped(e.zone, 97, y, 140, C_TEXT);
  }
}

void drawStats(Link link) {
  auto& d = canvas;
  drawHeader(linkDot(link));
  if (!historyLoaded || !history.statCount) {
    drawFitted(historyFailed ? "Not available" : historyLoaded ? "No alerts this week" : "Loading...", 72, C_MUTED);
    return;
  }
  int first = history.statCount > 3 ? scrollPos % history.statCount : 0;
  for (int r = 0; r < 3 && first + r < history.statCount; r++) {
    const ZoneStats& s = history.stats[first + r];
    int y = 24 + r * 37;
    d.fillRoundRect(3, y, d.width() - 6, 34, 5, C_PANEL);
    drawClipped(s.zone, 9, y + 9, 224, C_TEXT);
    char a[12], b[12], line[32];
    formatMinutes(s.todayMin, a, sizeof a);
    snprintf(line, sizeof line, "Today %d, %s", s.todayCount, a);
    drawClipped(line, 9, y + 25, 112, C_MUTED);
    formatMinutes(s.weekMin, b, sizeof b);
    snprintf(line, sizeof line, "Week %d, %s", s.weekCount, b);
    drawClipped(line, 123, y + 25, 112, C_MUTED);
  }
}

void menuText(int item, char* left, char* right, size_t size) {
  right[0] = 0;
  switch (item) {
    case MI_ZONES: {
      int watched = 0;
      for (int i = 0; i < ZONE_COUNT; i++) watched += (cfg.zones >> i) & 1;
      snprintf(left, size, "Zones");
      snprintf(right, size, "%d", watched);
      break;
    }
    case MI_NIGHT:
      snprintf(left, size, "Night mode");
      if (cfg.nightOn) snprintf(right, size, "%02d-%02d", cfg.nightStart, cfg.nightEnd);
      else snprintf(right, size, "Off");
      break;
    case MI_FLIP:
      snprintf(left, size, "Flip to silence");
      snprintf(right, size, cfg.flip ? "On" : "Off");
      break;
    case MI_POWER:
      snprintf(left, size, "Power");
      snprintf(right, size, cfg.power == POWER_AUTO ? "Auto" : cfg.power == POWER_NORMAL ? "Normal" : "Saver");
      break;
    case MI_BRIGHT:
      snprintf(left, size, "Brightness");
      snprintf(right, size, "%s", LEVEL_NAMES[cfg.bright]);
      break;
    case MI_VOLUME:
      snprintf(left, size, "Volume");
      snprintf(right, size, "%s", LEVEL_NAMES[cfg.volume]);
      break;
    case MI_TEST: snprintf(left, size, "Test alarm"); break;
    case MI_SETUP: snprintf(left, size, "Phone setup"); break;
    case MI_UPDATE:
      snprintf(left, size, "Update firmware");
      if (updatePercent >= 0) snprintf(right, size, "%d%%", updatePercent);
      else snprintf(right, size, "%s", updateStatus);
      break;
    case MI_DELAY:
      snprintf(left, size, "Last delay");
      if (lastDelay >= 0) snprintf(right, size, "%ld s", lastDelay);
      else snprintf(right, size, "Not yet");
      break;
    case MI_NETWORK:
      snprintf(left, size, "Network");
      if (WiFi.isConnected()) snprintf(right, size, "%.12s %d", WiFi.SSID().c_str(), WiFi.RSSI());
      else snprintf(right, size, "None");
      break;
    default: snprintf(left, size, "Back"); break;
  }
}

void drawRow(int r, const char* left, const char* right, bool selected) {
  auto& d = canvas;
  int y = 24 + r * 22;
  if (selected) d.fillRoundRect(3, y, d.width() - 6, 21, 5, C_BLUE);
  uint16_t text = selected ? C_BLACK : C_TEXT;
  drawClipped(left, 10, y + 10, 140, text);
  drawSmall(right, d.width() - 10, y + 10, middle_right, selected ? C_BLACK : C_MUTED);
}

void drawSettings(Link link) {
  drawHeader(linkDot(link));
  const int ROWS = menu == M_NONE ? 4 : 5;  // closed, the last row tells how to open it
  int count = menu == M_ZONES ? ZONE_COUNT + 1 : MI_COUNT;
  int top = menuIndex < ROWS ? 0 : menuIndex - ROWS + 1;
  char left[32], right[32];
  for (int r = 0; r < ROWS && top + r < count; r++) {
    int i = top + r;
    if (menu == M_ZONES) {
      if (i == ZONE_COUNT) {
        drawRow(r, "Back", "", i == menuIndex);
      } else {
        drawRow(r, KNOWN_ZONES[i].name, cfg.zones & (1u << i) ? "On" : "Off", i == menuIndex);
      }
    } else {
      menuText(i, left, right, sizeof left);
      drawRow(r, left, right, menu == M_MAIN && i == menuIndex);
    }
  }
  if (menu == M_NONE) drawSmall("press A to open", canvas.width() / 2, 123, middle_center, C_BLUE);
}

void drawPortal() {
  auto& d = canvas;
  d.fillScreen(C_WHITE);
  bool joined = WiFi.softAPgetStationNum() > 0;
  d.qrcode(joined ? "http://192.168.4.1/" : apQr.c_str(), 4, 4, 127, 4);
  int x = 136;
  d.setFont(&fonts::Font2);
  d.setTextDatum(top_left);
  d.setTextColor(C_BLACK);
  if (joined) {
    d.drawString("Phone joined", x, 8);
    d.drawString("Scan to open", x, 30);
    d.drawString("or browse to", x, 52);
    d.drawString("192.168.4.1", x, 70);
  } else {
    d.drawString("Scan with", x, 8);
    d.drawString("the Camera", x, 26);
    d.drawString(AP_NAME, x, 52);
    d.drawString(apPass, x, 70);
  }
  d.drawString("A: close", x, 112);
}

void render(Sound sound, Link link, long long now, unsigned long t) {
  bool alarm = isAlarm(sound);
  bool on = alarm && flashPhase(t, sound == S_BALLISTIC ? FLASH_FAST_MS : FLASH_MS);
  bool testing = due(testUntil, t), clearing = due(clearUntil, t);
  bool active = hasData && view.level != L_NONE;

  // Short blink every 2 s while an alert is active but silenced: it still says so in a dark room.
  digitalWrite(LED_PIN, (alarm ? on : active && t % 2000 < 120) ? HIGH : LOW);

  uint8_t wanted = alarm || clearing ? BRIGHT_ALARM : portalOn ? BRIGHT_ACTIVE : saverNow ? BRIGHT_SAVER : active ? BRIGHT_ACTIVE : BRIGHT_LEVELS[cfg.bright];
  screenOff = saverNow && !alarm && !clearing && !portalOn && t - lastInput > SCREEN_OFF_MS;
  static uint8_t brightness = 1;
  if (screenOff) wanted = 0;
  if (wanted != brightness) M5.Display.setBrightness(brightness = wanted);
  if (screenOff || (!alarm && t - lastDraw < DRAW_MS)) return;
  lastDraw = t;

  char timer[16] = "";
  if (testing) {
    drawAlarm("ALARM TEST", "This is a test", "", "press any button to stop", C_RED, on);
  } else if (sound == S_LINK) {
    drawAlarm(linkTitle(link), "Alerts may be missed", "", "press any button to silence", C_ORANGE, on);
  } else if (alarm) {
    if (view.since && timeIsSynced(now)) formatTimer(now - view.since, timer, sizeof timer);
    drawAlarm(viewTitle(view), view.zone[0] ? view.zone : "Kyiv area", view.threats[0] ? view.threats : timer,
              "press any button to silence", levelColor(view.level), on);
  } else if (portalOn) {
    drawPortal();
  } else if (clearing) {
    canvas.fillScreen(C_DEEP_GREEN);
    drawBattery(canvas.width() - 6, 5, C_WHITE);
    drawFitted("ALL CLEAR", 60, C_WHITE);
    drawSmall("The alert is over", canvas.width() / 2, 90, top_center, C_WHITE);
  } else {
    switch (page) {
      case P_MAP: drawMap(link); break;
      case P_DETAILS: drawDetails(link, now); break;
      case P_HISTORY: drawHistory(link, now); break;
      case P_STATS: drawStats(link); break;
      case P_SETTINGS: drawSettings(link); break;
      default: drawHome(link, now); break;
    }
  }
  canvas.pushSprite(0, 0);
}

// ---------- sound ----------

template <size_t N>
const Step* pattern(const Step (&steps)[N], int& n) {
  n = N;
  return steps;
}

const Step* stepsOf(Sound s, int& n) {
  switch (s) {
    case S_SIREN: return pattern(SIREN, n);
    case S_BALLISTIC: return pattern(BALLISTIC, n);
    case S_BEEPS: return pattern(BEEPS, n);
    case S_LINK: return pattern(LINK_LOST, n);
    case S_CHIME: return pattern(CHIME, n);
    case S_SOFT: return pattern(SOFT, n);
    default: n = 0; return nullptr;
  }
}

// Plays without blocking. Returns true when the pattern has just finished one round.
bool playSound(Sound s, unsigned long t) {
  if (s != playing) {
    playing = s;
    soundStep = 0;
    beepCycles = 0;
    nextStepAt = t;
    M5.Speaker.stop();
    M5.Speaker.setVolume(s == S_SIREN || s == S_BALLISTIC ? 255 : VOLUME_LEVELS[cfg.volume]);
  }
  int n;
  const Step* steps = stepsOf(s, n);
  if (!steps || (long)(t - nextStepAt) < 0) return false;
  if (soundStep == n) {
    soundStep = 0;
    return true;
  }
  const Step& step = steps[soundStep++];
  if (step.hz) M5.Speaker.tone(step.hz, step.ms);
  nextStepAt = t + step.ms;
  return false;
}

// ---------- input ----------

void activateMenuItem(unsigned long t) {
  switch (menuIndex) {
    case MI_ZONES: menu = M_ZONES; menuIndex = 0; break;
    case MI_NIGHT: cfg.nightOn = !cfg.nightOn; saveSettings(); break;
    case MI_FLIP: cfg.flip = !cfg.flip; saveSettings(); break;
    case MI_POWER: cfg.power = stepIndex(cfg.power, 3, 1); saveSettings(); break;
    case MI_BRIGHT: cfg.bright = stepIndex(cfg.bright, 3, 1); saveSettings(); break;
    case MI_VOLUME: cfg.volume = stepIndex(cfg.volume, 3, 1); saveSettings(); chimePending = true; break;
    case MI_UPDATE:
      if (view.level != L_NONE) updateStatus = "Not during alert";  // the download pauses the alerts for a while
      else updateWanted = true;
      break;
    case MI_TEST: testUntil = t + TEST_MS; break;
    case MI_SETUP: startPortal(); break;
    case MI_BACK: menu = M_NONE; menuIndex = 0; break;
    default: break;
  }
}

void handleButtons(bool a, bool b, bool c, unsigned long t) {
  if (portalOn) {
    if (a) stopPortal();
    return;
  }
  int move = b ? 1 : c ? -1 : 0;
  if (menu == M_NONE) {
    if (move) {
      page = (Page)stepIndex(page, PAGE_COUNT, move);
      scrollPos = 0;
    } else if (a) {
      if (page == P_SETTINGS) {
        menu = M_MAIN;
        menuIndex = 0;
      } else {
        scrollPos++;
      }
    }
  } else if (menu == M_MAIN) {
    if (move) menuIndex = stepIndex(menuIndex, MI_COUNT, move);
    else if (a) activateMenuItem(t);
  } else {
    if (move) {
      menuIndex = stepIndex(menuIndex, ZONE_COUNT + 1, move);
    } else if (a && menuIndex == ZONE_COUNT) {
      menu = M_MAIN;
      menuIndex = MI_ZONES;
    } else if (a) {
      unsigned next = cfg.zones ^ (1u << menuIndex);
      if (next) {  // at least one zone stays: a stick that watches nothing would look fine and never ring
        cfg.zones = next;
        saveSettings();
        historyLoaded = false;
      } else {
        chimePending = true;
      }
    }
  }
  lastDraw = 0;
}

// ---------- main ----------

void applyResult(const FetchResult& r, unsigned long t) {
  if (r.code != 200) {
    Serial.printf("fetch failed: %d, min heap %u\n", r.code, ESP.getMinFreeHeap());
    return;
  }
  bool changed = !hasData || feed.seq != r.feed.seq || feed.count != r.feed.count;
  long long now = time(nullptr);
  if (hasData && feed.seq != r.feed.seq && r.feed.msgTs && timeIsSynced(now)) lastDelay = max(0LL, now - r.feed.msgTs);
  if (trial) {  // reached the server: this firmware works and stays
    trial = false;
    prefs.putBool("trial", false);
    Serial.println("new firmware confirmed");
  }
  feed = r.feed;
  hasData = true;
  lastGood = t;
  if (changed) Serial.printf("seq %ld, %d zone(s) active, min heap %u\n", feed.seq, feed.count, ESP.getMinFreeHeap());
}

void setup() {
  Serial.begin(115200);
  M5.begin(M5.config());
  M5.Display.setRotation(1);
  M5.Speaker.setVolume(255);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) Serial.println("no memory for the screen buffer");
  poolLock = xSemaphoreCreateMutex();
  loadSettings();
  trial = prefs.getBool("trial", false);
  if (trial) {  // counted before anything else can crash
    int tries = prefs.getInt("tries", 0) + 1;
    prefs.putInt("tries", tries);
    if (tries >= TRIAL_BOOTS) rollBack("it keeps restarting");
  }
  WiFi.persistent(false);  // the list lives in our own settings; do not wear the flash on every join
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // we choose the network ourselves, so a lost one can give way to the next
  configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com");  // Kyiv
  setupRoutes();
  results = xQueueCreate(1, sizeof(FetchResult));
  historyResults = xQueueCreate(1, sizeof(HistoryResult));
  // Core 1 like loop(): a TLS handshake on core 0 starves its idle task and the watchdog reboots the chip.
  xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 1, nullptr, 1);
}

void loop() {
  M5.update();
  unsigned long t = millis();
  long long now = time(nullptr);

  if (!lastBattery || t - lastBattery >= BATTERY_EVERY_MS) {
    lastBattery = t;
    battery = M5.Power.getBatteryLevel();
    batteryMv = M5.Power.getBatteryVoltage();
    usb = updateUsb(usb, batteryMv);
    saverNow = cfg.power == POWER_SAVER || (cfg.power == POWER_AUTO && !usb);
    if (!usb && battery >= 0 && battery <= LOW_BATTERY && !batteryWarned) {
      batteryWarned = true;
      chimePending = true;
    } else if (battery > LOW_BATTERY + 5) {
      batteryWarned = false;
    }
    static int logTicks = 0;
    if (logTicks++ % 6 == 0) {
      float ax = 0, ay = 0, az = 0;
      M5.Imu.getAccel(&ax, &ay, &az);
      Serial.printf("battery %d%% %d mV, usb %d, saver %d, az %.2f, wifi %d\n", battery, batteryMv, usb, (bool)saverNow, az, WiFi.isConnected());
    }
  }

  static FetchResult r;
  if (xQueueReceive(results, &r, 0) == pdTRUE) {
    Level before = hasData ? view.level : L_NONE;
    applyResult(r, t);
    view = computeView(feed, cfg.zones);
    if (becameClear(before, view.level)) {
      clearUntil = t + CLEAR_SCREEN_MS;
      chimePending = true;
    }
  }
  static HistoryResult hr;
  if (xQueueReceive(historyResults, &hr, 0) == pdTRUE) {
    historyFailed = hr.code != 200;
    if (!historyFailed) history = hr.history;
    historyLoaded = true;
    lastHistory = t;
    lastDraw = 0;
    if (historyFailed) Serial.printf("history fetch failed: %d\n", hr.code);
  }
  bool historyPage = !portalOn && menu == M_NONE && (page == P_HISTORY || page == P_STATS);
  if (historyPage && (!historyLoaded || t - lastHistory >= HISTORY_EVERY_MS)) {
    lastHistory = t;  // one request at a time, also when the answer does not come
    historyWanted = true;
  }

  view = computeView(feed, cfg.zones);
  Link link = classifyLink(WiFi.status() == WL_CONNECTED, timeIsSynced(now), t - lastGood, hasData, feed, now);
  updateRinger(ringer, link, feed.seq);
  bool night = nightNow(now);
  if (trial && t > TRIAL_MS) rollBack("no answer from the server");
  // At night a lost link only beeps, and missiles would not arrive: keep saying so.
  if (night && linkIsBad(link) && ringer.linkAcked && t - lastNightBeep >= NIGHT_LINK_EVERY_MS) ringer.linkAcked = false;

  Sound sound = due(testUntil, t) ? S_SIREN : wantedSound(ringer, hasData, view, link, night);

  bool a = M5.BtnA.wasPressed(), b = M5.BtnB.wasPressed(), c = M5.BtnPWR.wasPressed();
  if (a || b || c) {
    lastInput = t;
    lastDraw = 0;
    if (due(testUntil, t)) {
      testUntil = 0;  // ends only the test: a real alert that came meanwhile keeps ringing
    } else if (isAlarm(sound)) {  // any button: nobody should hunt for the right one during a siren
      ackAll(ringer, view);
      clearUntil = 0;
      chimePending = false;
      page = P_HOME;
      menu = M_NONE;
      Serial.println("silenced");
    } else if (screenOff) {
      // the press only wakes the screen
    } else {
      handleButtons(a, b, c, t);
    }
    sound = due(testUntil, t) ? S_SIREN : wantedSound(ringer, hasData, view, link, night);
  }

  // Turned face down and left there: the same as pressing a button.
  if (cfg.flip && !saverNow && isAlarm(sound) && t - lastFlipCheck >= 100) {
    lastFlipCheck = t;
    float ax = 0, ay = 0, az = 0;
    if (M5.Imu.getAccel(&ax, &ay, &az) && az < FACE_DOWN_G) {
      if (!flipSince) flipSince = t;
      if (t - flipSince >= FLIP_HOLD_MS) {
        if (due(testUntil, t)) testUntil = 0;
        else ackAll(ringer, view);
        flipSince = 0;
        Serial.println("silenced by flipping");
        sound = wantedSound(ringer, hasData, view, link, night);
      }
    } else {
      flipSince = 0;
    }
  }
  if (!isAlarm(sound)) flipSince = 0;

  if (isAlarm(sound)) lastRing = t;
  if (sound == S_NONE && remindDue(view.level, night, t - lastRing)) {
    lastRing = t;
    remindPending = true;
  }
  if (sound == S_NONE && chimePending) sound = S_CHIME;
  bool reminding = sound == S_NONE && remindPending;
  if (reminding) sound = S_SOFT;
  if (playSound(sound, t)) {
    if (sound == S_CHIME) chimePending = false;
    if (sound == S_BEEPS && ++beepCycles >= BEEP_CYCLES) ackAlert(ringer, view);
    if (reminding) {
      remindPending = false;
    } else if (sound == S_SOFT) {  // one beep: an alert first, then a lost link has its turn the same way
      if (view.level != L_NONE && view.seq > ringer.ackedSeq) {
        ackAlert(ringer, view);
      } else {
        ringer.linkAcked = true;
        lastNightBeep = t;
      }
    }
  }

  if (!portalOn && !isAlarm(sound) && t - lastInput >= BACK_HOME_MS && (page != P_HOME || menu != M_NONE)) {
    page = P_HOME;
    menu = M_NONE;
    scrollPos = 0;
    lastDraw = 0;
  }

  if (!portalOn && !poolCount) startPortal();  // first start: nowhere to connect until a phone adds a network
  if (portalOn) {
    if (!poolCount) portalUntil = t + PORTAL_MS, portalStopRequested = false;
    dns.processNextRequest();
    server.handleClient();
    if (portalStopRequested || !due(portalUntil, t)) stopPortal();
  }

  render(sound, link, now, t);
  delay(portalOn || isAlarm(sound) ? 5 : 25);
}
