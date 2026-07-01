/*
  Flight Radar — round display (GC9A01 240x240)
  ESP8266 / ESP32 / ESP32-S3 auto-detected by web installer
  v1.4.0 — Improv WiFi: enter credentials in the browser during/after flash
*/
#include <Arduino.h>

#if defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
  WebServer settingsServer(80);
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  ESP8266WebServer settingsServer(80);
#endif

#include <WiFiClientSecure.h>
#if defined(ESP32)
  #include <HTTPClient.h>
#else
  #include <ESP8266HTTPClient.h>
#endif
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ImprovWiFiLibrary.h>

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

TFT_eSPI tft = TFT_eSPI();
ImprovWiFi improvSerial(&Serial);

const int CX = 120, CY = 120, MAXR = 112;
#define MAX_AC 24
#define CONFIG_FILE "/config.json"
#define BOOT_BTN 0

// --- common config/improv code ---
float homeLat = -37.8136, homeLon = 144.9631, radarRangeNm = 25.0;
char savedSSID[64] = "", savedPass[64] = "";
unsigned long btnHoldStart = 0; bool btnWasHeld = false;

void showMessage(const char* l1, const char* l2 = "", const char* l3 = "") {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (strlen(l1)) tft.drawString(l1, CX, CY - 14);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  if (strlen(l2)) tft.drawString(l2, CX, CY + 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  if (strlen(l3)) tft.drawString(l3, CX, CY + 18);
}

void loadConfig() {
  if (!LittleFS.begin()) return;
  if (!LittleFS.exists(CONFIG_FILE)) return;
  File f = LittleFS.open(CONFIG_FILE, "r"); if (!f) return;
  StaticJsonDocument<384> doc;
  if (!deserializeJson(doc, f)) {
    homeLat = doc["lat"]|homeLat; homeLon = doc["lon"]|homeLon;
    radarRangeNm = doc["range"]|radarRangeNm;
    strlcpy(savedSSID, doc["ssid"]|"", sizeof(savedSSID));
    strlcpy(savedPass, doc["pass"]|"", sizeof(savedPass));
  }
  f.close();
}
void saveConfig() {
  LittleFS.begin();
  StaticJsonDocument<384> doc;
  doc["lat"]=homeLat; doc["lon"]=homeLon; doc["range"]=radarRangeNm;
  doc["ssid"]=savedSSID; doc["pass"]=savedPass;
  File f = LittleFS.open(CONFIG_FILE,"w"); if(f){serializeJson(doc,f);f.close();}
}

bool improvConnectCallback(const char* ssid, const char* pass) {
  showMessage("Connecting...", ssid);
  WiFi.begin(ssid, pass);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t < 15000) delay(200);
  if (WiFi.status() != WL_CONNECTED) { showMessage("Failed", ssid); return false; }
  strlcpy(savedSSID, ssid, sizeof(savedSSID));
  strlcpy(savedPass, pass, sizeof(savedPass));
  saveConfig();
  return true;
}

void runImprovMode() {
  Serial.begin(115200);
  #if defined(ESP32)
    #if CONFIG_IDF_TARGET_ESP32S3
      auto chip = ImprovTypes::ChipFamily::CF_ESP32_S3;
    #elif CONFIG_IDF_TARGET_ESP32C3
      auto chip = ImprovTypes::ChipFamily::CF_ESP32_C3;
    #else
      auto chip = ImprovTypes::ChipFamily::CF_ESP32;
    #endif
  #else
    auto chip = ImprovTypes::ChipFamily::CF_ESP8266;
  #endif
  improvSerial.setDeviceInfo(chip, "FlightRadar", "1.4", "Flight Radar");
  improvSerial.setCustomConnectWiFi(improvConnectCallback);
  showMessage("Connect WiFi via", "installer or", "visit device page");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    improvSerial.handleSerial();
    if (millis()-start > 300000) ESP.restart();
    delay(10);
  }
}

void handleRoot() {
  char ip[20]; WiFi.localIP().toString().toCharArray(ip, sizeof(ip));
  char html[2600]; snprintf(html, sizeof(html), R"(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flight Radar</title>
<style>body{font-family:sans-serif;background:#06090a;color:#e9f3ee;max-width:440px;margin:40px auto;padding:0 20px}
h2{color:#39ff8a;margin-bottom:4px}.sub{color:#74867e;font-size:13px;margin-top:0}
label{display:block;margin-top:16px;color:#74867e;font-size:13px}
input{width:100%%;box-sizing:border-box;background:#0d1412;border:1px solid #1c2622;color:#e9f3ee;padding:10px;border-radius:6px;font-size:15px;margin-top:4px}
button{margin-top:20px;width:100%%;background:#39ff8a;color:#04130a;border:none;padding:12px;border-radius:8px;font-size:16px;font-weight:700;cursor:pointer}
.alt{background:#1c2622;color:#e9f3ee}.section{margin-top:28px;padding-top:20px;border-top:1px solid #1c2622}
.ip{font-family:monospace;color:#39ff8a}.hint{color:#74867e;font-size:12px;margin-top:14px;line-height:1.6}
</style></head><body>
<h2>Flight Radar</h2><p class="sub">Device: <span class="ip">%s</span></p>
<form method="POST" action="/save">
<label>Home latitude</label><input name="lat" value="%.4f">
<label>Home longitude</label><input name="lon" value="%.4f">
<label>Radar range (nm)</label><input name="range" value="%.1f">
<button>Save &amp; apply</button></form>
<div class="section">
<form method="POST" action="/changewifi">
<label>WiFi SSID</label><input name="ssid" value="%s" autocomplete="username">
<label>WiFi password</label><input name="pass" type="password" autocomplete="current-password">
<button class="alt">Update WiFi &amp; reboot</button></form></div>
<p class="hint">Hold BOOT button 5 s to re-enter the browser WiFi setup flow.</p>
</body></html>)", ip, homeLat, homeLon, radarRangeNm, savedSSID);
  settingsServer.send(200, "text/html", html);
}
void handleSave() {
  if (settingsServer.hasArg("lat"))   homeLat      = settingsServer.arg("lat").toFloat();
  if (settingsServer.hasArg("lon"))   homeLon      = settingsServer.arg("lon").toFloat();
  if (settingsServer.hasArg("range")) radarRangeNm = settingsServer.arg("range").toFloat();
  if (radarRangeNm<5) radarRangeNm=25;
  saveConfig();
  settingsServer.sendHeader("Location","/"); settingsServer.send(303);
}
void handleChangeWifi() {
  if (settingsServer.hasArg("ssid") && settingsServer.arg("ssid").length()>0) {
    settingsServer.arg("ssid").toCharArray(savedSSID,sizeof(savedSSID));
    settingsServer.arg("pass").toCharArray(savedPass,sizeof(savedPass));
    saveConfig();
  }
  settingsServer.send(200,"text/html",
    "<html><body style='font-family:sans-serif;background:#06090a;color:#39ff8a;text-align:center;padding-top:80px'>"
    "<p>WiFi updated. Rebooting...</p></body></html>");
  delay(500); ESP.restart();
}

// --- ADS-B ---
struct Aircraft { char flight[10]; float distNm, bearingDeg; int altFt, gsKt; };
Aircraft fleet[MAX_AC]; int fleetCount = 0;
float sweepAngle = 0;
unsigned long lastFetch = 0, lastDraw = 0;
const unsigned long FETCH_INTERVAL_MS = 20000, DRAW_INTERVAL_MS = 1000;

float toRad(float d){return d*PI/180.0;} float toDeg(float r){return r*180.0/PI;}
float haversineNm(float la1,float lo1,float la2,float lo2){
  const float R=3440.065; float dLa=toRad(la2-la1),dLo=toRad(lo2-lo1);
  float a=sin(dLa/2)*sin(dLa/2)+cos(toRad(la1))*cos(toRad(la2))*sin(dLo/2)*sin(dLo/2);
  return R*2*atan2(sqrt(a),sqrt(1-a));
}
float bearingDeg(float la1,float lo1,float la2,float lo2){
  float dLo=toRad(lo2-lo1),y=sin(dLo)*cos(toRad(la2));
  float x=cos(toRad(la1))*sin(toRad(la2))-sin(toRad(la1))*cos(toRad(la2))*cos(dLo);
  return fmod(toDeg(atan2(y,x))+360.0,360.0);
}

void fetchAircraft() {
  if (WiFi.status()!=WL_CONNECTED) return;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; char url[160];
  snprintf(url,sizeof(url),"https://api.adsb.lol/v2/point/%.4f/%.4f/%d",homeLat,homeLon,(int)radarRangeNm);
  if (!http.begin(client,url)) return;
  if (http.GET()!=200){http.end();return;}
  StaticJsonDocument<256> filter;
  filter["ac"][0]["flight"]=true;filter["ac"][0]["lat"]=true;filter["ac"][0]["lon"]=true;
  filter["ac"][0]["alt_baro"]=true;filter["ac"][0]["gs"]=true;
  DynamicJsonDocument doc(10240);
  if (deserializeJson(doc,http.getStream(),DeserializationOption::Filter(filter))){http.end();return;}
  http.end();
  JsonArray ac=doc["ac"].as<JsonArray>(); fleetCount=0;
  for (JsonObject a:ac){
    if(fleetCount>=MAX_AC||!a.containsKey("lat")||!a.containsKey("lon")) continue;
    Aircraft &cur=fleet[fleetCount];
    const char* fl=a["flight"]|""; strncpy(cur.flight,fl,9); cur.flight[9]=0;
    for(int i=strlen(cur.flight)-1;i>=0&&cur.flight[i]==' ';i--) cur.flight[i]=0;
    cur.distNm=haversineNm(homeLat,homeLon,a["lat"],a["lon"]);
    cur.bearingDeg=bearingDeg(homeLat,homeLon,a["lat"],a["lon"]);
    cur.altFt=a["alt_baro"].is<int>()?a["alt_baro"].as<int>():0;
    cur.gsKt=(int)(a["gs"].as<float>()+0.5); fleetCount++;
  }
  for(int i=1;i<fleetCount;i++){Aircraft key=fleet[i];int j=i-1;
    while(j>=0&&fleet[j].distNm>key.distNm){fleet[j+1]=fleet[j];j--;}fleet[j+1]=key;}
}

uint16_t altColor(int alt){
  if(alt<=0)return TFT_DARKGREY;if(alt<5000)return TFT_RED;
  if(alt<15000)return TFT_ORANGE;if(alt<30000)return TFT_YELLOW;return TFT_CYAN;
}
void drawRadar(){
  tft.fillScreen(TFT_BLACK);
  tft.drawCircle(CX,CY,MAXR,TFT_DARKGREY);
  tft.drawCircle(CX,CY,MAXR*2/3,TFT_DARKGREY);
  tft.drawCircle(CX,CY,MAXR/3,TFT_DARKGREY);
  tft.setTextColor(TFT_DARKGREY,TFT_BLACK);tft.setTextDatum(MC_DATUM);
  tft.drawString("N",CX,CY-MAXR+8);tft.drawString("S",CX,CY+MAXR-8);
  tft.drawString("E",CX+MAXR-8,CY);tft.drawString("W",CX-MAXR+8,CY);
  float rad=toRad(sweepAngle);
  tft.drawLine(CX,CY,CX+MAXR*sin(rad),CY-MAXR*cos(rad),TFT_DARKGREEN);
  for(int i=0;i<fleetCount;i++){
    Aircraft &a=fleet[i];float r=(a.distNm/radarRangeNm)*MAXR;if(r>MAXR)continue;
    float b=toRad(a.bearingDeg);
    tft.fillCircle(CX+r*sin(b),CY-r*cos(b),3,altColor(a.altFt));
  }
  tft.fillRect(0,206,240,34,TFT_BLACK);tft.setTextColor(TFT_WHITE,TFT_BLACK);
  if(fleetCount>0){Aircraft &n=fleet[0];char l1[40],l2[40];
    snprintf(l1,40,"%s  FL%03d",strlen(n.flight)?n.flight:"----",n.altFt/100);
    snprintf(l2,40,"%.1fnm  %dkt",n.distNm,n.gsKt);
    tft.drawString(l1,CX,214);tft.drawString(l2,CX,228);
  } else tft.drawString("No traffic",CX,220);
  sweepAngle+=12;if(sweepAngle>=360)sweepAngle-=360;
}

void setup(){
  pinMode(BOOT_BTN,INPUT_PULLUP);
  tft.init(); tft.setRotation(0);
  loadConfig();
  if (strlen(savedSSID)==0) {
    runImprovMode();
  } else {
    if (!connectSaved()) runImprovMode();
  }
  // show IP briefly
  char ipStr[20]; WiFi.localIP().toString().toCharArray(ipStr,sizeof(ipStr));
  showMessage("Connected!", ipStr, "visit in browser");
  delay(3500);
  settingsServer.on("/",handleRoot);
  settingsServer.on("/save",HTTP_POST,handleSave);
  settingsServer.on("/changewifi",HTTP_POST,handleChangeWifi);
  settingsServer.begin();
  fetchAircraft(); lastFetch=millis();
}

bool connectSaved(){
  if(strlen(savedSSID)==0) return false;
  showMessage("Connecting...",savedSSID);
  WiFi.begin(savedSSID,savedPass);
  unsigned long t=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-t<12000) delay(200);
  return WiFi.status()==WL_CONNECTED;
}

void loop(){
  unsigned long now=millis();
  settingsServer.handleClient();
  // boot button hold → clear creds → re-enter Improv
  if(digitalRead(BOOT_BTN)==LOW){
    if(btnHoldStart==0)btnHoldStart=now;
    if(!btnWasHeld&&now-btnHoldStart>5000){
      btnWasHeld=true;showMessage("Clearing WiFi...","Restarting");
      savedSSID[0]=0;savedPass[0]=0;saveConfig();delay(800);ESP.restart();
    }
  }else{btnHoldStart=0;btnWasHeld=false;}
  if(WiFi.status()!=WL_CONNECTED)ESP.restart();
  if(now-lastFetch>=FETCH_INTERVAL_MS||lastFetch==0){fetchAircraft();lastFetch=now;}
  if(now-lastDraw>=DRAW_INTERVAL_MS){drawRadar();lastDraw=now;}
}
