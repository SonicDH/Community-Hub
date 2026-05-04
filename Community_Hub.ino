/*
 * ╔═══════════════════════════════════════╗
 * ║      C O M M U N I T Y  H U B         ║
 * ║        Local Bulletin Board           ║
 * ╚═══════════════════════════════════════╝
 *
 * Hardware : ESP32-C3
 * Storage  : Internal Flash via LittleFS
 *
 * Libraries (Arduino Library Manager):
 *   - ArduinoJson   by Benoit Blanchon
 *
 * Board setting: ESP32-C3 Dev Module
 * Partition scheme: Default 4MB with spiffs (1.2MB APP / 1.5MB SPIFFS)
 * Uses the built-in WebServer (no extra libs needed).
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Update.h>
#include "FS.h"
#include "LittleFS.h"
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>


// ===================== CONFIG ===================== //
namespace Config {
  //========= Defaults — overridable at runtime via admin panel ========//

  const char* LOCALITY_NAME = "Community Hub";
  const char* BOARD_ICON    = "🌱";
  const char* BOARD_TAGLINE = "Take what you need • Share what you can";
  const char* BOARD_RULES   = "Be local • Be kind • No spam";
  const char* BOARD_FOOTER  = "Powered locally — no internet required";

  const char* ADMIN_KEY = "change_me"; // Please definintely do - either here or in the admin panel.

  const int LED_PIN = 4; 
 
  const int LED_DAY_BRIGHTNESS   = 80;
  const int LED_NIGHT_BRIGHTNESS = 20;
  const int NIGHT_START_HOUR     = 20;
  const int DAY_START_HOUR       = 7;


  //======= Settings that are NOT in the Admin Panel ==========//

  // Access Point settings
  // If you want to customize the AP info, this is the place to do it. 
  // SSID is what neighbours see in their WiFi list.
  // Leave AP_PASS empty ("") for an open network.
  const char* AP_SSID     = "Community Hub";
  const char* AP_PASS     = "";           // "" = open network
  const int   AP_CHANNEL  = 6;
  const int   AP_MAX_CONN = 20;

  // Default message expiration time
  const int DEFAULT_EXPIRY_HOURS = 72;


  // !!!! DO NOT CHANGE THESE !!!!
  // The MAX_MSGS amount is not arbitrary. The heap for the array needs to be sized accordingly.
  // And why would you even need to change it? 200 messages is an absurd amount anyhow. 
  const int MAX_MSGS             = 200;
  const char* STORAGE_FILE  = "/msgs.json";
  const char* TIME_FILE     = "/time.json";
  const char* LEDCFG_FILE   = "/led.json";

}

// ===================== RUNTIME IDENTITY =====================
// These shadow the Config defaults and can be changed via the admin panel.
// Persisted to /identity.json on the SD card.

String id_name    = Config::LOCALITY_NAME;
String id_icon    = Config::BOARD_ICON;
String id_tagline = Config::BOARD_TAGLINE;
String id_rules   = Config::BOARD_RULES;
String id_footer  = Config::BOARD_FOOTER;

void saveIdentityConfig() {
  DynamicJsonDocument doc(1024);
  doc["name"]    = id_name;
  doc["icon"]    = id_icon;
  doc["tagline"] = id_tagline;
  doc["rules"]   = id_rules;
  doc["footer"]  = id_footer;

  File tmp = LittleFS.open("/id.tmp", FILE_WRITE);
  if (!tmp) return;
  serializeJson(doc, tmp);
  tmp.close();
  LittleFS.remove("/identity.json");
  LittleFS.rename("/id.tmp", "/identity.json");
}

void loadIdentityConfig() {
  if (!LittleFS.exists("/identity.json")) return;
  File f = LittleFS.open("/identity.json");
  if (!f) return;
  DynamicJsonDocument doc(1024);
  if (!deserializeJson(doc, f)) {
    if (doc["name"].as<String>().length())    id_name    = doc["name"].as<String>();
    if (doc["icon"].as<String>().length())    id_icon    = doc["icon"].as<String>();
    if (doc["tagline"].as<String>().length()) id_tagline = doc["tagline"].as<String>();
    if (doc["rules"].as<String>().length())   id_rules   = doc["rules"].as<String>();
    if (doc["footer"].as<String>().length())  id_footer  = doc["footer"].as<String>();
  }
  f.close();
}

// ===================== RUNTIME ADMIN KEY =====================
// Shadows Config::ADMIN_KEY. Persisted to /adminkey.json.
// Config::ADMIN_KEY is the run-time fallback if the file is absent.

String adminKey    = Config::ADMIN_KEY;
String sessionToken    = "";  // set on successful auth, cleared on reboot
unsigned long tokenIssuedAt = 0; // millis() when token was generated
#define TOKEN_LIFETIME_MS  1800000UL  // 30 minutes

String generateToken() {
  String token = "";
  for (int i = 0; i < 4; i++) {
    uint32_t r = esp_random();
    char chunk[9];
    snprintf(chunk, sizeof(chunk), "%08x", r);
    token += chunk;
  }
  tokenIssuedAt = millis();
  return token;
}

void saveAdminKey() {
  DynamicJsonDocument doc(128);
  doc["key"] = adminKey;
  File tmp = LittleFS.open("/adminkey.tmp", FILE_WRITE);
  if (!tmp) return;
  serializeJson(doc, tmp);
  tmp.close();
  LittleFS.remove("/adminkey.json");
  LittleFS.rename("/adminkey.tmp", "/adminkey.json");
}

void loadAdminKey() {
  if (!LittleFS.exists("/adminkey.json")) return;
  File f = LittleFS.open("/adminkey.json");
  if (!f) return;
  DynamicJsonDocument doc(128);
  if (!deserializeJson(doc, f)) {
    String k = doc["key"] | "";
    if (k.length()) adminKey = k;
  }
  f.close();
}

// ===================== RUNTIME LED SETTINGS =====================
// These start from Config defaults but can be changed via the admin panel and are persisted to /led.json on the SD card.

int  led_day_brightness   = Config::LED_DAY_BRIGHTNESS;
int  led_night_brightness = Config::LED_NIGHT_BRIGHTNESS;
int  led_night_start      = Config::NIGHT_START_HOUR;
int  led_day_start        = Config::DAY_START_HOUR;
int  led_pin              = Config::LED_PIN;
bool led_enabled          = true;
bool led_pulse_enabled    = true;  // sine-wave pulsing on/off
bool led_activity_enabled = true;  // faster pulse on recent post activity

void saveLedConfig() {
  DynamicJsonDocument doc(512);
  doc["day_br"]   = led_day_brightness;
  doc["night_br"] = led_night_brightness;
  doc["night_st"] = led_night_start;
  doc["day_st"]   = led_day_start;
  doc["pin"]      = led_pin;
  doc["enabled"]  = led_enabled;
  doc["pulse"]    = led_pulse_enabled;
  doc["activity"] = led_activity_enabled;

  File tmp = LittleFS.open("/led.tmp", FILE_WRITE);
  if (!tmp) return;
  serializeJson(doc, tmp);
  tmp.close();
  LittleFS.remove(Config::LEDCFG_FILE);
  LittleFS.rename("/led.tmp", Config::LEDCFG_FILE);
}
void loadLedConfig() {
  if (!LittleFS.exists(Config::LEDCFG_FILE)) return;
  File f = LittleFS.open(Config::LEDCFG_FILE);
  if (!f) return;
  DynamicJsonDocument doc(512);
  if (!deserializeJson(doc, f)) {
    led_day_brightness   = doc["day_br"]   | Config::LED_DAY_BRIGHTNESS;
    led_night_brightness = doc["night_br"] | Config::LED_NIGHT_BRIGHTNESS;
    led_night_start      = doc["night_st"] | Config::NIGHT_START_HOUR;
    led_day_start        = doc["day_st"]   | Config::DAY_START_HOUR;
    led_pin              = doc["pin"]      | Config::LED_PIN;
    led_enabled          = doc["enabled"]  | true;
    led_pulse_enabled    = doc["pulse"]    | true;
    led_activity_enabled = doc["activity"] | true;
  }
  f.close();
}

// ===================== TIME =====================
unsigned long baseEpoch  = 0; // We use UNIX time in this house, son.
unsigned long baseMillis = 0;
unsigned long lastTimeSave = 0;

unsigned long nowSecs() {
  return baseEpoch + (millis() - baseMillis) / 1000;
}

bool setTimeFromString(String t) {
  if (t.length() != 13) return false;
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_mday = t.substring(0,  2).toInt();
  tm.tm_mon  = t.substring(2,  4).toInt() - 1;
  tm.tm_year = t.substring(4,  8).toInt() - 1900;
  tm.tm_hour = t.substring(9, 11).toInt();
  tm.tm_min  = t.substring(11, 13).toInt();
  time_t epoch = mktime(&tm);
  if (epoch <= 0) return false;
  baseEpoch  = epoch;
  baseMillis = millis();
  return true;
}

void saveTime() { // "I save more time with this one lifehack than any other way! Like and subscribe for more hastag relateable content."
  DynamicJsonDocument doc(256);
  doc["epoch"] = nowSecs();
  File tmp = LittleFS.open("/time.tmp", FILE_WRITE);
  if (!tmp) return;
  serializeJson(doc, tmp);
  tmp.close();
  LittleFS.remove(Config::TIME_FILE);
  LittleFS.rename("/time.tmp", Config::TIME_FILE);
}

void loadTime() {
  if (!LittleFS.exists(Config::TIME_FILE)) return;
  File f = LittleFS.open(Config::TIME_FILE);
  if (!f) return;
  DynamicJsonDocument doc(256);
  if (!deserializeJson(doc, f)) {
    baseEpoch  = doc["epoch"];
    baseMillis = millis();
  }
  f.close();
}

int currentHour() {
  time_t t = nowSecs();
  struct tm* tm = localtime(&t);
  return tm ? tm->tm_hour : 12;
}

// ===================== UPTIME =====================
unsigned long bootMillis = 0;

String formatUptime() {
  unsigned long secs  = (millis() - bootMillis) / 1000;
  unsigned long days  = secs / 86400; secs %= 86400;
  unsigned long hours = secs / 3600;  secs %= 3600;
  unsigned long mins  = secs / 60;
  char buf[32];
  if (days > 0)
    snprintf(buf, sizeof(buf), "↑ %lud %luh %lum", days, hours, mins);
  else if (hours > 0)
    snprintf(buf, sizeof(buf), "↑ %luh %lum", hours, mins);
  else
    snprintf(buf, sizeof(buf), "↑ %lum", mins);
  return String(buf);
}

// ===================== MESSAGES =====================
struct Message {
  uint16_t     id;
  String author;
  String type;
  String text;
  unsigned long expires;
};

Message msgs[Config::MAX_MSGS];
int msgCount = 0;
uint16_t nextMsgId = 1;
unsigned long lastPostTime = 0;

bool msgsDirty = false; // Ooh you're so dirty.
unsigned long lastMsgDirtyTime = 0;

void saveMessages() {
  DynamicJsonDocument doc(81920);  // ~80KB; sized for 200 worst-case messages
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < msgCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["id"]      = msgs[i].id;
    o["author"]  = msgs[i].author;
    o["type"]    = msgs[i].type;
    o["text"]    = msgs[i].text;
    o["expires"] = msgs[i].expires;
  }
  File tmp = LittleFS.open("/msgs.tmp", FILE_WRITE);
  if (!tmp) return;
  serializeJson(doc, tmp);
  tmp.close();
  LittleFS.remove(Config::STORAGE_FILE);
  LittleFS.rename("/msgs.tmp", Config::STORAGE_FILE);
}

void loadMessages() {
  if (!LittleFS.exists(Config::STORAGE_FILE)) return;
  File f = LittleFS.open(Config::STORAGE_FILE);
  if (!f) return;
  DynamicJsonDocument doc(81920);  // ~80KB; sized for 200 worst-case messages
  if (deserializeJson(doc, f)) { f.close(); return; }
  JsonArray arr = doc.as<JsonArray>();
  msgCount = 0;
  for (JsonObject o : arr) {
    if (msgCount >= Config::MAX_MSGS) break;
    msgs[msgCount].id      = o["id"] | nextMsgId;
    msgs[msgCount].author  = (const char*)o["author"];
    msgs[msgCount].type    = (const char*)o["type"];
    msgs[msgCount].text    = (const char*)o["text"];
    msgs[msgCount].expires = o["expires"];
    if (msgs[msgCount].id >= nextMsgId) nextMsgId = msgs[msgCount].id + 1;
    msgCount++;
  }
  f.close();
}

void addMessage(String author, String type, String text, int expiryHours) {
  if (msgCount >= Config::MAX_MSGS) {
    // Find the oldest expired post and evict it
    unsigned long now = nowSecs();
    int evict = -1;
    unsigned long oldest = ULONG_MAX;
    for (int i = 0; i < msgCount; i++) {
      if (msgs[i].expires <= now && msgs[i].expires < oldest) {
        oldest = msgs[i].expires;
        evict = i;
      }
    }
    if (evict < 0) return;  // No expired posts — board is genuinely full
    // Shift everything above the evicted slot down one
    for (int i = evict; i < msgCount - 1; i++) msgs[i] = msgs[i + 1];
    msgCount--;
  }
  msgs[msgCount].id      = nextMsgId++;
  msgs[msgCount].author  = author;
  msgs[msgCount].type    = type;
  msgs[msgCount].text    = text;
  msgs[msgCount].expires = nowSecs() + expiryHours * 3600;
  msgCount++;
  lastPostTime = nowSecs();
  if (!msgsDirty) { msgsDirty = true; lastMsgDirtyTime = millis(); }
}

// ===================== LED =====================
void updateLED() {
  if (!led_enabled) {
    analogWrite(led_pin, 0);
    return;
  }

  int  hour    = currentHour();
  bool isNight = (hour >= led_night_start || hour < led_day_start);
  int  maxBr   = map(isNight ? led_night_brightness : led_day_brightness, 0, 100, 0, 255);

  if (!led_pulse_enabled) {
    analogWrite(led_pin, maxBr);
    return;
  }

  bool recent = led_activity_enabled && (nowSecs() - lastPostTime) < (3 * 3600);
  float speed = recent ? 0.01f : 0.003f;
  int   bright = (int)((sin(millis() * speed) + 1.0f) * (maxBr / 2.0f)); // A sinewave may be a bit SPECIFIC of a choice, but I took it as a... SIGN!
  analogWrite(led_pin, bright);
}

// ===================== HTML: MAIN BOARD =====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Community Hub</title>
<style>
:root {
  --bg:            #ede8de;
  --surface:       #faf8f3;
  --surface2:      #f0ebe0;
  --border:        #b0a080;
  --border-light:  #d4c9b0;
  --ink:           #2c2416;
  --ink-muted:     #7a6a55;
  --accent:        #8aad87;
  --accent-dark:   #31502f;
  --accent-light:  #c8ddc6;

  --c-notice-fg:  #6b5a3e; --c-notice-bg: #f0e8d8; --c-notice-bar: #a09070;
  --c-offer-fg:   #2d5c2a; --c-offer-bg:  #daeeda; --c-offer-bar:  #5a8a57;
  --c-need-fg:    #7a4a10; --c-need-bg:   #f5e8d0; --c-need-bar:   #c4813a;
  --c-event-fg:   #1e4f70; --c-event-bg:  #d8eaf5; --c-event-bar:  #4a88b0;

  --radius: 6px;
  --shadow: 3px 4px 0 rgba(44,36,22,0.10);
}
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

body {
  background: var(--bg);
  color: var(--ink);
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  min-height: 100vh;
  display: flex;
  flex-direction: column;
}

/* ── Header ── */
.site-header {
  background: var(--accent-dark);
  padding: 14px 20px;
  display: flex;
  align-items: center;
  gap: 14px;
  border-bottom: 3px solid #1d3a1b;
  flex-wrap: wrap;
}
.header-title-block { flex: 1; min-width: 160px; }
.site-title {
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 20px;
  font-weight: bold;
  color: #d8edcf;
  line-height: 1.2;
}
.site-sub {
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 14px;
  color: var(--accent);
  margin-top: 2px;
}
.header-meta {
  text-align: right;
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 11px;
  color: var(--accent);
  line-height: 1.7;
}

/* ── Post form ── */
.post-form {
  background: var(--surface);
  border-bottom: 2px solid var(--border);
  padding: 14px 20px;
}

.form-row {
  display: flex;
  gap: 10px;
  align-items: flex-start;
  flex-wrap: wrap;
}

.field { display: flex; flex-direction: column; gap: 3px; }
.field label {
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 10px;
  letter-spacing: 1px;
  text-transform: uppercase;
  color: var(--ink-muted);
}
.field input,
.field textarea {
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  color: var(--ink);
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 14px;
  padding: 7px 10px;
  outline: none;
  transition: border-color .2s;
}
.field input:focus,
.field textarea:focus { border-color: var(--accent-dark); }
.field-name  { flex: 0 0 160px; }
.field-msg   { flex: 1 1 220px; }
.field textarea { resize: vertical; height: 90px; min-height: 60px; }
.char-hint {
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 10px;
  color: var(--ink-muted);
  text-align: right;
  height: 14px;
}
.char-hint.warn { color: var(--c-need-bar); }

/* Category buttons */
.field-cat { flex: 0 0 auto; }
.cat-btns {
  display: flex;
  gap: 5px;
  flex-wrap: wrap;
  padding-top: 1px;
}
.cat-btn {
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  color: var(--ink-muted);
  cursor: pointer;
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 11px;
  padding: 5px 10px;
  transition: all .15s;
  white-space: nowrap;
}
.cat-btn:hover { border-color: var(--border); color: var(--ink); background: var(--surface2); }
.cat-btn.active-Notice { background: var(--c-notice-bg); border-color: var(--c-notice-bar); color: var(--c-notice-fg); font-weight: bold; }
.cat-btn.active-Offer  { background: var(--c-offer-bg);  border-color: var(--c-offer-bar);  color: var(--c-offer-fg);  font-weight: bold; }
.cat-btn.active-Need   { background: var(--c-need-bg);   border-color: var(--c-need-bar);   color: var(--c-need-fg);   font-weight: bold; }
.cat-btn.active-Event  { background: var(--c-event-bg);  border-color: var(--c-event-bar);  color: var(--c-event-fg);  font-weight: bold; }

/* Expiry row */
.expiry-row {
  display: flex;
  gap: 6px;
  align-items: center;
  margin-top: 8px;
  flex-wrap: wrap;
}
.expiry-label {
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 10px;
  letter-spacing: 1px;
  text-transform: uppercase;
  color: var(--ink-muted);
  margin-right: 2px;
}
.exp-btn {
  background: var(--bg);
  border: 1px solid var(--border-light);
  border-radius: 99px;
  color: var(--ink-muted);
  cursor: pointer;
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 11px;
  padding: 3px 11px;
  transition: all .15s;
}
.exp-btn:hover  { border-color: var(--border); color: var(--ink); }
.exp-btn.active { background: var(--accent-dark); border-color: var(--accent-dark); color: var(--accent-light); }

.post-btn {
  background: var(--accent-dark);
  border: 2px solid #1d3a1b;
  border-radius: var(--radius);
  color: var(--accent-light);
  cursor: pointer;
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 12px;
  font-weight: bold;
  letter-spacing: 1px;
  padding: 8px 20px;
  margin-top: 8px;
  transition: background .15s;
  white-space: nowrap;
  align-self: flex-end;
}
.post-btn:hover { background: #3d6438; }

/* ── Filter bar ── */
.filter-bar {
  background: var(--surface2);
  border-bottom: 1px solid var(--border-light);
  padding: 9px 20px;
  display: flex;
  align-items: center;
  gap: 6px;
  flex-wrap: wrap;
}
.filter-label {
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 10px;
  letter-spacing: 1px;
  text-transform: uppercase;
  color: var(--ink-muted);
}
.sep { flex: 1; }
.ftab {
  background: none;
  border: 1px solid var(--border-light);
  border-radius: 99px;
  color: var(--ink-muted);
  cursor: pointer;
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 11px;
  padding: 3px 12px;
  transition: all .15s;
}
.ftab:hover  { border-color: var(--accent-dark); color: var(--accent-dark); }
.ftab.active { background: var(--accent-dark); border-color: var(--accent-dark); color: var(--accent-light); }

/* ── Board ── */
.board {
  flex: 1;
  padding: 16px 20px;
  columns: 3 260px;
  gap: 14px;
}

/* ── Card ── */
.card {
  display: inline-block;
  width: 100%;
  margin-bottom: 14px;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  box-shadow: var(--shadow);
  break-inside: avoid;
  overflow: hidden;
  animation: cardIn .2s ease;
}
@keyframes cardIn { from { opacity:0; transform:translateY(5px); } }
.card-stripe { height: 4px; }
.card-body { padding: 11px 13px 9px; }
.cat-badge {
  display: inline-block;
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 10px;
  font-weight: bold;
  letter-spacing: 1px;
  text-transform: uppercase;
  padding: 2px 9px;
  border-radius: 99px;
  margin-bottom: 7px;
}
.card-text { font-size: 14px; line-height: 1.6; color: var(--ink); word-break: break-word; }
.card-footer {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  padding: 7px 13px;
  background: var(--surface2);
  border-top: 1px solid var(--border-light);
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 11px;
  color: var(--ink-muted);
}
.card-author { font-weight: bold; color: var(--ink); }
.card-expiry { font-size: 10px; }
.card-expiry.soon { color: var(--c-need-bar); }

.type-Notice .card-stripe { background: var(--c-notice-bar); }
.type-Notice .cat-badge   { background: var(--c-notice-bg); color: var(--c-notice-fg); }
.type-Offer  .card-stripe { background: var(--c-offer-bar);  }
.type-Offer  .cat-badge   { background: var(--c-offer-bg);  color: var(--c-offer-fg);  }
.type-Need   .card-stripe { background: var(--c-need-bar);  }
.type-Need   .cat-badge   { background: var(--c-need-bg);   color: var(--c-need-fg);   }
.type-Event  .card-stripe { background: var(--c-event-bar); }
.type-Event  .cat-badge   { background: var(--c-event-bg);  color: var(--c-event-fg);  }

.empty {
  column-span: all;
  text-align: center;
  padding: 60px 20px;
  color: var(--ink-muted);
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 13px;
  line-height: 2.2;
}

/* ── Footer ── */
.site-footer {
  background: var(--surface2);
  border-top: 1px solid var(--border-light);
  padding: 7px 20px;
  display: flex;
  justify-content: space-between;
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 10px;
  color: var(--ink-muted);
}

@media (max-width: 540px) {
  .board { columns: 1; padding: 12px; }
  .form-row { flex-direction: column; }
  .field-name { flex: 1 1 auto; }
  .post-btn { width: 100%; text-align: center; }
}
</style>
</head>
<body>

<header class="site-header">
  <div class="header-title-block">
    <div class="site-title" id="boardTitle">COMMUNITY HUB</div>
    <div class="site-sub"   id="boardTagline"></div>
  </div>
  <div class="header-meta">
    <div id="boardRules"></div>
    <div id="postCount">— posts</div>
  </div>
</header>
<div id="fullBanner" style="display:none;background:#a30000;color:#fff;
     text-align:center;padding:10px 16px;font-weight:600">
  📋 This board is currently full. Check back once some posts have expired. 📋
</div>
<div class="post-form">
  <div class="form-row">

    <div class="field field-name">
      <label>Your Name</label>
      <input id="nameIn" maxlength="24" placeholder="neighbor"
             autocomplete="off" spellcheck="false">
      <div class="char-hint" id="nameHint"></div>
    </div>

    <div class="field field-cat">
      <label>Category</label>
      <div class="cat-btns">
        <button class="cat-btn" onclick="setType('Notice',this)">📌 Notice</button>
        <button class="cat-btn" onclick="setType('Offer', this)">🌱 Offer</button>
        <button class="cat-btn" onclick="setType('Need',  this)">🤝 Need</button>
        <button class="cat-btn" onclick="setType('Event', this)">📅 Event</button>
      </div>
      <div class="char-hint"></div>
    </div>

    <div class="field field-msg">
      <label>Message</label>
      <textarea id="msgIn" maxlength="300"
                placeholder="What's on the board?"
                spellcheck="false"></textarea>
      <div class="char-hint" id="msgHint"></div>
    </div>

  </div>

  <div class="expiry-row">
    <span class="expiry-label">Expires:</span>
    <button class="exp-btn" onclick="setExpiry(24,  this)">1 day</button>
    <button class="exp-btn" onclick="setExpiry(72,  this)">3 days</button>
    <button class="exp-btn active" onclick="setExpiry(168, this)">1 week</button>
    <button class="post-btn" id="postBtn" onclick="doPost()">POST</button>
  </div>
</div>

<div class="filter-bar">
  <span class="filter-label">Show:</span>
  <button class="ftab active" onclick="setFilter('',       this)">All</button>
  <button class="ftab"        onclick="setFilter('Notice', this)">📌 Notice</button>
  <button class="ftab"        onclick="setFilter('Offer',  this)">🌱 Offer</button>
  <button class="ftab"        onclick="setFilter('Need',   this)">🤝 Need</button>
  <button class="ftab"        onclick="setFilter('Event',  this)">📅 Event</button>
  <span class="sep"></span>
  <button class="ftab active" id="sNew" onclick="setSort('new', this)">New</button>
  <button class="ftab"        id="sExp" onclick="setSort('exp', this)">Expiring</button>
</div>

<div class="board" id="board"></div>

<footer class="site-footer">
  <span id="boardFooter"></span>
  <span id="uptimeDisplay">—</span>
</footer>

<script>
// First thing's first:
function checkBoardStatus() {
  fetch('/api/status')
    .then(r => r.json())
    .then(d => {
      const banner = document.getElementById('fullBanner');
      const btn    = document.getElementById('postBtn');
      if (d.full) {
        banner.style.display = 'block';
        if (btn) btn.disabled = true;
      } else {
        banner.style.display = 'none';
        if (btn) btn.disabled = false;
      }
    })
    .catch(() => {});
}
// ── XSS-safe escaping ──────────────────────────
function esc(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

function timeLeft(expSecs) {
  const now  = Math.floor(Date.now() / 1000);
  const left = expSecs - now;
  if (left <= 0)      return { label: 'expired',   soon: true  };
  if (left < 3600)    return { label: Math.floor(left / 60) + 'm left',   soon: true  };
  if (left < 86400)   return { label: Math.floor(left / 3600) + 'h left', soon: left < 10800 };
  return { label: Math.floor(left / 86400) + 'd left', soon: false };
}

// ── State ──────────────────────────────────────
let currentType   = 'Notice';
let currentExpiry = 168;
let activeFilter  = '';
let activeSort    = 'new';

// ── Restore name from localStorage ────────────
const savedName = localStorage.getItem('cn_name');
if (savedName) document.getElementById('nameIn').value = savedName;

document.getElementById('nameIn').addEventListener('input', function () {
  localStorage.setItem('cn_name', this.value);
  const h = document.getElementById('nameHint');
  const n = this.value.length;
  h.textContent = n > 18 ? n + '/24' : '';
  h.className   = 'char-hint' + (n > 20 ? ' warn' : '');
});

document.getElementById('msgIn').addEventListener('input', function () {
  const h = document.getElementById('msgHint');
  const n = this.value.length;
  h.textContent = n > 240 ? n + '/300' : '';
  h.className   = 'char-hint' + (n > 270 ? ' warn' : '');
});

document.getElementById('msgIn').addEventListener('keydown', function (e) {
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); doPost(); }
});

// ── Category selection ─────────────────────────
function setType(t, btn) {
  currentType = t;
  document.querySelectorAll('.cat-btn').forEach(b => {
    b.className = 'cat-btn';
  });
  btn.classList.add('active-' + t);
}
// Set default active on load
document.querySelector('.cat-btn').classList.add('active-Notice');

// ── Expiry selection ───────────────────────────
function setExpiry(h, btn) {
  currentExpiry = h;
  document.querySelectorAll('.exp-btn').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
}

// ── Filter / sort ──────────────────────────────
function setFilter(f, btn) {
  activeFilter = f;
  document.querySelectorAll('.filter-bar .ftab').forEach(b => {
    if (b.id === 'sNew' || b.id === 'sExp') return;
    b.classList.remove('active');
  });
  btn.classList.add('active');
  if (lastData.length) render(lastData);
}

function setSort(s, btn) {
  activeSort = s;
  document.getElementById('sNew').classList.toggle('active', s === 'new');
  document.getElementById('sExp').classList.toggle('active', s === 'exp');
  load();
}

// ── Data ───────────────────────────────────────
let lastData = [];

async function load() {
  try {
    checkBoardStatus();
    const r  = await fetch('/messages');
    lastData = await r.json();
    render(lastData);
    document.getElementById('lastUpdate').textContent =
      'Updated ' + new Date().toLocaleTimeString();
  } catch (_) {
    document.getElementById('lastUpdate').textContent = 'Connection lost';
  }
}

function render(data) {
  let filtered = activeFilter ? data.filter(m => m.type === activeFilter) : data;

  if (activeSort === 'exp') {
    filtered = [...filtered].sort((a, b) => a.expires - b.expires);
  }

  document.getElementById('postCount').textContent =
    data.length + ' post' + (data.length !== 1 ? 's' : '');

  const board = document.getElementById('board');

  if (filtered.length === 0) {
    board.innerHTML = '<div class="empty">Nothing here yet.<br>Be the first to post.</div>';
    return;
  }

  board.innerHTML = filtered.map(m => {
    const tl = timeLeft(m.expires);
    return `
<div class="card type-${esc(m.type)}">
  <div class="card-stripe"></div>
  <div class="card-body">
    <span class="cat-badge">${esc(m.type)}</span>
    <div class="card-text">${esc(m.text)}</div>
  </div>
  <div class="card-footer">
    <span class="card-author">${esc(m.author)}</span>
    <span class="card-expiry${tl.soon ? ' soon' : ''}">${tl.label}</span>
  </div>
</div>`;
  }).join('');
}

// ── Post ───────────────────────────────────────
async function doPost() {
  checkBoardStatus();
  const n = document.getElementById('nameIn').value.trim() || 'neighbor';
  const t = document.getElementById('msgIn').value.trim();
  if (!t) return;

  localStorage.setItem('cn_name', n);
  document.getElementById('nameIn').value = n;

  await fetch('/post', {
    method: 'POST',
    body: JSON.stringify({ author: n, type: currentType, text: t, expiry: currentExpiry })
  });

  document.getElementById('msgIn').value      = '';
  document.getElementById('msgHint').textContent = '';
  load();
}

// ── Board info ─────────────────────────────────
function loadInfo() {
  fetch('/info').then(r => r.json()).then(d => {
    document.getElementById('boardTitle').textContent   = d.icon + '  ' + d.name;
    document.getElementById('boardTagline').textContent = d.tagline;
    document.getElementById('boardRules').textContent   = d.rules;
    document.getElementById('boardFooter').textContent  = d.footer;
    document.getElementById('uptimeDisplay').textContent = d.uptime;
  });
}

setInterval(load,     60000);
setInterval(loadInfo, 60000);
load();
loadInfo();
checkBoardStatus();
</script>
</body>
</html>
)rawliteral";

// Escapes a string for safe injection into a JS double-quoted string literal.
String jsEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if      (c == '"')  out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else                out += c;
  }
  return out;
}

// ===================== HTML: ADMIN PANEL =====================
// The admin key is NEVER sent to the browser.
// The gate POSTs the key to /admin/auth which returns a session token.
// All subsequent admin calls use token= not key=.

String buildAdminPage() {
  String page = F(R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Admin — Community Hub</title>
<style>
:root {
  --bg: #ede8de; --surface: #faf8f3; --surface2: #f0ebe0;
  --border: #b0a080; --border-light: #d4c9b0;
  --ink: #2c2416; --ink-muted: #7a6a55;
  --accent-dark: #31502f; --accent-light: #c8ddc6;
  --danger: #c0392b; --radius: 6px;
}
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
body {
  background: var(--bg);
  color: var(--ink);
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  min-height: 100vh;
  display: flex;
  flex-direction: column;
}
.site-header {
  background: var(--accent-dark);
  padding: 14px 20px;
  border-bottom: 3px solid #1d3a1b;
}
.site-title {
  font-size: 18px; font-weight: bold;
  letter-spacing: 3px; color: #d8edcf;
}
.site-sub { font-size: 10px; letter-spacing: 2px; color: #8aad87; margin-top: 2px; }

/* ── Gate ── */
#gate {
  max-width: 360px;
  margin: 80px auto 0;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 28px 28px 24px;
  box-shadow: 3px 4px 0 rgba(44,36,22,.1);
}
#gate h2 {
  font-size: 13px; letter-spacing: 2px; text-transform: uppercase;
  color: var(--ink-muted); margin-bottom: 14px;
}
#gate input {
  width: 100%;
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  color: var(--ink);
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 14px;
  padding: 8px 12px;
  outline: none;
  margin-bottom: 10px;
}
#gate input:focus { border-color: var(--accent-dark); }
#gate button {
  width: 100%;
  background: var(--accent-dark);
  border: 2px solid #1d3a1b;
  border-radius: var(--radius);
  color: var(--accent-light);
  cursor: pointer;
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 12px; font-weight: bold;
  letter-spacing: 1px;
  padding: 9px;
}
#gate button:hover { background: #3d6438; }
#gate .error {
  font-size: 11px; color: var(--danger);
  margin-top: 8px; text-align: center;
  min-height: 16px;
}

/* ── Admin panel ── */
#panel { display: none; padding: 24px 20px; max-width: 640px; }
.section {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  margin-bottom: 18px;
  overflow: hidden;
}
.section-head {
  background: var(--surface2);
  border-bottom: 1px solid var(--border-light);
  padding: 8px 14px;
  font-size: 12px; letter-spacing: 1px;
  font-weight: bolder;
  text-transform: uppercase; color: var(--ink-muted);
}
.section-body { padding: 14px; display: flex; flex-direction: column; gap: 10px; }
.row { display: flex; gap: 8px; align-items: flex-end; flex-wrap: wrap; }
.fld { display: flex; flex-direction: column; gap: 3px; }
.fld label {
  font-size: 10px; letter-spacing: 1px;
  text-transform: uppercase; color: var(--ink-muted);
}
.fld input[type="text"],
.fld input[type="number"],
.fld input[type="datetime-local"] {
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  color: var(--ink);
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 13px;
  padding: 6px 10px;
  outline: none;
  width: 110px;
}
.fld input[type="text"],
.fld input[type="datetime-local"] { width: 200px; }
.fld input[type="number"] { width: 80px; }
.fld input:focus { border-color: var(--accent-dark); }

.fld input[type="range"] {
  width: 140px;
  accent-color: var(--accent-dark);
  margin-top: 4px;
}
.range-val { font-size: 12px; color: var(--ink); min-width: 28px; }

.btn {
  background: var(--accent-dark);
  border: 1px solid #1d3a1b;
  border-radius: var(--radius);
  color: var(--accent-light);
  cursor: pointer;
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 11px; font-weight: bold;
  letter-spacing: 1px;
  padding: 7px 14px;
  transition: background .15s;
  white-space: nowrap;
}
.btn:hover { background: #3d6438; }
.btn.danger {
  background: #8b1a10;
  border-color: #6b1208;
}
.btn.danger:hover { background: var(--danger); }

.feedback {
  font-size: 11px;
  color: var(--accent-dark);
  min-height: 16px;
}
textarea.restore-area {
  width: 100%;
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  color: var(--ink);
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  font-size: 11px;
  padding: 8px;
  resize: vertical;
  height: 120px;
  outline: none;
}
textarea.restore-area:focus { border-color: var(--accent-dark); }

/* ── Post list ── */
.post-list { display: flex; flex-direction: column; gap: 6px; margin-top: 4px; }
.post-row {
  display: flex;
  align-items: center;
  gap: 10px;
  background: var(--bg);
  border: 1px solid var(--border-light);
  border-radius: var(--radius);
  padding: 8px 10px;
  font-size: 12px;
}
.post-row-info { flex: 1; min-width: 0; }
.post-row-meta {
  font-size: 10px;
  color: var(--ink-muted);
  margin-bottom: 2px;
}
.post-row-text {
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  color: var(--ink);
}
.post-empty {
  font-size: 12px;
  color: var(--ink-muted);
  font-style: italic;
  padding: 8px 0;
}
</style>
</head>
<body>

<header class="site-header">
  <div class="site-title">COMMUNITY HUB</div>
  <div class="site-sub">ADMIN PANEL</div>
</header>

<!-- Password gate -->
<div id="gate">
  <h2>Admin Access</h2>
  <input type="password" id="keyIn" placeholder="Enter admin key"
         autocomplete="off">
  <button onclick="tryLogin()">Unlock 🔑</button>
  <div class="error" id="gateErr"></div>
</div>

<!-- Admin controls (hidden until authenticated) -->
<div id="panel">

  <div class="section">
    <div class="section-head">Admin Key</div>
    <div class="section-body">
      <div class="row">
        <div class="fld">
          <label>New Key (min 4 characters)</label>
          <input type="password" id="newKey" maxlength="64" autocomplete="new-password"
                 placeholder="Enter new key" style="width:200px">
        </div>
        <div class="fld">
          <label>Confirm</label>
          <input type="password" id="newKeyConfirm" maxlength="64"
                 placeholder="Confirm new key" style="width:200px">
        </div>
        <button class="btn" onclick="doSetKey()">Change Key</button>
      </div>
      <div class="feedback" id="keyFb"></div>
    </div>
  </div>

  <div class="section">
    <div class="section-head">Board Identity</div>
    <div class="section-body">
      <div class="row">
        <div class="fld" style="flex:0 0 60px">
          <label>Icon</label>
          <input type="text" id="idIcon" maxlength="4" style="width:60px;font-size:20px;text-align:center;padding:4px 6px;">
        </div>
        <div class="fld" style="flex:1 1 200px">
          <label>Board Name</label>
          <input type="text" id="idName" maxlength="48" style="width:100%">
        </div>
      </div>
      <div class="fld" style="flex:1">
        <label>Tagline</label>
        <input type="text" id="idTagline" maxlength="80" style="width:100%">
      </div>
      <div class="fld" style="flex:1">
        <label>Rules</label>
        <input type="text" id="idRules" maxlength="80" style="width:100%">
      </div>
      <div class="fld" style="flex:1">
        <label>Footer</label>
        <input type="text" id="idFooter" maxlength="80" style="width:100%">
      </div>
      <div class="row">
        <button class="btn" onclick="doIdentity()">Save Identity</button>
      </div>
      <div class="feedback" id="idFb"></div>
    </div>
  </div>

  <div class="section">
    <div class="section-head">Set Time</div>
    <div class="section-body">
      <div class="row">
        <div class="fld">
          <label>Date &amp; Time</label>
          <input type="datetime-local" id="timeIn">
        </div>
        <button class="btn" onclick="doTime()">Set Time</button>
      </div>
      <div class="feedback" id="timeFb"></div>
    </div>
  </div>

  <div class="section">
  <div class="section-head">LED Settings</div>
  <div class="section-body">
    <div class="row">
      <div class="fld">
        <label>Day Brightness (0–100)</label>
        <input type="range" id="ledDayBr" min="0" max="100" value="80"
               oninput="document.getElementById('ledDayBrVal').textContent=this.value">
        <span class="range-val" id="ledDayBrVal">80</span>
      </div>
      <div class="fld">
        <label>Night Brightness (0–100)</label>
        <input type="range" id="ledNightBr" min="0" max="100" value="20"
               oninput="document.getElementById('ledNightBrVal').textContent=this.value">
        <span class="range-val" id="ledNightBrVal">20</span>
      </div>
    </div>
    <div class="row">
      <div class="fld">
        <label>Day Start (hour 0-23)</label>
        <input type="number" id="ledDayStart" min="0" max="23" value="7">
      </div>
      <div class="fld">
        <label>Night Start (hour 0-23)</label>
        <input type="number" id="ledNightStart" min="0" max="23" value="20">
      </div>
      <div class="fld">
        <label>GPIO Pin</label>
        <input type="number" id="ledPin" min="0" max="48" value="4" style="width:70px">
      </div>
    </div>
    <div class="row" style="gap:18px;margin-top:4px">
      <label style="display:flex;align-items:center;gap:6px;font-size:13px;cursor:pointer">
        <input type="checkbox" id="ledEnabled"  onchange="updateLedToggles()"> LED Enabled
      </label>
      <label style="display:flex;align-items:center;gap:6px;font-size:13px;cursor:pointer">
        <input type="checkbox" id="ledPulse"    onchange="updateLedToggles()"> Pulsing
      </label>
      <label style="display:flex;align-items:center;gap:6px;font-size:13px;cursor:pointer">
        <input type="checkbox" id="ledActivity"> Activity Mode
      </label>
    </div>
    <div class="row">
      <button class="btn" onclick="doLed()">Save LED</button>
    </div>
    <div class="feedback" id="ledFb"></div>
  </div>
</div>

  <div class="section">
    <div class="section-head">Manage Posts</div>
    <div class="section-body">
      <div class="row">
        <button class="btn" onclick="loadPostList()">Refresh List</button>
        <button class="btn danger" onclick="confirmClear()">Clear All Posts</button>
        <button class="btn"        onclick="doAction('/admin/flush',  'backupFb', false)">Force Save</button>
      </div>
      <div id="postListFb" class="feedback"></div>
      <div id="postList"></div>
    </div>
  </div>

  <div class="section">
    <div class="section-head">Backups</div>
    <div class="section-body">
      <div class="row">
        <button class="btn"        onclick="doAction('/admin/backup', 'backupFb', true)">Download Backup</button>
      </div>
      <div class="feedback" id="backupFb"></div>
      <div class="fld"><label>Restore Backup:</label></div>
      <textarea class="restore-area" id="restoreIn"
                placeholder="Paste backup JSON here…"></textarea>
      <div class="row">
        <button class="btn" onclick="doRestore()">Restore</button>
      </div>
      <div class="feedback" id="restoreFb"></div>
    </div>
  </div>

  <div class="section">
    <div class="section-head">Firmware Update (OTA)</div>
    <div class="section-body">
      <div class="fld">
        <label>Binary (.bin file)</label>
        <input type="file" id="otaFile" accept=".bin"
               style="font-size:12px;color:var(--ink)">
      </div>
      <div class="row" style="margin-top:4px">
        <button class="btn danger" onclick="doOTA()">Upload &amp; Reboot</button>
      </div>
      <div id="otaProgress" style="display:none;margin-top:8px">
        <div style="background:var(--border-light);border-radius:99px;height:8px;overflow:hidden">
          <div id="otaBar" style="background:var(--accent-dark);height:100%;width:0%;transition:width .2s"></div>
        </div>
        <div class="feedback" id="otaFb" style="margin-top:6px"></div>
      </div>
    </div>
  </div>

</div><!-- #panel -->

<script>
// ── Key is injected server-side ─────────────────────────────────────────────
)rawliteral");

  // No key is ever sent to the browser.
  // Authentication is done by POSTing to /admin/auth which returns a session token.
  page += "let SESSION_TOKEN = '';\n";

  // Pre-fill identity fields with current runtime values (safely escaped)
  page += "window.addEventListener('DOMContentLoaded', () => {\n";
  page += "  document.getElementById('idName').value    = \"" + jsEscape(id_name)    + "\";\n";
  page += "  document.getElementById('idIcon').value    = \"" + jsEscape(id_icon)    + "\";\n";
  page += "  document.getElementById('idTagline').value = \"" + jsEscape(id_tagline) + "\";\n";
  page += "  document.getElementById('idRules').value   = \"" + jsEscape(id_rules)   + "\";\n";
  page += "  document.getElementById('idFooter').value  = \"" + jsEscape(id_footer)  + "\";\n";
  page += "});\n";

  page += F(R"rawliteral(
// ── Gate ────────────────────────────────────────────────────────────────────
function tryLogin() {
  const val = document.getElementById('keyIn').value;
  if (!val) return;

  fetch('/admin/auth', {
    method: 'POST',
    body: JSON.stringify({ key: val })
  })
  .then(r => {
    if (!r.ok) throw new Error('forbidden');
    return r.text();
  })
  .then(token => {
    SESSION_TOKEN = token;
    document.getElementById('gate').style.display  = 'none';
    document.getElementById('panel').style.display = 'block';
    const now = new Date();
    now.setSeconds(0, 0);
    document.getElementById('timeIn').value = now.toISOString().slice(0, 16);
    loadLedValues();
    loadPostList();
  })
  .catch(() => {
    document.getElementById('gateErr').textContent = 'Incorrect key.';
  });
}
document.getElementById('keyIn').addEventListener('keydown', e => {
  if (e.key === 'Enter') tryLogin();
});

// ── Helpers ──────────────────────────────────────────────────────────────────
function api(path) {
  return path + (path.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(SESSION_TOKEN);
}

function apiFetch(url, options) {
  return fetch(url, options).then(r => {
    if (r.status === 403) {
      SESSION_TOKEN = '';
      document.getElementById('panel').style.display = 'none';
      document.getElementById('gate').style.display  = 'block';
      document.getElementById('gateErr').textContent = 'Session expired. Please log in again.';
      document.getElementById('keyIn').value = '';
      throw new Error('session expired');
    }
    return r;
  });
}

function fb(id, msg) {
  const el = document.getElementById(id);
  el.textContent = msg;
  setTimeout(() => { el.textContent = ''; }, 4000);
}

// ── Change admin key ──────────────────────────────────────────────────────────
function doSetKey() {
  const n = document.getElementById('newKey').value;
  const c = document.getElementById('newKeyConfirm').value;
  if (n.length < 4)  { fb('keyFb', '✗ Key must be at least 4 characters'); return; }
  if (n !== c)       { fb('keyFb', '✗ Keys do not match'); return; }
  apiFetch(api('/admin/setkey') + '&newkey=' + encodeURIComponent(n))
    .then(r => r.text())
    .then(msg => {
      fb('keyFb', '✓ ' + msg);
      // Reload after short delay so the page re-fetches with the new injected key
      setTimeout(() => location.reload(), 1500);
    })
    .catch(() => fb('keyFb', '✗ Request failed'));
}

// ── Identity ──────────────────────────────────────────────────────────────────
function doIdentity() {
  const params = new URLSearchParams({
    name:    document.getElementById('idName').value.trim(),
    icon:    document.getElementById('idIcon').value.trim(),
    tagline: document.getElementById('idTagline').value.trim(),
    rules:   document.getElementById('idRules').value.trim(),
    footer:  document.getElementById('idFooter').value.trim()
  });
  apiFetch(api('/admin/identity/set') + '&' + params.toString())
    .then(r => r.text())
    .then(msg => fb('idFb', '✓ ' + msg))
    .catch(() => fb('idFb', '✗ Request failed'));
}

// ── Time ─────────────────────────────────────────────────────────────────────
function doTime() {
  const raw = document.getElementById('timeIn').value; // "2026-04-22T14:30"
  if (!raw) { fb('timeFb', '✗ Please pick a date and time'); return; }
  const [date, time] = raw.split('T');
  const [y, m, d]   = date.split('-');
  const formatted   = d + m + y + '-' + time.replace(':', '');
  apiFetch(api('/admin/time') + '&time=' + formatted)
    .then(r => r.text())
    .then(msg => fb('timeFb', '✓ ' + msg))
    .catch(() => fb('timeFb', '✗ Request failed'));
}

// ── LED ──────────────────────────────────────────────────────────────────────
function loadLedValues() {
  apiFetch(api('/admin/led/get'))
    .then(r => r.json())
    .then(d => {
      document.getElementById('ledDayBr').value              = d.day_br;
      document.getElementById('ledDayBrVal').textContent     = d.day_br;
      document.getElementById('ledNightBr').value            = d.night_br;
      document.getElementById('ledNightBrVal').textContent   = d.night_br;
      document.getElementById('ledDayStart').value           = d.day_st;
      document.getElementById('ledNightStart').value         = d.night_st;
      document.getElementById('ledPin').value                = d.pin;
      document.getElementById('ledEnabled').checked          = d.enabled;
      document.getElementById('ledPulse').checked            = d.pulse;
      document.getElementById('ledActivity').checked         = d.activity;
      updateLedToggles();
    })
    .catch(() => {});
}

function updateLedToggles() {
  const enabled = document.getElementById('ledEnabled').checked;
  const pulse   = document.getElementById('ledPulse').checked;
  document.getElementById('ledPulse').disabled    = !enabled;
  document.getElementById('ledActivity').disabled = !enabled || !pulse;
}

function doLed() {
  const params = new URLSearchParams({
    day_br:   document.getElementById('ledDayBr').value,
    night_br: document.getElementById('ledNightBr').value,
    day_st:   document.getElementById('ledDayStart').value,
    night_st: document.getElementById('ledNightStart').value,
    pin:      document.getElementById('ledPin').value,
    enabled:  document.getElementById('ledEnabled').checked  ? '1' : '0',
    pulse:    document.getElementById('ledPulse').checked    ? '1' : '0',
    activity: document.getElementById('ledActivity').checked ? '1' : '0'
  });
  apiFetch(api('/admin/led/set') + '&' + params.toString())
    .then(r => r.text())
    .then(msg => fb('ledFb', '✓ ' + msg))
    .catch(() => fb('ledFb', '✗ Request failed'));
}

// ── Board actions ─────────────────────────────────────────────────────────────
async function doAction(path, fbId, isDownload) {
  const r   = await apiFetch(api(path));
  const txt = await r.text();
  if (isDownload) {
    const a = document.createElement('a');
    a.href     = 'data:application/json,' + encodeURIComponent(txt);
    a.download = 'community_hub_backup.json';
    a.click();
    fb(fbId, '✓ Download started');
  } else {
    fb(fbId, '✓ ' + txt);
  }
}

function confirmClear() {
  if (!confirm('Delete ALL posts? This cannot be undone.')) return;
  apiFetch(api('/admin/clear'))
    .then(r => r.text())
    .then(msg => fb('backupFb', '✓ ' + msg))
    .catch(() => fb('backupFb', '✗ Failed'));
}

// ── Manage Posts ──────────────────────────────────────────────────────────────
function esc(s) {
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function timeLeftShort(exp) {
  const left = exp - Math.floor(Date.now() / 1000);
  if (left <= 0)     return 'expired';
  if (left < 3600)   return Math.floor(left/60) + 'm';
  if (left < 86400)  return Math.floor(left/3600) + 'h';
  return Math.floor(left/86400) + 'd';
}

async function loadPostList() {
  const container = document.getElementById('postList');
  container.innerHTML = '<div class="post-empty">Loading…</div>';
  try {
    const r    = await fetch('/messages');
    const data = await r.json();
    if (data.length === 0) {
      container.innerHTML = '<div class="post-empty">No active posts.</div>';
      return;
    }
    container.innerHTML = '<div class="post-list">' +
      data.map(m => `
<div class="post-row" id="pr-${m.id}">
  <div class="post-row-info">
    <div class="post-row-meta">${esc(m.type)} · ${esc(m.author)} · ${timeLeftShort(m.expires)} left</div>
    <div class="post-row-text">${esc(m.text)}</div>
  </div>
  <button class="btn danger" onclick="deletePost(${m.id})">Delete</button>
</div>`).join('') +
    '</div>';
  } catch (_) {
    container.innerHTML = '<div class="post-empty">Failed to load posts.</div>';
  }
}

async function deletePost(id) {
  if (!confirm('Delete this post?')) return;
  const r = await apiFetch(api('/admin/delete/post') + '&id=' + id);
  if (r.ok) {
    const row = document.getElementById('pr-' + id);
    if (row) row.remove();
    fb('postListFb', '✓ Post deleted');
  } else {
    fb('postListFb', '✗ Delete failed');
  }
}

// ── OTA ───────────────────────────────────────────────────────────────────────
function doOTA() {
  const fileInput = document.getElementById('otaFile');
  if (!fileInput.files.length) {
    fb('otaFb', '✗ Please select a .bin file first');
    document.getElementById('otaProgress').style.display = 'block';
    return;
  }
  const file = fileInput.files[0];
  if (!file.name.endsWith('.bin')) {
    fb('otaFb', '✗ File must be a .bin firmware file');
    document.getElementById('otaProgress').style.display = 'block';
    return;
  }
  if (!confirm('Upload ' + file.name + ' and reboot?\n\nDo not close this page until complete.')) return;

  document.getElementById('otaProgress').style.display = 'block';
  document.getElementById('otaBar').style.width = '0%';
  fb('otaFb', 'Uploading…');

  const formData = new FormData();
  formData.append('firmware', file);

  const xhr = new XMLHttpRequest();
  xhr.open('POST', api('/admin/ota'));

  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      const pct = Math.round((e.loaded / e.total) * 100);
      document.getElementById('otaBar').style.width = pct + '%';
      fb('otaFb', 'Uploading… ' + pct + '%');
    }
  };

  xhr.onload = () => {
    document.getElementById('otaBar').style.width = '100%';
    if (xhr.status === 200) {
      fb('otaFb', '✓ ' + xhr.responseText + ' — connection will drop shortly');
    } else {
      fb('otaFb', '✗ Upload failed: ' + xhr.responseText);
    }
  };

  xhr.onerror = () => fb('otaFb', '✗ Connection lost — board may be rebooting');

  xhr.send(formData);
}

// ── Restore ───────────────────────────────────────────────────────────────────
function doRestore() {
  const body = document.getElementById('restoreIn').value.trim();
  if (!body) return;
  apiFetch(api('/admin/restore'), { method: 'POST', body })
    .then(r => r.text())
    .then(msg => fb('restoreFb', '✓ ' + msg))
    .catch(() => fb('restoreFb', '✗ Failed'));
}
</script>
</body>
</html>
)rawliteral");

  return page;
}


// ===================== WEB SERVER =====================
DNSServer dnsServer;
WebServer server(80);

bool checkKey() { //WOTS DA PASSWARD?
  if (sessionToken.length() == 0)                      return false;
  if (millis() - tokenIssuedAt > TOKEN_LIFETIME_MS)    return false;
  if (!server.hasArg("token"))                         return false;
  return server.arg("token") == sessionToken;
}

// Strip angle brackets and trim whitespace to prevent HTML injection.
// Applied to all user-supplied text before storage.
// Because users are hostile, whether they mean to be or not.
String sanitize(const String& s, int maxLen) {
  String out;
  out.reserve(s.length());
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c != '<' && c != '>') out += c;
  }
  out.trim();
  if ((int)out.length() > maxLen) out = out.substring(0, maxLen);
  return out;
}

// Accept only the four known post types; fall back to "Notice".
String validateType(const String& t) {
  if (t == "Notice" || t == "Offer" || t == "Need" || t == "Event") return t;
  return "Notice";
}

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleAdmin() {
  server.send(200, "text/html; charset=utf-8", buildAdminPage());
}

void handleAdminAuth() {
  // Key submitted via POST body as JSON: {"key":"..."}
  // Never echoed back — only a token is returned on success.
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "bad request");
    return;
  }
  String submitted = doc["key"] | "";
  if (submitted == adminKey) {
    sessionToken = generateToken();
    server.send(200, "text/plain", sessionToken);
  } else {
    server.send(403, "text/plain", "forbidden");
  }
}

void handleInfo() {
  DynamicJsonDocument doc(512);
  doc["name"]    = id_name;
  doc["icon"]    = id_icon;
  doc["tagline"] = id_tagline;
  doc["rules"]   = id_rules;
  doc["footer"]  = id_footer;
  doc["uptime"]  = formatUptime();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleMessages() {
  DynamicJsonDocument doc(8192);
  JsonArray arr = doc.to<JsonArray>();
  unsigned long now = nowSecs();
  for (int i = 0; i < msgCount; i++) {
    if (msgs[i].expires < now) continue;
    JsonObject o = arr.createNestedObject();
    o["id"]      = msgs[i].id;
    o["author"]  = msgs[i].author;
    o["type"]    = msgs[i].type;
    o["text"]    = msgs[i].text;
    o["expires"] = msgs[i].expires;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handlePost() {
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "bad json");
    return;
  }
  String author = sanitize(doc["author"] | "neighbor", 24); // Won't you be my neighbor?
  String type   = validateType(doc["type"] | "Notice");
  String text   = sanitize(doc["text"]   | "", 300);
  int    expiry = doc["expiry"] | Config::DEFAULT_EXPIRY_HOURS;

  if (author.isEmpty()) author = "neighbor";
  if (text.isEmpty())   { server.send(400, "text/plain", "empty message"); return; }

  addMessage(author, type, text, expiry);
  server.send(200, "text/plain", "ok");
}

// ── Admin handlers ────────────────────────────────────────────────────────────

void handleAdminIdentityGet() {
  if (!checkKey()) { server.send(403, "text/plain", "forbidden"); return; }
  DynamicJsonDocument doc(1024);
  doc["name"]    = id_name;
  doc["icon"]    = id_icon;
  doc["tagline"] = id_tagline;
  doc["rules"]   = id_rules;
  doc["footer"]  = id_footer;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleAdminIdentitySet() {
  if (!checkKey()) { server.send(403, "text/plain", "forbidden"); return; }
  if (server.hasArg("name")    && server.arg("name").length())
    id_name    = sanitize(server.arg("name"),    48);
  if (server.hasArg("icon")    && server.arg("icon").length())
    id_icon    = sanitize(server.arg("icon"),     8);
  if (server.hasArg("tagline"))
    id_tagline = sanitize(server.arg("tagline"), 100);
  if (server.hasArg("rules"))
    id_rules   = sanitize(server.arg("rules"),   100);
  if (server.hasArg("footer"))
    id_footer  = sanitize(server.arg("footer"),  100);
  saveIdentityConfig();
  server.send(200, "text/plain", "identity saved");
}

void handleAdminTime() {
  if (!checkKey()) { server.send(403, "text/plain", "forbidden"); return; }
  if (!setTimeFromString(server.arg("time"))) {
    server.send(400, "text/plain", "bad format — use DDMMYYYY-HHMM");
    return;
  }
  saveTime();
  server.send(200, "text/plain", "time set");
}
//bored
void handleAdminLedGet() {
  if (!checkKey()) { server.send(403, "text/plain", "forbidden"); return; }
  DynamicJsonDocument doc(512);
  doc["day_br"]   = led_day_brightness;
  doc["night_br"] = led_night_brightness;
  doc["day_st"]   = led_day_start;
  doc["night_st"] = led_night_start;
  doc["pin"]      = led_pin;
  doc["enabled"]  = led_enabled;
  doc["pulse"]    = led_pulse_enabled;
  doc["activity"] = led_activity_enabled;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}
//so bored
void handleAdminLedSet() {
  if (!checkKey()) { server.send(403, "text/plain", "forbidden"); return; }
  if (server.hasArg("day_br"))   led_day_brightness   = constrain(server.arg("day_br").toInt(),   0, 100);
  if (server.hasArg("night_br")) led_night_brightness = constrain(server.arg("night_br").toInt(), 0, 100);
  if (server.hasArg("day_st"))   led_day_start        = constrain(server.arg("day_st").toInt(),   0, 23);
  if (server.hasArg("night_st")) led_night_start      = constrain(server.arg("night_st").toInt(), 0, 23);
  if (server.hasArg("pin")) {
    int newPin = constrain(server.arg("pin").toInt(), 0, 48);
    if (newPin != led_pin) {
      analogWrite(led_pin, 0);
      pinMode(led_pin, INPUT);
      led_pin = newPin;
      pinMode(led_pin, OUTPUT);
    }
  }
  if (server.hasArg("enabled"))  led_enabled          = server.arg("enabled")  == "1";
  if (server.hasArg("pulse"))    led_pulse_enabled    = server.arg("pulse")    == "1";
  if (server.hasArg("activity")) led_activity_enabled = server.arg("activity") == "1";
  saveLedConfig();
  server.send(200, "text/plain", "LED settings saved");
}

void handleAdminBackup() { // C'mon shawty, back that data up!
  if (!checkKey()) { server.send(403, "text/plain", "forbidden"); return; }
  if (msgsDirty) { saveMessages(); msgsDirty = false; }
  File f = LittleFS.open(Config::STORAGE_FILE);
  if (!f) { server.send(500, "text/plain", "no file"); return; }
  String out = f.readString();
  f.close();
  server.send(200, "application/json", out);
}

void handleAdminRestore() {
  if (!checkKey()) { server.send(403, "text/plain", "forbidden"); return; }
  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "bad json");
    return;
  }
  msgCount = 0;
  for (JsonObject o : doc.as<JsonArray>()) {
    if (msgCount >= Config::MAX_MSGS) break;
    msgs[msgCount].author  = (const char*)o["author"];
    msgs[msgCount].type    = (const char*)o["type"];
    msgs[msgCount].text    = (const char*)o["text"];
    msgs[msgCount].expires = o["expires"];
    msgCount++;
  }
  saveMessages();
  server.send(200, "text/plain", "restored " + String(msgCount) + " messages");
}

void handleAdminSetKey() {
  if (!checkKey()) { server.send(403, "text/plain", "forbidden"); return; }
  String newKey = server.arg("newkey");
  newKey.trim();
  if (newKey.length() < 4) {
    server.send(400, "text/plain", "key must be at least 4 characters"); // Ugh, size queen
    return;
  }
  adminKey = newKey;
  saveAdminKey();
  server.send(200, "text/plain", "key updated — page will reload");
}

void handleAdminOTA() {
  if (!checkKey()) { server.send(403, "text/plain", "forbidden"); return; }
  server.send(200, "text/plain", Update.hasError() ? "UPDATE FAILED" : "UPDATE OK — rebooting");
  delay(500);
  ESP.restart();
}

void handleAdminOTAUpload() {
  if (!checkKey()) return;
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] Starting: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
    Serial.printf("[OTA] Written %u bytes\n", upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[OTA] Success: %u bytes total\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

void handleAdminFlush() {
  if (!checkKey()) { server.send(403, "text/plain", "forbidden"); return; }
  saveMessages();
  msgsDirty = false;
  saveTime();
  server.send(200, "text/plain", "flushed");
}

void handleAdminDeletePost() {
  if (!checkKey()) { server.send(403, "text/plain", "forbidden"); return; }
  if (!server.hasArg("id")) { server.send(400, "text/plain", "missing id"); return; }
  uint16_t targetId = (uint16_t)server.arg("id").toInt();
  for (int i = 0; i < msgCount; i++) {
    if (msgs[i].id == targetId) {
      // Shift remaining messages down to fill the gap
      for (int j = i; j < msgCount - 1; j++) msgs[j] = msgs[j + 1];
      msgCount--;
      if (!msgsDirty) { msgsDirty = true; lastMsgDirtyTime = millis(); }
      server.send(200, "text/plain", "deleted");
      return;
    }
  }
  server.send(404, "text/plain", "not found");
}

void handleAdminClear() {
  if (!checkKey()) { server.send(403, "text/plain", "forbidden"); return; }
  msgCount = 0;
  saveMessages();
  server.send(200, "text/plain", "cleared");
}

// ===================== SETUP =====================
void setup() {
  bootMillis = millis();
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("╔══════════════════════════════╗");
  Serial.println("║      C O M M U N I T Y       ║");
  Serial.println("║            H U B             ║");
  Serial.println("╚══════════════════════════════╝");

  pinMode(led_pin, OUTPUT);

  // ── WiFi Access Point ──
  WiFi.mode(WIFI_AP);
  WiFi.softAP(Config::AP_SSID, Config::AP_PASS[0] ? Config::AP_PASS : nullptr,
              Config::AP_CHANNEL, 0, Config::AP_MAX_CONN);
  delay(200); // softAP needs a moment to settle

  IPAddress apIP = WiFi.softAPIP();
  Serial.println("✓ Access Point started.");
  Serial.print("  SSID : "); Serial.println(Config::AP_SSID);
  Serial.print("  IP   : "); Serial.println(apIP);
  Serial.print("  Pass : "); Serial.println(Config::AP_PASS[0] ? Config::AP_PASS : "(open)");
  Serial.print("  Admin: http://"); Serial.print(apIP); Serial.println("/admin");

  // ── DNS — redirect every hostname to us ──
  dnsServer.start(53, "*", apIP);

  // ── LittleFS ──
  if (!LittleFS.begin(true)) {
    Serial.println("⚠  LittleFS init failed — running without persistence."); // if this fails, we got problems.
  } else {
    Serial.println("✓ LittleFS mounted.");
    loadTime();
    loadLedConfig();
    loadAdminKey();
    loadIdentityConfig();
    loadMessages();
    Serial.printf("  Loaded %d message(s).\n", msgCount);
  }

  // ── Routes ──
  server.on("/",                   handleRoot);
  server.on("/admin",              handleAdmin);
  server.on("/admin/auth", HTTP_POST, handleAdminAuth);
  server.on("/info",               handleInfo);
  server.on("/messages",           handleMessages);
  server.on("/post",   HTTP_POST,  handlePost);
  server.on("/api/status", HTTP_GET, []() {
    unsigned long now = nowSecs();
    bool hasExpired = false;
    for (int i = 0; i < msgCount; i++) {
      if (msgs[i].expires <= now) { hasExpired = true; break; }
    }
    bool boardFull = (msgCount >= Config::MAX_MSGS) && !hasExpired;
    DynamicJsonDocument doc(64);
    doc["full"] = boardFull;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // ── Captive portal detection endpoints ──────────────────────────────────────
  // DNS resolves ALL hostnames to our IP, so only the path matters.
  // We redirect each known probe URL to "/" to trigger the portal popup.
  // Even with all this, doesn't always work. Samsung devices are especially persnickety.

  // Apple (iOS / macOS)
  server.on("/hotspot-detect.html",       []() { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/library/test/success.html", []() { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/success.html",              []() { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/captive.apple.com",         []() { server.sendHeader("Location", "/"); server.send(302); });

  // Android / Google
  server.on("/generate_204",              []() { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/gen_204",                   []() { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/connectivitycheck",         []() { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/connectivity-check",        []() { server.sendHeader("Location", "/"); server.send(302); });

  // Windows (NCSI + connecttest)
  server.on("/connecttest.txt",           []() { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/ncsi.txt",                  []() { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/redirect",                  []() { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/fwlink/",                   []() { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/fwlink",                    []() { server.sendHeader("Location", "/"); server.send(302); });

  // Firefox browser
  server.on("/success.txt",               []() { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/canonical.html",            []() { server.sendHeader("Location", "/"); server.send(302); });

  server.on("/admin/identity/get",  handleAdminIdentityGet);
  server.on("/admin/identity/set",  handleAdminIdentitySet);
  server.on("/admin/time",          handleAdminTime);
  server.on("/admin/led/get",       handleAdminLedGet);
  server.on("/admin/led/set",       handleAdminLedSet);
  server.on("/admin/backup",        handleAdminBackup);
  server.on("/admin/restore", HTTP_POST, handleAdminRestore);
  server.on("/admin/setkey",        handleAdminSetKey);
  server.on("/admin/ota", HTTP_POST, handleAdminOTA, handleAdminOTAUpload);
  server.on("/admin/flush",         handleAdminFlush);
  server.on("/admin/clear",         handleAdminClear);
  server.on("/admin/delete/post",   handleAdminDeletePost);

  // Catch-all: redirect everything else to the board (required for captive portal)
  server.onNotFound([]() { server.sendHeader("Location", "/"); server.send(302); });

  server.begin();
  Serial.println("✓ HTTP server started.\n");
}

// ===================== LOOP =====================
void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  updateLED();

  unsigned long now = millis();

  if (msgsDirty && (now - lastMsgDirtyTime) >= 60000) {
    saveMessages();
    msgsDirty = false;
  }

  if (now - lastTimeSave > 1800000) {
    saveTime();
    lastTimeSave = now;
  }
  
}