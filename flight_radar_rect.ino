/*
  Flight Radar — rectangular display variant (ILI9341, 240x320)
  ----------------------------------------------------------------
  Same logic as the round GC9A01 build, with a layout adapted to a tall
  rectangular screen: radar circle in the upper portion, an expanded
  3-aircraft list filling the extra space below.

  Works on NodeMCU V3 (ESP8266) and generic ESP32 dev boards — chip
  detected automatically by the web installer.

  Polls api.adsb.lol for nearby aircraft. No hardcoded WiFi credentials —
  first boot opens a "FlightRadar-Setup" access point for configuration.
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

// ---- TFT_eSPI inline configuration (ILI9341, 240x320 portrait) ----
#define USER_SETUP_LOADED 1
#define ILI9341_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#if defined(ESP32)
  #define TFT_MOSI 23
  #define TFT_SCLK 18
  #define TFT_CS    5
  #define TFT_DC    2
  #define TFT_RST   4
#else
  #define TFT_MOSI 13   // D7
  #define TFT_SCLK 14   // D5
  #define TFT_CS   15   // D8
  #define TFT_DC    2   // D4
  #define TFT_RST   4   // D2
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

const int SCREEN_W = 240, SCREEN_H = 320;
const int CX = 120, CY = 112, MAXR = 100;     // radar circle, upper portion
const int LIST_Y = 218;          // expanded list, lower portion

float sweepAngle = 0;
unsigned long lastFetch = 0;
unsigned long lastDraw  = 0;

const unsigned long FETCH_INTERVAL_MS = 20000;
const unsigned long DRAW_INTERVAL_MS  = 1000;

float homeLat = -37.8136;
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
  if (f) { serializeJson(doc, f); f.close(); }
}

float toRad(float d) { return d * PI / 180.0; }
float toDeg(float r) { return r * 180.0 / PI; }

float haversineNm(float lat1, float lon1, float lat2, float lon2) {
  const float R = 3440.065;
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
  int midY = SCREEN_H / 2;
  tft.drawString("Connect WiFi to:", SCREEN_W / 2, midY - 20);
  tft.drawString("FlightRadar-Setup", SCREEN_W / 2, midY);
  tft.drawString("to configure", SCREEN_W / 2, midY + 20);

  bool ok = wm.autoConnect("FlightRadar-Setup");

  homeLat = atof(custom_lat.getValue());
  homeLon = atof(custom_lon.getValue());
  radarRangeNm = atof(custom_range.getValue());
  if (radarRangeNm < 5) radarRangeNm = 25;

  if (shouldSaveConfig) saveConfig();

  tft.fillScreen(TFT_BLACK);
  if (!ok) {
    tft.drawString("WiFi setup timed out", SCREEN_W / 2, midY);
    tft.drawString("Restarting...", SCREEN_W / 2, midY + 20);
    delay(3000);
    ESP.restart();
  }
}

void fetchAircraft() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  char url[160];
  snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
           homeLat, homeLon, (int)radarRangeNm);

  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code != 200) { http.end(); return; }

  StaticJsonDocument<256> filter;
  filter["ac"][0]["flight"]   = true;
  filter["ac"][0]["lat"]      = true;
  filter["ac"][0]["lon"]      = true;
  filter["ac"][0]["alt_baro"] = true;
  filter["ac"][0]["gs"]       = true;

  DynamicJsonDocument doc(10240);
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
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
    for (int i = strlen(cur.flight) - 1; i >= 0 && cur.flight[i] == ' '; i--) cur.flight[i] = 0;

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

  // divider + expanded list — extra vertical room a rectangular screen has
  tft.drawFastHLine(0, LIST_Y - 6, SCREEN_W, TFT_DARKGREY);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("NEAREST TRAFFIC", 8, LIST_Y);

  if (fleetCount == 0) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("No traffic nearby", 8, LIST_Y + 26);
  } else {
    int rows = fleetCount < 3 ? fleetCount : 3;
    for (int i = 0; i < rows; i++) {
      Aircraft &a = fleet[i];
      char line[48];
      snprintf(line, sizeof(line), "%-8s FL%03d  %.1fnm %dkt",
               strlen(a.flight) ? a.flight : "----", a.altFt / 100, a.distNm, a.gsKt);
      tft.setTextColor(altColor(a.altFt), TFT_BLACK);
      tft.drawString(line, 8, LIST_Y + 24 + i * 24);
    }
  }
  tft.setTextDatum(MC_DATUM);

  sweepAngle += 12;
  if (sweepAngle >= 360) sweepAngle -= 360;
}

void setup() {
  tft.init();
  tft.setRotation(0); // portrait, 240x320
  setupWiFi();
  fetchAircraft();
  lastFetch = millis();
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    ESP.restart();
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
