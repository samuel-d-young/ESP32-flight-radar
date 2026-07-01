/*
  Flight Radar — round display (GC9A01, 240x240)
  ESP8266 NodeMCU V3 or generic ESP32 dev board
  -----------------------------------------------
  v1.3.0 — adds a settings web server at the device IP on port 80.
  Visit http://<device-ip>/ in a browser on the same network to change
  lat/lon and radar range without reflashing.
  Hold BOOT/FLASH button for 5s to re-open the WiFiManager portal
  (for changing WiFi network).
*/

#include <Arduino.h>

#if defined(ESP32)
  #include <WiFi.h>
  #include <HTTPClient.h>
  #include <WebServer.h>
  #define BOOT_BTN 0
  WebServer settingsServer(80);
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266HTTPClient.h>
  #include <ESP8266WebServer.h>
  #define BOOT_BTN 0
  ESP8266WebServer settingsServer(80);
#endif

#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#define USER_SETUP_LOADED 1
#define GC9A01_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 240
#if defined(ESP32)
  #define TFT_MOSI 23
  #define TFT_SCLK 18
  #define TFT_CS    5
  #define TFT_DC    2
  #define TFT_RST   4
#else
  #define TFT_MOSI 13
  #define TFT_SCLK 14
  #define TFT_CS   15
  #define TFT_DC    2
  #define TFT_RST   4
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

struct Aircraft { char flight[10]; float distNm, bearingDeg; int altFt, gsKt; };
Aircraft fleet[MAX_AC];
int fleetCount = 0;

TFT_eSPI tft = TFT_eSPI();
const int CX = 120, CY = 120, MAXR = 112;
float sweepAngle = 0;
unsigned long lastFetch = 0, lastDraw = 0, lastBtnCheck = 0;
unsigned long btnHoldStart = 0;
bool btnWasHeld = false;
const unsigned long FETCH_INTERVAL_MS = 20000;
const unsigned long DRAW_INTERVAL_MS  = 1000;

float homeLat = -37.8136, homeLon = 144.9631, radarRangeNm = 25.0;
bool shouldSaveConfig = false;
void saveConfigCallback() { shouldSaveConfig = true; }

// ---------- config ----------
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
  doc["lat"] = homeLat; doc["lon"] = homeLon; doc["range"] = radarRangeNm;
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

// ---------- settings web server ----------
void handleRoot() {
  char ip[20]; WiFi.localIP().toString().toCharArray(ip, sizeof(ip));
  char html[2048];
  snprintf(html, sizeof(html), R"(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flight Radar Settings</title>
<style>
  body{font-family:sans-serif;background:#06090a;color:#e9f3ee;max-width:420px;margin:40px auto;padding:0 20px}
  h2{color:#39ff8a;margin-bottom:4px}
  p.sub{color:#74867e;font-size:13px;margin-top:0}
  label{display:block;margin-top:16px;color:#74867e;font-size:13px}
  input{width:100%%;box-sizing:border-box;background:#0d1412;border:1px solid #1c2622;color:#e9f3ee;
        padding:10px;border-radius:6px;font-size:15px;margin-top:4px}
  button{margin-top:24px;width:100%%;background:#39ff8a;color:#04130a;border:none;
         padding:12px;border-radius:8px;font-size:16px;font-weight:700;cursor:pointer}
  .ip{font-family:monospace;color:#39ff8a}
</style></head>
<body>
<h2>Flight Radar</h2>
<p class="sub">Device IP: <span class="ip">%s</span></p>
<form method="POST" action="/save">
  <label>Home latitude</label>
  <input name="lat" value="%.4f">
  <label>Home longitude</label>
  <input name="lon" value="%.4f">
  <label>Radar range (nautical miles)</label>
  <input name="range" value="%.1f">
  <button>Save &amp; apply</button>
</form>
<p style="margin-top:24px;color:#74867e;font-size:12px">
  To change WiFi: hold the BOOT button on the board for 5 seconds.
</p>
</body></html>)", ip, homeLat, homeLon, radarRangeNm);
  settingsServer.send(200, "text/html", html);
}

void handleSave() {
  if (settingsServer.hasArg("lat"))   homeLat      = settingsServer.arg("lat").toFloat();
  if (settingsServer.hasArg("lon"))   homeLon      = settingsServer.arg("lon").toFloat();
  if (settingsServer.hasArg("range")) radarRangeNm = settingsServer.arg("range").toFloat();
  if (radarRangeNm < 5) radarRangeNm = 25;
  saveConfig();
  settingsServer.sendHeader("Location", "/");
  settingsServer.send(303);
  // show notification on display
  tft.fillRect(0, 100, 240, 40, TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Settings saved!", 120, 120);
  lastFetch = 0; // trigger immediate data refresh
}

// ---------- math ----------
float toRad(float d) { return d * PI / 180.0; }
float toDeg(float r) { return r * 180.0 / PI; }

float haversineNm(float lat1, float lon1, float lat2, float lon2) {
  const float R = 3440.065;
  float dLat = toRad(lat2-lat1), dLon = toRad(lon2-lon1);
  float a = sin(dLat/2)*sin(dLat/2)+cos(toRad(lat1))*cos(toRad(lat2))*sin(dLon/2)*sin(dLon/2);
  return R * 2 * atan2(sqrt(a), sqrt(1-a));
}
float bearingDegFrom(float lat1, float lon1, float lat2, float lon2) {
  float dLon = toRad(lon2-lon1);
  float y = sin(dLon)*cos(toRad(lat2));
  float x = cos(toRad(lat1))*sin(toRad(lat2))-sin(toRad(lat1))*cos(toRad(lat2))*cos(dLon);
  return fmod(toDeg(atan2(y,x))+360.0, 360.0);
}

// ---------- WiFi ----------
void setupWiFi() {
  loadConfig();
  char latBuf[16], lonBuf[16], rangeBuf[8];
  dtostrf(homeLat,1,4,latBuf); dtostrf(homeLon,1,4,lonBuf); dtostrf(radarRangeNm,1,1,rangeBuf);
  WiFiManagerParameter p_lat("lat","Home latitude",latBuf,16);
  WiFiManagerParameter p_lon("lon","Home longitude",lonBuf,16);
  WiFiManagerParameter p_range("range","Radar range (nm)",rangeBuf,8);
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.addParameter(&p_lat); wm.addParameter(&p_lon); wm.addParameter(&p_range);
  wm.setConfigPortalTimeout(180);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE,TFT_BLACK); tft.setTextDatum(MC_DATUM);
  tft.drawString("Connect WiFi to:", 120, 100);
  tft.drawString("FlightRadar-Setup", 120, 120);
  tft.drawString("to configure", 120, 140);
  bool ok = wm.autoConnect("FlightRadar-Setup");
  homeLat = atof(p_lat.getValue()); homeLon = atof(p_lon.getValue());
  radarRangeNm = atof(p_range.getValue()); if (radarRangeNm<5) radarRangeNm=25;
  if (shouldSaveConfig) saveConfig();
  tft.fillScreen(TFT_BLACK);
  if (!ok) {
    tft.drawString("WiFi failed, restarting", 120, 120);
    delay(3000); ESP.restart();
  }
  // show IP on screen for 4 seconds
  tft.setTextColor(TFT_GREEN,TFT_BLACK);
  tft.drawString("Connected!", 120, 106);
  tft.setTextColor(TFT_WHITE,TFT_BLACK);
  tft.drawString(WiFi.localIP().toString(), 120, 126);
  tft.setTextColor(TFT_DARKGREY,TFT_BLACK);
  tft.drawString("visit in browser", 120, 146);
  delay(4000);
}

// ---------- ADS-B fetch ----------
void fetchAircraft() {
  if (WiFi.status()!=WL_CONNECTED) return;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  char url[160];
  snprintf(url,sizeof(url),"https://api.adsb.lol/v2/point/%.4f/%.4f/%d",homeLat,homeLon,(int)radarRangeNm);
  if (!http.begin(client,url)) return;
  if (http.GET()!=200) { http.end(); return; }
  StaticJsonDocument<256> filter;
  filter["ac"][0]["flight"]=true; filter["ac"][0]["lat"]=true; filter["ac"][0]["lon"]=true;
  filter["ac"][0]["alt_baro"]=true; filter["ac"][0]["gs"]=true;
  DynamicJsonDocument doc(10240);
  if (deserializeJson(doc,http.getStream(),DeserializationOption::Filter(filter))) { http.end(); return; }
  http.end();
  JsonArray ac = doc["ac"].as<JsonArray>();
  fleetCount = 0;
  for (JsonObject a : ac) {
    if (fleetCount>=MAX_AC || !a.containsKey("lat") || !a.containsKey("lon")) continue;
    Aircraft &cur = fleet[fleetCount];
    const char* fl = a["flight"]|""; strncpy(cur.flight,fl,9); cur.flight[9]=0;
    for(int i=strlen(cur.flight)-1;i>=0&&cur.flight[i]==' ';i--) cur.flight[i]=0;
    cur.distNm     = haversineNm(homeLat,homeLon,a["lat"].as<float>(),a["lon"].as<float>());
    cur.bearingDeg = bearingDegFrom(homeLat,homeLon,a["lat"].as<float>(),a["lon"].as<float>());
    cur.altFt = a["alt_baro"].is<int>() ? a["alt_baro"].as<int>() : 0;
    cur.gsKt  = (int)(a["gs"].as<float>()+0.5);
    fleetCount++;
  }
  for(int i=1;i<fleetCount;i++){Aircraft key=fleet[i];int j=i-1;
    while(j>=0&&fleet[j].distNm>key.distNm){fleet[j+1]=fleet[j];j--;} fleet[j+1]=key;}
}

// ---------- draw ----------
uint16_t altColor(int alt) {
  if(alt<=0) return TFT_DARKGREY; if(alt<5000) return TFT_RED;
  if(alt<15000) return TFT_ORANGE; if(alt<30000) return TFT_YELLOW; return TFT_CYAN;
}

void drawRadar() {
  tft.fillScreen(TFT_BLACK);
  tft.drawCircle(CX,CY,MAXR,TFT_DARKGREY);
  tft.drawCircle(CX,CY,MAXR*2/3,TFT_DARKGREY);
  tft.drawCircle(CX,CY,MAXR/3,TFT_DARKGREY);
  tft.setTextColor(TFT_DARKGREY,TFT_BLACK); tft.setTextDatum(MC_DATUM);
  tft.drawString("N",CX,CY-MAXR+8); tft.drawString("S",CX,CY+MAXR-8);
  tft.drawString("E",CX+MAXR-8,CY); tft.drawString("W",CX-MAXR+8,CY);
  float rad=toRad(sweepAngle);
  tft.drawLine(CX,CY,CX+MAXR*sin(rad),CY-MAXR*cos(rad),TFT_DARKGREEN);
  for(int i=0;i<fleetCount;i++){
    Aircraft &a=fleet[i]; float r=(a.distNm/radarRangeNm)*MAXR;
    if(r>MAXR) continue; float brad=toRad(a.bearingDeg);
    tft.fillCircle(CX+r*sin(brad),CY-r*cos(brad),3,altColor(a.altFt));
  }
  tft.fillRect(0,206,240,34,TFT_BLACK);
  tft.setTextColor(TFT_WHITE,TFT_BLACK);
  if(fleetCount>0){
    Aircraft &n=fleet[0]; char l1[40],l2[40];
    snprintf(l1,40,"%s  FL%03d",strlen(n.flight)?n.flight:"----",n.altFt/100);
    snprintf(l2,40,"%.1fnm  %dkt",n.distNm,n.gsKt);
    tft.drawString(l1,CX,214); tft.drawString(l2,CX,228);
  } else { tft.drawString("No traffic nearby",CX,220); }
  sweepAngle+=12; if(sweepAngle>=360) sweepAngle-=360;
}

void setup() {
  pinMode(BOOT_BTN, INPUT_PULLUP);
  tft.init(); tft.setRotation(0);
  setupWiFi();
  settingsServer.on("/", handleRoot);
  settingsServer.on("/save", HTTP_POST, handleSave);
  settingsServer.begin();
  fetchAircraft(); lastFetch=millis();
}

void loop() {
  unsigned long now=millis();
  settingsServer.handleClient();
  // 5-second boot-button hold reopens WiFiManager portal
  if(digitalRead(BOOT_BTN)==LOW){
    if(btnHoldStart==0) btnHoldStart=now;
    if(now-btnHoldStart>5000 && !btnWasHeld){
      btnWasHeld=true;
      tft.fillScreen(TFT_BLACK); tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE,TFT_BLACK);
      tft.drawString("Opening setup portal...",120,120);
      WiFiManager wm; wm.setConfigPortalTimeout(180);
      wm.startConfigPortal("FlightRadar-Setup");
      ESP.restart();
    }
  } else { btnHoldStart=0; btnWasHeld=false; }
  if(WiFi.status()!=WL_CONNECTED) ESP.restart();
  if(now-lastFetch>=FETCH_INTERVAL_MS||lastFetch==0){fetchAircraft();lastFetch=now;}
  if(now-lastDraw>=DRAW_INTERVAL_MS){drawRadar();lastDraw=now;}
}
