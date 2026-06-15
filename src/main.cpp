/*
 * MyTides — tide display for Swansboro, NC on CYD (ESP32-2432S028)
 *
 * HARDWARE
 *   Board : ESP32-2432S028 (Cheap Yellow Display)
 *   Screen: 2.8" ILI9341V SPI TFT, 320x240 landscape
 *
 *   Display SPI  CLK=14  MISO=12  MOSI=13  CS=15  DC=2  BL=21
 *   Touch SPI    CLK=25  MISO=39  MOSI=32  CS=33        IRQ=36 (unused)
 *
 *   IMPORTANT: touch is on a COMPLETELY SEPARATE SPI bus from the display.
 *   Using display pins (14/12/13) for touch produces zero response.
 *   Touch uses SPIClass(HSPI) on pins 25/39/32/33.
 *
 * TFT_eSPI (build_flags)
 *   -DUSER_SETUP_LOADED   required — suppresses default User_Setup.h
 *   -DILI9341_2_DRIVER    must be _2_ variant (not ILI9341_DRIVER); it's the V display
 *   -DSPI_FREQUENCY=27000000  55 MHz causes white screen on this unit
 *   delay(100) after tft.init() is required before any drawing
 *   After changing build_flags always run PlatformIO Clean before rebuilding
 *
 * TOUCH DRIVER
 *   Bare-metal XPT2046 over SPIClass(HSPI). TFT_eSPI built-in touch and the
 *   paulstoffregen/XPT2046_Touchscreen library both failed (wrong SPI bus assumed).
 *   XPT2046 commands: 0xD0=X  0x90=Y  0xB0=Z1 pressure
 *   Landscape mapping: raw-Y (inverted) -> screen-X, raw-X -> screen-Y
 *   Adjust TOUCH_X_MIN/MAX and TOUCH_Y_MIN/MAX if taps are consistently off.
 *
 * CONFIG PORTAL
 *   Hold BOOT (GPIO 0) on power-up -> AP "MyTides-Config" -> browser 192.168.4.1
 *   Station search: tidesandcurrents.noaa.gov/mdapi/latest/webapi/tidepredstations.json?q=<query>
 *   NVS namespace "mytides": ssid, pass, station_id, station_name, rotation
 *
 * NOAA TIDE API
 *   api.tidesandcurrents.noaa.gov/api/prod/datagetter?product=predictions&...
 *   Subordinate stations (e.g. Swansboro 8656613) reject the datum parameter.
 *   Code tries MLLW->STND->MSL->MTL->IGLD->MHHW->"" (empty = omit datum).
 *
 * TIME
 *   configTime(0,0,"pool.ntp.org") + setenv("TZ","EST5EDT,M3.2.0,M11.1.0",1)
 *   POSIX TZ string handles DST automatically; fixed UTC offsets do not.
 *
 * GEAR ICON / ROTATION
 *   Tapping gear (upper-right corner) opens rotation config screen.
 *   Choice saved to NVS key "rotation" and applied on next boot.
 */

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

// CYD touch SPI is on a SEPARATE bus from the display
#define TOUCH_CLK  25
#define TOUCH_MISO 39
#define TOUCH_MOSI 32
#define TOUCH_CS   33

// Raw XPT2046 ADC ranges for CYD 2.8"
// For landscape (rotation 1): raw Y axis → screen X, raw X axis (inverted) → screen Y
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3900
#define TOUCH_Y_MIN 200
#define TOUCH_Y_MAX 3900

SPIClass touchSPI(HSPI);

#define TFT_BL_PIN  21
#define CONFIG_BTN   0

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;
WebServer   server(80);
DNSServer   dnsServer;

// Config portal state
bool   portalWifiConnected = false;
String tempSSID, tempPass, connectError;

// Saved config (loaded from NVS on normal boot)
String cfgSSID, cfgPass, cfgStationId, cfgStationName;
uint8_t cfgRotation = 1;
bool    cfgWifiEnabled = true;

unsigned long lastFetchMs = 0;

const char* ntpServer = "pool.ntp.org";
const char* timezone  = "EST5EDT,M3.2.0,M11.1.0";

// Tide data
JsonDocument tideDoc;
JsonArray    predictions;
float        minTide, maxTide;
String       highTideEvents[2];
String       lowTideEvents[2];
int          cachedHourOffset = -9999;
int          cachedTodayDoy  = -1;
int          hourOffset      = 0;  // hours from today's midnight; swipe shifts by 1
static int16_t drawPred[500];      // tide heights ×100 for offline-safe chart drawing
static int     drawPredCount = 0;
static int     drawPredStart = 0;  // absolute minute of drawPred[0]
static bool    wifiOffline   = false;
static time_t  cachedWStart  = 0;  // Unix time of window start when data was last fetched

// Forward declarations
bool hasConfig();

// ── Shared page chrome ────────────────────────────────────────────────────────

static const char PAGE_STYLE[] =
  "<style>"
  "body{font-family:sans-serif;max-width:460px;margin:40px auto;padding:0 20px}"
  "h1{color:#00aaff;margin-bottom:4px}"
  "h2{color:#aaa;font-size:14px;font-weight:normal;margin-top:0}"
  "label{display:block;margin-top:16px;font-weight:bold}"
  "input[type=text],input[type=password]{"
  "  width:100%;padding:9px;margin-top:4px;box-sizing:border-box;"
  "  border:1px solid #ccc;border-radius:4px;font-size:15px}"
  ".btn{display:block;margin-top:20px;width:100%;padding:13px;"
  "  background:#00aaff;color:#fff;border:none;border-radius:4px;"
  "  font-size:16px;cursor:pointer;text-align:center}"
  ".err{color:#c00;margin-top:12px;padding:10px;background:#fee;"
  "  border-radius:4px}"
  ".station{padding:10px 12px;margin-top:8px;border:1px solid #ddd;"
  "  border-radius:6px;cursor:pointer;background:#fafafa;color:#222}"
  ".station:hover{background:#e8f4ff;border-color:#00aaff}"
  ".station strong{display:block;color:#111}"
  ".station small{color:#666}"
  ".none{color:#888;margin-top:16px}"
  "</style>";

// ── Config portal — step 1: WiFi ──────────────────────────────────────────────

void handleRoot() {
  if (portalWifiConnected) {
    server.sendHeader("Location", "/stations");
    server.send(302);
    return;
  }
  String html = "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MyTides Setup</title>";
  html += PAGE_STYLE;
  html += "</head><body>"
    "<h1>MyTides Setup</h1>"
    "<h2>Step 1 of 2 — WiFi</h2>"
    "<form method='POST' action='/connect'>"
    "<label>WiFi Network Name</label>"
    "<input type='text' name='ssid' value='" + cfgSSID + "' required autocomplete='off'>"
    "<label>WiFi Password</label>"
    "<input type='password' name='pass' placeholder='(leave blank to keep saved)' autocomplete='off'>"
    "<button class='btn' type='submit'>Connect &rarr;</button>";
  if (connectError.length() > 0) {
    html += "<p class='err'>" + connectError + "</p>";
    connectError = "";
  }
  html += "</form></body></html>";
  server.send(200, "text/html", html);
}

void handleConnect() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (pass.length() == 0) pass = cfgPass; // keep saved password

  // Show a "connecting" page while we try
  server.send(200, "text/html",
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='12;url=/stations'>"
    "<title>Connecting...</title>"
    + String(PAGE_STYLE) +
    "</head><body>"
    "<h1>MyTides Setup</h1>"
    "<h2>Connecting to <strong>" + ssid + "</strong>...</h2>"
    "<p>This page will redirect automatically.</p>"
    "</body></html>");

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Connecting to WiFi...", tft.width() / 2, tft.height() / 2, 2);

  WiFi.begin(ssid.c_str(), pass.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 24) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    tempSSID = ssid;
    tempPass = pass;
    portalWifiConnected = true;
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawCentreString("WiFi Connected!", tft.width() / 2, 40, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Search for your", tft.width() / 2, 90, 2);
    tft.drawCentreString("tide station in browser", tft.width() / 2, 110, 2);
  } else {
    WiFi.disconnect();
    connectError = "Could not connect to &ldquo;" + ssid + "&rdquo;. Check credentials.";
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawCentreString("WiFi Failed", tft.width() / 2, tft.height() / 2, 4);
  }
}

// ── Config portal — step 2: Station search ────────────────────────────────────

String buildStationResults(String query) {
  query.trim();
  if (query.length() == 0) {
    return "<p class='err'>Please enter a city or station name.</p>";
  }

  String q = query; q.replace(" ", "%20");
  String url = "https://tidesandcurrents.noaa.gov/mdapi/latest/webapi/tidepredstations.json?q=" + q;

  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.begin(tls, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int code = http.GET();
  if (code != 200) {
    http.end();
    return "<p class='err'>NOAA error: HTTP " + String(code) + "</p>";
  }

  String body = http.getString();
  http.end();

  if (body.length() == 0) return "<p class='err'>Empty response from NOAA.</p>";

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) return "<p class='err'>Parse error: " + String(err.c_str()) + "</p>";

  JsonArray stations = doc["stationList"].as<JsonArray>();
  if (stations.isNull()) stations = doc["stations"].as<JsonArray>();
  if (stations.size() == 0) {
    return "<p class='none'>No stations found for &ldquo;" + query + "&rdquo;.</p>";
  }

  String out;
  int count = 0;
  for (JsonObject s : stations) {
    // Field names vary slightly between NOAA endpoints
    String id    = s["stationId"] | s["id"] | "";
    String name  = s["etidesStnName"] | s["name"] | "";
    String state = s["state"] | "";
    if (id.length() == 0 || name.length() == 0) continue;

    String label = name + (state.length() > 0 ? ", " + state : "");
    out += "<form method='POST' action='/save'>"
           "<input type='hidden' name='station_id' value='" + id + "'>"
           "<input type='hidden' name='station_name' value='" + label + "'>"
           "<button class='btn station' type='submit'>"
           "<strong>" + name + "</strong>"
           "<small>" + state + " &mdash; ID: " + id + "</small>"
           "</button></form>";
    if (++count >= 20) break;
  }

  if (count == 0)
    return "<p class='none'>No stations found for &ldquo;" + query + "&rdquo;.</p>";

  return out;
}

void handleStations() {
  if (!portalWifiConnected) {
    server.sendHeader("Location", "/");
    server.send(302);
    return;
  }

  String query   = server.arg("q");
  String results = "";
  if (query.length() > 0) results = buildStationResults(query);

  String html = "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MyTides Setup</title>";
  html += PAGE_STYLE;
  html += "</head><body>"
    "<h1>MyTides Setup</h1>"
    "<h2>Step 2 of 2 &mdash; Tide Station</h2>"
    "<form method='GET' action='/stations'>"
    "<label>Search for a tide station by name or city</label>"
    "<input type='text' name='q' value='" + query + "' placeholder='e.g. Swansboro' autofocus>"
    "<button class='btn' type='submit'>Search</button>"
    "</form>"
    + results +
    "</body></html>";

  server.send(200, "text/html", html);
}

void handleSave() {
  prefs.begin("mytides", false);
  prefs.putString("ssid",         tempSSID);
  prefs.putString("pass",         tempPass);
  prefs.putString("station_id",   server.arg("station_id"));
  prefs.putString("station_name", server.arg("station_name"));
  prefs.end();

  server.send(200, "text/html",
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saved</title>"
    + String(PAGE_STYLE) +
    "</head><body style='text-align:center;padding-top:60px'>"
    "<h1 style='color:#00cc44'>&#10003; Saved!</h1>"
    "<p>Restarting MyTides...</p>"
    "</body></html>");
  delay(1500);
  ESP.restart();
}

void startConfigPortal() {
  // Update TFT after WiFi auto-connect attempt
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawCentreString("Setup Mode", tft.width() / 2, 15, 4);
  if (portalWifiConnected) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawCentreString("WiFi Connected!", tft.width() / 2, 60, 4);
  }
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Connect phone to WiFi:", tft.width() / 2, portalWifiConnected ? 105 : 65, 2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawCentreString("MyTides-Config", tft.width() / 2, portalWifiConnected ? 123 : 83, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Then open browser to:", tft.width() / 2, portalWifiConnected ? 163 : 123, 2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawCentreString("192.168.4.1", tft.width() / 2, portalWifiConnected ? 181 : 141, 4);

  WiFi.mode(WIFI_AP_STA);

  // If credentials are saved, try connecting before the user has to re-enter them
  if (hasConfig()) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Connecting to WiFi...", tft.width() / 2, tft.height() / 2, 2);
    WiFi.begin(cfgSSID.c_str(), cfgPass.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(500); attempts++; }
    if (WiFi.status() == WL_CONNECTED) {
      tempSSID = cfgSSID;
      tempPass = cfgPass;
      portalWifiConnected = true;
    } else {
      WiFi.disconnect();
    }
  }

  WiFi.softAP("MyTides-Config");

  // Captive portal DNS: answer every domain with our IP
  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));

  server.on("/",         HTTP_GET,  handleRoot);
  server.on("/connect",  HTTP_POST, handleConnect);
  server.on("/stations", HTTP_GET,  handleStations);
  server.on("/save",     HTTP_POST, handleSave);

  // Captive portal detection endpoints for iOS, Android, Windows, macOS
  auto redirect = []() { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302); };
  server.on("/hotspot-detect.html",  HTTP_GET, redirect);
  server.on("/library/test/success.html", HTTP_GET, redirect);
  server.on("/generate_204",         HTTP_GET, redirect);
  server.on("/gen_204",              HTTP_GET, redirect);
  server.on("/ncsi.txt",             HTTP_GET, redirect);
  server.on("/connecttest.txt",      HTTP_GET, redirect);
  server.onNotFound(redirect);

  server.begin();

  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
  }
}

// ── Config NVS helpers ────────────────────────────────────────────────────────

void loadConfig() {
  prefs.begin("mytides", true);
  cfgSSID        = prefs.getString("ssid", "");
  cfgPass        = prefs.getString("pass", "");
  cfgStationId   = prefs.getString("station_id",   "8656613");
  cfgStationName = prefs.getString("station_name", "Swansboro, NC");
  cfgRotation    = prefs.getUChar("rotation", 1);
  cfgWifiEnabled = prefs.getBool("wifi_on", true);
  prefs.end();
}

bool hasConfig() {
  return cfgSSID.length() > 0 && cfgPass.length() > 0;
}

// ── WiFi / Time ───────────────────────────────────────────────────────────────

bool connectToWiFi() {
  WiFi.begin(cfgSSID.c_str(), cfgPass.c_str());
  for (int i = 0; i < 40; i++) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(500);
  }
  return false;
}

void initTime() {
  configTime(0, 0, ntpServer);
  setenv("TZ", timezone, 1);
  tzset();
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 20)
    delay(500), attempts++;
}

// ── Tides ─────────────────────────────────────────────────────────────────────

bool fetchTidePredictions(const String& datum) {
  struct tm now;
  getLocalTime(&now);
  struct tm midTm = now;
  midTm.tm_hour = midTm.tm_min = midTm.tm_sec = 0;
  midTm.tm_isdst = -1;
  time_t midnight = mktime(&midTm);

  // 24-hour window starting at hourOffset hours from today's midnight
  time_t wStart = midnight + (time_t)hourOffset * 3600;
  time_t wEnd   = wStart + 86400;
  struct tm sTm, eTm;
  localtime_r(&wStart, &sTm);
  localtime_r(&wEnd,   &eTm);
  char sDate[9], eDate[9];
  strftime(sDate, sizeof(sDate), "%Y%m%d", &sTm);
  strftime(eDate, sizeof(eDate), "%Y%m%d", &eTm);

  // Subordinate/reference stations don't use a datum — omit the parameter when empty
  String datumParam = datum.length() > 0 ? "&datum=" + datum : "";

  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?"
               "product=predictions&application=web_services&station=" +
               cfgStationId + "&begin_date=" + String(sDate) +
               "&end_date=" + String(eDate) +
               datumParam + "&units=english&time_zone=lst&format=json";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(10000);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  tideDoc.clear();
  DeserializationError error = deserializeJson(tideDoc, http.getStream());
  http.end();

  if (error) return false;

  // Check for NOAA API error in the response body
  if (tideDoc.containsKey("error")) return false;

  predictions = tideDoc["predictions"].as<JsonArray>();
  return predictions.size() >= 3;
}

bool getTidePredictions() {
  // Try datums in order; "" = no datum parameter (required for subordinate/reference stations)
  const char* datums[] = { "MLLW", "STND", "MSL", "MTL", "IGLD", "MHHW", "" };
  for (const char* datum : datums) {
    if (fetchTidePredictions(datum)) return true;
  }

  // All datums failed — show the actual NOAA error message if available
  tft.fillScreen(TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  String msg = tideDoc["error"]["message"] | "No tide data for this station.";

  // Word-wrap at ~34 chars per line (font 2 on 320px screen)
  int y = 10;
  while (msg.length() > 0 && y < 160) {
    int cut = msg.length();
    if (cut > 34) {
      cut = msg.lastIndexOf(' ', 34);
      if (cut <= 0) cut = 34;
    }
    tft.drawString(msg.substring(0, cut), 5, y, 2);
    msg = (cut < (int)msg.length()) ? msg.substring(cut + 1) : "";
    msg.trim();
    y += 22;
  }

  tft.setTextColor(TFT_YELLOW, TFT_RED);
  tft.drawString("Hold BOOT+Reset to", 5, y + 8, 2);
  tft.drawString("pick a new station", 5, y + 28, 2);
  return false;
}

// Returns minutes since today's midnight for a NOAA timestamp "YYYY-MM-DD HH:MM"
static int predAbsMin(const String& ts, time_t midnight) {
  struct tm t = {};
  t.tm_year  = ts.substring(0, 4).toInt() - 1900;
  t.tm_mon   = ts.substring(5, 7).toInt() - 1;
  t.tm_mday  = ts.substring(8, 10).toInt();
  t.tm_hour  = ts.substring(11, 13).toInt();
  t.tm_min   = ts.substring(14, 16).toInt();
  t.tm_isdst = -1;
  return (int)((mktime(&t) - midnight) / 60);
}

void processTidePredictions() {
  struct tm now;
  getLocalTime(&now);
  struct tm midTm = now;
  midTm.tm_hour = midTm.tm_min = midTm.tm_sec = 0;
  midTm.tm_isdst = -1;
  time_t midnight = mktime(&midTm);

  int winStart = hourOffset * 60;
  int winEnd   = winStart + 1440;

  minTide = 10.0;
  maxTide = -10.0;
  highTideEvents[0] = ""; highTideEvents[1] = "";
  lowTideEvents[0]  = ""; lowTideEvents[1]  = "";
  int highTideCount = 0, lowTideCount = 0;

  for (JsonObject p : predictions) {
    int am = predAbsMin(p["t"].as<String>(), midnight);
    if (am < winStart || am >= winEnd) continue;
    float h = p["v"].as<float>();
    if (h < minTide) minTide = h;
    if (h > maxTide) maxTide = h;
  }

  int lastTrend = 0;
  for (int i = 1; i < (int)predictions.size(); i++) {
    int am0 = predAbsMin(predictions[i - 1]["t"].as<String>(), midnight);
    int am1 = predAbsMin(predictions[i]["t"].as<String>(), midnight);
    if (am1 < winStart || am0 >= winEnd) { lastTrend = 0; continue; }

    float prev    = predictions[i - 1]["v"].as<float>();
    float current = predictions[i]["v"].as<float>();
    int trend = (current > prev) ? 1 : ((current < prev) ? -1 : lastTrend);

    if (trend != lastTrend && lastTrend != 0) {
      JsonObject peak = predictions[i - 1];
      String t = peak["t"].as<String>();
      int hour24 = t.substring(11, 13).toInt();
      String mins = t.substring(14, 16);
      int hour12 = (hour24 % 12 == 0) ? 12 : hour24 % 12;
      String suffix = (hour24 < 12) ? "am" : "pm";
      String label = String(hour12) + ":" + mins + suffix +
                     " (" + String(peak["v"].as<float>(), 1) + "ft)";
      if (lastTrend == 1 && highTideCount < 2) highTideEvents[highTideCount++] = label;
      else if (lastTrend == -1 && lowTideCount < 2) lowTideEvents[lowTideCount++] = label;
    }
    lastTrend = trend;
  }
}

// ── Bare-metal XPT2046 touch driver ──────────────────────────────────────────
// CYD touch is on its own SPI bus: CLK=25, MISO=39, MOSI=32, CS=33

static uint16_t xptSample(uint8_t cmd) {
  digitalWrite(TOUCH_CS, LOW);
  touchSPI.transfer(cmd);
  uint16_t val = ((uint16_t)touchSPI.transfer(0) << 8 | touchSPI.transfer(0)) >> 3;
  digitalWrite(TOUCH_CS, HIGH);
  return val & 0xFFF;
}

static int16_t xptZ() {
  touchSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  int16_t z = xptSample(0xB0); // Z1 pressure channel
  touchSPI.endTransaction();
  return z;
}

bool isTouched() { return xptZ() > 200; }

bool getTouchPoint(uint16_t& sx, uint16_t& sy) {
  touchSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  int16_t z = xptSample(0xB0);
  if (z < 200) { touchSPI.endTransaction(); return false; }

  // Average 4 samples for stability
  int32_t rawX = 0, rawY = 0;
  for (int i = 0; i < 4; i++) {
    rawX += xptSample(0xD0); // X channel
    rawY += xptSample(0x90); // Y channel
  }
  touchSPI.endTransaction();
  rawX /= 4; rawY /= 4;

  if (rawX < 100 || rawX > 4000 || rawY < 100 || rawY > 4000) return false;

  int W = tft.width(), H = tft.height();
  switch (cfgRotation % 4) {
    case 0: // Portrait
      sx = map(rawX, TOUCH_X_MAX, TOUCH_X_MIN, 0, W);
      sy = map(rawY, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, H);
      break;
    case 1: // Landscape (default, confirmed working)
      sx = map(rawY, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, W);
      sy = map(rawX, TOUCH_X_MIN, TOUCH_X_MAX, 0, H);
      break;
    case 2: // Portrait flipped — 180° from portrait
      sx = map(rawX, TOUCH_X_MIN, TOUCH_X_MAX, 0, W);
      sy = map(rawY, TOUCH_Y_MAX, TOUCH_Y_MIN, 0, H);
      break;
    case 3: // Landscape flipped — 180° from landscape
      sx = map(rawY, TOUCH_Y_MAX, TOUCH_Y_MIN, 0, W);
      sy = map(rawX, TOUCH_X_MAX, TOUCH_X_MIN, 0, H);
      break;
  }
  sx = constrain(sx, 0, W - 1);
  sy = constrain(sy, 0, H - 1);
  return true;
}

void drawGear(int cx, int cy, uint16_t color) {
  const int R = 10, r = 4, ts = 4;
  tft.fillCircle(cx, cy, R, color);
  tft.fillCircle(cx, cy, r, TFT_BLACK);
  for (int i = 0; i < 6; i++) {
    float a = i * (PI / 3.0f);
    int tx = cx + (int)round(cos(a) * (R + 3));
    int ty = cy + (int)round(sin(a) * (R + 3));
    tft.fillRect(tx - ts / 2, ty - ts / 2, ts, ts, color);
  }
}

void showRotationConfig() {
  int W = tft.width(), H = tft.height();
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Display Rotation", W / 2, 8, 4);

  const char* labels[] = {
    "Portrait  (0)",
    "Landscape  (90)",
    "Portrait Flipped  (180)",
    "Landscape Flipped  (270)"
  };
  const int btnH = 36, btnX = 10, btnW = W - 20;
  int btnY[4];
  for (int i = 0; i < 4; i++) {
    btnY[i] = 46 + i * (btnH + 6);
    bool cur = (i == (int)cfgRotation);
    uint16_t bg = cur ? 0x07E0 : TFT_NAVY;
    tft.fillRoundRect(btnX, btnY[i], btnW, btnH, 5, bg);
    tft.drawRoundRect(btnX, btnY[i], btnW, btnH, 5, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, bg);
    tft.drawCentreString(labels[i], W / 2, btnY[i] + 12, 2);
  }

  // Cancel button
  int cancelY = H - 36;
  tft.fillRoundRect(btnX, cancelY, btnW, 30, 5, TFT_DARKGREY);
  tft.drawRoundRect(btnX, cancelY, btnW, 30, 5, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawCentreString("Cancel", W / 2, cancelY + 9, 2);

  unsigned long lastTap = 0;
  while (true) {
    uint16_t tx, ty;
    if (getTouchPoint(tx, ty) && millis() - lastTap > 400) {
      lastTap = millis();

      for (int i = 0; i < 4; i++) {
        if (tx >= (uint16_t)btnX && tx <= (uint16_t)(btnX + btnW) &&
            ty >= (uint16_t)btnY[i] && ty <= (uint16_t)(btnY[i] + btnH)) {
          cfgRotation = i;
          prefs.begin("mytides", false);
          prefs.putUChar("rotation", cfgRotation);
          prefs.end();
          tft.setRotation(cfgRotation);
          return;
        }
      }

      if (tx >= (uint16_t)btnX && tx <= (uint16_t)(btnX + btnW) &&
          ty >= (uint16_t)cancelY && ty <= (uint16_t)(cancelY + 30)) {
        return;
      }
    }
    delay(50);
  }
}

void showGearMenu() {
  int W = tft.width(), H = tft.height();
  const int btnH = 36, btnX = 10, btnW = W - 20;
  int cancelY = H - 36;

  auto drawMenu = [&]() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Settings", W / 2, 8, 4);

    // Orientation
    int orientY = 46;
    tft.fillRoundRect(btnX, orientY, btnW, btnH, 5, TFT_NAVY);
    tft.drawRoundRect(btnX, orientY, btnW, btnH, 5, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawCentreString("Orientation", W / 2, orientY + 12, 2);

    // WiFi toggle
    int wifiY = 46 + btnH + 6;
    uint16_t wifiBg = cfgWifiEnabled ? 0x03E0 : 0x7800; // green : dark red
    tft.fillRoundRect(btnX, wifiY, btnW, btnH, 5, wifiBg);
    tft.drawRoundRect(btnX, wifiY, btnW, btnH, 5, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, wifiBg);
    tft.drawCentreString(cfgWifiEnabled ? "Auto Fetch: ON" : "Auto Fetch: OFF", W / 2, wifiY + 12, 2);

    // Cancel
    tft.fillRoundRect(btnX, cancelY, btnW, 30, 5, TFT_DARKGREY);
    tft.drawRoundRect(btnX, cancelY, btnW, 30, 5, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.drawCentreString("Cancel", W / 2, cancelY + 9, 2);
  };

  drawMenu();

  int orientY = 46;
  int wifiY   = 46 + btnH + 6;

  unsigned long lastTap = 0;
  while (true) {
    uint16_t tx, ty;
    if (getTouchPoint(tx, ty) && millis() - lastTap > 400) {
      lastTap = millis();

      if (tx >= (uint16_t)btnX && tx <= (uint16_t)(btnX + btnW)) {
        if (ty >= (uint16_t)orientY && ty <= (uint16_t)(orientY + btnH)) {
          while (isTouched()) delay(20);
          showRotationConfig();
          return;
        }

        if (ty >= (uint16_t)wifiY && ty <= (uint16_t)(wifiY + btnH)) {
          while (isTouched()) delay(20);
          cfgWifiEnabled = !cfgWifiEnabled;
          prefs.begin("mytides", false);
          prefs.putBool("wifi_on", cfgWifiEnabled);
          prefs.end();
          if (cfgWifiEnabled) {
            cachedHourOffset = -9999; // force immediate fetch on return
          } else {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
          }
          drawMenu();
          continue;
        }

        if (ty >= (uint16_t)cancelY && ty <= (uint16_t)(cancelY + 30)) {
          return;
        }
      }
    }
    delay(50);
  }
}

// Wifi-off icon: dot + 3 upward arcs + red slash, centred at (cx, cy)
void drawWifiOffIcon(int cx, int cy) {
  const uint16_t gray = 0x8410;
  tft.fillCircle(cx, cy, 2, gray);
  const float a0 = PI * 7.0f / 6.0f;   // 210°
  const float a1 = PI * 11.0f / 6.0f;  // 330° — arc goes upward in screen coords
  int radii[3] = { 5, 8, 11 };
  for (int ri = 0; ri < 3; ri++) {
    int r = radii[ri];
    for (float a = a0; a <= a1; a += 0.15f)
      tft.drawPixel(cx + (int)round(cos(a) * r), cy + (int)round(sin(a) * r), gray);
  }
  tft.drawLine(cx - 8, cy - 11, cx + 7, cy + 4, TFT_RED);
  tft.drawLine(cx - 7, cy - 11, cx + 8, cy + 4, TFT_RED);
}

// Populate drawPred[] from the live predictions JSON array after a successful fetch
static void populateDrawPred(time_t midnight) {
  drawPredCount = 0;
  bool first = true;
  for (JsonObject p : predictions) {
    if (drawPredCount >= 500) break;
    int am = predAbsMin(p["t"].as<String>(), midnight);
    if (first) { drawPredStart = am; first = false; }
    drawPred[drawPredCount++] = (int16_t)(p["v"].as<float>() * 100.0f);
  }
}

static void saveTideCache() {
  prefs.begin("tideCache", false);
  prefs.putFloat("minT",    minTide);
  prefs.putFloat("maxT",    maxTide);
  prefs.putString("hi0",    highTideEvents[0]);
  prefs.putString("hi1",    highTideEvents[1]);
  prefs.putString("lo0",    lowTideEvents[0]);
  prefs.putString("lo1",    lowTideEvents[1]);
  prefs.putInt("pStart",    drawPredStart);
  prefs.putInt("nPred",     drawPredCount);
  prefs.putInt("hourOff",   hourOffset);
  prefs.putUInt("wStart",   (uint32_t)cachedWStart);
  prefs.putBytes("pVals",   drawPred, drawPredCount * sizeof(int16_t));
  prefs.end();
}

static bool loadTideCache() {
  prefs.begin("tideCache", true);
  int n = prefs.getInt("nPred", 0);
  if (n == 0) { prefs.end(); return false; }
  drawPredCount     = min(n, 500);
  drawPredStart     = prefs.getInt("pStart", 0);
  hourOffset        = prefs.getInt("hourOff", 0);
  minTide           = prefs.getFloat("minT", 0.0f);
  maxTide           = prefs.getFloat("maxT", 1.0f);
  highTideEvents[0] = prefs.getString("hi0", "");
  highTideEvents[1] = prefs.getString("hi1", "");
  lowTideEvents[0]  = prefs.getString("lo0", "");
  lowTideEvents[1]  = prefs.getString("lo1", "");
  cachedWStart      = (time_t)prefs.getUInt("wStart", 0);
  prefs.getBytes("pVals", drawPred, drawPredCount * sizeof(int16_t));
  prefs.end();
  return true;
}

void drawTideChart(const struct tm& timeinfo) {
  // Compute today's midnight for absolute-minute calculations
  struct tm midTm = timeinfo;
  midTm.tm_hour = midTm.tm_min = midTm.tm_sec = 0;
  midTm.tm_isdst = -1;
  time_t midnight = mktime(&midTm);

  int winStart = hourOffset * 60;
  int winEnd   = winStart + 1440;

  tft.fillScreen(TFT_BLACK);

  // Date header: use the cached window-start time when offline (NTP may be wrong)
  time_t wStartT = (wifiOffline && cachedWStart != 0)
                   ? cachedWStart
                   : midnight + (time_t)hourOffset * 3600;
  struct tm wStartTm;
  localtime_r(&wStartT, &wStartTm);
  char displayDate[20];
  strftime(displayDate, sizeof(displayDate), "%A, %b %d", &wStartTm);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawCentreString(displayDate, tft.width() / 2, 5, 4);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawCentreString(cfgStationName.c_str(), tft.width() / 2, 33, 2);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("High", 5, 45, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(highTideEvents[0], 75, 50, 2);
  if (highTideEvents[1] != "") tft.drawString(highTideEvents[1], 195, 50, 2);

  tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
  tft.drawString("Low ", 5, 80, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(lowTideEvents[0], 75, 85, 2);
  if (lowTideEvents[1] != "") tft.drawString(lowTideEvents[1], 195, 85, 2);

  const int gX = 10, gY = 115;
  const int gW = tft.width() - 20, gH = tft.height() - 125;
  tft.drawRect(gX, gY, gW, gH, TFT_DARKGREY);

  long minH = (long)(minTide * 100);
  long maxH = (long)(maxTide * 100);
  int lastX = -1, lastY = -1;
  for (int i = 0; i < drawPredCount; i++) {
    int am = drawPredStart + i * 6;
    if (am < winStart || am >= winEnd) { lastX = -1; continue; }
    int x = map(am, winStart, winEnd, gX, gX + gW);
    int y = map((long)drawPred[i], minH, maxH, gY + gH, gY);
    if (lastX != -1) tft.drawLine(lastX, lastY, x, y, TFT_SKYBLUE);
    lastX = x;
    lastY = y;
  }

  // "Now" line only when online (offline data is stale)
  if (!wifiOffline) {
    int nowAbsMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    if (nowAbsMin >= winStart && nowAbsMin < winEnd) {
      int nowX = map(nowAbsMin, winStart, winEnd, gX, gX + gW);
      tft.drawFastVLine(nowX, gY, gH, TFT_RED);
    }
  }

  drawGear(tft.width() - 16, 16, TFT_DARKGREY);
  if (wifiOffline) drawWifiOffIcon(16, 16);
}

void fetchAndDisplayTides() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.drawString("Failed to get time!", 10, 10, 2);
    return;
  }

  if (hourOffset != cachedHourOffset || timeinfo.tm_yday != cachedTodayDoy) {
    if (!cfgWifiEnabled) {
      drawTideChart(timeinfo);
      return;
    }

    if (WiFi.status() != WL_CONNECTED) {
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawCentreString("Updating tides...", tft.width() / 2, tft.height() / 2 - 14, 2);
      WiFi.begin(cfgSSID.c_str(), cfgPass.c_str());
      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(500); attempts++; }
    }

    bool fetched = false;
    if (WiFi.status() == WL_CONNECTED) {
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawCentreString("Fetching tide data...", tft.width() / 2, tft.height() / 2, 2);
      if (getTidePredictions()) {
        processTidePredictions();
        struct tm midTm = timeinfo;
        midTm.tm_hour = midTm.tm_min = midTm.tm_sec = 0;
        midTm.tm_isdst = -1;
        time_t midnight = mktime(&midTm);
        cachedWStart = midnight + (time_t)hourOffset * 3600;
        populateDrawPred(midnight);
        saveTideCache();
        cachedHourOffset = hourOffset;
        cachedTodayDoy   = timeinfo.tm_yday;
        wifiOffline = false;
        fetched = true;
      }
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    if (!fetched) {
      wifiOffline = true;
      if (drawPredCount == 0) return;  // no data at all — stay on loading screen
    }
  }

  drawTideChart(timeinfo);
}

// ── Setup / Loop ──────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);
  pinMode(CONFIG_BTN, INPUT_PULLUP);

  loadConfig(); // must come before tft.init so saved rotation is ready

  tft.init();
  delay(100);
  tft.setRotation(cfgRotation);
  tft.fillScreen(TFT_BLACK);

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);

  if (!hasConfig() || digitalRead(CONFIG_BTN) == LOW) {
    startConfigPortal(); // never returns
  }

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Connecting to WiFi...", 10, 10, 2);
  if (!connectToWiFi()) {
    wifiOffline = true;
    tft.fillScreen(TFT_BLACK);
    struct tm timeinfo = {};
    getLocalTime(&timeinfo);
    if (loadTideCache()) {
      drawTideChart(timeinfo);
    } else {
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawCentreString("No WiFi", tft.width() / 2, tft.height() / 2 - 20, 4);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawCentreString("No cached data available", tft.width() / 2, tft.height() / 2 + 10, 2);
      tft.drawCentreString("Hold BOOT + Reset to reconfigure", tft.width() / 2, tft.height() / 2 + 30, 2);
    }
    lastFetchMs = millis();
    return;
  }
  tft.fillRect(0, 0, tft.width(), 30, TFT_BLACK);

  initTime();
  delay(500);
  fetchAndDisplayTides();
  lastFetchMs = millis();
}

void loop() {
  static uint16_t touchDownX = 0, touchDownY = 0;
  static uint16_t touchLastX = 0;
  static bool touchActive = false;

  uint16_t mx = 0, my = 0;
  bool touched = getTouchPoint(mx, my);

  if (touched) {
    if (!touchActive) {
      touchDownX = mx;
      touchDownY = my;
      touchActive = true;
    }
    touchLastX = mx;
  } else if (touchActive) {
    touchActive = false;
    int deltaX = (int)touchLastX - (int)touchDownX;

    if (abs(deltaX) > 50) {
      // Swipe: left = next hour, right = previous hour
      hourOffset += (deltaX < 0) ? 1 : -1;
      hourOffset = constrain(hourOffset, -168, 168);
      fetchAndDisplayTides();
      lastFetchMs = millis();
    } else {
      // Tap: check gear icon
      int W = tft.width();
      int gx = W - 16, gy = 16;
      if (abs((int)touchDownX - gx) <= 30 && abs((int)touchDownY - gy) <= 30) {
        showGearMenu();
        fetchAndDisplayTides();
        lastFetchMs = millis();
      }
    }
  }

  if (millis() - lastFetchMs >= 15UL * 60 * 1000) {
    fetchAndDisplayTides();
    lastFetchMs = millis();
  }

  delay(100);
}
