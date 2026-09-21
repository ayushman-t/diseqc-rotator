// ESP32 DiSEqC 1.2 rotator driver
// Copyright (c) 2026 Ayushman Tripathi, https://radioastronomy.in
// MIT License: anyone may use, copy, modify and redistribute this file,
// with or without changes, provided this notice is kept. No warranty.
//
// GPIO -> R -> 1uF -> coax centre. DC in through 2x 330uH.
// Serial (115200): e N, w N, h, g DEG, s, z, r HEX, t, i
// HTTP: /  (web UI)  /east?steps=N  /west?steps=N  /halt  /goto?deg=X  /raw?cmd=HEX  /status

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>

// Copy secrets.example.h to secrets.h and fill it in. secrets.h is git-ignored.
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

// The motor's mechanical range. Angles outside it are clamped, not sent.
#define MAX_DEG    75.0f
// 0x80..0xFF in a drive command means 128..1 steps. Anything below 0x80 is a
// run time in seconds, so a step count above 128 must never reach the wire.
#define MAX_STEPS  128

WebServer server(80);

// ---- state for /status ----
String lastCmd = "none";
float  targetDeg = 0;
unsigned long bootMs = 0;

// ---- tone ----
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

// ---- DiSEqC 1.2 ----
static uint8_t stepByte(long s) {
  if (s <= 0) return 0x00;                 // run until halt
  if (s > MAX_STEPS) s = MAX_STEPS;
  return (uint8_t)(256 - s);
}
void cmdHalt()            { uint8_t f[] = {0xE0,0x31,0x60}; sendFrame(f,3); lastCmd = "halt"; }
void cmdDriveEast(long s) { uint8_t f[] = {0xE0,0x31,0x68,stepByte(s)}; sendFrame(f,4); lastCmd = "east " + String(s <= 0 ? 0 : min(s, (long)MAX_STEPS)); }
void cmdDriveWest(long s) { uint8_t f[] = {0xE0,0x31,0x69,stepByte(s)}; sendFrame(f,4); lastCmd = "west " + String(s <= 0 ? 0 : min(s, (long)MAX_STEPS)); }
void cmdGotoAngle(float deg) {
  if (isnan(deg)) return;
  deg = constrain(deg, -MAX_DEG, MAX_DEG);
  bool east = deg >= 0;
  uint16_t a = (uint16_t)(fabsf(deg) * 16.0f + 0.5f);
  uint8_t f[] = {0xE0,0x31,0x6E,(uint8_t)((east ? 0xE0 : 0xD0) | ((a >> 8) & 0x0F)),(uint8_t)(a & 0xFF)};
  sendFrame(f,5); targetDeg = deg; lastCmd = "goto " + String(deg, 1);
}
void cmdStoreRef() { uint8_t f[] = {0xE0,0x31,0x6A,0x00}; sendFrame(f,4); lastCmd = "store ref"; }
void cmdGotoRef()  { uint8_t f[] = {0xE0,0x31,0x6B,0x00}; sendFrame(f,4); targetDeg = 0; lastCmd = "goto ref"; }

// Accepts hex digits and spaces only, 1 to 8 whole bytes. Returns false otherwise.
static bool sendRawHex(String hex) {
  hex.replace(" ", "");
  if (hex.length() == 0 || hex.length() % 2 || hex.length() > 16) return false;
  for (size_t i = 0; i < hex.length(); i++) if (!isxdigit(hex.charAt(i))) return false;
  uint8_t buf[8]; size_t n = 0;
  for (size_t i = 0; i < hex.length(); i += 2) buf[n++] = strtoul(hex.substring(i,i+2).c_str(), nullptr, 16);
  hex.toUpperCase();
  sendFrame(buf, n); lastCmd = "raw " + hex;
  return true;
}

// A number, optionally signed, with an optional fraction. Empty is not a number.
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

// ---- web UI (self contained) ----
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
.big{font-size:44px;font-weight:600;text-align:center;margin:6px 0}
.kv{display:flex;justify-content:space-between;color:#aaa;font-size:14px;margin:4px 0}
.ok{color:#5c5}.bad{color:#e55}
</style></head><body>
<h1>Dish rotator</h1><small id=ip></small>

<div class=card style="text-align:center">
<svg viewBox="0 0 240 205" width="240" height="200" style="max-width:100%">
 <defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#7aa7ff"/><stop offset="1" stop-color="#2d4f9e"/></linearGradient></defs>
 <line x1="120" y1="118" x2="120" y2="190" stroke="#666" stroke-width="10" stroke-linecap="round"/>
 <line x1="80" y1="192" x2="160" y2="192" stroke="#666" stroke-width="10" stroke-linecap="round"/>
 <g id="dish" transform="rotate(0 120 118)">
  <path d="M36 62 Q120 150 204 62 Q120 96 36 62 Z" fill="url(#g)" stroke="#9cc" stroke-width="2"/>
  <path d="M56 70 Q120 128 184 70" fill="none" stroke="#bde" stroke-width="1" opacity="0.6"/>
  <path d="M78 78 Q120 112 162 78" fill="none" stroke="#bde" stroke-width="1" opacity="0.6"/>
  <line x1="120" y1="112" x2="120" y2="36" stroke="#ccc" stroke-width="3"/>
  <circle cx="120" cy="34" r="6" fill="#ffd166"/>
  <circle cx="120" cy="34" r="12" fill="none" stroke="#ffd166" stroke-width="1" opacity="0.5"/>
  <circle cx="120" cy="34" r="18" fill="none" stroke="#ffd166" stroke-width="1" opacity="0.25"/>
 </g>
</svg>
</div>

<div class=card>
<div class=kv><span>Target</span><span id=tgt>0.0&deg;</span></div>
<div class=kv><span>Last command</span><span id=last>-</span></div>
<div class=kv><span>Uptime</span><span id=up>-</span></div>
<div class=kv><span>Link</span><span id=link class=ok>-</span></div>
</div>

<div class=card>
<div class=big id=azval>0.0&deg;</div>
<input type=range id=az min=-75 max=75 step=0.5 value=0 oninput="azval.innerHTML=this.value+'&deg;';dish.setAttribute('transform','rotate('+this.value+' 120 118)')">
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

<script>
function hit(u){fetch(u).catch(()=>{});setTimeout(poll,300)}
function go(v){if(v==='')return;hit('/goto?deg='+encodeURIComponent(v))}
function poll(){fetch('/status').then(r=>r.json()).then(s=>{
 tgt.innerHTML=s.target.toFixed(1)+'&deg;';dish.setAttribute('transform','rotate('+s.target+' 120 118)');last.textContent=s.last;
 up.textContent=Math.floor(s.uptime/60)+' min';ip.textContent=s.ip;
 link.textContent='online';link.className='ok'}).catch(()=>{link.textContent='offline';link.className='bad'})}
setInterval(poll,1000);poll();
</script>
<div style="text-align:center;color:#777;font-size:13px;margin:18px 0 6px">
DiSEqC dish rotator &middot; Ayushman Tripathi &middot; <a href="https://radioastronomy.in" style="color:#7aa7ff;text-decoration:none">radioastronomy.in</a><br>
MIT licensed. Use it, change it, share it.
</div>
</body></html>)HTML";

void setupHttp() {
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/status", []() {
    String j = "{\"target\":" + String(targetDeg, 1) +
               ",\"last\":\"" + lastCmd + "\"" +
               ",\"uptime\":" + String((millis() - bootMs) / 1000) +
               ",\"ip\":\"" + WiFi.localIP().toString() + "\"" +
               ",\"rssi\":" + String(WiFi.RSSI()) + "}";
    server.send(200, "application/json", j);
  });
  // A missing or malformed argument is an error, never a move.
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
  server.on("/raw", []() {
    if (sendRawHex(server.arg("cmd"))) server.send(200, "text/plain", "raw");
    else server.send(400, "text/plain", "cmd=HEX required, 1 to 8 bytes");
  });
}

// ---- serial ----
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
    case 't': toneOn(); delay(5000); toneOff(); Serial.println("tone 5s"); break;
    case 'i': Serial.println(WiFi.localIP()); break;
    default:  Serial.println("cmds: e N, w N, h, g DEG, s, z, r HEX, t, i");
  }
}

// ---- wifi with retry ----
unsigned long lastTry = 0;
bool netUp = false;

void setup() {
  Serial.begin(115200);
  bootMs = millis();
  ledcAttach(DISEQC_PIN, TONE_FREQ, TONE_RES);
  toneOff();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("dish-rotator");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  setupHttp();                       // handlers are registered once
  ArduinoOTA.setHostname("dish-rotator");
  ArduinoOTA.setPassword(OTA_PASS);
  Serial.println("ready. try: e 5");
}

void loop() {
  handleSerial();
  if (WiFi.status() == WL_CONNECTED) {
    if (!netUp) { server.begin(); ArduinoOTA.begin(); netUp = true; Serial.print("IP: "); Serial.println(WiFi.localIP()); }
    server.handleClient();
    ArduinoOTA.handle();
  } else {
    if (netUp) { server.stop(); ArduinoOTA.end(); netUp = false; }
    if (millis() - lastTry > 10000) { lastTry = millis(); WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); Serial.println("retrying WiFi"); }
  }
}
