// DiSEqC 1.2 dish rotator for ESP32
// Ayushman Tripathi, www.radioastronomy.in
// Free for anyone to use, for anything. No warranty.

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"
#define OTA_PASS  "change-me"
#endif

#define DISEQC_PIN   25

#define TONE_FREQ  22000
#define TONE_RES   8
#define TONE_DUTY  128

#define MAX_DEG    75.0f
#define MAX_STEPS  128

WebServer server(80);

String lastCmd = "none";
float  targetDeg = 0;
unsigned long bootMs = 0;

Preferences prefs;
float posDeg = 0;
float moveTo = 0;
bool  moving = false;
float refDeg = 0;
float degPerSec  = 1.9f;
float degPerStep = 0.1f;
float azZero = NAN;
int   azDir  = 1;
unsigned long lastTick = 0, lastSave = 0;

static void savePos() { prefs.putFloat("pos", posDeg); lastSave = millis(); }

static void tick() {
  unsigned long now = millis();
  if (moving) {
    float step = degPerSec * (now - lastTick) / 1000.0f;
    float d = moveTo - posDeg;
    if (fabsf(d) <= step) { posDeg = moveTo; moving = false; savePos(); }
    else { posDeg += d > 0 ? step : -step; if (now - lastSave > 5000) savePos(); }
  }
  lastTick = now;
}

static float azOf(float deg) {
  float a = fmodf(azZero + azDir * deg, 360.0f);
  return a < 0 ? a + 360.0f : a;
}

static inline void toneOn()  { ledcWrite(DISEQC_PIN, TONE_DUTY); }
static inline void toneOff() { ledcWrite(DISEQC_PIN, 0); }

static void sendBit(bool one) {
  if (one) { toneOn(); delayMicroseconds(500);  toneOff(); delayMicroseconds(1000); }
  else     { toneOn(); delayMicroseconds(1000); toneOff(); delayMicroseconds(500);  }
}
static void sendByte(uint8_t b) {
  uint8_t ones = 0;
  for (int i = 7; i >= 0; i--) { bool bit = (b >> i) & 1; ones += bit; sendBit(bit); }
  sendBit(!(ones & 1));
}
static void sendFrame(const uint8_t* d, size_t n) {
  toneOff(); delay(15);
  portDISABLE_INTERRUPTS();
  for (size_t i = 0; i < n; i++) sendByte(d[i]);
  portENABLE_INTERRUPTS();
  toneOff(); delay(100);
}

static void trackFrame(const uint8_t* d, size_t n) {
  if (n < 3 || (d[1] != 0x31 && d[1] != 0x30 && d[1] != 0x00)) return;
  tick();
  uint8_t nn = n > 3 ? d[3] : 0;
  float to = posDeg; bool go = false;
  switch (d[2]) {
    case 0x60: moving = false; savePos(); break;
    case 0x68: case 0x69: {
      float span = nn == 0 ? 2 * MAX_DEG : nn >= 0x80 ? (256 - nn) * degPerStep : nn * degPerSec;
      to = posDeg + (d[2] == 0x68 ? span : -span); go = true; break; }
    case 0x6A: if (nn == 0) { refDeg = posDeg; prefs.putFloat("ref", refDeg); } break;
    case 0x6B: if (nn == 0) { to = targetDeg = refDeg; go = true; } break;
    case 0x6E: if (n >= 5) {
      to = (((d[3] & 0x0F) << 8) | d[4]) / 16.0f;
      if ((d[3] & 0xF0) == 0xD0) to = -to;
      targetDeg = to; go = true; } break;
  }
  if (go) { moveTo = constrain(to, -MAX_DEG, MAX_DEG); moving = fabsf(moveTo - posDeg) > 0.01f; }
}
static void send(const uint8_t* d, size_t n) { sendFrame(d, n); trackFrame(d, n); }

static uint8_t stepByte(long s) {
  if (s <= 0) return 0x00;
  if (s > MAX_STEPS) s = MAX_STEPS;
  return (uint8_t)(256 - s);
}
void cmdHalt()            { uint8_t f[] = {0xE0,0x31,0x60}; send(f,3); lastCmd = "halt"; }
void cmdDriveEast(long s) { uint8_t f[] = {0xE0,0x31,0x68,stepByte(s)}; send(f,4); lastCmd = "east " + String(s <= 0 ? 0 : min(s, (long)MAX_STEPS)); }
void cmdDriveWest(long s) { uint8_t f[] = {0xE0,0x31,0x69,stepByte(s)}; send(f,4); lastCmd = "west " + String(s <= 0 ? 0 : min(s, (long)MAX_STEPS)); }
void cmdGotoAngle(float deg) {
  if (isnan(deg)) return;
  deg = constrain(deg, -MAX_DEG, MAX_DEG);
  bool east = deg >= 0;
  uint16_t a = (uint16_t)(fabsf(deg) * 16.0f + 0.5f);
  uint8_t f[] = {0xE0,0x31,0x6E,(uint8_t)((east ? 0xE0 : 0xD0) | ((a >> 8) & 0x0F)),(uint8_t)(a & 0xFF)};
  send(f,5); lastCmd = "goto " + String(deg, 1);
}
void cmdStoreRef() { uint8_t f[] = {0xE0,0x31,0x6A,0x00}; send(f,4); lastCmd = "store ref"; }
void cmdGotoRef()  { uint8_t f[] = {0xE0,0x31,0x6B,0x00}; send(f,4); lastCmd = "goto ref"; }

static bool sendRawHex(String hex) {
  hex.replace(" ", "");
  if (hex.length() == 0 || hex.length() % 2 || hex.length() > 16) return false;
  for (size_t i = 0; i < hex.length(); i++) if (!isxdigit(hex.charAt(i))) return false;
  uint8_t buf[8]; size_t n = 0;
  for (size_t i = 0; i < hex.length(); i += 2) buf[n++] = strtoul(hex.substring(i,i+2).c_str(), nullptr, 16);
  hex.toUpperCase();
  send(buf, n); lastCmd = "raw " + hex;
  return true;
}

static bool isNumber(const String& s) {
  if (!s.length()) return false;
  bool digit = false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (isdigit(c)) digit = true;
    else if (!((c == '-' || c == '+') && i == 0) && c != '.') return false;
  }
  return digit;
}

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Dish rotator</title>
<style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:16px;max-width:520px;margin:auto}
h1{font-size:20px;margin:0 0 4px}small{color:#888}
.card{background:#1c1c1c;border-radius:12px;padding:16px;margin:14px 0}
.row{display:flex;gap:10px;margin:8px 0}
button{flex:1;padding:16px 0;font-size:17px;border:0;border-radius:10px;background:#2b2b2b;color:#eee}
button:active{background:#444}
.halt{background:#8b1e1e}
input[type=number]{width:100%;padding:14px;font-size:18px;border-radius:10px;border:1px solid #333;background:#0d0d0d;color:#eee;box-sizing:border-box}
input[type=range]{width:100%}
.big{font-size:44px;font-weight:600;text-align:center;margin:2px 0 0}
.sub{text-align:center;color:#aaa;font-size:15px;margin:0 0 4px}
.kv{display:flex;justify-content:space-between;color:#aaa;font-size:14px;margin:4px 0}
.ok{color:#5c5}.bad{color:#e55}.mv{color:#ffd166}
.note{color:#777;font-size:12px;margin:8px 0 0}
</style></head><body>
<h1>Dish rotator</h1><small id=ip></small>

<div class=card style="text-align:center">
<svg viewBox="-14 -18 268 226" width="268" height="226" style="max-width:100%">
 <defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#7aa7ff"/><stop offset="1" stop-color="#2d4f9e"/></linearGradient></defs>
 <g id="scale"></g>
 <line x1="120" y1="118" x2="120" y2="190" stroke="#666" stroke-width="10" stroke-linecap="round"/>
 <line x1="80" y1="192" x2="160" y2="192" stroke="#666" stroke-width="10" stroke-linecap="round"/>
 <g id="ghost" transform="rotate(0 120 118)" opacity="0.35">
  <path d="M36 62 Q120 150 204 62 Q120 96 36 62 Z" fill="none" stroke="#ffd166" stroke-width="1.5" stroke-dasharray="4 3"/>
  <line x1="120" y1="112" x2="120" y2="14" stroke="#ffd166" stroke-width="1.5" stroke-dasharray="4 3"/>
 </g>
 <g id="dish" transform="rotate(0 120 118)">
  <path d="M36 62 Q120 150 204 62 Q120 96 36 62 Z" fill="url(#g)" stroke="#9cc" stroke-width="2"/>
  <path d="M56 70 Q120 128 184 70" fill="none" stroke="#bde" stroke-width="1" opacity="0.6"/>
  <path d="M78 78 Q120 112 162 78" fill="none" stroke="#bde" stroke-width="1" opacity="0.6"/>
  <line x1="120" y1="112" x2="120" y2="36" stroke="#ccc" stroke-width="3"/>
  <circle cx="120" cy="34" r="6" fill="#ffd166"/>
  <circle cx="120" cy="34" r="12" fill="none" stroke="#ffd166" stroke-width="1" opacity="0.5"/>
  <path d="M120 16 l-5 9 h10 z" fill="#ffd166"/>
 </g>
</svg>
<div class=big id=cur>0.0&deg;</div>
<div class=sub id=azl>&nbsp;</div>
</div>

<div class=card>
<div class=kv><span>State</span><span id=st>-</span></div>
<div class=kv><span>Target</span><span id=tgt>0.0&deg;</span></div>
<div class=kv><span>Last command</span><span id=last>-</span></div>
<div class=kv><span>Uptime</span><span id=up>-</span></div>
<div class=kv><span>Link</span><span id=link class=ok>-</span></div>
</div>

<div class=card>
<div class=kv><span>Go to</span><span id=azval>0.0&deg;</span></div>
<input type=range id=az min=-75 max=75 step=0.5 value=0 oninput="drag=true;clearTimeout(dragT);dragT=setTimeout(function(){drag=false},8000);azval.innerHTML=(+this.value).toFixed(1)+'&deg;';rot('ghost',this.value)">
<div class=row><button onclick="go(az.value)">Go to angle</button></div>
<div class=row><button onclick="az.value=0;azval.innerHTML='0.0&deg;';go(0)">Centre (0&deg;)</button></div>
</div>

<div class=card>
<div class=row>
<button onclick="hit('/west?steps=1')">&larr; W 1</button>
<button onclick="hit('/west?steps=10')">&larr; W 10</button>
<button onclick="hit('/east?steps=10')">E 10 &rarr;</button>
<button onclick="hit('/east?steps=1')">E 1 &rarr;</button>
</div>
<div class=row>
<button onclick="hit('/west?steps=0')">Run West</button>
<button class=halt onclick="hit('/halt')">HALT</button>
<button onclick="hit('/east?steps=0')">Run East</button>
</div>
</div>

<div class=card>
<div class=row><input type=number id=deg placeholder="angle, e.g. 23.5" step=0.1></div>
<div class=row><button onclick="go(deg.value)">Go</button><button onclick="hit('/raw?cmd=E0316A00')">Store as 0</button></div>
</div>

<div class=card>
<div class=kv><span>Calibration</span><span id=calinfo>-</span></div>
<div class=row><input type=number id=aznow placeholder="dish points at true azimuth, e.g. 318" step=0.1></div>
<div class=row><button onclick="if(aznow.value!=='')hit('/cal?aznow='+encodeURIComponent(aznow.value))">Set azimuth</button></div>
<div class=row><input type=number id=scl placeholder="motor scale reads, e.g. -12.5" step=0.1></div>
<div class=row><button onclick="if(scl.value!=='')hit('/setpos?deg='+encodeURIComponent(scl.value))">Set motor angle</button></div>
<div class=note>The motor sends nothing back. The angle shown is worked out from the commands sent and the motor speed. If it drifts, read the scale on the motor and set it here. Neither button moves the dish.</div>
</div>

<script>
var drag=false,mv=false,dragT=0;
function rot(id,d){document.getElementById(id).setAttribute('transform','rotate('+d+' 120 118)')}
(function(){var h='';for(var a=-75;a<=75;a+=15){var r=a*Math.PI/180,s=Math.sin(r),c=Math.cos(r),big=(a%45==0||Math.abs(a)==75);
 h+='<line x1="'+(120+108*s)+'" y1="'+(118-108*c)+'" x2="'+(120+(big?116:113)*s)+'" y2="'+(118-(big?116:113)*c)+'" stroke="#666" stroke-width="'+(big?2:1)+'"/>';
 if(big)h+='<text x="'+(120+127*s)+'" y="'+(122-127*c)+'" fill="#888" font-size="10" text-anchor="middle">'+(a==0?'0':(a<0?'W':'E')+Math.abs(a))+'</text>'}
 h+='<path d="M'+(120+108*Math.sin(-75*Math.PI/180))+' '+(118-108*Math.cos(75*Math.PI/180))+' A108 108 0 0 1 '+(120+108*Math.sin(75*Math.PI/180))+' '+(118-108*Math.cos(75*Math.PI/180))+'" fill="none" stroke="#444" stroke-width="1"/>';
 document.getElementById('scale').innerHTML=h})();
function hit(u){drag=false;fetch(u).catch(function(){});setTimeout(poll1,300)}
function go(v){if(v==='')return;hit('/goto?deg='+encodeURIComponent(v))}
function show(s){
 cur.innerHTML=s.pos.toFixed(1)+'&deg;';rot('dish',s.pos);if(!drag)rot('ghost',s.target);
 azl.textContent=s.az===null?'azimuth not calibrated':'azimuth '+s.az.toFixed(1)+'°';
 tgt.innerHTML=s.target.toFixed(1)+'&deg;';last.textContent=s.last;
 st.textContent=s.moving?'moving':'idle';st.className=s.moving?'mv':'';
 up.textContent=Math.floor(s.uptime/60)+' min';ip.textContent=s.ip;
 calinfo.textContent=(s.az0===null?'az not set':'0° = az '+s.az0.toFixed(1)+'°')+', '+s.speed.toFixed(2)+'°/s';
 link.textContent='online';link.className='ok';mv=s.moving}
function poll1(){fetch('/status').then(function(r){return r.json()}).then(show).catch(function(){link.textContent='offline';link.className='bad'})}
function loop(){fetch('/status').then(function(r){return r.json()}).then(show).catch(function(){link.textContent='offline';link.className='bad';mv=false}).then(function(){setTimeout(loop,mv?250:1000)})}
loop();
</script>
<div style="text-align:center;color:#777;font-size:13px;margin:18px 0 6px">
DiSEqC dish rotator &middot; Ayushman Tripathi &middot; <a href="https://www.radioastronomy.in" style="color:#7aa7ff;text-decoration:none">www.radioastronomy.in</a><br>
Free for anyone to use, for anything.
</div>
</body></html>)HTML";

void setupHttp() {
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/status", []() {
    tick();
    bool cal = !isnan(azZero);
    String j = "{\"target\":" + String(targetDeg, 1) +
               ",\"pos\":" + String(posDeg, 2) +
               ",\"moving\":" + (moving ? "true" : "false") +
               ",\"az\":" + (cal ? String(azOf(posDeg), 1) : String("null")) +
               ",\"az0\":" + (cal ? String(azZero, 1) : String("null")) +
               ",\"dir\":" + String(azDir) +
               ",\"speed\":" + String(degPerSec, 2) +
               ",\"step\":" + String(degPerStep, 3) +
               ",\"last\":\"" + lastCmd + "\"" +
               ",\"uptime\":" + String((millis() - bootMs) / 1000) +
               ",\"ip\":\"" + WiFi.localIP().toString() + "\"" +
               ",\"rssi\":" + String(WiFi.RSSI()) + "}";
    server.send(200, "application/json", j);
  });
  server.on("/east", []() {
    if (!isNumber(server.arg("steps"))) { server.send(400, "text/plain", "steps=N required"); return; }
    cmdDriveEast(server.arg("steps").toInt()); server.send(200, "text/plain", "east");
  });
  server.on("/west", []() {
    if (!isNumber(server.arg("steps"))) { server.send(400, "text/plain", "steps=N required"); return; }
    cmdDriveWest(server.arg("steps").toInt()); server.send(200, "text/plain", "west");
  });
  server.on("/halt", []() { cmdHalt(); server.send(200, "text/plain", "halt"); });
  server.on("/goto", []() {
    if (!isNumber(server.arg("deg"))) { server.send(400, "text/plain", "deg=X required"); return; }
    cmdGotoAngle(server.arg("deg").toFloat()); server.send(200, "text/plain", "goto");
  });
  server.on("/gotoaz", []() {
    if (!isNumber(server.arg("az"))) { server.send(400, "text/plain", "az=X required"); return; }
    if (isnan(azZero)) { server.send(409, "text/plain", "azimuth not calibrated, use /cal?aznow=X"); return; }
    float d = fmodf(azDir * (server.arg("az").toFloat() - azZero), 360.0f);
    if (d > 180) d -= 360; if (d < -180) d += 360;
    if (fabsf(d) > MAX_DEG) { server.send(400, "text/plain", "azimuth outside the motor's range"); return; }
    cmdGotoAngle(d); server.send(200, "text/plain", "goto " + String(d, 1));
  });
  server.on("/setpos", []() {
    if (!isNumber(server.arg("deg"))) { server.send(400, "text/plain", "deg=X required"); return; }
    posDeg = constrain(server.arg("deg").toFloat(), -MAX_DEG, MAX_DEG); moving = false; savePos();
    server.send(200, "text/plain", "pos " + String(posDeg, 1));
  });
  server.on("/cal", []() {
    bool any = false;
    if (isNumber(server.arg("dir")))   { azDir = server.arg("dir").toInt() < 0 ? -1 : 1; prefs.putInt("dir", azDir); any = true; }
    if (isNumber(server.arg("az0")))   { azZero = fmodf(server.arg("az0").toFloat() + 360.0f, 360.0f); prefs.putFloat("az0", azZero); any = true; }
    if (isNumber(server.arg("aznow"))) { tick(); azZero = fmodf(server.arg("aznow").toFloat() - azDir * posDeg + 720.0f, 360.0f); prefs.putFloat("az0", azZero); any = true; }
    if (isNumber(server.arg("speed"))) { degPerSec  = constrain(server.arg("speed").toFloat(), 0.1f, 10.0f); prefs.putFloat("speed", degPerSec); any = true; }
    if (isNumber(server.arg("step")))  { degPerStep = constrain(server.arg("step").toFloat(), 0.01f, 2.0f); prefs.putFloat("step", degPerStep); any = true; }
    if (!any) { server.send(400, "text/plain", "give aznow, az0, dir, speed or step"); return; }
    server.send(200, "text/plain", "saved");
  });
  server.on("/raw", []() {
    if (sendRawHex(server.arg("cmd"))) server.send(200, "text/plain", "raw");
    else server.send(400, "text/plain", "cmd=HEX required, 1 to 8 bytes");
  });
}

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n'); line.trim();
  if (!line.length()) return;
  char c = line.charAt(0);
  String arg = line.length() > 1 ? line.substring(1) : ""; arg.trim();
  switch (c) {
    case 'e': if (isNumber(arg)) { cmdDriveEast(arg.toInt()); Serial.println("east"); } else Serial.println("e N"); break;
    case 'w': if (isNumber(arg)) { cmdDriveWest(arg.toInt()); Serial.println("west"); } else Serial.println("w N"); break;
    case 'h': cmdHalt();                   Serial.println("halt"); break;
    case 'g': if (isNumber(arg)) { cmdGotoAngle(arg.toFloat()); Serial.println("goto"); } else Serial.println("g DEG"); break;
    case 's': cmdStoreRef();               Serial.println("stored ref 0"); break;
    case 'z': cmdGotoRef();                Serial.println("goto ref 0"); break;
    case 'r': Serial.println(sendRawHex(arg) ? "raw sent" : "r HEX, 1 to 8 bytes"); break;
    case 'p': if (isNumber(arg)) { posDeg = constrain(arg.toFloat(), -MAX_DEG, MAX_DEG); moving = false; savePos(); Serial.println("pos set"); } else Serial.println("p DEG"); break;
    case 't': toneOn(); delay(5000); toneOff(); Serial.println("tone 5s"); break;
    case 'i': Serial.println(WiFi.localIP()); break;
    default:  Serial.println("cmds: e N, w N, h, g DEG, s, z, r HEX, p DEG, t, i");
  }
}

unsigned long lastTry = 0;
bool netUp = false;

void setup() {
  Serial.begin(115200);
  bootMs = millis();
  prefs.begin("rotator", false);
  posDeg     = prefs.getFloat("pos", 0);
  refDeg     = prefs.getFloat("ref", 0);
  azZero     = prefs.getFloat("az0", NAN);
  azDir      = prefs.getInt("dir", 1);
  degPerSec  = prefs.getFloat("speed", 1.9f);
  degPerStep = prefs.getFloat("step", 0.1f);
  targetDeg  = posDeg;
  ledcAttach(DISEQC_PIN, TONE_FREQ, TONE_RES);
  toneOff();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("dish-rotator");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  setupHttp();
  ArduinoOTA.setHostname("dish-rotator");
  ArduinoOTA.setPassword(OTA_PASS);
  Serial.println("ready. try: e 5");
}

void loop() {
  handleSerial();
  tick();
  if (WiFi.status() == WL_CONNECTED) {
    if (!netUp) { server.begin(); ArduinoOTA.begin(); netUp = true; Serial.print("IP: "); Serial.println(WiFi.localIP()); }
    server.handleClient();
    ArduinoOTA.handle();
  } else {
    if (netUp) { server.stop(); ArduinoOTA.end(); netUp = false; }
    if (millis() - lastTry > 10000) { lastTry = millis(); WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); Serial.println("retrying WiFi"); }
  }
}
