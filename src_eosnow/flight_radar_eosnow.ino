/*
  Flight Radar — Eosnow / Waveshare ESP32-S3-Touch-LCD-1.85 (360x360 round)
  ST77916 display over QSPI, CST816 touch, TCA9554 IO expander.
  v1.4.0 — Improv WiFi setup, settings web server.

  Board: ESP32S3 Dev Module | PSRAM: OPI | Flash: 16MB QIO
  Display driver is ESP-IDF esp_lcd_st77916 (QSPI) — vendored locally.
  We render into a PSRAM framebuffer and push it with LCD_addWindow().
*/
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ImprovWiFiLibrary.h>
#include <math.h>

#include "Display_ST77916.h"

WebServer settingsServer(80);
ImprovWiFi improvSerial(&Serial);

#define SCR 360
#define CXc 180
#define CYc 168
#define MAXR 165
#define BANNER_Y 330
#define MAX_AC 24
#define CONFIG_FILE "/config.json"
#define BOOT_BTN 0

// ---- RGB565 framebuffer in PSRAM (one full frame = 360*360*2 = 259KB) ----
static uint16_t* fb = nullptr;

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
// palette
#define C_BLACK   0x0000
#define C_DGREY   0x39E7
#define C_GREEN   0x07E0
#define C_DGREEN  0x03E0
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_ORANGE  0xFD20
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF

float homeLat=-37.8136, homeLon=144.9631, radarRangeNm=25.0;
char savedSSID[64]="", savedPass[64]="";
unsigned long btnHoldStart=0; bool btnWasHeld=false;
float sweepAngle=0;
unsigned long lastFetch=0, lastDraw=0;
const unsigned long FETCH_INTERVAL_MS=20000, DRAW_INTERVAL_MS=1000;

struct Aircraft{char flight[10];float distNm,bearingDeg;int altFt,gsKt;};
Aircraft fleet[MAX_AC]; int fleetCount=0;

// ---- framebuffer primitives ----
static inline void fbPix(int x,int y,uint16_t c){
  if(x<0||x>=SCR||y<0||y>=SCR) return;
  fb[y*SCR+x]=c;
}
void fbClear(uint16_t c){ for(int i=0;i<SCR*SCR;i++) fb[i]=c; }

void fbCircle(int cx,int cy,int r,uint16_t c){
  int x=r,y=0,err=1-r;
  while(x>=y){
    fbPix(cx+x,cy+y,c);fbPix(cx-x,cy+y,c);fbPix(cx+x,cy-y,c);fbPix(cx-x,cy-y,c);
    fbPix(cx+y,cy+x,c);fbPix(cx-y,cy+x,c);fbPix(cx+y,cy-x,c);fbPix(cx-y,cy-x,c);
    y++; if(err<0) err+=2*y+1; else {x--;err+=2*(y-x)+1;}
  }
}
void fbFillCircle(int cx,int cy,int r,uint16_t c){
  for(int y=-r;y<=r;y++) for(int x=-r;x<=r;x++)
    if(x*x+y*y<=r*r) fbPix(cx+x,cy+y,c);
}
void fbLine(int x0,int y0,int x1,int y1,uint16_t c){
  int dx=abs(x1-x0),dy=-abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx+dy;
  while(1){ fbPix(x0,y0,c); if(x0==x1&&y0==y1) break;
    int e2=2*err; if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} }
}

// ---- minimal 5x7 font ----
#include "font5x7.h"
void fbChar(int x,int y,char ch,uint16_t c,int sz){
  if(ch<32||ch>126) ch='?';
  const uint8_t* g=FONT5X7[ch-32];
  for(int col=0;col<5;col++){
    uint8_t bits=g[col];
    for(int row=0;row<7;row++){
      if(bits&(1<<row))
        for(int a=0;a<sz;a++) for(int b=0;b<sz;b++) fbPix(x+col*sz+a,y+row*sz+b,c);
    }
  }
}
void fbText(int x,int y,const char* s,uint16_t c,int sz){
  int cx=x; while(*s){ fbChar(cx,y,*s,c,sz); cx+=6*sz; s++; }
}
void fbTextC(int cx,int y,const char* s,uint16_t c,int sz){
  int w=strlen(s)*6*sz; fbText(cx-w/2,y,s,c,sz);
}

void pushFrame(){ LCD_addWindow(0,0,SCR-1,SCR-1,fb); }

// ---- config ----
void loadConfig(){
  if(!LittleFS.begin(true)) return;
  if(!LittleFS.exists(CONFIG_FILE)) return;
  File f=LittleFS.open(CONFIG_FILE,"r"); if(!f) return;
  StaticJsonDocument<384> doc;
  if(!deserializeJson(doc,f)){
    homeLat=doc["lat"]|homeLat; homeLon=doc["lon"]|homeLon;
    radarRangeNm=doc["range"]|radarRangeNm;
    strlcpy(savedSSID,doc["ssid"]|"",sizeof(savedSSID));
    strlcpy(savedPass,doc["pass"]|"",sizeof(savedPass));
  } f.close();
}
void saveConfig(){
  LittleFS.begin(true);
  StaticJsonDocument<384> doc;
  doc["lat"]=homeLat;doc["lon"]=homeLon;doc["range"]=radarRangeNm;
  doc["ssid"]=savedSSID;doc["pass"]=savedPass;
  File f=LittleFS.open(CONFIG_FILE,"w"); if(f){serializeJson(doc,f);f.close();}
}

void centerMsg(const char* l1,const char* l2,const char* l3){
  fbClear(C_BLACK);
  if(l1&&*l1) fbTextC(CXc,150,l1,C_WHITE,2);
  if(l2&&*l2) fbTextC(CXc,180,l2,C_GREEN,2);
  if(l3&&*l3) fbTextC(CXc,210,l3,C_DGREY,2);
  pushFrame();
}

bool improvConnectCallback(const char* ssid,const char* pass){
  centerMsg("Connecting...",ssid,"");
  WiFi.begin(ssid,pass);
  unsigned long t=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-t<15000) delay(200);
  if(WiFi.status()!=WL_CONNECTED){centerMsg("Failed",ssid,"");return false;}
  strlcpy(savedSSID,ssid,sizeof(savedSSID));strlcpy(savedPass,pass,sizeof(savedPass));
  saveConfig(); return true;
}
void runImprovMode(){
  Serial.begin(115200);
  improvSerial.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32_S3,"FlightRadar","1.4","Flight Radar");
  improvSerial.setCustomConnectWiFi(improvConnectCallback);
  centerMsg("Connect WiFi via","installer or","visit device page");
  unsigned long start=millis();
  while(WiFi.status()!=WL_CONNECTED){
    improvSerial.handleSerial();
    if(millis()-start>300000) ESP.restart();
    delay(10);
  }
}
bool connectSaved(){
  if(strlen(savedSSID)==0) return false;
  centerMsg("Connecting...",savedSSID,"");
  WiFi.begin(savedSSID,savedPass);
  unsigned long t=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-t<12000) delay(200);
  return WiFi.status()==WL_CONNECTED;
}

void handleRoot(){
  char ip[20]; WiFi.localIP().toString().toCharArray(ip,sizeof(ip));
  char html[2600]; snprintf(html,sizeof(html),R"(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flight Radar</title>
<style>body{font-family:sans-serif;background:#06090a;color:#e9f3ee;max-width:440px;margin:40px auto;padding:0 20px}
h2{color:#39ff8a;margin-bottom:4px}.sub{color:#74867e;font-size:13px;margin-top:0}
label{display:block;margin-top:16px;color:#74867e;font-size:13px}
input{width:100%%;box-sizing:border-box;background:#0d1412;border:1px solid #1c2622;color:#e9f3ee;padding:10px;border-radius:6px;font-size:15px;margin-top:4px}
button{margin-top:20px;width:100%%;background:#39ff8a;color:#04130a;border:none;padding:12px;border-radius:8px;font-size:16px;font-weight:700;cursor:pointer}
.alt{background:#1c2622;color:#e9f3ee}.section{margin-top:28px;padding-top:20px;border-top:1px solid #1c2622}
.ip{font-family:monospace;color:#39ff8a}.hint{color:#74867e;font-size:12px;margin-top:14px}
</style></head><body>
<h2>Flight Radar</h2><p class="sub">Device: <span class="ip">%s</span></p>
<form method="POST" action="/save">
<label>Home latitude</label><input name="lat" value="%.4f">
<label>Home longitude</label><input name="lon" value="%.4f">
<label>Radar range (nm)</label><input name="range" value="%.1f">
<button>Save &amp; apply</button></form>
<div class="section"><form method="POST" action="/changewifi">
<label>WiFi SSID</label><input name="ssid" value="%s" autocomplete="username">
<label>WiFi password</label><input name="pass" type="password" autocomplete="current-password">
<button class="alt">Update WiFi &amp; reboot</button></form></div>
<p class="hint">Hold BOOT button 5 s to re-enter browser WiFi setup.</p>
</body></html>)",ip,homeLat,homeLon,radarRangeNm,savedSSID);
  settingsServer.send(200,"text/html",html);
}
void handleSave(){
  if(settingsServer.hasArg("lat"))   homeLat     =settingsServer.arg("lat").toFloat();
  if(settingsServer.hasArg("lon"))   homeLon     =settingsServer.arg("lon").toFloat();
  if(settingsServer.hasArg("range")) radarRangeNm=settingsServer.arg("range").toFloat();
  if(radarRangeNm<5) radarRangeNm=25; saveConfig();
  settingsServer.sendHeader("Location","/"); settingsServer.send(303);
  lastFetch=0;
}
void handleChangeWifi(){
  if(settingsServer.hasArg("ssid")&&settingsServer.arg("ssid").length()>0){
    settingsServer.arg("ssid").toCharArray(savedSSID,sizeof(savedSSID));
    settingsServer.arg("pass").toCharArray(savedPass,sizeof(savedPass));
    saveConfig();
  }
  settingsServer.send(200,"text/html","<html><body style='font-family:sans-serif;background:#06090a;color:#39ff8a;text-align:center;padding-top:80px'><p>Rebooting...</p></body></html>");
  delay(500); ESP.restart();
}

float toRad(float d){return d*PI/180.0;} float toDeg(float r){return r*180.0/PI;}
float haversineNm(float la1,float lo1,float la2,float lo2){
  const float R=3440.065;float dLa=toRad(la2-la1),dLo=toRad(lo2-lo1);
  float a=sin(dLa/2)*sin(dLa/2)+cos(toRad(la1))*cos(toRad(la2))*sin(dLo/2)*sin(dLo/2);
  return R*2*atan2(sqrt(a),sqrt(1-a));
}
float bearingDeg(float la1,float lo1,float la2,float lo2){
  float dLo=toRad(lo2-lo1),y=sin(dLo)*cos(toRad(la2));
  float x=cos(toRad(la1))*sin(toRad(la2))-sin(toRad(la1))*cos(toRad(la2))*cos(dLo);
  return fmod(toDeg(atan2(y,x))+360.0,360.0);
}

void fetchAircraft(){
  if(WiFi.status()!=WL_CONNECTED) return;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; char url[160];
  snprintf(url,sizeof(url),"https://api.adsb.lol/v2/point/%.4f/%.4f/%d",homeLat,homeLon,(int)radarRangeNm);
  if(!http.begin(client,url)) return;
  if(http.GET()!=200){http.end();return;}
  StaticJsonDocument<256> filter;
  filter["ac"][0]["flight"]=true;filter["ac"][0]["lat"]=true;filter["ac"][0]["lon"]=true;
  filter["ac"][0]["alt_baro"]=true;filter["ac"][0]["gs"]=true;
  DynamicJsonDocument doc(10240);
  if(deserializeJson(doc,http.getStream(),DeserializationOption::Filter(filter))){http.end();return;}
  http.end();
  JsonArray ac=doc["ac"].as<JsonArray>(); fleetCount=0;
  for(JsonObject a:ac){
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

uint16_t altColor(int a){
  if(a<=0)return C_DGREY;if(a<5000)return C_RED;
  if(a<15000)return C_ORANGE;if(a<30000)return C_YELLOW;return C_CYAN;
}

void drawRadar(){
  fbClear(C_BLACK);
  fbCircle(CXc,CYc,MAXR,C_DGREY);
  fbCircle(CXc,CYc,MAXR*2/3,C_DGREY);
  fbCircle(CXc,CYc,MAXR/3,C_DGREY);
  fbTextC(CXc,CYc-MAXR+6,"N",C_DGREY,2);
  fbTextC(CXc,CYc+MAXR-18,"S",C_DGREY,2);
  fbTextC(CXc+MAXR-12,CYc-6,"E",C_DGREY,2);
  fbTextC(CXc-MAXR+12,CYc-6,"W",C_DGREY,2);
  float rad=toRad(sweepAngle);
  fbLine(CXc,CYc,CXc+MAXR*sin(rad),CYc-MAXR*cos(rad),C_DGREEN);
  for(int i=0;i<fleetCount;i++){
    Aircraft &a=fleet[i];float r=(a.distNm/radarRangeNm)*MAXR;if(r>MAXR)continue;
    float b=toRad(a.bearingDeg);
    fbFillCircle(CXc+r*sin(b),CYc-r*cos(b),5,altColor(a.altFt));
  }
  if(fleetCount==0){ fbTextC(CXc,BANNER_Y,"No traffic nearby",C_WHITE,2); }
  else{
    int rows=min(fleetCount,2);
    for(int i=0;i<rows;i++){Aircraft &a=fleet[i];char ln[48];
      snprintf(ln,48,"%s FL%03d %.0fnm %dkt",strlen(a.flight)?a.flight:"----",a.altFt/100,a.distNm,a.gsKt);
      fbTextC(CXc,BANNER_Y+i*18,ln,altColor(a.altFt),2);
    }
  }
  pushFrame();
  sweepAngle+=12; if(sweepAngle>=360) sweepAngle-=360;
}

void setup(){
  pinMode(BOOT_BTN,INPUT_PULLUP);
  // allocate framebuffer in PSRAM
  fb=(uint16_t*)heap_caps_malloc(SCR*SCR*sizeof(uint16_t),MALLOC_CAP_SPIRAM);
  if(!fb) fb=(uint16_t*)malloc(SCR*SCR*sizeof(uint16_t));
  I2C_Init();
  TCA9554PWR_Init(0x00);
  Backlight_Init();
  LCD_Init();
  loadConfig();
  if(strlen(savedSSID)==0){runImprovMode();}
  else{if(!connectSaved())runImprovMode();}
  char ipStr[20]; WiFi.localIP().toString().toCharArray(ipStr,sizeof(ipStr));
  centerMsg("Connected!",ipStr,"visit in browser");
  delay(3500);
  settingsServer.on("/",handleRoot);
  settingsServer.on("/save",HTTP_POST,handleSave);
  settingsServer.on("/changewifi",HTTP_POST,handleChangeWifi);
  settingsServer.begin();
  fetchAircraft(); lastFetch=millis();
}

void loop(){
  unsigned long now=millis();
  settingsServer.handleClient();
  if(digitalRead(BOOT_BTN)==LOW){
    if(btnHoldStart==0)btnHoldStart=now;
    if(!btnWasHeld&&now-btnHoldStart>5000){
      btnWasHeld=true;centerMsg("Clearing WiFi...","Restarting","");
      savedSSID[0]=0;savedPass[0]=0;saveConfig();delay(800);ESP.restart();
    }
  }else{btnHoldStart=0;btnWasHeld=false;}
  if(WiFi.status()!=WL_CONNECTED)ESP.restart();
  if(now-lastFetch>=FETCH_INTERVAL_MS||lastFetch==0){fetchAircraft();lastFetch=now;}
  if(now-lastDraw>=DRAW_INTERVAL_MS){drawRadar();lastDraw=now;}
}
