/*
  Flight Radar — ESP32-4848S040 variant
  ----------------------------------------------------------------
  All-in-one ESP32-S3 board with an integrated 480x480 ST7701 RGB-parallel
  display and GT911 capacitive touch (sold under names like "ESP32-S3-N16R8
  480x480", Guition/JCZN ESP32-4848S040). This board uses a 16-bit RGB
  parallel bus + 3-wire SPI init, NOT plain SPI — TFT_eSPI cannot drive it,
  so this variant uses LovyanGFX instead. Touch is used for a single
  feature: tap anywhere to force an immediate traffic refresh.

  Board settings required when compiling:
    Board: "ESP32S3 Dev Module"
    PSRAM: OPI PSRAM   |   Flash Size: 16MB   |   Flash Mode: QIO 80MHz
    USB CDC On Boot: Disabled   |   Upload Mode: UART0 / Hardware CDC

  Polls api.adsb.lol for nearby aircraft. No hardcoded WiFi credentials —
  first boot opens a "FlightRadar-Setup" access point for configuration.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

// ---- LovyanGFX device definition for this exact board ----
class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_RGB _bus_instance;
  lgfx::Panel_ST7701_guition_esp32_4848S040 _panel_instance;
  lgfx::Light_PWM _light_instance;
  lgfx::Touch_GT911 _touch_instance;

  LGFX(void) {
    { auto cfg = _panel_instance.config();
      cfg.memory_width  = 480; cfg.memory_height = 480;
      cfg.panel_width   = 480; cfg.panel_height  = 480;
      cfg.offset_x = 0; cfg.offset_y = 0;
      _panel_instance.config(cfg); }

    { auto cfg = _panel_instance.config_detail();
      cfg.pin_cs = 39; cfg.pin_sclk = 48; cfg.pin_mosi = 47;
      _panel_instance.config_detail(cfg); }

    { auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;
      // Blue
      cfg.pin_d0 = 4;  cfg.pin_d1 = 5;  cfg.pin_d2 = 6;  cfg.pin_d3 = 7;  cfg.pin_d4 = 15;
      // Green
      cfg.pin_d5 = 8;  cfg.pin_d6 = 20; cfg.pin_d7 = 3;  cfg.pin_d8 = 46; cfg.pin_d9 = 9; cfg.pin_d10 = 10;
      // Red
      cfg.pin_d11 = 11; cfg.pin_d12 = 12; cfg.pin_d13 = 13; cfg.pin_d14 = 14; cfg.pin_d15 = 0;

      cfg.pin_henable = 18; cfg.pin_vsync = 17; cfg.pin_hsync = 16; cfg.pin_pclk = 21;
      cfg.freq_write = 14000000;

      cfg.hsync_polarity = 0; cfg.hsync_front_porch = 10; cfg.hsync_pulse_width = 8; cfg.hsync_back_porch = 50;
      cfg.vsync_polarity = 0; cfg.vsync_front_porch = 10; cfg.vsync_pulse_width = 8; cfg.vsync_back_porch = 20;
      cfg.pclk_active_neg = 1; cfg.de_idle_high = 0; cfg.pclk_idle_high = 0;
      _bus_instance.config(cfg); }
    _panel_instance.setBus(&_bus_instance);

    { auto cfg = _light_instance.config();
      cfg.pin_bl = 38;
      cfg.freq   = 150;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance); }

    { auto cfg = _touch_instance.config();
      cfg.x_min = 0; cfg.x_max = 479; cfg.y_min = 0; cfg.y_max = 479;
      cfg.pin_int = -1; cfg.pin_rst = -1;
      cfg.bus_shared = false;
      cfg.i2c_port = 1; cfg.i2c_addr = 0x5D;
      cfg.pin_sda = 19; cfg.pin_scl = 45; cfg.freq = 400000;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance); }

    setPanel(&_panel_instance);
  }
};

LGFX tft;

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

const int CX = 240, CY = 232, MAXR = 210;     // bigger canvas than the SPI variants
const int BANNER_Y = 446;

float sweepAngle = 0;
unsigned long lastFetch = 0;
unsigned long lastDraw  = 0;
unsigned long lastTouchRefresh = 0;

const unsigned long FETCH_INTERVAL_MS = 20000;
const unsigned long DRAW_INTERVAL_MS  = 1000;
const unsigned long TOUCH_DEBOUNCE_MS = 3000;

float homeLat = -37.8136;
float homeLon = 144.9631;
float radarRangeNm = 25.0;

bool shouldSaveConfig = false;
void saveConfigCallback() { shouldSaveConfig = true; }

void loadConfig() {
  if (!LittleFS.begin(true)) return;
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
  tft.setTextSize(2);
  tft.drawString("Connect WiFi to:", 240, 210);
  tft.drawString("FlightRadar-Setup", 240, 240);
  tft.drawString("to configure", 240, 270);
  tft.setTextSize(1);

  bool ok = wm.autoConnect("FlightRadar-Setup");

  homeLat = atof(custom_lat.getValue());
  homeLon = atof(custom_lon.getValue());
  radarRangeNm = atof(custom_range.getValue());
  if (radarRangeNm < 5) radarRangeNm = 25;

  if (shouldSaveConfig) saveConfig();

  tft.fillScreen(TFT_BLACK);
  if (!ok) {
    tft.setTextSize(2);
    tft.drawString("WiFi setup timed out", 240, 240);
    tft.drawString("Restarting...", 240, 270);
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
  tft.startWrite();
  tft.fillScreen(TFT_BLACK);

  tft.drawCircle(CX, CY, MAXR, TFT_DARKGREY);
  tft.drawCircle(CX, CY, MAXR * 2 / 3, TFT_DARKGREY);
  tft.drawCircle(CX, CY, MAXR * 1 / 3, TFT_DARKGREY);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.drawString("N", CX, CY - MAXR + 16);
  tft.drawString("S", CX, CY + MAXR - 16);
  tft.drawString("E", CX + MAXR - 16, CY);
  tft.drawString("W", CX - MAXR + 16, CY);

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
    tft.fillCircle(bx, by, 6, altColor(a.altFt));
  }

  // bottom banner — top 3 nearest, this board has the room for it
  tft.fillRect(0, BANNER_Y, 480, 480 - BANNER_Y, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  if (fleetCount == 0) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("No traffic nearby", 16, BANNER_Y + 4);
  } else {
    int rows = fleetCount < 2 ? fleetCount : 2;
    for (int i = 0; i < rows; i++) {
      Aircraft &a = fleet[i];
      char line[48];
      snprintf(line, sizeof(line), "%-8s FL%03d  %.1fnm %dkt",
               strlen(a.flight) ? a.flight : "----", a.altFt / 100, a.distNm, a.gsKt);
      tft.setTextColor(altColor(a.altFt), TFT_BLACK);
      tft.drawString(line, 16, BANNER_Y + 4 + i * 17);
    }
  }
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.endWrite();

  sweepAngle += 12;
  if (sweepAngle >= 360) sweepAngle -= 360;
}

void setup() {
  tft.init();
  tft.setBrightness(255);
  setupWiFi();
  fetchAircraft();
  lastFetch = millis();
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    ESP.restart();
  }

  int32_t tx, ty;
  if (tft.getTouch(&tx, &ty)) {
    if (now - lastTouchRefresh > TOUCH_DEBOUNCE_MS) {
      lastTouchRefresh = now;
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.drawString("Refreshing...", CX, CY);
      fetchAircraft();
      lastFetch = now;
    }
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
