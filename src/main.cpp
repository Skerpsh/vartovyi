#include <DNSServer.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <time.h>

#include "alert.h"
#include "ca_roots.h"
// The public server; a self-hosted bridge builds with -DALERT_URL='"https://host/peace-stick"'.
#ifndef ALERT_URL
#define ALERT_URL "https://62.238.111.55.nip.io/peace-stick"
#endif
#ifndef VERSION
#define VERSION "dev"  // a release gets its tag from the build
#endif
// Optional: src/secrets.h with WIFI_SSID and WIFI_PASS seeds the first network. Without it the stick opens phone setup.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

const unsigned long POLL_MS = 3000, POLL_SAVER_MS = 6000;
const unsigned long WIFI_RETRY_MS = 15000, WIFI_JOIN_MS = 12000;
const unsigned long HISTORY_EVERY_MS = 60000;
const unsigned long FLASH_MS = 150, FLASH_FAST_MS = 80, DRAW_MS = 90, DRAW_SAVER_MS = 500;
const unsigned long TRIAL_MS = 300000;    // a new firmware that reaches no server in this time gives way to the old one
const int TRIAL_BOOTS = 3;
// The Plus2 backlight never goes below about 16% (M5GFX offset 40), so the levels sit far apart: ~18%, ~49%, 100%.
const uint8_t BRIGHT_LEVELS[] = {8, 100, 255}, VOLUME_LEVELS[] = {90, 170, 255};
const char* const LEVEL_NAMES[] = {"Low", "Medium", "High"};
const char* const IMPACT_NAMES[] = {"Off", "Low", "High"};
const unsigned long CLEAR_SCREEN_MS = 5000, TEST_MS = 5000, BATTERY_EVERY_MS = 10000;
const unsigned long BACK_HOME_MS = 60000, SCREEN_OFF_MS = 30000, PORTAL_MS = 300000;
const unsigned long FLIP_HOLD_MS = 1000;  // long enough that bouncing in a bag does not silence an alarm
const float FACE_DOWN_G = -0.8f;
const int BEEP_CYCLES = 6;                // drones do not ring forever, a missile alert does
const int LOW_BATTERY = 10;
const int MAX_NETS = 8;
const uint8_t LED_PIN = 19;               // the red LED on the Plus2, shared with the IR transmitter
const uint8_t BRIGHT_ACTIVE = 160, BRIGHT_ALARM = 255;
const char* const AP_NAME = "Vartovyi";

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
// The chrome comes from one accent colour, the stick's orange unless the user picks another theme.
// Danger keeps its own colours, so no theme is red, yellow or green.
struct Theme {
  const char* name;
  uint8_t r, g, b;
};
const Theme THEMES[] = {{"Orange", 255, 156, 0}, {"Cyan", 0, 229, 255},  {"Magenta", 255, 40, 190},
                        {"Violet", 165, 95, 255}, {"Blue", 45, 130, 255}, {"Ice", 205, 225, 255}};
const int THEME_COUNT = sizeof THEMES / sizeof THEMES[0];
const uint16_t C_BG = rgb(5, 5, 7);
uint16_t C_ACCENT, C_DIM, C_PANEL, C_SCAN, C_TEXT, C_MUTED;
const uint16_t C_GREEN = rgb(57, 255, 20), C_CYAN = rgb(0, 229, 255), C_YELLOW = rgb(255, 228, 0);
const uint16_t C_RED = rgb(255, 32, 56), C_BLACK = rgb(0, 0, 0), C_WHITE = rgb(255, 255, 255);

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

enum Page { P_HOME, P_MAP, P_DETAILS, P_HISTORY, P_STATS, P_SYSTEM, PAGE_COUNT };
enum Menu { M_NONE, M_MAIN, M_ZONES, M_REGIONS };
enum MenuItem { MI_LANG, MI_REGION, MI_ZONES, MI_THEME, MI_IMPACT, MI_VOLUME, MI_BRIGHT, MI_NIGHT, MI_FLIP, MI_POWER, MI_TEST, MI_SETUP, MI_UPDATE, MI_COUNT };
enum PowerMode { POWER_AUTO, POWER_NORMAL, POWER_SAVER };

struct Settings {
  char oblast[REGION_ID] = "14", oblastName[ZONE_NAME] = "Kyivska";  // Kyiv, where the first firmware lived
  char oblastUa[40] = "Київська";
  int lang = 0;  // LANG_UA: the firmware speaks Ukrainian unless the user picks English
  unsigned zones = DEFAULT_ZONES;
  int theme = 0;
  int impact = 1;  // knocks break the screen for a moment: 0 off, 1 hits and drops, 2 also swings
  bool nightOn = true, flip = true;
  int nightStart = 23, nightEnd = 7;
  int power = POWER_AUTO;
  int bright = 1, volume = 2;  // indexes into BRIGHT_LEVELS and VOLUME_LEVELS
};
struct Net {
  String ssid, pass;
};

M5Canvas canvas(&M5.Display);  // draw off screen, so a fast flash does not tear
QueueHandle_t results, historyResults, regionResults;
SemaphoreHandle_t poolLock;
Preferences prefs;
WebServer server(80);
DNSServer dns;

Settings cfg;
char oblastNow[REGION_ID] = "14";  // the fetch task's copy of cfg.oblast, under poolLock
RegionList regions;                // the oblasts to choose from, once the server sent them
volatile bool regionsWanted = true;
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
bool confirmUpdate = false;  // asking whether to update during an alert
volatile int updatePercent = -1;
const char* volatile updateStatus = "";
bool remindPending = false, trial = false;
unsigned long lastRing = 0, lastNightBeep = 0;
long lastDelay = -1;

Page page = P_HOME;
Menu menu = M_NONE;
int menuIndex = 0;

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

// ---------- language ----------
// The interface is written in English; Ukrainian comes from this table, and zone and oblast names from the bridge.
// The boot screen draws with drawRaw() and stays English.
enum Lang { LANG_UA, LANG_EN };

const char* T(const char* en) {
  static const struct {
    const char *en, *ua;
  } PHRASES[] = {
    {"HOME", "ГОЛОВНА"},
    {"MAP", "КАРТА"},
    {"ACTIVE", "АКТИВНІ"},
    {"HISTORY", "ІСТОРІЯ"},
    {"STATS", "СТАТИСТИКА"},
    {"SYSTEM", "СИСТЕМА"},
    {"MENU", "МЕНЮ"},
    {"ZONES", "РАЙОНИ"},
    {"OBLAST", "ОБЛАСТЬ"},
    {"FIRMWARE", "ПРОШИВКА"},
    {"NEXT", "ДАЛІ"},
    {"BACK", "НАЗАД"},
    {"SET", "ЗМІНИТИ"},
    {"OPEN", "ВІДКРИТИ"},
    {"RUN", "ЗАПУСК"},
    {"TOGGLE", "ЗМІНИТИ"},
    {"CHOOSE", "ОБРАТИ"},
    {"NEXT THEME", "ЗМІНИТИ"},
    {"CANCEL", "СКАСУВАТИ"},
    {"UPDATE", "ОНОВИТИ"},
    {"Oblast", "Область"},
    {"Zones", "Райони"},
    {"Theme", "Тема"},
    {"Impact FX", "Ефекти удару"},
    {"Volume", "Гучність"},
    {"Brightness", "Яскравість"},
    {"Night mode", "Нічний режим"},
    {"Flip to silence", "Тиша переворотом"},
    {"Power", "Живлення"},
    {"Test alarm", "Тест сирени"},
    {"Phone setup", "Налаштування"},
    {"Update firmware", "Оновлення"},
    {"On", "Увімк"},
    {"Off", "Вимк"},
    {"Auto", "Авто"},
    {"Normal", "Звичайне"},
    {"Saver", "Економ"},
    {"Low", "Низька"},
    {"Medium", "Середня"},
    {"High", "Висока"},
    {"Orange", "Помаранч"},
    {"Cyan", "Ціан"},
    {"Magenta", "Пурпур"},
    {"Violet", "Фіолет"},
    {"Blue", "Синій"},
    {"Ice", "Лід"},
    {"%d of %d", "%d з %d"},
    {"Checking", "Перевірка"},
    {"None published", "Немає"},
    {"Bad signature", "Хибний підпис"},
    {"Up to date", "Актуальна"},
    {"Downloading", "Завантаження"},
    {"Failed", "Помилка"},
    {"Bad image", "Хибний образ"},
    {"NO WI-FI", "НЕМАЄ WI-FI"},
    {"SERVER SILENT", "СЕРВЕР МОВЧИТЬ"},
    {"BRIDGE DOWN", "МІСТ НЕ ПРАЦЮЄ"},
    {"API SILENT", "API МОВЧИТЬ"},
    {"UNCONFIRMED", "НЕПІДТВЕРДЖЕНО"},
    {"CHECK ALERTS", "ПЕРЕВІРТЕ ТРИВОГИ"},
    {"BALLISTIC", "БАЛІСТИКА"},
    {"MISSILE ALERT", "РАКЕТИ"},
    {"AIRCRAFT ALERT", "АВІАЦІЯ"},
    {"RED ALERT", "ЧЕРВОНА ТРИВОГА"},
    {"DRONE ALERT", "ЗАГРОЗА БПЛА"},
    {"YELLOW ALERT", "ЖОВТА ТРИВОГА"},
    {"AIR ALERT", "ПОВІТРЯНА ТРИВОГА"},
    {"CRUISE", "КРИЛАТІ РАКЕТИ"},
    {"MISSILES", "РАКЕТИ"},
    {"AIRCRAFT", "АВІАЦІЯ"},
    {"DRONES", "БПЛА"},
    {"ARTILLERY", "АРТИЛЕРІЯ"},
    {"URBAN_FIGHTS", "ВУЛИЧНІ БОЇ"},
    {"CHEMICAL", "ХІМІЧНА"},
    {"NUCLEAR", "РАДІАЦІЙНА"},
    {"ALL CLEAR", "ТРИВОГИ НЕМАЄ"},
    {"ALL-CLEAR", "ВІДБІЙ"},
    {"THREAT ENDED // STAND DOWN", "ЗАГРОЗУ ЗНЯТО"},
    {"LINKING", "З’ЄДНАННЯ"},
    {"ALERTS MAY BE MISSED", "МОЖНА ПРОПУСТИТИ ТРИВОГУ"},
    {"wi-fi up, reaching the server", "wi-fi є, шукаю сервер"},
    {"searching for wi-fi", "шукаю wi-fi"},
    {"%s // LAST KNOWN STATE", "%s // ОСТАННІ ДАНІ"},
    {"%s: may still be active", "%s: може ще тривати"},
    {"OBL %s  //  WATCH %s  //  NET %s %ddBm  //  SEQ %ld  //  %s  //  ", "ОБЛ %s  //  СТЕЖУ %s  //  МЕРЕЖА %s %ddBm  //  ПОДІЯ %ld  //  %s  //  "},
    {"NIGHT MODE", "НІЧНИЙ РЕЖИМ"},
    {"SAVER", "ЕКОНОМ"},
    {"VER", "ВЕРСІЯ"},
    {"%d of %d zones", "%d з %d районів"},
    {"LOADING", "ЗАВАНТАЖЕННЯ"},
    {"%d ACTIVE", "%d АКТИВНИХ"},
    {"NO THREATS", "ЗАГРОЗ НЕМАЄ"},
    {"in the zones you watch", "у ваших районах"},
    {"threat not specified", "загрозу не вказано"},
    {"NO DATA", "НЕМАЄ ДАНИХ"},
    {"NO EVENTS", "ПОДІЙ НЕМАЄ"},
    {"QUIET WEEK", "ТИХИЙ ТИЖДЕНЬ"},
    {"TODAY %d / %s", "СЬОГОДНІ %d / %s"},
    {"WEEK %d / %s", "ТИЖДЕНЬ %d / %s"},
    {"NET", "МЕРЕЖА"},
    {"LINK", "ЗВ’ЯЗОК"},
    {"DELAY", "ЗАТРИМКА"},
    {"POWER", "ЖИВЛЕННЯ"},
    {"offline", "немає"},
    {"nominal", "норма"},
    {"connecting", "з’єднання"},
    {"%ld s, last alert", "%ld с, остання тривога"},
    {"not measured yet", "ще не виміряно"},
    {"battery", "батарея"},
    {", saver", ", економ"},
    {"%s, up %lum", "%s, працює %lu хв"},
    {"ALARM TEST", "ТЕСТ СИРЕНИ"},
    {"This is a test", "Це перевірка"},
    {"Alerts may be missed", "Тривогу можна пропустити"},
    {"ANY KEY = SILENCE", "БУДЬ-ЯКА КНОПКА = ТИША"},
    {"ALERT IS ACTIVE", "ТРИВОГА ТРИВАЄ"},
    {"It takes about a minute,", "Це займе близько хвилини,"},
    {"and alerts cannot arrive.", "тривоги тим часом не надходять."},
    {"Update anyway?", "Все одно оновити?"},
    {"SETUP", "НАЛАШТУВАННЯ"},
    {"phone joined", "підключено"},
    {"scan again or", "скануйте знову"},
    {"open", "або відкрийте"},
    {"scan with", "скануйте"},
    {"the camera", "камерою"},
    {"M5 / BACK: EXIT", "M5 / НАЗАД: ВИХІД"},
    {"IMPACT DETECTED", "УДАР ВИЯВЛЕНО"},
    {"RECALIBRATING%.*s", "КАЛІБРУВАННЯ%.*s"},
    {"%dm", "%d хв"},
    {"%dh %02dm", "%d год %02d хв"},
    {"Wi-Fi networks", "Мережі Wi-Fi"},
    {"The stick joins the first one in range, top to bottom.", "Стік підключається до першої доступної мережі, згори донизу."},
    {"Up", "Вгору"},
    {"Remove", "Видалити"},
    {"The list is full. Remove a network to add another.", "Список повний. Видаліть мережу, щоб додати іншу."},
    {"Network name", "Назва мережі"},
    {"Password", "Пароль"},
    {"Add network", "Додати мережу"},
    {"iPhone hotspot: turn on Maximize Compatibility, and keep the Personal Hotspot screen open while the stick connects.", "Хотспот iPhone: увімкніть «Максимальна сумісність» і тримайте екран «Режим модема» відкритим, поки стік підключається."},
    {"Save oblast", "Зберегти область"},
    {"Alert zones", "Райони тривог"},
    {"Loading the zones of this oblast: reload the page in a few seconds.", "Райони цієї області завантажуються: оновіть сторінку за кілька секунд."},
    {"Watching any raion also includes alerts for the whole oblast.", "Якщо стежите хоча б за одним районом, тривоги на всю область теж рахуються."},
    {"Save zones", "Зберегти райони"},
    {"Drones beep once at night. Missiles always ring.", "Вночі дрони — один гудок. Ракети — завжди сирена."},
    {"Save night mode", "Зберегти нічний режим"},
    {"Display and sound", "Екран і звук"},
    {"Screen brightness", "Яскравість екрана"},
    {"Volume of drones and all-clear (missiles are always at maximum)", "Гучність дронів і відбою (ракети завжди на максимумі)"},
    {"Save", "Зберегти"},
    {"Close setup", "Закрити налаштування"},
    {"Setup closed. You can leave this network.", "Налаштування закрито. Можна відключитися від цієї мережі."},
    {"Network name must be 1-32 characters.", "Назва мережі: від 1 до 32 символів."},
    {"Password must be 8-63 characters, or empty for an open network.", "Пароль: від 8 до 63 символів, або порожній для відкритої мережі."},
    {"Saved %s.", "Збережено %s."},
    {"The list is full.", "Список повний."},
    {"Network list updated.", "Список мереж оновлено."},
    {"Keep at least one zone, or the stick would never ring.", "Залиште хоча б один район, інакше стік ніколи не задзвонить."},
    {"Zones saved.", "Райони збережено."},
    {"Hours must be 0-23.", "Години: від 0 до 23."},
    {"Night mode saved.", "Нічний режим збережено."},
    {"Unknown value.", "Невідоме значення."},
    {"Display and sound saved.", "Екран і звук збережено."},
    {"Oblast saved. Its zones appear below in a few seconds: reload the page.", "Область збережено. Її райони з’являться нижче за кілька секунд: оновіть сторінку."},
    {"Unknown oblast.", "Невідома область."},
  };
  if (cfg.lang != LANG_UA || !en || !*en) return en;
  for (auto& p : PHRASES)
    if (!strcmp(p.en, en)) return p.ua;
  for (int i = 0; i < feed.list.count; i++)
    if (!strcmp(feed.list.zones[i].name, en) && feed.list.zones[i].ua[0]) return feed.list.zones[i].ua;
  for (int i = 0; i < regions.count; i++)
    if (!strcmp(regions.regions[i].name, en) && regions.regions[i].ua[0]) return regions.regions[i].ua;
  if (!strcmp(en, cfg.oblastName) && cfg.oblastUa[0]) return cfg.oblastUa;
  return en;
}

// A comma list of threats ("BALLISTIC,DRONES" or "BALLISTIC, DRONES"), each word in the interface language.
const char* threatsText(const char* list, char* out, size_t size) {
  out[0] = 0;
  while (list && *list) {
    while (*list == ' ' || *list == ',') list++;
    const char* end = list;
    while (*end && *end != ',') end++;
    char one[24];
    copyValue(one, sizeof one, list, end - list);
    if (one[0]) snprintf(out + strlen(out), size - strlen(out), "%s%s", out[0] ? ", " : "", T(one));
    list = end;
  }
  return out;
}

// Minutes as "45m" / "1h 35m", in the interface language.
const char* minutesText(int minutes, char* out, size_t size) {
  if (minutes < 60) snprintf(out, size, T("%dm"), minutes);
  else snprintf(out, size, T("%dh %02dm"), minutes / 60, minutes % 60);
  return out;
}

// A view's zone line ("Buchanskyi +2", "Fastivskyi to Kyiv") with the zone in the interface language.
const char* zoneText(const char* line, char* out, size_t size) {
  for (int i = 0; i < feed.list.count; i++) {
    size_t n = strlen(feed.list.zones[i].name);
    if (n && !strncmp(line, feed.list.zones[i].name, n) && (line[n] == 0 || line[n] == ' ')) {
      snprintf(out, size, "%s%s", T(feed.list.zones[i].name), line + n);
      return out;
    }
  }
  snprintf(out, size, "%s", T(line));
  return out;
}

// ---------- settings ----------

// Everything but the accent is the accent mixed with black, grey or white, so any theme stays one family.
struct Rgb {
  int r, g, b;
  uint16_t c565() const { return rgb(r, g, b); }
};
Rgb mixed(const Theme& t, int with, int pct) {
  return {t.r + (with - t.r) * pct / 100, t.g + (with - t.g) * pct / 100, t.b + (with - t.b) * pct / 100};
}
// The phone page in the same colours: accent, dim, panel, muted, text.
const char* const PAGE_COLORS[] = {"#ff9c00", "#693c0a", "#1e1306", "#967c60", "#ffeed6"};
char themeHex[5][8];

void applyTheme() {
  const Theme& t = THEMES[cfg.theme];
  const Rgb all[] = {mixed(t, 0, 0), mixed(t, 0, 59), mixed(t, 0, 88), mixed(t, 120, 60), mixed(t, 255, 85)};
  C_ACCENT = all[0].c565(), C_DIM = all[1].c565(), C_PANEL = all[2].c565(), C_MUTED = all[3].c565(), C_TEXT = all[4].c565();
  C_SCAN = mixed(t, 0, 93).c565();
  for (int i = 0; i < 5; i++) snprintf(themeHex[i], sizeof themeHex[i], "#%02x%02x%02x", all[i].r, all[i].g, all[i].b);
}

void savePool() {
  prefs.putInt("nets", poolCount);
  for (int i = 0; i < poolCount; i++) {
    prefs.putString(("s" + String(i)).c_str(), pool[i].ssid);
    prefs.putString(("p" + String(i)).c_str(), pool[i].pass);
  }
}

void shareOblast() {
  xSemaphoreTake(poolLock, portMAX_DELAY);
  strcpy(oblastNow, cfg.oblast);
  xSemaphoreGive(poolLock);
}

void saveSettings() {
  prefs.putString("obl", cfg.oblast);
  prefs.putString("oblName", cfg.oblastName);
  prefs.putString("oblUa", cfg.oblastUa);
  prefs.putInt("lang", cfg.lang);
  shareOblast();
  prefs.putUInt("zones", cfg.zones);
  prefs.putBool("night", cfg.nightOn);
  prefs.putBool("flip", cfg.flip);
  prefs.putInt("nStart", cfg.nightStart);
  prefs.putInt("nEnd", cfg.nightEnd);
  prefs.putInt("power", cfg.power);
  prefs.putInt("bright", cfg.bright);
  prefs.putInt("volume", cfg.volume);
  prefs.putInt("theme", cfg.theme);
  prefs.putInt("impact", cfg.impact);
  maskNow = cfg.zones;
}

void loadSettings() {
  prefs.begin("peace", false);
  prefs.getString("obl", cfg.oblast, sizeof cfg.oblast);  // an older firmware saved none: Kyiv stays
  prefs.getString("oblName", cfg.oblastName, sizeof cfg.oblastName);
  // Firmware before the Ukrainian names kept none: "Київська" fits only Kyiv; others wait for the server's list.
  if (!prefs.getString("oblUa", cfg.oblastUa, sizeof cfg.oblastUa) && strcmp(cfg.oblast, "14")) cfg.oblastUa[0] = 0;
  cfg.lang = constrain(prefs.getInt("lang", 0), 0, 1);
  shareOblast();
  cfg.zones = prefs.getUInt("zones", DEFAULT_ZONES);
  cfg.nightOn = prefs.getBool("night", true);
  cfg.flip = prefs.getBool("flip", true);
  cfg.nightStart = prefs.getInt("nStart", 23);
  cfg.nightEnd = prefs.getInt("nEnd", 7);
  cfg.power = prefs.getInt("power", POWER_AUTO);
  cfg.bright = constrain(prefs.getInt("bright", 1), 0, 2);
  cfg.volume = constrain(prefs.getInt("volume", 2), 0, 2);
  cfg.theme = constrain(prefs.getInt("theme", 0), 0, THEME_COUNT - 1);
  cfg.impact = constrain(prefs.getInt("impact", 1), 0, 2);
  applyTheme();
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

// Public keys from keys/, built into the firmware (platformio.ini): the release key signs firmware
// (sign-release.sh), the feed key is the bridge's and signs every alert file.
extern const char RELEASE_KEY[] asm("_binary_keys_release_public_pem_start");
extern const char FEED_KEY[] asm("_binary_keys_feed_public_pem_start");

// True if sigB64, a base64 DER ECDSA signature, signs this SHA-256 with the PEM public key.
bool signedBy(const char* pem, const uint8_t hash[32], const char* sigB64, size_t sigLen) {
  uint8_t sig[160];
  size_t len = 0;
  if (mbedtls_base64_decode(sig, sizeof sig, &len, (const uint8_t*)sigB64, sigLen)) return false;
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  bool ok = !mbedtls_pk_parse_public_key(&pk, (const uint8_t*)pem, strlen(pem) + 1) &&
            !mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sig, len);
  mbedtls_pk_free(&pk);
  return ok;
}

// A file from the bridge (alerts, history, the oblasts) counts only with its signature: a server that only looks like
// ours cannot ring, silence, or rewrite what the stick shows.
bool feedSigned(const String& body) {
  size_t signedLen = 0, sigLen = 0;
  const char* sig = nullptr;
  if (!splitSigned(body.c_str(), signedLen, sig, sigLen)) return false;
  uint8_t hash[32];
  mbedtls_sha256_ret((const uint8_t*)body.c_str(), signedLen, hash, 0);
  return signedBy(FEED_KEY, hash, sig, sigLen);
}

// Runs in the fetch task, on request only. The release manifest names the version and the firmware's SHA-256 and is
// signed with the release key: the stick installs only a newer version than its own, and only that exact image.
// So the server alone can neither hand out firmware of its own nor roll a stick back to an older release.
void runUpdate() {
  updatePercent = -1;
  String manifest, sig;
  updateStatus = "Checking";
  int code = fetchText(ALERT_URL "/release", manifest);
  if (code != 200 || fetchText(ALERT_URL "/release-sig", sig) != 200) {
    updateStatus = "None published";
    return;
  }
  sig.trim();
  uint8_t hash[32];
  mbedtls_sha256_ret((const uint8_t*)manifest.c_str(), manifest.length(), hash, 0);
  char version[16], wanted[65];
  if (!signedBy(RELEASE_KEY, hash, sig.c_str(), sig.length()) || !parseManifest(manifest.c_str(), version, sizeof version, wanted)) {
    updateStatus = "Bad signature";
    return;
  }
  if (versionNumber(version) <= versionNumber(VERSION)) {
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
  bool ok = code == 200 && size > 0 && Update.begin(size), genuine = false;
  if (ok) {
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    WiFiClient* in = http.getStreamPtr();
    static uint8_t buf[1024];
    int done = 0;
    for (unsigned long last = millis(); ok && done < size && millis() - last < 15000;) {
      int n = in->read(buf, min((int)sizeof buf, size - done));
      if (n <= 0) {
        delay(5);
        continue;
      }
      last = millis();
      mbedtls_sha256_update_ret(&sha, buf, n);
      ok = Update.write(buf, n) == (size_t)n;
      done += n;
      updatePercent = done * 100LL / size;
    }
    mbedtls_sha256_finish_ret(&sha, hash);
    mbedtls_sha256_free(&sha);
    char got[65];
    for (int i = 0; i < 32; i++) snprintf(got + 2 * i, 3, "%02x", hash[i]);
    genuine = ok && done == size && !strcmp(got, wanted);
    ok = genuine && Update.end();
    if (!ok) Update.abort();
  }
  http.end();
  tls.stop();
  if (!ok) {
    Serial.printf("update failed: http %d, genuine %d, %s\n", code, genuine, Update.errorString());
    updateStatus = code == 200 && !genuine ? "Bad image" : "Failed";
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
      String body, base;
      xSemaphoreTake(poolLock, portMAX_DELAY);
      base = String(ALERT_URL "/v2/") + oblastNow;
      xSemaphoreGive(poolLock);
      r.code = fetchText((base + ".txt").c_str(), body);
      if (r.code == 200 && !feedSigned(body)) r.code = -101;  // not signed by our bridge
      if (r.code == 200 && !parseFeed(body.c_str(), r.feed)) r.code = -100;  // answered, but not our file
      xQueueOverwrite(results, &r);
      if (updateWanted) {
        updateWanted = false;
        runUpdate();
      }
      if (historyWanted) {
        historyWanted = false;
        h.code = fetchText((base + "-history.txt").c_str(), body);
        if (h.code == 200 && !feedSigned(body)) h.code = -101;
        if (h.code == 200 && !parseHistory(body.c_str(), maskNow, h.history)) h.code = -100;
        xQueueOverwrite(historyResults, &h);
      }
      if (regionsWanted) {  // once: the list changes only with a new release of the bridge
        static RegionList rl;
        if (fetchText(ALERT_URL "/v2/regions.txt", body) == 200 && feedSigned(body) && parseRegions(body.c_str(), rl)) {
          regionsWanted = false;
          xQueueOverwrite(regionResults, &rl);
        }
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
  h += F("<!doctype html><html lang=uk><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Vartovyi</title><style>"
         "body{margin:0;background:#050507;color:#ffeed6;font:16px ui-monospace,Menlo,Consolas,monospace;padding:16px;max-width:560px}"
         "h1{font-size:22px;margin:4px 0 16px;color:#ff9c00;letter-spacing:.1em}h2{font-size:15px;color:#ff9c00;text-transform:uppercase;letter-spacing:.06em;margin:0 0 10px}"
         "section{background:#1e1306;border-left:3px solid #ff9c00;clip-path:polygon(0 0,calc(100% - 14px) 0,100% 14px,100% 100%,0 100%);padding:14px;margin:0 0 14px}"
         ".row{display:flex;align-items:center;gap:8px;padding:8px 0;border-top:1px solid #693c0a}.row:first-of-type{border-top:0}.row span{flex:1;overflow-wrap:anywhere}"
         "input[type=text],input[type=password],input[type=number],select{width:100%;box-sizing:border-box;background:#050507;color:#ffeed6;border:1px solid #693c0a;border-radius:0;padding:11px;font-size:16px;margin:0 0 8px}"
         "button{background:#ff9c00;color:#000;border:0;border-radius:0;padding:11px 16px;font-size:16px;font-weight:600}"
         "button.s{background:#693c0a;color:#ffeed6;padding:7px 11px;font-size:14px;font-weight:500}label{display:flex;gap:10px;align-items:center;padding:7px 0}"
         "input[type=checkbox]{width:20px;height:20px}.tip,.msg{font-size:14px;color:#967c60}.msg{color:#39ff14;margin:0 0 12px}.times{display:flex;gap:8px}"
         "</style></head><body><h1>Vartovyi</h1>");
  if (message.length()) h += "<p class=msg>" + htmlEscape(message) + "</p>";

  h += String("<section><h2>") + T("Wi-Fi networks") + "</h2><p class=tip>" + T("The stick joins the first one in range, top to bottom.") + "</p>";
  xSemaphoreTake(poolLock, portMAX_DELAY);
  for (int i = 0; i < poolCount; i++) {
    h += "<form class=row method=post action=/net><input type=hidden name=i value=" + String(i) + "><span>" + htmlEscape(pool[i].ssid) + "</span>";
    if (i) h += String("<button class=s name=do value=up>") + T("Up") + "</button>";
    if (poolCount > 1) h += String("<button class=s name=do value=del>") + T("Remove") + "</button>";
    h += F("</form>");
  }
  bool full = poolCount >= MAX_NETS;
  xSemaphoreGive(poolLock);
  if (full) {
    h += String("<p class=tip>") + T("The list is full. Remove a network to add another.") + "</p>";
  } else {
    h += String("<form method=post action=/add style='margin-top:12px'><input type=text name=ssid maxlength=32 list=near placeholder='") +
         T("Network name") + "' autocapitalize=off autocorrect=off required><datalist id=near>";
    for (int i = 0; i < nearbyCount; i++) h += "<option value=\"" + htmlEscape(nearby[i]) + "\">";
    h += String("</datalist><input type=password name=pass maxlength=63 placeholder='") + T("Password") + "'><button>" + T("Add network") +
         "</button></form><p class=tip>" +
         T("iPhone hotspot: turn on Maximize Compatibility, and keep the Personal Hotspot screen open while the stick connects.") + "</p>";
  }
  h += String("</section><section><h2>") + T("Oblast") + "</h2><form method=post action=/region><select name=r aria-label=Oblast>";
  if (!regions.count) h += "<option value=" + String(cfg.oblast) + ">" + htmlEscape(T(cfg.oblastName)) + "</option>";
  for (int i = 0; i < regions.count; i++) {
    const Region& r = regions.regions[i];
    h += "<option value=" + String(r.id) + (strcmp(r.id, cfg.oblast) ? ">" : " selected>") + htmlEscape(T(r.name)) + "</option>";
  }
  h += String("</select><button>") + T("Save oblast") + "</button></form></section><section><h2>" + T("Alert zones") +
       "</h2><form method=post action=/zones>";
  if (!feed.list.count) h += String("<p class=tip>") + T("Loading the zones of this oblast: reload the page in a few seconds.") + "</p>";
  for (int i = 0; i < feed.list.count; i++) {
    h += "<label><input type=checkbox name=z" + String(i) + (cfg.zones & (1u << i) ? " checked>" : ">") + htmlEscape(T(feed.list.zones[i].name)) + "</label>";
  }
  h += String("<p class=tip>") + T("Watching any raion also includes alerts for the whole oblast.") + "</p><button>" + T("Save zones") +
       "</button></form></section><section><h2>" + T("Night mode") + "</h2><form method=post action=/night><label><input type=checkbox name=on";
  if (cfg.nightOn) h += F(" checked");
  h += String(">") + T("Drones beep once at night. Missiles always ring.") +
       "</label><div class=times><input type=number name=start min=0 max=23 aria-label='Start hour' value=" + String(cfg.nightStart) +
       "><input type=number name=end min=0 max=23 aria-label='End hour' value=" + String(cfg.nightEnd) + "></div><button>" +
       T("Save night mode") + "</button></form></section><section><h2>" + T("Display and sound") + "</h2><form method=post action=/display>";
  const char* const NAMES[] = {"bright", "volume"};
  const char* const LABELS[] = {"Screen brightness", "Volume of drones and all-clear (missiles are always at maximum)"};
  const int VALUES[] = {cfg.bright, cfg.volume};
  for (int k = 0; k < 2; k++) {
    h += String("<label for=") + NAMES[k] + ">" + T(LABELS[k]) + "</label><select id=" + NAMES[k] + " name=" + NAMES[k] + ">";
    for (int i = 0; i < 3; i++) h += "<option value=" + String(i) + (VALUES[k] == i ? " selected>" : ">") + T(LEVEL_NAMES[i]) + "</option>";
    h += F("</select>");
  }
  h += String("<button>") + T("Save") + "</button></form></section><form method=post action=/done><button class=s>" + T("Close setup") +
       "</button></form><p class=tip>Vartovyi " VERSION "</p></body></html>";
  for (int i = 0; i < 5; i++) h.replace(PAGE_COLORS[i], themeHex[i]);  // the page is written in the orange theme
  return h;
}

void showPage(const String& message) {
  server.send(200, "text/html; charset=utf-8", setupPage(T(message.c_str())));
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
  char saved[64];
  snprintf(saved, sizeof saved, T("Saved %s."), ssid.c_str());
  showPage(room ? String(saved) : String("The list is full."));
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

void chooseOblast(const Region& r) {
  if (!strcmp(r.id, cfg.oblast)) return;
  strcpy(cfg.oblast, r.id);
  strcpy(cfg.oblastName, r.name);
  strcpy(cfg.oblastUa, r.ua);
  cfg.zones = DEFAULT_ZONES;
  saveSettings();
  feed = Feed{};  // the old oblast's zones and choices mean nothing here
  hasData = historyLoaded = false;
  ringer.ackedSeq = NEVER_ACKED;  // an alert already on in the new oblast rings
  lastDraw = 0;
}

void saveRegion() {
  String id = server.arg("r");
  for (int i = 0; i < regions.count; i++)
    if (id == regions.regions[i].id) {
      chooseOblast(regions.regions[i]);
      return showPage("Oblast saved. Its zones appear below in a few seconds: reload the page.");
    }
  showPage("Unknown oblast.");
}

void saveZones() {
  unsigned mask = 0;
  for (int i = 0; i < feed.list.count; i++)
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
  server.on("/region", HTTP_POST, saveRegion);
  server.on("/night", HTTP_POST, saveNight);
  server.on("/done", HTTP_POST, [] {
    server.send(200, "text/html; charset=utf-8", "<!doctype html><meta name=viewport content='width=device-width'><body style='background:#050507;color:" + String(themeHex[0]) + ";font:18px ui-monospace,monospace;padding:24px'>" + String(T("Setup closed. You can leave this network.")));
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
// Cyberdeck look: black glass with scanlines, the stick's orange for the chrome, cut corners, a short glitch
// when the screen changes. The colours of danger stay apart from the chrome: red, yellow, and neon green for clear.

const int HEAD_H = 17, FOOT_Y = 120;  // content lives between them

void scanlines() {
  canvas.fillScreen(C_BG);
  for (int y = 1; y < canvas.height(); y += 3) canvas.drawFastHLine(0, y, canvas.width(), C_SCAN);
}

// A panel with two corners cut off, the shape everything here is made of.
void chamfer(int x, int y, int w, int h, int c, uint16_t color, bool fill) {
  auto& d = canvas;
  if (fill) {
    d.fillRect(x, y + c, w, h - 2 * c, color);
    d.fillRect(x + c, y, w - c, c, color);
    d.fillRect(x, y + h - c, w - c, c, color);
    d.fillTriangle(x, y + c, x + c, y, x + c, y + c, color);
    d.fillTriangle(x + w - c - 1, y + h - c - 1, x + w - 1, y + h - c - 1, x + w - c - 1, y + h - 1, color);
  } else {
    d.drawLine(x + c, y, x + w - 1, y, color);
    d.drawLine(x + w - 1, y, x + w - 1, y + h - c - 1, color);
    d.drawLine(x + w - 1, y + h - c - 1, x + w - c - 1, y + h - 1, color);
    d.drawLine(x + w - c - 1, y + h - 1, x, y + h - 1, color);
    d.drawLine(x, y + h - 1, x, y + c, color);
    d.drawLine(x, y + c, x + c, y, color);
  }
}

// HUD corner marks around a box.
void brackets(int x, int y, int w, int h, uint16_t color) {
  auto& d = canvas;
  const int L = 7;
  d.drawFastHLine(x, y, L, color), d.drawFastVLine(x, y, L, color);
  d.drawFastHLine(x + w - L, y, L, color), d.drawFastVLine(x + w - 1, y, L, color);
  d.drawFastHLine(x, y + h - 1, L, color), d.drawFastVLine(x, y + h - L, L, color);
  d.drawFastHLine(x + w - L, y + h - 1, L, color), d.drawFastVLine(x + w - 1, y + h - L, L, color);
}

// Black and colour stripes, as on a hazard sign.
void hazard(int y, int h, uint16_t color) {
  auto& d = canvas;
  d.fillRect(0, y, d.width(), h, C_BLACK);
  for (int x = -h; x < d.width(); x += 14) {
    d.fillTriangle(x, y + h, x + h, y, x + h + 7, y, color);
    d.fillTriangle(x, y + h, x + h + 7, y, x + 7, y + h, color);
  }
}

// Tektur, the one interface font, in the sizes tools/make-fonts.py builds; only the logo keeps Orbitron.
extern const uint8_t VLW_T11[] asm("_binary_fonts_t11_vlw_start");
extern const uint8_t VLW_T14[] asm("_binary_fonts_t14_vlw_start");
extern const uint8_t VLW_T18[] asm("_binary_fonts_t18_vlw_start");
extern const uint8_t VLW_T24[] asm("_binary_fonts_t24_vlw_start");
extern const uint8_t VLW_T32[] asm("_binary_fonts_t32_vlw_start");
extern const uint8_t VLW_T44[] asm("_binary_fonts_t44_vlw_start");
lgfx::VLWfont T11, T14, T18, T24, T32, T44;

void loadFonts() {
  static lgfx::PointerWrapper data[6];
  const uint8_t* const SRC[] = {VLW_T11, VLW_T14, VLW_T18, VLW_T24, VLW_T32, VLW_T44};
  lgfx::VLWfont* const FONT[] = {&T11, &T14, &T18, &T24, &T32, &T44};
  for (int i = 0; i < 6; i++) {
    data[i].set(SRC[i]);
    if (!FONT[i]->loadFont(&data[i])) Serial.printf("font %d did not load\n", i);
  }
}

// Draws text as it is: the boot screen is always in English.
void drawRaw(const char* text, int x, int y, uint8_t datum, uint16_t color, const lgfx::IFont* font = &T14) {
  canvas.setFont(font);
  canvas.setTextSize(1);
  canvas.setTextDatum(datum);
  canvas.setTextColor(color);
  canvas.drawString(text, x, y);
}

void drawText(const char* text, int x, int y, uint8_t datum, uint16_t color, const lgfx::IFont* font = &T14) {
  drawRaw(T(text), x, y, datum, color, font);
}

void drawSmall(const char* text, int x, int y, uint8_t datum, uint16_t color) { drawText(text, x, y, datum, color); }

// Text in a box of fixed width; what does not fit runs as a ticker: it rests at the start, then scrolls round.
void drawMarquee(const char* text, int x, int y, int width, uint16_t color, const lgfx::IFont* font = &T14) {
  auto& d = canvas;
  d.setFont(font);
  d.setTextSize(1);
  int w = d.textWidth(text);
  d.setClipRect(x, y - 10, max(width, 1), 20);
  if (w <= width) {
    drawRaw(text, x, y, middle_left, color, font);
  } else {
    const int GAP = 30, REST_MS = 1500, PX_MS = 35;  // about 28 px a second
    int loop = w + GAP;
    unsigned long t = millis() % (REST_MS + (unsigned long)loop * PX_MS);
    int shift = t < REST_MS ? 0 : (t - REST_MS) / PX_MS;
    drawRaw(text, x - shift, y, middle_left, color, font);
    drawRaw(text, x - shift + loop, y, middle_left, color, font);
  }
  d.clearClipRect();
}

void drawClipped(const char* text, int x, int y, int width, uint16_t color) { drawMarquee(T(text), x, y, width, color); }

// The biggest size that fits the width.
void drawFitted(const char* text, int y, uint16_t color, int width = 228) {
  static const lgfx::IFont* const FONTS[] = {&T32, &T24, &T18, &T14};
  auto& d = canvas;
  text = T(text);
  d.setTextSize(1);
  for (auto font : FONTS) {
    d.setFont(font);
    if (d.textWidth(text) <= width || font == FONTS[3]) break;
  }
  d.setTextDatum(middle_center);
  d.setTextColor(color);
  d.drawString(text, d.width() / 2, y);
}

// Big digits, shrunk to fit: the clock and the alert timers.
void drawBig(const char* text, int x, int y, uint16_t color, int width) {
  auto& d = canvas;
  d.setFont(&T44);
  d.setTextSize(1);
  int w = d.textWidth(text);
  if (w > width) d.setTextSize((float)width / w);
  d.setTextDatum(middle_center);
  d.setTextColor(color);
  d.drawString(text, x, y);
  d.setTextSize(1);
}

void drawBattery(int right, int y, uint16_t fg) {
  if (battery < 0) return;
  auto& d = canvas;
  const int SEG = 5, W = 4, GAP = 1;
  int x = right - SEG * (W + GAP);
  uint16_t fill = fg != C_MUTED ? fg : battery <= 15 ? C_RED : battery <= 35 ? C_YELLOW : C_ACCENT;
  int lit = (constrain(battery, 0, 100) + 19) / 20;
  for (int i = 0; i < SEG; i++) d.fillRect(x + i * (W + GAP), y, W, 9, i < lit ? fill : C_DIM);
  char text[8];
  snprintf(text, sizeof text, usb ? "+%d%%" : "%d%%", battery);
  drawText(text, x - 3, y + 5, middle_right, fg == C_MUTED ? C_TEXT : fg);
}

uint16_t levelColor(Level l) {
  switch (l) {
    case L_NONE: return C_GREEN;
    case L_LOST: return C_CYAN;
    case L_ALERT:
    case L_YELLOW: return C_YELLOW;
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

uint16_t linkColor(Link link) { return link == LINK_WAIT ? C_MUTED : linkIsBad(link) ? C_YELLOW : C_GREEN; }

// [02] MAP //VARTOVYI ............ link  battery
void drawHeader(const char* name, int number, Link link) {
  auto& d = canvas;
  scanlines();
  char num[4];
  snprintf(num, sizeof num, "%02d", number);
  chamfer(0, 0, 22, 15, 4, C_ACCENT, true);
  drawText(num, 11, 8, middle_center, C_BLACK);
  name = T(name);
  drawRaw(name, 27, 8, middle_left, C_ACCENT);
  int brandAt = 27 + d.textWidth(name) + 3;
  d.setFont(&T11);
  if (brandAt + d.textWidth("//VARTOVYI") < d.width() - 90) drawRaw("//VARTOVYI", brandAt, 9, middle_left, C_DIM, &T11);
  d.fillRect(d.width() - 85, 4, 7, 7, linkColor(link));
  drawBattery(d.width() - 2, 3, C_MUTED);
  d.drawFastHLine(0, 16, d.width(), C_DIM);
  d.drawFastHLine(0, 16, 60, C_ACCENT);
}

// What each button does here, pointing at it: the right side button sits on top, power at the bottom, M5 to the right.
void drawFooter(const char* next, const char* back, const char* ok) {
  auto& d = canvas;
  d.drawFastHLine(0, FOOT_Y, d.width(), C_DIM);
  int x = 4, y = FOOT_Y + 8;
  if (next[0]) {
    d.fillTriangle(x, y + 4, x + 8, y + 4, x + 4, y - 4, C_ACCENT);
    drawText(next, x + 12, y, middle_left, C_TEXT);
  }
  x = next[0] ? 86 : 4;
  if (back[0]) {
    d.fillTriangle(x, y - 4, x + 8, y - 4, x + 4, y + 4, C_ACCENT);
    drawText(back, x + 12, y, middle_left, C_TEXT);
  }
  if (ok[0]) {
    ok = T(ok);
    d.setFont(&T14);
    int w = d.textWidth(ok) + 30;
    chamfer(d.width() - w, FOOT_Y + 2, w, 13, 3, C_ACCENT, true);
    drawRaw("M5", d.width() - w + 5, y, middle_left, C_BLACK, &T11);
    drawText(ok, d.width() - 4, y, middle_right, C_BLACK);
  }
}

void drawPageFooter() { drawFooter("NEXT", page == P_HOME ? "" : "HOME", "MENU"); }

// A short burst of torn lines and colour fringes after the screen changes.
unsigned long glitchUntil = 0;
void glitch(unsigned long t) { glitchUntil = t + 220; }
void drawGlitch(unsigned long t) {
  if (!due(glitchUntil, t)) return;
  auto& d = canvas;
  for (int i = 0; i < 5; i++) {
    int y = esp_random() % d.height(), h = 1 + esp_random() % 3, x = esp_random() % d.width();
    d.fillRect(x, y, 20 + esp_random() % 120, h, i % 2 ? C_CYAN : C_ACCENT);
  }
  int y = 20 + esp_random() % 80;
  d.fillRect(0, y, d.width(), 2, C_BLACK);
}

// Alternates a solid colour with black, readable from across the room, with hazard stripes top and bottom.
void drawAlarm(const char* title, const char* line1, const char* line2, uint16_t color, bool on) {
  auto& d = canvas;
  uint16_t bg = on ? color : C_BLACK;
  uint16_t fg = on ? onColor(color) : color;
  d.fillScreen(bg);
  hazard(0, 12, color);
  hazard(d.height() - 12, 12, color);
  drawFitted(title, 38, fg);
  drawFitted(line1, 72, fg, 220);
  if (line2[0]) drawText(line2, d.width() / 2, 98, middle_center, fg);
  chamfer(d.width() / 2 - 58, d.height() - 14, 116, 14, 4, C_BLACK, true);
  drawText("ANY KEY = SILENCE", d.width() / 2, d.height() - 7, middle_center, color, &T11);
}

const char* watchingText(char* line, size_t size) {
  int watched = watchedCount(feed.list, cfg.zones);
  if (watched == 1) {
    for (int i = 0; i < feed.list.count; i++)
      if (cfg.zones & (1u << i)) snprintf(line, size, "%s", feed.list.zones[i].name);
  } else if (!feed.list.count || watched == feed.list.count) {
    snprintf(line, size, "%s", cfg.oblastName);
  } else {
    snprintf(line, size, T("%d of %d zones"), watched, feed.list.count);
  }
  return line;
}

// A line of status that runs right to left.
void drawTicker(int y, long long now) {
  auto& d = canvas;
  char watch[32], text[160];
  char ver[24];
  snprintf(ver, sizeof ver, "%s %s", T("VER"), VERSION);
  snprintf(text, sizeof text, T("OBL %s  //  WATCH %s  //  NET %s %ddBm  //  SEQ %ld  //  %s  //  "), T(cfg.oblastName),
           watchingText(watch, sizeof watch), WiFi.isConnected() ? WiFi.SSID().c_str() : "--", WiFi.isConnected() ? WiFi.RSSI() : 0,
           feed.seq, nightNow(now) ? T("NIGHT MODE") : saverNow ? T("SAVER") : ver);
  d.setFont(&T14);
  d.setTextSize(1);
  int w = d.textWidth(text), shift = (millis() / 40) % w;
  d.setClipRect(0, y - 8, d.width(), 16);
  for (int x = -shift; x < d.width(); x += w) drawText(text, x, y, middle_left, C_MUTED);
  d.clearClipRect();
}

void drawHome(Link link, long long now) {
  auto& d = canvas;
  drawHeader("HOME", 1, link);
  drawPageFooter();
  if (view.level != L_NONE) {  // an alert the user silenced: calm, and impossible to mistake for all clear
    uint16_t color = levelColor(view.level);
    chamfer(4, 21, d.width() - 8, 26, 6, color, true);
    drawFitted(viewTitle(view), 34, onColor(color), 220);
    char timer[16] = "--:--:--";
    if (view.since && timeIsSynced(now)) formatTimer(now - view.since, timer, sizeof timer);
    drawBig(timer, d.width() / 2, 73, color, 200);
    char line[64];
    char zone[64], threats[96];
    zoneText(view.zone, zone, sizeof zone);
    if (linkIsBad(link)) snprintf(line, sizeof line, T("%s // LAST KNOWN STATE"), T(linkTitle(link)));
    else if (view.level == L_LOST) snprintf(line, sizeof line, T("%s: may still be active"), zone);
    else snprintf(line, sizeof line, "%s  %s", zone, threatsText(view.threats, threats, sizeof threats));
    drawClipped(line, 6, 107, d.width() - 12, linkIsBad(link) ? C_YELLOW : C_TEXT);
    return;
  }
  if (link == LINK_WAIT || linkIsBad(link)) {
    bool wait = link == LINK_WAIT;
    brackets(8, 24, d.width() - 16, 70, wait ? C_ACCENT : C_YELLOW);
    drawFitted(wait ? "LINKING" : linkTitle(link), 50, wait ? C_ACCENT : C_YELLOW, 200);
    if (wait) {
      for (int i = 0, lit = millis() / 150 % 12; i < 12; i++) d.fillRect(38 + i * 14, 74, 10, 6, i <= lit ? C_ACCENT : C_DIM);
    } else {
      drawText("ALERTS MAY BE MISSED", d.width() / 2, 78, middle_center, C_TEXT);
    }
    drawText(WiFi.status() == WL_CONNECTED ? "wi-fi up, reaching the server" : "searching for wi-fi", d.width() / 2, 106, middle_center, C_MUTED);
    return;
  }
  char clock[6] = "--:--";
  time_t raw = now;
  struct tm local;
  if (timeIsSynced(now) && localtime_r(&raw, &local)) strftime(clock, sizeof clock, "%H:%M", &local);
  brackets(30, 21, d.width() - 60, 54, C_DIM);
  drawBig(clock, d.width() / 2, 48, C_ACCENT, 170);
  chamfer(4, 79, d.width() - 8, 20, 5, C_GREEN, false);
  d.fillRect(10, 85, 8, 8, C_GREEN);
  drawText("ALL CLEAR", 24, 89, middle_left, C_GREEN);
  char watch[32];
  drawClipped(watchingText(watch, sizeof watch), 112, 89, d.width() - 120, C_TEXT);
  drawTicker(110, now);
}

const ZoneAlert* zoneAlert(int index) {
  for (int i = 0; i < feed.count; i++)
    if (feed.zones[i].level != L_NONE && zoneIndex(feed.list, feed.zones[i].name) == index) return &feed.zones[i];
  return nullptr;
}

// The oblast's zones as tiles, city first and the oblast itself last. Watched zones fill with the alert's colour,
// others only get an outline; a watched quiet zone has an orange frame, one you do not watch a dim one.
void drawMap(Link link) {
  auto& d = canvas;
  drawHeader("MAP", 2, link);
  drawPageFooter();
  const ZoneList& l = feed.list;
  if (!l.count) {
    drawFitted("LOADING", 68, C_DIM);
    return;
  }
  const int X = 3, Y = 20, COLS = 3, TW = 76, GAP = 3;
  int rows = (l.count + 1 + COLS - 1) / COLS;  // one more cell counts the active zones
  int TH = min(24, (FOOT_Y - Y - 2 - (rows - 1) * GAP) / rows);
  for (int i = 0; i < l.count; i++) {
    int x = X + i % COLS * (TW + GAP), y = Y + i / COLS * (TH + GAP);
    const ZoneAlert* a = zoneAlert(i);
    bool wanted = zoneWanted(l, i, cfg.zones);
    uint16_t text = wanted ? C_TEXT : C_MUTED;
    if (a && wanted) {
      chamfer(x, y, TW, TH, 4, levelColor(a->level), true);
      text = onColor(levelColor(a->level));
    } else {
      chamfer(x, y, TW, TH, 4, C_PANEL, true);
      chamfer(x, y, TW, TH, 4, a ? levelColor(a->level) : wanted ? C_ACCENT : C_DIM, false);
    }
    const char* label = cfg.lang == LANG_UA && l.zones[i].uaLabel[0] ? l.zones[i].uaLabel : l.zones[i].label;
    d.setFont(&T14);
    drawRaw(label, x + TW / 2, y + TH / 2, middle_center, text, d.textWidth(label) > TW - 6 ? &T11 : &T14);
  }
  char line[20];
  snprintf(line, sizeof line, T("%d ACTIVE"), feed.count);
  int x = X + l.count % COLS * (TW + GAP), y = Y + l.count / COLS * (TH + GAP);
  drawText(line, x + TW / 2, y + TH / 2, middle_center, feed.count ? C_YELLOW : C_DIM);
}

int autoScroll(int count, int perScreen) {  // lists turn their own pages: M5 stays the menu button everywhere
  return count > perScreen ? (int)(millis() / 3000 % ((count + perScreen - 1) / perScreen)) * perScreen : 0;
}

void drawDetails(Link link, long long now) {
  auto& d = canvas;
  drawHeader("ACTIVE", 3, link);
  drawPageFooter();
  const ZoneAlert* shown[MAX_ZONES];
  int count = 0;
  for (int i = 0; i < feed.count; i++)
    if (zoneMatters(feed.zones[i], feed.list, cfg.zones)) shown[count++] = &feed.zones[i];
  if (!count) {
    brackets(20, 30, d.width() - 40, 70, C_DIM);
    drawFitted("NO THREATS", 58, C_GREEN, 180);
    drawText("in the zones you watch", d.width() / 2, 82, middle_center, C_MUTED);
    return;
  }
  int first = autoScroll(count, 3);
  for (int r = 0; r < 3 && first + r < count; r++) {
    const ZoneAlert& z = *shown[first + r];
    int y = 20 + r * 33;
    chamfer(3, y, d.width() - 6, 31, 5, C_PANEL, true);
    d.fillRect(3, y + 5, 4, 21, levelColor(z.level));
    drawClipped(z.name, 12, y + 9, 140, C_TEXT);
    char timer[16] = "";
    if (z.since && timeIsSynced(now)) formatTimer(now - z.since, timer, sizeof timer);
    drawText(timer, d.width() - 8, y + 9, middle_right, levelColor(z.level));
    char threats[96];
    drawClipped(z.threats[0] ? threatsText(z.threats, threats, sizeof threats) : "threat not specified", 12, y + 23, d.width() - 22, C_MUTED);
  }
}

void drawHistory(Link link, long long now) {
  auto& d = canvas;
  drawHeader("HISTORY", 4, link);
  drawPageFooter();
  if (!historyLoaded || !history.eventCount) {
    drawFitted(historyFailed ? "NO DATA" : historyLoaded ? "NO EVENTS" : "LOADING", 68, C_DIM);
    return;
  }
  const int ROWS = 5;
  int first = autoScroll(history.eventCount, ROWS);
  struct tm today {}, local {};
  time_t raw = now;
  localtime_r(&raw, &today);
  for (int r = 0; r < ROWS && first + r < history.eventCount; r++) {
    const HistoryEvent& e = history.events[first + r];
    int y = 28 + r * 20;
    raw = e.ts;
    localtime_r(&raw, &local);
    char when[14];
    strftime(when, sizeof when, local.tm_yday == today.tm_yday ? "%H:%M" : "%d.%m %H:%M", &local);
    d.fillRect(4, y - 4, 8, 8, levelColor(e.level));
    drawClipped(when, 17, y, 80, C_ACCENT);
    drawClipped(e.zone, 97, y, 140, e.level == L_NONE ? C_MUTED : C_TEXT);
    d.drawFastHLine(17, y + 10, d.width() - 21, C_SCAN);
  }
}

void drawStats(Link link) {
  auto& d = canvas;
  drawHeader("STATS", 5, link);
  drawPageFooter();
  if (!historyLoaded || !history.statCount) {
    drawFitted(historyFailed ? "NO DATA" : historyLoaded ? "QUIET WEEK" : "LOADING", 68, historyLoaded && !historyFailed ? C_GREEN : C_DIM);
    return;
  }
  int first = autoScroll(history.statCount, 3);
  for (int r = 0; r < 3 && first + r < history.statCount; r++) {
    const ZoneStats& s = history.stats[first + r];
    int y = 20 + r * 33;
    chamfer(3, y, d.width() - 6, 31, 5, C_PANEL, true);
    drawClipped(s.zone, 9, y + 9, 224, C_TEXT);
    char a[12], b[12], line[32];
    minutesText(s.todayMin, a, sizeof a);
    snprintf(line, sizeof line, T("TODAY %d / %s"), s.todayCount, a);
    drawClipped(line, 9, y + 23, 112, C_ACCENT);
    minutesText(s.weekMin, b, sizeof b);
    snprintf(line, sizeof line, T("WEEK %d / %s"), s.weekCount, b);
    drawClipped(line, 123, y + 23, 112, C_MUTED);
  }
}

void drawSystem(Link link, long long now) {
  auto& d = canvas;
  drawHeader("SYSTEM", 6, link);
  drawPageFooter();
  char rows[6][2][40];
  snprintf(rows[0][0], 40, "NET");
  if (WiFi.isConnected()) snprintf(rows[0][1], 40, "%.20s %d dBm", WiFi.SSID().c_str(), WiFi.RSSI());
  else snprintf(rows[0][1], 40, "offline");
  snprintf(rows[1][0], 40, "LINK");
  snprintf(rows[1][1], 40, "%s", link == LINK_OK ? "nominal" : link == LINK_WAIT ? "connecting" : linkTitle(link));
  snprintf(rows[2][0], 40, "OBLAST");
  snprintf(rows[2][1], 40, "%s", cfg.oblastName);
  snprintf(rows[3][0], 40, "DELAY");
  if (lastDelay >= 0) snprintf(rows[3][1], 40, T("%ld s, last alert"), lastDelay);
  else snprintf(rows[3][1], 40, "not measured yet");
  snprintf(rows[4][0], 40, "POWER");
  snprintf(rows[4][1], 40, "%d.%02d V %s%s", batteryMv / 1000, batteryMv % 1000 / 10, usb ? "usb" : T("battery"), saverNow ? T(", saver") : "");
  snprintf(rows[5][0], 40, "FIRMWARE");
  snprintf(rows[5][1], 40, T("%s, up %lum"), VERSION, millis() / 60000);
  int valuesAt = 0;  // after the widest label in this language
  d.setFont(&T14);
  for (int r = 0; r < 6; r++) valuesAt = max(valuesAt, (int)d.textWidth(T(rows[r][0])));
  valuesAt += 6 + 8;
  for (int r = 0; r < 6; r++) {
    int y = 26 + r * 16;
    drawText(rows[r][0], 6, y, middle_left, C_ACCENT);
    drawClipped(rows[r][1], valuesAt, y, d.width() - valuesAt - 4, r == 1 && link != LINK_OK ? C_YELLOW : C_TEXT);
  }
}

void menuText(int item, char* left, char* right, size_t size) {
  right[0] = 0;
  switch (item) {
    case MI_LANG:  // in both languages, so either reader finds it
      snprintf(left, size, "Мова · Language");
      snprintf(right, size, cfg.lang == LANG_UA ? "Українська" : "English");
      break;
    case MI_REGION:
      snprintf(left, size, "Oblast");
      snprintf(right, size, "%s", cfg.oblastName);
      break;
    case MI_ZONES:
      snprintf(left, size, "Zones");
      snprintf(right, size, T("%d of %d"), watchedCount(feed.list, cfg.zones), feed.list.count);
      break;
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
    case MI_THEME:
      snprintf(left, size, "Theme");
      snprintf(right, size, "%s", THEMES[cfg.theme].name);
      break;
    case MI_IMPACT:
      snprintf(left, size, "Impact FX");
      snprintf(right, size, "%s", IMPACT_NAMES[cfg.impact]);
      break;
    case MI_TEST: snprintf(left, size, "Test alarm"); break;
    case MI_SETUP: snprintf(left, size, "Phone setup"); break;
    case MI_UPDATE:
      snprintf(left, size, "Update firmware");
      if (updatePercent >= 0) snprintf(right, size, "%d%%", updatePercent);
      else snprintf(right, size, "%s", *updateStatus ? updateStatus : VERSION);
      break;
    default: left[0] = 0; break;
  }
}

// The selection bar slides to its row with a fading trail and a cyan fringe, then the row's name decodes out of noise.
const unsigned long SLIDE_MS = 170, DECODE_MS = 280;
int barFromY = FOOT_Y, barShownY = FOOT_Y;  // where the bar started its move, and where it was last drawn
unsigned long barMovedAt = 0;

void moveBar(unsigned long t) {
  barFromY = barShownY;
  barMovedAt = t;
}
bool menuAnimating(unsigned long t) { return barMovedAt && t - barMovedAt < SLIDE_MS + DECODE_MS; }
float easeOut(float p) { return 1 - (1 - p) * (1 - p) * (1 - p); }

// Letters not yet decoded show as noise, resolving left to right. Works on whole UTF-8 letters, so Cyrillic stays intact.
void decode(const char* src, char* out, size_t size, float progress) {
  static const char NOISE[] = "#$%&*+<>/|=01ABCDEFXZ";
  size_t letters = 0;
  for (const char* p = src; *p; p++) letters += ((uint8_t)*p & 0xC0) != 0x80;
  size_t shown = progress >= 1 ? letters : (size_t)(letters * progress), i = 0, o = 0;
  for (const char* p = src; *p && o + 4 < size; i++) {
    size_t n = 1;
    while (((uint8_t)p[n] & 0xC0) == 0x80) n++;  // the rest of this letter's bytes
    if (i < shown || *p == ' ') memcpy(out + o, p, n), o += n;
    else out[o++] = NOISE[esp_random() % (sizeof NOISE - 1)];
    p += n;
  }
  out[o] = 0;
}

void drawRow(int r, const char* left, const char* right, bool onBar, float decoded) {
  auto& d = canvas;
  int y = 20 + r * 20;
  char shown[64];
  decode(T(left), shown, sizeof shown, decoded);  // translated first, so the noise resolves into the words shown
  right = T(right);
  d.setFont(&T14);
  int room = d.width() - 9 - d.textWidth(right) - 8 - 10;  // the name gets what the value leaves
  drawMarquee(shown, 10, y + 10, max(room, 20), onBar ? C_BLACK : C_TEXT);
  drawRaw(right, d.width() - 9, y + 10, middle_right, onBar ? C_BLACK : C_ACCENT);
}

// Asked when an update is chosen during an alert: the stick hears nothing from the server while it downloads.
void drawUpdateWarning(Link link) {
  auto& d = canvas;
  drawHeader("FIRMWARE", MI_UPDATE + 1, link);
  hazard(18, 8, C_YELLOW);
  drawFitted("ALERT IS ACTIVE", 41, C_YELLOW, 220);
  drawText("It takes about a minute,", d.width() / 2, 64, middle_center, C_TEXT);
  drawText("and alerts cannot arrive.", d.width() / 2, 81, middle_center, C_TEXT);
  drawText("Update anyway?", d.width() / 2, 102, middle_center, C_ACCENT);
  drawFooter("", "CANCEL", "UPDATE");
}

void drawMenu(Link link) {
  if (confirmUpdate) return drawUpdateWarning(link);
  auto& d = canvas;
  const int ROWS = 5;
  int count = menu == M_ZONES ? feed.list.count : menu == M_REGIONS ? regions.count : MI_COUNT;
  drawHeader(menu == M_ZONES ? "ZONES" : menu == M_REGIONS ? "OBLAST" : "MENU", menuIndex + 1, link);
  const char* ok = menu == M_ZONES ? "TOGGLE" : menu == M_REGIONS ? "CHOOSE" : "SET";
  if (menu == M_MAIN && (menuIndex == MI_REGION || menuIndex == MI_ZONES)) ok = "OPEN";
  if (menu == M_MAIN && (menuIndex == MI_TEST || menuIndex == MI_SETUP || menuIndex == MI_UPDATE)) ok = "RUN";
  if (menu == M_MAIN && menuIndex == MI_THEME) ok = "NEXT THEME";
  drawFooter("NEXT", "BACK", ok);
  if (!count) {
    drawFitted("LOADING", 68, C_DIM);
    return;
  }
  int top = menuIndex < ROWS ? 0 : menuIndex - ROWS + 1;

  unsigned long since = millis() - barMovedAt;
  float slide = barMovedAt ? min(1.0f, since / (float)SLIDE_MS) : 1;
  float decoded = barMovedAt && since < SLIDE_MS + DECODE_MS ? (since > SLIDE_MS ? (since - SLIDE_MS) / (float)DECODE_MS : 0) : 1;
  int targetY = 20 + (menuIndex - top) * 20;
  int y = barFromY + (int)((targetY - barFromY) * easeOut(slide));
  if (slide < 1) {
    for (int k = 2; k >= 1; k--) {
      float back = max(0.0f, slide - 0.22f * k);
      chamfer(2, barFromY + (int)((targetY - barFromY) * easeOut(back)), d.width() - 4, 19, 5, k == 2 ? C_PANEL : C_DIM, true);
    }
    chamfer(5, y + 1, d.width() - 6, 19, 5, C_CYAN, false);
  }
  chamfer(2, y, d.width() - 4, 19, 5, C_ACCENT, true);
  barShownY = y;

  char left[32], right[32];
  for (int r = 0; r < ROWS && top + r < count; r++) {
    int i = top + r;
    bool onBar = i == menuIndex && slide >= 1;
    float dec = i == menuIndex ? decoded : 1;
    if (menu == M_ZONES) {
      drawRow(r, feed.list.zones[i].name, cfg.zones & (1u << i) ? "[x]" : "[ ]", onBar, dec);
    } else if (menu == M_REGIONS) {
      drawRow(r, regions.regions[i].name, strcmp(regions.regions[i].id, cfg.oblast) ? "" : "<<", onBar, dec);
    } else {
      menuText(i, left, right, sizeof left);
      drawRow(r, left, right, onBar, dec);
    }
  }
  // where in the list: a thin bar on the right edge
  int h = (FOOT_Y - 22) * min(count, ROWS) / count;
  d.fillRect(d.width() - 2, 20 + (FOOT_Y - 22 - h) * top / max(1, count - ROWS), 2, h, C_ACCENT);
}

void drawPortal() {
  auto& d = canvas;
  scanlines();
  bool joined = WiFi.softAPgetStationNum() > 0;
  d.fillRect(0, 0, 135, 135, C_WHITE);  // a QR code needs a light background to scan
  d.qrcode(joined ? "http://192.168.4.1/" : apQr.c_str(), 4, 4, 127, 4);
  int x = 142;
  drawText("SETUP", x, 10, middle_left, C_ACCENT);
  d.drawFastHLine(x, 19, 94, C_DIM);
  if (joined) {
    drawText("phone joined", x, 32, middle_left, C_GREEN);
    drawText("scan again or", x, 54, middle_left, C_TEXT);
    drawText("open", x, 70, middle_left, C_TEXT);
    drawText("192.168.4.1", x, 86, middle_left, C_ACCENT);
  } else {
    drawText("scan with", x, 32, middle_left, C_TEXT);
    drawText("the camera", x, 48, middle_left, C_TEXT);
    drawText(AP_NAME, x, 70, middle_left, C_ACCENT);
    drawText(apPass.c_str(), x, 86, middle_left, C_MUTED);
  }
  chamfer(x, 111, 92, 18, 4, C_ACCENT, true);
  drawText("M5 / BACK: EXIT", x + 46, 120, middle_center, C_BLACK, &T11);
}

void drawClear() {
  auto& d = canvas;
  scanlines();
  brackets(6, 8, d.width() - 12, d.height() - 16, C_GREEN);
  chamfer(20, 34, d.width() - 40, 44, 8, C_GREEN, true);
  drawFitted("ALL-CLEAR", 56, C_BLACK, 190);
  drawText("THREAT ENDED // STAND DOWN", d.width() / 2, 96, middle_center, C_GREEN);
}

// An old monitor switching on: a line grows from the centre, then the picture opens up and down.
// Drawn over whatever is on the canvas; p runs from 0 to 1.
void crtOpen(float p) {
  auto& d = canvas;
  int W = d.width(), H = d.height();
  if (p < 0.35f) {
    int w = max(2, (int)(W * easeOut(p / 0.35f)));
    d.fillScreen(C_BLACK);
    d.fillRect((W - w) / 2, H / 2 - 1, w, 2, C_TEXT);
    d.drawFastHLine((W - w) / 2, H / 2 - 2, w, C_ACCENT);
    d.drawFastHLine((W - w) / 2, H / 2 + 1, w, C_ACCENT);
    return;
  }
  int h = max(4, (int)(H * easeOut((p - 0.35f) / 0.65f))), top = (H - h) / 2;
  d.fillRect(0, 0, W, top, C_BLACK);
  d.fillRect(0, top + h, W, H - top - h, C_BLACK);
  d.drawFastHLine(0, top, W, C_ACCENT);
  d.drawFastHLine(0, top + h - 1, W, C_ACCENT);
}

const unsigned long WAKE_MS = 380;
unsigned long wokeAt = 0;  // the screen came back from saver sleep

// Power on: the monitor opens, a boot log runs, the name decodes out of noise. Any button skips it.
// The fetch task already runs meanwhile, so the first alerts are not late.
void bootAnimation() {
  auto& d = canvas;
  const unsigned long OPEN = 700, STEP = 300, LOG = 2300, LOGO_FOR = 2600, LOGO = LOG + LOGO_FOR, DECODE = 900;
  char oblast[ZONE_NAME];
  snprintf(oblast, sizeof oblast, "%s", cfg.oblastName);
  for (char* c = oblast; *c; c++) *c = toupper(*c);
  const char* const LINES[][2] = {{"> core", "OK"}, {"> display", "OK"}, {"> audio", "OK"}, {"> oblast", oblast},
                                  {"> uplink", poolCount ? "ARMED" : "SETUP"}};
  const int N = sizeof LINES / sizeof LINES[0];
  unsigned long start = millis();
  for (unsigned long e = 0; e < LOGO; e = millis() - start) {
    M5.update();
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnPWR.wasPressed()) break;
    scanlines();
    if (e < LOGO - LOGO_FOR) {
      drawRaw("VARTOVYI // AIR ALERT TERMINAL", 6, 10, middle_left, C_ACCENT, &T11);
      d.drawFastHLine(6, 17, d.width() - 12, C_DIM);
      int shown = e < OPEN ? 0 : min(N, (int)((e - OPEN) / STEP) + 1);
      for (int i = 0; i < shown; i++) {
        int y = 31 + i * 15;
        bool last = i == shown - 1 && e < OPEN + (unsigned long)(i + 1) * STEP;  // still being typed
        drawRaw(LINES[i][0], 6, y, middle_left, C_TEXT);
        d.setFont(&T14);
        int dotsFrom = 6 + d.textWidth(LINES[i][0]) + 4;
        for (int x = dotsFrom; x < 160; x += 6) d.fillRect(x, y + 3, 2, 2, C_DIM);
        if (!last) drawRaw(LINES[i][1], d.width() - 8, y, middle_right, i == 4 && !poolCount ? C_YELLOW : C_GREEN);
      }
      float bar = e < OPEN ? 0 : min(1.0f, (e - OPEN) / (float)(LOG - OPEN));
      d.drawRect(6, 112, d.width() - 12, 10, C_DIM);
      d.fillRect(8, 114, (int)((d.width() - 16) * bar), 6, C_ACCENT);
    } else {
      float p = min(1.0f, (e - (LOGO - LOGO_FOR)) / (float)DECODE);
      char name[12];
      decode("VARTOVYI", name, sizeof name, p);
      brackets(10, 22, d.width() - 20, 76, C_DIM);
      if (p < 1) drawRaw(name, d.width() / 2 + 2, 58, middle_center, C_CYAN, &fonts::Orbitron_Light_32);
      drawRaw(name, d.width() / 2, 56, middle_center, C_ACCENT, &fonts::Orbitron_Light_32);
      drawRaw(VERSION " // DEVELOPED BY SKERPSH", d.width() / 2, 88, middle_center, C_MUTED);
      glitchUntil = p < 1 ? millis() + 40 : 0;
      drawGlitch(millis());
    }
    if (e < OPEN) crtOpen(e / (float)OPEN);
    d.pushSprite(0, 0);
    delay(30);
  }
  glitch(millis());
}

// A knock leaves the screen "broken" for a moment: rows torn sideways, stuck pixel columns, noise, and on a hard hit
// a flash of inverted colours. The picture also feels the motion: it lags behind a swing on a spring, a hit sends
// a wave of tears across from the side it came from, a twist skews it, a fall floats it up and the landing slams
// it towards the real floor.
const unsigned long FX_MS[] = {0, 900, 2500, 2300}, INVERT_MS = 130, WAVE_MS = 450;
KnockSense knock{};
Knock fxKind = K_NONE;
unsigned long fxAt = 0;
uint32_t fxSeed = 1;
bool inverted = false;
Side waveFrom = SIDE_LEFT;
Spring shiftX{}, shiftY{}, skew{};  // the whole picture across and down, and the skew of its top edge
float gravX = 0, gravY = 0, gravZ = 1;  // where down is, from the sensor while the stick is still

// Sensor axes on the screen, held with M5 to the right of the screen: x to the right, y down.
// Measured on the stick: standing on its bottom edge reads +ax, the M5 side down reads +ay.
#define AXIS_RIGHT(ax, ay) (ay)
#define AXIS_DOWN(ax, ay) (ax)

uint32_t fxRand(uint32_t& s) {  // the same numbers for the whole knock, so stuck columns stay put
  s ^= s << 13, s ^= s >> 17, s ^= s << 5;
  return s;
}

float fxLevel(unsigned long t) {
  if (fxKind == K_NONE) return 0;
  unsigned long e = t - fxAt;
  if (e >= FX_MS[fxKind]) return fxKind = K_NONE, 0;
  return (fxKind == K_SWING ? 0.5f : 1.0f) * (1 - e / (float)FX_MS[fxKind]);
}

bool fxMoving(unsigned long t) { return fxKind != K_NONE || shiftX.moving() || shiftY.moving() || skew.moving(); }

// Clipped here: the library trims a rect's size but not a start past the edge, and then writes outside the canvas.
void scrollArea(int x, int y, int w, int h, int dx, int dy) {
  int W = canvas.width(), H = canvas.height();
  if (x < 0) w += x, x = 0;
  if (y < 0) h += y, y = 0;
  w = min(w, W - x), h = min(h, H - y);
  if ((!dx && !dy) || w <= 0 || h <= 0) return;
  canvas.setScrollRect(x, y, w, h);
  canvas.scroll(dx, dy);
}

// The motion part: skew, then the whole picture off its place, with colour fringes on the edges it left.
void drawMotion() {
  auto& d = canvas;
  int W = d.width(), H = d.height();
  d.setBaseColor(C_BLACK);
  int top = (int)skew.x;
  if (top) for (int y = 0; y < H; y += 5) scrollArea(0, y, W, 5, top * (H / 2 - y) / (H / 2), 0);
  int dx = (int)shiftX.x, dy = (int)shiftY.x;
  scrollArea(0, 0, W, H, dx, dy);
  d.clearScrollRect();
  if (dx > 1) d.drawFastVLine(dx - 1, 0, H, C_CYAN);
  if (dx < -1) d.drawFastVLine(W + dx, 0, H, rgb(255, 0, 200));
  if (dy > 1) d.drawFastHLine(0, dy - 1, W, C_CYAN);
  if (dy < -1) d.drawFastHLine(0, H + dy, W, rgb(255, 0, 200));
}

void drawImpact(unsigned long t) {
  auto& d = canvas;
  int W = d.width(), H = d.height();
  drawMotion();
  float k = fxLevel(t);
  if (k <= 0) return;
  unsigned long e = t - fxAt;
  const uint16_t BROKEN[] = {C_CYAN, rgb(255, 0, 200), C_GREEN, C_WHITE};
  d.setBaseColor(C_BLACK);
  int lean = shiftX.v > 0 ? 1 : -1;  // tears lean the way the picture is swinging
  for (int i = 0, n = 1 + (int)(k * 6); i < n; i++) {  // new ones every frame, so the picture shudders
    int y = esp_random() % H, h = 2 + esp_random() % (3 + (int)(12 * k));
    scrollArea(0, y, W, h, lean * (int)(esp_random() % (1 + (int)(30 * k))), 0);
  }
  if (fxKind >= K_STRIKE && e < WAVE_MS) {  // the wave: a band of torn pixels crossing from where the hit came
    float p = e / (float)WAVE_MS;
    bool across = waveFrom == SIDE_LEFT || waveFrom == SIDE_RIGHT;
    int span = across ? W : H, at = (int)(p * span);
    if (waveFrom == SIDE_RIGHT || waveFrom == SIDE_BOTTOM) at = span - at;
    for (int i = 0; i < 6; i++) {
      int off = at - 12 + (int)(esp_random() % 24), len = 4 + esp_random() % 14, jolt = (int)(esp_random() % 17) - 8;
      if (across) scrollArea(off, 0, len, H, 0, jolt);
      else scrollArea(0, off, W, len, jolt, 0);
    }
    d.clearScrollRect();
    for (int i = 0; i < 10; i++) {
      int a = at - 10 + (int)(esp_random() % 20), b = esp_random() % (across ? H : W);
      if (across) d.fillRect(a, b, 2 + esp_random() % 5, 1 + esp_random() % 6, BROKEN[esp_random() % 4]);
      else d.fillRect(b, a, 1 + esp_random() % 6, 2 + esp_random() % 5, BROKEN[esp_random() % 4]);
    }
  }
  d.clearScrollRect();
  uint32_t s = fxSeed;
  for (int i = 0, n = (int)(k * 5 + 0.5f); i < n; i++) d.drawFastVLine(fxRand(s) % W, 0, H, BROKEN[fxRand(s) % 4]);
  for (int i = 0, n = (int)(k * 12); i < n; i++)
    d.fillRect(esp_random() % W, esp_random() % H, 2 + esp_random() % 10, 1 + esp_random() % 4, BROKEN[esp_random() % 4]);
  if (fxKind == K_DROP && e > 250) {
    chamfer(28, 46, W - 56, 42, 6, C_BLACK, true);
    chamfer(28, 46, W - 56, 42, 6, C_RED, false);
    drawText("IMPACT DETECTED", W / 2, 59, middle_center, C_RED);
    char line[24];
    snprintf(line, sizeof line, T("RECALIBRATING%.*s"), (int)(e / 250 % 4), "...");
    drawText(line, W / 2, 76, middle_center, C_ACCENT);
  }
}

void senseImpact(Sound sound, unsigned long t) {
  static unsigned long last = 0;
  if (!cfg.impact || screenOff || portalOn || isAlarm(sound)) {  // an alarm stays readable
    shiftX = shiftY = skew = Spring{};
    last = t;
    return;
  }
  if (t - last < 20) return;
  float dt = min(0.05f, (t - last) / 1000.0f);
  last = t;
  float ax = 0, ay = 0, az = 0, rx = 0, ry = 0, rz = 0;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return;
  M5.Imu.getGyro(&rx, &ry, &rz);
  float g = sqrtf(ax * ax + ay * ay + az * az);
  if (g > 0.85f && g < 1.15f) gravX += (ax - gravX) * 0.08f, gravY += (ay - gravY) * 0.08f, gravZ += (az - gravZ) * 0.08f;
  float right = AXIS_RIGHT(ax - gravX, ay - gravY), down = AXIS_DOWN(ax - gravX, ay - gravY);  // motion, in g
  float floorRight = AXIS_RIGHT(gravX, gravY), floorDown = AXIS_DOWN(gravX, gravY);       // where the floor is
  Knock k = senseKnock(knock, g, t, cfg.impact);
  bool falling = knock.fallSince && t - knock.fallSince >= FREEFALL_MS;
#ifdef IMU_DEBUG
  static unsigned long printed = 0;
  if (t - printed >= 200) {
    printed = t;
    Serial.printf("imu g %.2f  floor right %.2f down %.2f  motion right %.2f down %.2f  twist %.0f\n", g, floorRight, floorDown, right, down, rz);
  }
#endif
  // The picture lags behind the motion. On Low only knocks move it, on High any brisk motion does.
  bool brisk = cfg.impact == 2 && right * right + down * down > 0.12f;
  if (brisk || k != K_NONE) {
    float push = 2500 * dt * (k != K_NONE ? 2 : 1);
    shiftX.v -= right * push;
    shiftY.v -= down * push;
  }
  if (cfg.impact == 2 && fabsf(rz) > 150) skew.v += rz * 0.5f * dt * 50;
  if (falling) shiftX.v -= floorRight * 400 * dt, shiftY.v -= floorDown * 400 * dt;  // weightless: it floats up
  if (k == K_DROP) shiftX.v += floorRight * 900, shiftY.v += floorDown * 900;        // and lands hard
  shiftX.step(dt, 40);
  shiftY.step(dt, 30);
  skew.step(dt, 30);
  if (k == K_NONE || (fxLevel(t) > 0 && k < fxKind)) return;
  if (k >= K_STRIKE) waveFrom = hitSide(right, down);
  fxKind = k, fxAt = t, fxSeed = esp_random() | 1;
  lastDraw = 0;
}

void render(Sound sound, Link link, long long now, unsigned long t) {
  bool alarm = isAlarm(sound);
  bool on = alarm && flashPhase(t, sound == S_BALLISTIC ? FLASH_FAST_MS : FLASH_MS);
  bool testing = due(testUntil, t), clearing = due(clearUntil, t);
  bool active = hasData && view.level != L_NONE;

  // Short blink every 2 s while an alert is active but silenced: it still says so in a dark room.
  digitalWrite(LED_PIN, (alarm ? on : active && t % 2000 < 120) ? HIGH : LOW);

  // The user's brightness always counts; saver halves it; only a siren or an all-clear goes to full.
  uint8_t level = BRIGHT_LEVELS[cfg.bright];
  uint8_t wanted = alarm || clearing ? BRIGHT_ALARM : portalOn ? max(level, BRIGHT_ACTIVE) : saverNow ? max(8, level / 2) : level;
  screenOff = saverNow && !alarm && !clearing && !portalOn && t - lastInput > SCREEN_OFF_MS;
  static uint8_t brightness = 1;
  if (screenOff) wanted = 0;
  if (wanted != brightness) {
    M5.Display.setBrightness(brightness = wanted);
    Serial.printf("brightness %u (setting %d, saver %d)\n", wanted, cfg.bright, (bool)saverNow);
  }
  // The ticker and the glitch move; in saver mode the screen stays still to spare the battery.
  if (alarm) fxKind = K_NONE;
  bool invert = fxKind >= K_STRIKE && t - fxAt < INVERT_MS;
  if (invert != inverted) M5.Display.invertDisplay(inverted = invert);
  bool moving = due(glitchUntil, t) || menuAnimating(t) || t - wokeAt < WAKE_MS + 30 || fxMoving(t);
  unsigned long every = moving ? 30 : saverNow ? DRAW_SAVER_MS : DRAW_MS;
  if (screenOff || (!alarm && t - lastDraw < every)) return;
  lastDraw = t;

  char timer[16] = "";
  if (testing) {
    drawAlarm("ALARM TEST", "This is a test", "", C_RED, on);
  } else if (sound == S_LINK) {
    drawAlarm(linkTitle(link), "Alerts may be missed", "", C_YELLOW, on);
  } else if (alarm) {
    if (view.since && timeIsSynced(now)) formatTimer(now - view.since, timer, sizeof timer);
    char zone[64], threats[96];
    zoneText(view.zone[0] ? view.zone : cfg.oblastName, zone, sizeof zone);
    drawAlarm(viewTitle(view), zone, view.threats[0] ? threatsText(view.threats, threats, sizeof threats) : timer, levelColor(view.level), on);
  } else if (portalOn) {
    drawPortal();
  } else if (clearing) {
    drawClear();
  } else if (menu != M_NONE) {
    drawMenu(link);
  } else {
    switch (page) {
      case P_MAP: drawMap(link); break;
      case P_DETAILS: drawDetails(link, now); break;
      case P_HISTORY: drawHistory(link, now); break;
      case P_STATS: drawStats(link); break;
      case P_SYSTEM: drawSystem(link, now); break;
      default: drawHome(link, now); break;
    }
  }
  if (!alarm) drawGlitch(t);
  if (!alarm) drawImpact(t);
  if (!alarm && t - wokeAt < WAKE_MS) crtOpen((t - wokeAt) / (float)WAKE_MS);
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
// The right side button (on top when M5 is to the right of the screen) goes forward, power (at the bottom) goes back,
// M5 opens the menu and chooses. The footer of every screen says so.

void activateMenuItem(unsigned long t) {
  switch (menuIndex) {
    case MI_LANG:
      cfg.lang = cfg.lang == LANG_UA ? LANG_EN : LANG_UA;
      saveSettings();
      glitch(t);
      break;
    case MI_REGION:
      menu = M_REGIONS;
      menuIndex = 0;
      for (int i = 0; i < regions.count; i++)
        if (!strcmp(regions.regions[i].id, cfg.oblast)) menuIndex = i;
      break;
    case MI_ZONES: menu = M_ZONES; menuIndex = 0; break;
    case MI_NIGHT: cfg.nightOn = !cfg.nightOn; saveSettings(); break;
    case MI_FLIP: cfg.flip = !cfg.flip; saveSettings(); break;
    case MI_POWER: cfg.power = stepIndex(cfg.power, 3, 1); saveSettings(); break;
    case MI_BRIGHT: cfg.bright = stepIndex(cfg.bright, 3, 1); saveSettings(); break;
    case MI_VOLUME: cfg.volume = stepIndex(cfg.volume, 3, 1); saveSettings(); chimePending = true; break;
    case MI_IMPACT: cfg.impact = stepIndex(cfg.impact, 3, 1); saveSettings(); break;
    case MI_THEME:
      cfg.theme = stepIndex(cfg.theme, THEME_COUNT, 1);
      applyTheme();
      saveSettings();
      glitch(t);
      break;
    case MI_UPDATE:
      if (view.level != L_NONE) confirmUpdate = true;  // the download pauses the alerts: the user decides
      else updateWanted = true;
      break;
    case MI_TEST: testUntil = t + TEST_MS; break;
    case MI_SETUP: startPortal(); break;
    default: break;
  }
}

void goBack() {
  if (menu == M_ZONES || menu == M_REGIONS) {
    menuIndex = menu == M_ZONES ? MI_ZONES : MI_REGION;
    menu = M_MAIN;
  } else if (menu == M_MAIN) {
    menu = M_NONE;
  } else {
    page = P_HOME;
  }
}

void handleButtons(bool ok, bool next, bool back, unsigned long t) {
  if (portalOn) {
    if (ok || back) stopPortal();
    return;
  }
  if (confirmUpdate) {  // only M5 goes on; the side button does nothing, so a stray press cannot start it
    if (ok) updateWanted = true;
    if (ok || back) confirmUpdate = false, lastDraw = 0;
    return;
  }
  Page wasPage = page;
  Menu wasMenu = menu;
  int wasIndex = menuIndex;
  if (back) {
    goBack();
  } else if (menu == M_NONE) {
    if (next) page = (Page)stepIndex(page, PAGE_COUNT, 1);
    else if (ok) menu = M_MAIN, menuIndex = 0;
  } else {
    int count = menu == M_ZONES ? feed.list.count : menu == M_REGIONS ? regions.count : MI_COUNT;
    if (next) {
      menuIndex = stepIndex(menuIndex, count, 1);
    } else if (ok && menu == M_MAIN) {
      activateMenuItem(t);
    } else if (ok && menu == M_REGIONS && menuIndex < count) {
      chooseOblast(regions.regions[menuIndex]);
      goBack();
    } else if (ok && menu == M_ZONES && menuIndex < count) {
      unsigned bit = cfg.zones ^ (1u << menuIndex);
      if (watchedCount(feed.list, bit)) {  // at least one zone stays: a stick that watches nothing would look fine and never ring
        cfg.zones = bit;
        saveSettings();
        historyLoaded = false;
      } else {
        chimePending = true;
      }
    }
  }
  if (page != wasPage || menu != wasMenu) glitch(t);
  if (menu != M_NONE && (menu != wasMenu || menuIndex != wasIndex)) {
    if (wasMenu == M_NONE) barShownY = FOOT_Y;  // a menu that opens: the bar rises from the bottom
    moveBar(t);
  }
  lastDraw = 0;
}

// ---------- main ----------

// Internal RAM is what TLS needs: one connection takes a single block of about 40 KB.
size_t largestInternal() { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

void applyResult(const FetchResult& r, unsigned long t) {
  static int starved = 0;
  if (r.code != 200) {
    size_t largest = largestInternal();
    Serial.printf("fetch failed: %d, free %u, largest block %u, min free %u\n", r.code, ESP.getFreeHeap(), largest, ESP.getMinFreeHeap());
    // Memory too broken up for TLS does not mend by itself: a stick that cannot connect must not just sit there silent.
    if (r.code < 0 && largest < 45000 && ++starved >= 20) {
      Serial.println("not enough memory for TLS, restarting");
      delay(100);
      ESP.restart();
    }
    return;
  }
  starved = 0;
  bool changed = !hasData || feed.seq != r.feed.seq || feed.count != r.feed.count;
  long long now = time(nullptr);
  if (hasData && feed.seq != r.feed.seq && r.feed.msgTs && timeIsSynced(now)) lastDelay = max(0LL, now - r.feed.msgTs);
  if (trial) {  // reached the server: this firmware works and stays
    trial = false;
    prefs.putBool("trial", false);
    Serial.println("new firmware confirmed");
  }
  lastGood = t;
  if (strcmp(r.feed.obl, cfg.oblast)) return;  // fetched just before the oblast changed
  feed = r.feed;
  hasData = true;
  if (changed) Serial.printf("seq %ld, %d zone(s) active, min heap %u\n", feed.seq, feed.count, ESP.getMinFreeHeap());
}

void setup() {
  Serial.begin(115200);
  M5.begin(M5.config());
  M5.Display.setRotation(1);
  M5.Speaker.setVolume(255);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  canvas.setPsram(psramFound());  // the 65 KB screen buffer in PSRAM leaves internal RAM to TLS
  if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) Serial.println("no memory for the screen buffer");
  loadFonts();
  Serial.printf("psram %u, free internal %u, largest block %u\n", ESP.getPsramSize(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL), largestInternal());
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
  regionResults = xQueueCreate(1, sizeof(RegionList));
  // Core 1 like loop(): a TLS handshake on core 0 starves its idle task and the watchdog reboots the chip.
  xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 1, nullptr, 1);
  bootAnimation();
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
  static RegionList rl;
  if (xQueueReceive(regionResults, &rl, 0) == pdTRUE) regions = rl;
  static HistoryResult hr;
  if (xQueueReceive(historyResults, &hr, 0) == pdTRUE && (hr.code != 200 || !strcmp(hr.history.obl, cfg.oblast))) {
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
      wokeAt = t;  // the press only wakes the screen
    } else {
      handleButtons(a, b, c, t);
    }
    sound = due(testUntil, t) ? S_SIREN : wantedSound(ringer, hasData, view, link, night);
  }

  senseImpact(sound, t);

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
    confirmUpdate = false;
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
