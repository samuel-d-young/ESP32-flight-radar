/*
  Flight Radar — ESP32-4848S040 (480x480 RGB panel, LovyanGFX)
  v1.3.0 — settings web server at device IP, tap to refresh, boot-button hold.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_RGB _bus; lgfx::Panel_ST7701_guition_esp32_4848S040 _panel;
  lgfx::Light_PWM _light; lgfx::Touch_GT911 _touch;
  LGFX(void){
    {auto c=_panel.config();c.memory_width=480;c.memory_height=480;c.panel_width=480;c.panel_height=480;_panel.config(c);}
    {auto c=_panel.config_detail();c.pin_cs=39;c.pin_sclk=48;c.pin_mosi=47;_panel.config_detail(c);}
    {auto c=_bus.config();c.panel=&_panel;
      c.pin_d0=4;c.pin_d1=5;c.pin_d2=6;c.pin_d3=7;c.pin_d4=15;
      c.pin_d5=8;c.pin_d6=20;c.pin_d7=3;c.pin_d8=46;c.pin_d9=9;c.pin_d10=10;
      c.pin_d11=11;c.pin_d12=12;c.pin_d13=13;c.pin_d14=14;c.pin_d15=0;
      c.pin_henable=18;c.pin_vsync=17;c.pin_hsync=16;c.pin_pclk=21;c.freq_write=14000000;
      c.hsync_polarity=0;c.hsync_front_porch=10;c.hsync_pulse_width=8;c.hsync_back_porch=50;
      c.vsync_polarity=0;c.vsync_front_porch=10;c.vsync_pulse_width=8;c.vsync_back_porch=20;
      c.pclk_active_neg=1;c.de_idle_high=0;c.pclk_idle_high=0;_bus.config(c);}
    _panel.setBus(&_bus);
    {auto c=_light.config();c.pin_bl=38;c.freq=150;_light.config(c);_panel.setLight(&_light);}
    {auto c=_touch.config();c.x_min=0;c.x_max=479;c.y_min=0;c.y_max=479;
      c.pin_int=-1;c.pin_rst=-1;c.bus_shared=false;
      c.i2c_port=1;c.i2c_addr=0x5D;c.pin_sda=19;c.pin_scl=45;c.freq=400000;
      _touch.config(c);_panel.setTouch(&_touch);}
    setPanel(&_panel);
  }
};

LGFX tft;
WebServer settingsServer(80);

#define MAX_AC 24
#define CONFIG_FILE "/config.json"
#define BOOT_BTN 0

const int CX=240,CY=232,MAXR=210,BANNER_Y=446;
struct Aircraft{char flight[10];float distNm,bearingDeg;int altFt,gsKt;};
Aircraft fleet[MAX_AC]; int fleetCount=0;
float sweepAngle=0;
unsigned long lastFetch=0,lastDraw=0,lastTouch=0,btnHoldStart=0;
bool btnWasHeld=false;
const unsigned long FETCH_INTERVAL_MS=20000,DRAW_INTERVAL_MS=1000,TOUCH_DB=3000;
float homeLat=-37.8136,homeLon=144.9631,radarRangeNm=25.0;
bool shouldSaveConfig=false;
void saveConfigCallback(){shouldSaveConfig=true;}

void loadConfig(){
  if(!LittleFS.begin(true)) return;
  if(!LittleFS.exists(CONFIG_FILE)) return;
  File f=LittleFS.open(CONFIG_FILE,"r"); if(!f) return;
  StaticJsonDocument<256> doc;
  if(!deserializeJson(doc,f)){homeLat=doc["lat"]|homeLat;homeLon=doc["lon"]|homeLon;radarRangeNm=doc["range"]|radarRangeNm;}
  f.close();
}
void saveConfig(){
  StaticJsonDocument<256> doc; doc["lat"]=homeLat;doc["lon"]=homeLon;doc["range"]=radarRangeNm;
  File f=LittleFS.open(CONFIG_FILE,"w"); if(f){serializeJson(doc,f);f.close();}
}

void handleRoot(){
  char ip[20]; WiFi.localIP().toString().toCharArray(ip,sizeof(ip));
  char html[2048];
  snprintf(html,sizeof(html),R"(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flight Radar Settings</title>
<style>body{font-family:sans-serif;background:#06090a;color:#e9f3ee;max-width:420px;margin:40px auto;padding:0 20px}
h2{color:#39ff8a;margin-bottom:4px}p.sub{color:#74867e;font-size:13px;margin-top:0}
label{display:block;margin-top:16px;color:#74867e;font-size:13px}
input{width:100%%;box-sizing:border-box;background:#0d1412;border:1px solid #1c2622;color:#e9f3ee;padding:10px;border-radius:6px;font-size:15px;margin-top:4px}
button{margin-top:24px;width:100%%;background:#39ff8a;color:#04130a;border:none;padding:12px;border-radius:8px;font-size:16px;font-weight:700;cursor:pointer}
.ip{font-family:monospace;color:#39ff8a}</style></head>
<body><h2>Flight Radar</h2><p class="sub">Device IP: <span class="ip">%s</span></p>
<form method="POST" action="/save">
<label>Home latitude</label><input name="lat" value="%.4f">
<label>Home longitude</label><input name="lon" value="%.4f">
<label>Radar range (nautical miles)</label><input name="range" value="%.1f">
<button>Save &amp; apply</button></form>
<p style="margin-top:24px;color:#74867e;font-size:12px">Tap the screen to force a data refresh. Hold BOOT button 5s to re-open WiFi setup.</p>
</body></html>)",ip,homeLat,homeLon,radarRangeNm);
  settingsServer.send(200,"text/html",html);
}
void handleSave(){
  if(settingsServer.hasArg("lat"))   homeLat     =settingsServer.arg("lat").toFloat();
  if(settingsServer.hasArg("lon"))   homeLon     =settingsServer.arg("lon").toFloat();
  if(settingsServer.hasArg("range")) radarRangeNm=settingsServer.arg("range").toFloat();
  if(radarRangeNm<5) radarRangeNm=25;
  saveConfig();
  settingsServer.sendHeader("Location","/"); settingsServer.send(303);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_GREEN,TFT_BLACK);
  tft.setTextSize(2); tft.drawString("Settings saved!",CX,CY); tft.setTextSize(1);
  lastFetch=0;
}

float toRad(float d){return d*PI/180.0;} float toDeg(float r){return r*180.0/PI;}
float haversineNm(float la1,float lo1,float la2,float lo2){
  const float R=3440.065;float dLa=toRad(la2-la1),dLo=toRad(lo2-lo1);
  float a=sin(dLa/2)*sin(dLa/2)+cos(toRad(la1))*cos(toRad(la2))*sin(dLo/2)*sin(dLo/2);
  return R*2*atan2(sqrt(a),sqrt(1-a));
}
float bearingDegFrom(float la1,float lo1,float la2,float lo2){
  float dLo=toRad(lo2-lo1),y=sin(dLo)*cos(toRad(la2));
  float x=cos(toRad(la1))*sin(toRad(la2))-sin(toRad(la1))*cos(toRad(la2))*cos(dLo);
  return fmod(toDeg(atan2(y,x))+360.0,360.0);
}

void setupWiFi(){
  loadConfig();
  char laBuf[16],loBuf[16],rBuf[8];
  dtostrf(homeLat,1,4,laBuf);dtostrf(homeLon,1,4,loBuf);dtostrf(radarRangeNm,1,1,rBuf);
  WiFiManagerParameter p_lat("lat","Home latitude",laBuf,16);
  WiFiManagerParameter p_lon("lon","Home longitude",loBuf,16);
  WiFiManagerParameter p_range("range","Radar range (nm)",rBuf,8);
  WiFiManager wm; wm.setSaveConfigCallback(saveConfigCallback);
  wm.addParameter(&p_lat);wm.addParameter(&p_lon);wm.addParameter(&p_range);
  wm.setConfigPortalTimeout(180);
  tft.fillScreen(TFT_BLACK);tft.setTextColor(TFT_WHITE,TFT_BLACK);tft.setTextDatum(MC_DATUM);tft.setTextSize(2);
  tft.drawString("Connect WiFi to:",240,210);tft.drawString("FlightRadar-Setup",240,245);tft.drawString("to configure",240,280);
  tft.setTextSize(1);
  bool ok=wm.autoConnect("FlightRadar-Setup");
  homeLat=atof(p_lat.getValue());homeLon=atof(p_lon.getValue());
  radarRangeNm=atof(p_range.getValue());if(radarRangeNm<5)radarRangeNm=25;
  if(shouldSaveConfig)saveConfig();
  tft.fillScreen(TFT_BLACK);
  if(!ok){tft.setTextSize(2);tft.drawString("WiFi failed",240,240);delay(3000);ESP.restart();}
  tft.setTextSize(2);tft.setTextColor(TFT_GREEN,TFT_BLACK);tft.drawString("Connected!",240,210);
  tft.setTextColor(TFT_WHITE,TFT_BLACK);tft.drawString(WiFi.localIP().toString(),240,248);
  tft.setTextColor(TFT_DARKGREY,TFT_BLACK);tft.drawString("visit in browser",240,286);
  tft.setTextSize(1);delay(4000);
}

void fetchAircraft(){
  if(WiFi.status()!=WL_CONNECTED) return;
  WiFiClientSecure client;client.setInsecure();
  HTTPClient http;char url[160];
  snprintf(url,sizeof(url),"https://api.adsb.lol/v2/point/%.4f/%.4f/%d",homeLat,homeLon,(int)radarRangeNm);
  if(!http.begin(client,url)) return;
  if(http.GET()!=200){http.end();return;}
  StaticJsonDocument<256> filter;
  filter["ac"][0]["flight"]=true;filter["ac"][0]["lat"]=true;filter["ac"][0]["lon"]=true;
  filter["ac"][0]["alt_baro"]=true;filter["ac"][0]["gs"]=true;
  DynamicJsonDocument doc(10240);
  if(deserializeJson(doc,http.getStream(),DeserializationOption::Filter(filter))){http.end();return;}
  http.end();
  JsonArray ac=doc["ac"].as<JsonArray>();fleetCount=0;
  for(JsonObject a:ac){
    if(fleetCount>=MAX_AC||!a.containsKey("lat")||!a.containsKey("lon")) continue;
    Aircraft &cur=fleet[fleetCount];
    const char* fl=a["flight"]|"";strncpy(cur.flight,fl,9);cur.flight[9]=0;
    for(int i=strlen(cur.flight)-1;i>=0&&cur.flight[i]==' ';i--)cur.flight[i]=0;
    cur.distNm=haversineNm(homeLat,homeLon,a["lat"].as<float>(),a["lon"].as<float>());
    cur.bearingDeg=bearingDegFrom(homeLat,homeLon,a["lat"].as<float>(),a["lon"].as<float>());
    cur.altFt=a["alt_baro"].is<int>()?a["alt_baro"].as<int>():0;
    cur.gsKt=(int)(a["gs"].as<float>()+0.5);fleetCount++;
  }
  for(int i=1;i<fleetCount;i++){Aircraft key=fleet[i];int j=i-1;
    while(j>=0&&fleet[j].distNm>key.distNm){fleet[j+1]=fleet[j];j--;}fleet[j+1]=key;}
}

uint16_t altColor(int alt){
  if(alt<=0)return TFT_DARKGREY;if(alt<5000)return TFT_RED;
  if(alt<15000)return TFT_ORANGE;if(alt<30000)return TFT_YELLOW;return TFT_CYAN;
}

void drawRadar(){
  tft.startWrite();tft.fillScreen(TFT_BLACK);
  tft.drawCircle(CX,CY,MAXR,TFT_DARKGREY);
  tft.drawCircle(CX,CY,MAXR*2/3,TFT_DARKGREY);
  tft.drawCircle(CX,CY,MAXR/3,TFT_DARKGREY);
  tft.setTextColor(TFT_DARKGREY,TFT_BLACK);tft.setTextDatum(MC_DATUM);tft.setTextSize(2);
  tft.drawString("N",CX,CY-MAXR+16);tft.drawString("S",CX,CY+MAXR-16);
  tft.drawString("E",CX+MAXR-16,CY);tft.drawString("W",CX-MAXR+16,CY);
  float rad=toRad(sweepAngle);
  tft.drawLine(CX,CY,CX+MAXR*sin(rad),CY-MAXR*cos(rad),TFT_DARKGREEN);
  for(int i=0;i<fleetCount;i++){
    Aircraft &a=fleet[i];float r=(a.distNm/radarRangeNm)*MAXR;if(r>MAXR)continue;
    float brad=toRad(a.bearingDeg);
    tft.fillCircle(CX+r*sin(brad),CY-r*cos(brad),6,altColor(a.altFt));
  }
  tft.fillRect(0,BANNER_Y,480,480-BANNER_Y,TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  if(fleetCount==0){tft.setTextColor(TFT_WHITE,TFT_BLACK);tft.drawString("No traffic nearby",16,BANNER_Y+4);}
  else{int rows=min(fleetCount,2);
    for(int i=0;i<rows;i++){Aircraft &a=fleet[i];char ln[48];
      snprintf(ln,48,"%-8s FL%03d  %.1fnm %dkt",strlen(a.flight)?a.flight:"----",a.altFt/100,a.distNm,a.gsKt);
      tft.setTextColor(altColor(a.altFt),TFT_BLACK);tft.drawString(ln,16,BANNER_Y+4+i*17);
    }
  }
  tft.setTextSize(1);tft.setTextDatum(MC_DATUM);tft.endWrite();
  sweepAngle+=12;if(sweepAngle>=360)sweepAngle-=360;
}

void setup(){
  pinMode(BOOT_BTN,INPUT_PULLUP);
  tft.init();tft.setBrightness(255);
  setupWiFi();
  settingsServer.on("/",handleRoot);
  settingsServer.on("/save",HTTP_POST,handleSave);
  settingsServer.begin();
  fetchAircraft();lastFetch=millis();
}

void loop(){
  unsigned long now=millis();
  settingsServer.handleClient();
  int32_t tx,ty;
  if(tft.getTouch(&tx,&ty)&&now-lastTouch>TOUCH_DB){
    lastTouch=now;
    tft.setTextDatum(MC_DATUM);tft.setTextColor(TFT_GREEN,TFT_BLACK);
    tft.setTextSize(2);tft.drawString("Refreshing...",CX,CY);tft.setTextSize(1);
    fetchAircraft();lastFetch=now;
  }
  if(digitalRead(BOOT_BTN)==LOW){
    if(btnHoldStart==0)btnHoldStart=now;
    if(now-btnHoldStart>5000&&!btnWasHeld){
      btnWasHeld=true;tft.fillScreen(TFT_BLACK);tft.setTextDatum(MC_DATUM);
      tft.setTextSize(2);tft.setTextColor(TFT_WHITE,TFT_BLACK);
      tft.drawString("Opening WiFi portal...",240,240);tft.setTextSize(1);
      WiFiManager wm;wm.setConfigPortalTimeout(180);wm.startConfigPortal("FlightRadar-Setup");
      ESP.restart();
    }
  }else{btnHoldStart=0;btnWasHeld=false;}
  if(WiFi.status()!=WL_CONNECTED)ESP.restart();
  if(now-lastFetch>=FETCH_INTERVAL_MS||lastFetch==0){fetchAircraft();lastFetch=now;}
  if(now-lastDraw>=DRAW_INTERVAL_MS){drawRadar();lastDraw=now;}
}
