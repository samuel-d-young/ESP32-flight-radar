/*
  Flight Radar — works on NodeMCU V3 (ESP8266) AND generic ESP32 dev boards
  --------------------------------------------------------------------------
  Same firmware source, two compiled outputs (flight_radar_esp8266.bin and
  flight_radar_esp32.bin). The web installer auto-detects which chip is
  plugged in and flashes the matching one.

  NOTE: classic AVR Arduinos (Uno/Nano/Mega) cannot run this or be flashed
  from a browser at all — they don't have the Espressif serial bootloader
  that browser-based flashing tools (and this firmware) depend on.

  Polls the free api.adsb.lol ADS-B aggregator for aircraft near a home
  location and renders them on a radar-style sweep display (GC9A01, 240x240).

  No hardcoded WiFi credentials — on first boot the device opens a WiFi
  access point called "FlightRadar-Setup" for configuring WiFi + location.
*/

#include <Arduino.h>

#if defined(ESP32)
  #include <WiFi.h>
  #include <HTTPClient.h>
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266HTTPClient.h>
#endif

#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ---- TFT_eSPI inline configuration (GC9A01 round display) ----
// Defining these before including TFT_eSPI.h overrides the library's
// own User_Setup.h, so no extra files need to be copied anywhere.
// Pin sets differ between board families — NodeMCU's D-pin labels map to
// different GPIOs than ESP32 dev boards, so wire according to whichever
// block below matches your board (see the web installer's display library
// for the same numbers laid out per-board).
#define USER_SETUP_LOADED 1
#define GC9A01_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 240

#if defined(ESP32)
  #define TFT_MOSI 23   // GPIO23 -> display SDA
  #define TFT_SCLK 18   // GPIO18 -> display SCL
  #define TFT_CS    5   // GPIO5  -> display CS
  #define TFT_DC    2   // GPIO2  -> display DC
  #define TFT_RST   4   // GPIO4  -> display RST
#else
  #define TFT_MOSI 13   // D7 -> display SDA
  #define TFT_SCLK 14   // D5 -> display SCL
  #define TFT_CS   15   // D8 -> display CS
  #define TFT_DC    2   // D4 -> display DC
  #define TFT_RST   4   // D2 -> display RST
#endif

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define SMOOTH_FONT
#define SPI_FREQUENCY 27000000
#include <TFT_eSPI.h>
#include <SPI.h>

#define MAX_AC 24
#define CONFIG_FILE "/config.json"

struct Aircraft {
  char  flight[10];
  float distNm;
  float bearingDeg;
  int   altFt;
  int   gsKt;
};

Aircraft fleet[MAX_AC];
int fleetCount = 0;

TFT_eSPI tft = TFT_eSPI();

float sweepAngle = 0;
unsigned long lastFetch = 0;
unsigned long lastDraw  = 0;

const int CX = 120, CY = 120, MAXR = 112;
const unsigned long FETCH_INTERVAL_MS = 20000;
const unsigned long DRAW_INTERVAL_MS  = 1000;

// runtime config — loaded from flash, set via the captive setup portal
float homeLat = -37.8136;   // default: Melbourne CBD
float homeLon = 144.9631;
float radarRangeNm = 25.0;

bool shouldSaveConfig = false;
void saveConfigCallback() { shouldSaveConfig = true; }

void loadConfig() {
  if (!LittleFS.begin()) return;
  if (!LittleFS.exists(CONFIG_FILE)) return;
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) return;
  StaticJsonDocument<256> doc;
  if (!deserializeJson(doc, f)) {
    homeLat      = doc["lat"]   | homeLat;
    homeLon      = doc["lon"]   | homeLon;
    radarRangeNm = doc["range"] | radarRangeNm;
  }
  f.close();
}

void saveConfig() {
  StaticJsonDocument<256> doc;
  doc["lat"] = homeLat;
  doc["lon"] = homeLon;
  doc["range"] = radarRangeNm;
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (f) {
    serializeJson(doc, f);
    f.close();
  }
}

// ---------------- math helpers ----------------
float toRad(float d) { return d * PI / 180.0; }
float toDeg(float r) { return r * 180.0 / PI; }

float haversineNm(float lat1, float lon1, float lat2, float lon2) {
  const float R = 3440.065; // earth radius, nautical miles
  float dLat = toRad(lat2 - lat1);
  float dLon = toRad(lon2 - lon1);
  float a = sin(dLat / 2) * sin(dLat / 2) +
            cos(toRad(lat1)) * cos(toRad(lat2)) * sin(dLon / 2) * sin(dLon / 2);
  float c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return R * c;
}

float bearingDegFrom(float lat1, float lon1, float lat2, float lon2) {
  float dLon = toRad(lon2 - lon1);
  float y = sin(dLon) * cos(toRad(lat2));
  float x = cos(toRad(lat1)) * sin(toRad(lat2)) - sin(toRad(lat1)) * cos(toRad(lat2)) * cos(dLon);
  float brng = toDeg(atan2(y, x));
  return fmod(brng + 360.0, 360.0);
}

// ---------------- WiFi / setup portal ----------------
void setupWiFi() {
  loadConfig();

  char latBuf[16], lonBuf[16], rangeBuf[8];
  dtostrf(homeLat, 1, 4, latBuf);
  dtostrf(homeLon, 1, 4, lonBuf);
  dtostrf(radarRangeNm, 1, 1, rangeBuf);

  WiFiManagerParameter custom_lat("lat", "Home latitude", latBuf, 16);
  WiFiManagerParameter custom_lon("lon", "Home longitude", lonBuf, 16);
  WiFiManagerParameter custom_range("range", "Radar range (nm)", rangeBuf, 8);

  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.addParameter(&custom_lat);
  wm.addParameter(&custom_lon);
  wm.addParameter(&custom_range);
  wm.setConfigPortalTimeout(180);

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connect WiFi to:", CX, CY - 20);
  tft.drawString("FlightRadar-Setup", CX, CY);
  tft.drawString("to configure", CX, CY + 20);

  bool ok = wm.autoConnect("FlightRadar-Setup");

  homeLat = atof(custom_lat.getValue());
  homeLon = atof(custom_lon.getValue());
  radarRangeNm = atof(custom_range.getValue());
  if (radarRangeNm < 5) radarRangeNm = 25;

  if (shouldSaveConfig) {
    saveConfig();
  }

  tft.fillScreen(TFT_BLACK);
  if (!ok) {
    tft.drawString("WiFi setup timed out", CX, CY);
    tft.drawString("Restarting...", CX, CY + 20);
    delay(3000);
    ESP.restart();
  }
}

// ---------------- fetch + parse ----------------
void fetchAircraft() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure(); // skip TLS cert validation — fine for a hobby display

  HTTPClient http;
  char url[160];
  snprintf(url, sizeof(url),
           "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
           homeLat, homeLon, (int)radarRangeNm);

  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code != 200) {
    http.end();
    return;
  }

  StaticJsonDocument<256> filter;
  filter["ac"][0]["flight"]   = true;
  filter["ac"][0]["lat"]      = true;
  filter["ac"][0]["lon"]      = true;
  filter["ac"][0]["alt_baro"] = true;
  filter["ac"][0]["gs"]       = true;

  DynamicJsonDocument doc(10240);
  DeserializationError err = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) return;

  JsonArray ac = doc["ac"].as<JsonArray>();
  fleetCount = 0;
  for (JsonObject a : ac) {
    if (fleetCount >= MAX_AC) break;
    if (!a.containsKey("lat") || !a.containsKey("lon")) continue;

    float lat = a["lat"].as<float>();
    float lon = a["lon"].as<float>();

    Aircraft &cur = fleet[fleetCount];
    const char* fl = a["flight"] | "";
    strncpy(cur.flight, fl, sizeof(cur.flight) - 1);
    cur.flight[sizeof(cur.flight) - 1] = 0;
    for (int i = strlen(cur.flight) - 1; i >= 0 && cur.flight[i] == ' '; i--)
      cur.flight[i] = 0;

    cur.distNm     = haversineNm(homeLat, homeLon, lat, lon);
    cur.bearingDeg = bearingDegFrom(homeLat, homeLon, lat, lon);
    cur.altFt = a["alt_baro"].is<int>() ? a["alt_baro"].as<int>() : 0;
    cur.gsKt  = (int)(a["gs"].as<float>() + 0.5);

    fleetCount++;
  }

  for (int i = 1; i < fleetCount; i++) {
    Aircraft key = fleet[i];
    int j = i - 1;
    while (j >= 0 && fleet[j].distNm > key.distNm) {
      fleet[j + 1] = fleet[j];
      j--;
    }
    fleet[j + 1] = key;
  }
}

// ---------------- drawing ----------------
uint16_t altColor(int alt) {
  if (alt <= 0)    return TFT_DARKGREY;
  if (alt < 5000)  return TFT_RED;
  if (alt < 15000) return TFT_ORANGE;
  if (alt < 30000) return TFT_YELLOW;
  return TFT_CYAN;
}

void drawRadar() {
  tft.fillScreen(TFT_BLACK);

  tft.drawCircle(CX, CY, MAXR, TFT_DARKGREY);
  tft.drawCircle(CX, CY, MAXR * 2 / 3, TFT_DARKGREY);
  tft.drawCircle(CX, CY, MAXR * 1 / 3, TFT_DARKGREY);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("N", CX, CY - MAXR + 8);
  tft.drawString("S", CX, CY + MAXR - 8);
  tft.drawString("E", CX + MAXR - 8, CY);
  tft.drawString("W", CX - MAXR + 8, CY);

  float rad = toRad(sweepAngle);
  int sx = CX + MAXR * sin(rad);
  int sy = CY - MAXR * cos(rad);
  tft.drawLine(CX, CY, sx, sy, TFT_DARKGREEN);

  for (int i = 0; i < fleetCount; i++) {
    Aircraft &a = fleet[i];
    float r = (a.distNm / radarRangeNm) * MAXR;
    if (r > MAXR) continue;
    float brad = toRad(a.bearingDeg);
    int bx = CX + r * sin(brad);
    int by = CY - r * cos(brad);
    tft.fillCircle(bx, by, 3, altColor(a.altFt));
  }

  tft.fillRect(0, 206, 240, 34, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  if (fleetCount > 0) {
    Aircraft &n = fleet[0];
    char line1[40], line2[40];
    snprintf(line1, sizeof(line1), "%s  FL%03d",
             strlen(n.flight) ? n.flight : "----", n.altFt / 100);
    snprintf(line2, sizeof(line2), "%.1fnm  %dkt", n.distNm, n.gsKt);
    tft.drawString(line1, CX, 214);
    tft.drawString(line2, CX, 228);
  } else {
    tft.drawString("No traffic nearby", CX, 220);
  }

  sweepAngle += 12;
  if (sweepAngle >= 360) sweepAngle -= 360;
}

// ---------------- setup / loop ----------------
void setup() {
  tft.init();
  tft.setRotation(0);
  setupWiFi();
  fetchAircraft();
  lastFetch = millis();
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    ESP.restart(); // simplest recovery path: reboot back into the portal flow
  }

  if (now - lastFetch >= FETCH_INTERVAL_MS) {
    fetchAircraft();
    lastFetch = now;
  }

  if (now - lastDraw >= DRAW_INTERVAL_MS) {
    drawRadar();
    lastDraw = now;
  }
}
