// Addressable LED strip on ESP32-C3 SuperMini
// FastLED + WiFiManager (captive portal) + web panel + Serial reset
// Settings are in include/config.h

#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WiFiManager.h>          // tzapu/WiFiManager
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include "config.h"

CRGB leds[NUM_LEDS];
WebServer server(80);
Preferences prefs;
WiFiManager wm;

const char* MODE_NAME[M_COUNT] = MODE_NAMES;
const uint8_t RESET_SEQ[RESET_MAGIC_LEN] = RESET_MAGIC;

struct State {
  bool     on         = true;
  uint8_t  mode       = M_RAINBOW;
  uint8_t  brightness = DEF_BRIGHTNESS;
  uint8_t  speed      = DEF_SPEED;   // 1..100
  CRGB     color      = CRGB(255, 60, 0);  // for solid/breathe/comet/chase
} st;

bool dirty = false;
uint32_t dirtyAt = 0;
bool mdnsUp = false;
bool webUp = false;

// ---------- persistence ----------
void save() { prefs.putBytes("st", &st, sizeof(st)); dirty = false; }
void load() {
  if (prefs.getBytesLength("st") == sizeof(st)) prefs.getBytes("st", &st, sizeof(st));
  if (st.mode >= M_COUNT) st.mode = M_RAINBOW;
  if (st.brightness == 0) st.brightness = DEF_BRIGHTNESS;
  if (st.speed == 0 || st.speed > 100) st.speed = DEF_SPEED;
}
void touch() { dirty = true; dirtyAt = millis(); }
void saveIfNeeded() { if (dirty && millis() - dirtyAt > 3000) save(); }

// ---------- effects ----------
void renderFrame() {
  static uint8_t hue = 0;
  static uint8_t heat[NUM_LEDS];
  static uint16_t cometPos = 0;
  static uint8_t breath = 0;
  static bool breathUp = true;
  static uint8_t chaseStep = 0;

  if (!st.on) { fill_solid(leds, NUM_LEDS, CRGB::Black); FastLED.show(); return; }

  const uint8_t spd = st.speed;  // 1..100

  switch (st.mode) {
    case M_SOLID:
      fill_solid(leds, NUM_LEDS, st.color);
      break;

    case M_RAINBOW:
      fill_rainbow(leds, NUM_LEDS, hue, 256 / NUM_LEDS + 1);
      hue += (spd / 12) + 1;
      break;

    case M_FIRE: {                 // classic Fire2012
      uint8_t cooling = 55, sparking = 120;
      for (int i = 0; i < NUM_LEDS; i++)
        heat[i] = qsub8(heat[i], random8(0, ((cooling * 10) / NUM_LEDS) + 2));
      for (int k = NUM_LEDS - 1; k >= 2; k--)
        heat[k] = (heat[k-1] + heat[k-2] + heat[k-2]) / 3;
      if (random8() < sparking) {
        int y = random8(7);
        heat[y] = qadd8(heat[y], random8(160, 255));
      }
      for (int j = 0; j < NUM_LEDS; j++)
        leds[j] = HeatColor(heat[j]);
      break;
    }

    case M_COMET: {                // moving comet with a tail
      fadeToBlackBy(leds, NUM_LEDS, 40);
      cometPos += (spd / 10) + 1;
      int p = (cometPos / 4) % NUM_LEDS;
      leds[p] = st.color;
      break;
    }

    case M_TWINKLE:                // random sparkles
      fadeToBlackBy(leds, NUM_LEDS, 24);
      if (random8() < spd + 20) leds[random16(NUM_LEDS)] = st.color;
      break;

    case M_BREATHE: {              // single-color breathing
      breath += breathUp ? 3 : -3;
      if (breath >= 250) breathUp = false;
      if (breath <= 5)   breathUp = true;
      CRGB c = st.color; c.nscale8(breath);
      fill_solid(leds, NUM_LEDS, c);
      break;
    }

    case M_CHASE: {                // theater chase
      fill_solid(leds, NUM_LEDS, CRGB::Black);
      for (int i = chaseStep % 3; i < NUM_LEDS; i += 3) leds[i] = st.color;
      static uint32_t last = 0;
      if (millis() - last > (uint32_t)(400 - spd * 3)) { chaseStep++; last = millis(); }
      break;
    }
  }
  FastLED.show();
}

// ---------- web ----------
const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LED Strip</title><style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0 auto;padding:16px;max-width:480px}
h1{font-size:22px}button{padding:12px 6px;border:0;border-radius:10px;background:#2a2a2a;color:#eee;font-size:15px}
button.on{background:#e8a33a;color:#111}.grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin:12px 0}
label{display:block;margin-top:16px;font-size:14px;color:#aaa}input[type=range]{width:100%}
input[type=color]{width:100%;height:44px;border:0;background:none}
</style></head><body><h1>LED Strip</h1>
<button id="pw" style="width:100%">Power</button>
<div class="grid" id="modes"></div>
<label>Color</label><input type="color" id="col">
<label>Brightness</label><input type="range" id="br" min="1" max="255">
<label>Speed</label><input type="range" id="sp" min="1" max="100">
<script>
const M=[["solid","Solid"],["rainbow","Rainbow"],["fire","Fire"],["comet","Comet"],["twinkle","Twinkle"],["breathe","Breathe"],["chase","Chase"]];
let S={};const g=i=>document.getElementById(i);
M.forEach(([k,n])=>{let b=document.createElement('button');b.textContent=n;b.id='m_'+k;b.onclick=()=>set('mode='+k+'&on=1');g('modes').appendChild(b)});
function draw(s){S=s;g('pw').className=s.on?'on':'';M.forEach(([k])=>g('m_'+k).className=(s.on&&s.mode==k)?'on':'');
if(document.activeElement!=g('col'))g('col').value='#'+s.color;
if(document.activeElement!=g('br'))g('br').value=s.brightness;
if(document.activeElement!=g('sp'))g('sp').value=s.speed}
function set(q){fetch('/api/set?'+q).then(r=>r.json()).then(draw).catch(()=>{})}
function later(f){let t;return e=>{clearTimeout(t);t=setTimeout(()=>f(e),120)}}
g('pw').onclick=()=>set('on='+(S.on?0:1));
g('col').oninput=later(e=>set('color='+e.target.value.slice(1)));
g('br').oninput=later(e=>set('brightness='+e.target.value));
g('sp').oninput=later(e=>set('speed='+e.target.value));
(function p(){fetch('/api/state').then(r=>r.json()).then(draw).catch(()=>{});setTimeout(p,3000)})();
</script></body></html>)HTML";

String stateJson() {
  char b[160];
  snprintf(b, sizeof(b),
    "{\"on\":%s,\"mode\":\"%s\",\"color\":\"%02x%02x%02x\",\"brightness\":%d,\"speed\":%d}",
    st.on ? "true" : "false", MODE_NAME[st.mode],
    st.color.r, st.color.g, st.color.b, st.brightness, st.speed);
  return String(b);
}

void broadcastState();  // control protocol, below

void handleSet() {
  if (server.hasArg("on"))   st.on = server.arg("on").toInt() != 0;
  if (server.hasArg("mode")) {
    String m = server.arg("mode");
    for (int i = 0; i < M_COUNT; i++) if (m == MODE_NAME[i]) st.mode = i;
  }
  if (server.hasArg("color")) {
    long v = strtol(server.arg("color").c_str(), nullptr, 16);
    st.color = CRGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
  }
  if (server.hasArg("brightness")) st.brightness = constrain(server.arg("brightness").toInt(), 1, 255);
  if (server.hasArg("speed"))      st.speed      = constrain(server.arg("speed").toInt(), 1, 100);
  touch();
  broadcastState();  // let protocol hosts (e.g. Home Assistant) see changes made from the web UI
  server.send(200, "application/json", stateJson());
}

void setupWeb() {
  server.on("/", []() { server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/api/state", []() { server.send(200, "application/json", stateJson()); });
  server.on("/api/set", handleSet);
  server.begin();
}

// ---------- Serial reset ----------
void checkResetMagic(uint8_t b) {
  static uint8_t buf[RESET_MAGIC_LEN] = {0};
  memmove(buf, buf + 1, RESET_MAGIC_LEN - 1);
  buf[RESET_MAGIC_LEN - 1] = b;
  if (memcmp(buf, RESET_SEQ, RESET_MAGIC_LEN) == 0) {
    Serial.println("\n[RESET] erasing settings and Wi-Fi...");
    prefs.clear();
    wm.resetSettings();
    delay(300);
    ESP.restart();
  }
}

// ---------- control protocol over Serial and TCP (see PROTOCOL.md) ----------
enum : uint8_t {
  CMD_PING = 0x00, CMD_GET_STATE = 0x01, CMD_SET_POWER = 0x02, CMD_SET_MODE = 0x03,
  CMD_SET_COLOR = 0x04, CMD_SET_BRIGHTNESS = 0x05, CMD_SET_SPEED = 0x06, CMD_SET_ALL = 0x07,
  CMD_SAVE = 0x08,
  RSP_PONG = 0x80, RSP_STATE = 0x81, RSP_ERROR = 0xEE
};
enum : uint8_t { ERR_CHECKSUM = 0x01, ERR_UNKNOWN_CMD = 0x02, ERR_LENGTH = 0x03, ERR_VALUE = 0x04 };

// per-connection frame parser state
struct ProtoParser {
  enum { WAIT_SOF, GET_CMD, GET_LEN, GET_DATA, GET_CS } stage = WAIT_SOF;
  uint8_t cmd = 0, len = 0, pos = 0, cs = 0, buf[PROTO_MAX_PAYLOAD];
  uint32_t lastByte = 0;
};

ProtoParser serialParser;
WiFiServer  protoServer(PROTO_TCP_PORT);
WiFiClient  protoClients[PROTO_MAX_CLIENTS];
ProtoParser clientParsers[PROTO_MAX_CLIENTS];
uint32_t    clientSince[PROTO_MAX_CLIENTS];
bool        protoUp = false;

void sendFrame(Print& out, uint8_t cmd, const uint8_t* data, uint8_t len) {
  uint8_t f[4 + PROTO_MAX_PAYLOAD];
  uint8_t cs = cmd ^ len;
  f[0] = PROTO_SOF; f[1] = cmd; f[2] = len;
  for (uint8_t i = 0; i < len; i++) { f[3 + i] = data[i]; cs ^= data[i]; }
  f[3 + len] = cs;
  out.write(f, 4 + len);  // one write = one TCP packet
}
void sendError(Print& out, uint8_t code) { sendFrame(out, RSP_ERROR, &code, 1); }
void sendState(Print& out) {
  uint8_t d[7] = { st.on, st.mode, st.color.r, st.color.g, st.color.b, st.brightness, st.speed };
  sendFrame(out, RSP_STATE, d, sizeof(d));
}

// every host learns about every change, whoever made it
void broadcastState() {
  sendState(Serial);
  for (auto& c : protoClients) if (c.connected()) sendState(c);
}

// payload length each command expects; -1 = unknown command
int payloadLen(uint8_t cmd) {
  switch (cmd) {
    case CMD_PING: case CMD_GET_STATE: case CMD_SAVE: return 0;
    case CMD_SET_POWER: case CMD_SET_MODE: case CMD_SET_BRIGHTNESS: case CMD_SET_SPEED: return 1;
    case CMD_SET_COLOR: return 3;
    case CMD_SET_ALL: return 7;
    default: return -1;
  }
}

bool validMode(uint8_t m)       { return m < M_COUNT; }
bool validBrightness(uint8_t v) { return v >= 1; }
bool validSpeed(uint8_t v)      { return v >= 1 && v <= 100; }

// `out` is the connection the command came from: replies and errors go there,
// state changes are broadcast to every host
void handleCommand(uint8_t cmd, const uint8_t* p, uint8_t len, Print& out) {
  int need = payloadLen(cmd);
  if (need < 0)    { sendError(out, ERR_UNKNOWN_CMD); return; }
  if (len != need) { sendError(out, ERR_LENGTH); return; }

  switch (cmd) {
    case CMD_PING: {
      // version | "LMC3" | MAC (= USB serial number and mDNS "id": identifies the board)
      const size_t sigLen = sizeof(PROTO_SIGNATURE) - 1;
      uint8_t v[1 + sigLen + 6] = { PROTO_VERSION };
      memcpy(v + 1, PROTO_SIGNATURE, sigLen);
      WiFi.macAddress(v + 1 + sigLen);
      sendFrame(out, RSP_PONG, v, sizeof(v));
      return;
    }
    case CMD_GET_STATE: sendState(out); return;
    case CMD_SAVE: save(); sendState(out); return;
    case CMD_SET_POWER: st.on = p[0] != 0; break;
    case CMD_SET_MODE:
      if (!validMode(p[0])) { sendError(out, ERR_VALUE); return; }
      st.mode = p[0]; break;
    case CMD_SET_COLOR: st.color = CRGB(p[0], p[1], p[2]); break;
    case CMD_SET_BRIGHTNESS:
      if (!validBrightness(p[0])) { sendError(out, ERR_VALUE); return; }
      st.brightness = p[0]; break;
    case CMD_SET_SPEED:
      if (!validSpeed(p[0])) { sendError(out, ERR_VALUE); return; }
      st.speed = p[0]; break;
    case CMD_SET_ALL:
      // validate everything first so a bad frame changes nothing
      if (!validMode(p[1]) || !validBrightness(p[5]) || !validSpeed(p[6])) { sendError(out, ERR_VALUE); return; }
      st.on = p[0] != 0; st.mode = p[1]; st.color = CRGB(p[2], p[3], p[4]);
      st.brightness = p[5]; st.speed = p[6]; break;
  }
  touch();
  broadcastState();  // also the reply to the sender
}

// byte-by-byte frame parser: SOF | CMD | LEN | PAYLOAD[LEN] | XOR(CMD, LEN, PAYLOAD)
void feedProtocol(ProtoParser& ps, uint8_t b, Print& out) {
  // drop a half-received frame if the sender went quiet
  if (ps.stage != ProtoParser::WAIT_SOF && millis() - ps.lastByte > PROTO_TIMEOUT_MS)
    ps.stage = ProtoParser::WAIT_SOF;
  ps.lastByte = millis();

  switch (ps.stage) {
    case ProtoParser::WAIT_SOF: if (b == PROTO_SOF) ps.stage = ProtoParser::GET_CMD; break;
    case ProtoParser::GET_CMD:  ps.cmd = b; ps.cs = b; ps.stage = ProtoParser::GET_LEN; break;
    case ProtoParser::GET_LEN:
      if (b > PROTO_MAX_PAYLOAD) { sendError(out, ERR_LENGTH); ps.stage = ProtoParser::WAIT_SOF; break; }
      ps.len = b; ps.cs ^= b; ps.pos = 0;
      ps.stage = ps.len ? ProtoParser::GET_DATA : ProtoParser::GET_CS;
      break;
    case ProtoParser::GET_DATA:
      ps.buf[ps.pos++] = b; ps.cs ^= b;
      if (ps.pos == ps.len) ps.stage = ProtoParser::GET_CS;
      break;
    case ProtoParser::GET_CS:
      ps.stage = ProtoParser::WAIT_SOF;
      if (b != ps.cs) sendError(out, ERR_CHECKSUM);
      else handleCommand(ps.cmd, ps.buf, ps.len, out);
      break;
  }
}

void pollSerial() {
  while (Serial.available()) {
    uint8_t b = Serial.read();
    checkResetMagic(b);  // factory reset is Serial-only: never from the network
    feedProtocol(serialParser, b, Serial);
  }
}

void pollTcp() {
  if (!protoUp) return;

  // accept a new host; when all slots are busy, drop the oldest
  // (e.g. a host that vanished without closing its connection)
  WiFiClient incoming = protoServer.available();
  if (incoming) {
    int slot = 0;
    for (int i = 0; i < PROTO_MAX_CLIENTS; i++) {
      if (!protoClients[i].connected()) { slot = i; break; }
      if (clientSince[i] < clientSince[slot]) slot = i;
    }
    protoClients[slot].stop();
    protoClients[slot] = incoming;
    protoClients[slot].setNoDelay(true);
    clientParsers[slot] = ProtoParser();
    clientSince[slot] = millis();
  }

  for (int i = 0; i < PROTO_MAX_CLIENTS; i++) {
    WiFiClient& c = protoClients[i];
    while (c.connected() && c.available()) feedProtocol(clientParsers[i], (uint8_t)c.read(), c);
  }
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nLED strip: starting");

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  fill_solid(leds, NUM_LEDS, CRGB::Black); FastLED.show();

  prefs.begin("led", false);
  load();

  // captive portal: starts the "LED-Strip-Setup" AP if Wi-Fi isn't configured yet.
  // Non-blocking, so the strip and Serial control work while the portal is up;
  // after PORTAL_TIMEOUT the portal closes and the board keeps running offline.
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT);
  wm.setHostname(HOSTNAME);
  wm.autoConnect(AP_SSID, strlen(AP_PASSWORD) ? AP_PASSWORD : nullptr);
}

void loop() {
  static uint32_t lastFrame = 0;

  pollSerial();
  pollTcp();
  wm.process();
  if (webUp) server.handleClient();

  // start the web panel, protocol server and mDNS once Wi-Fi is actually connected
  // (not while the portal is up — it owns port 80)
  if (WiFi.status() == WL_CONNECTED && !wm.getConfigPortalActive()) {
    if (!webUp) { setupWeb(); webUp = true; }
    if (!protoUp) { protoServer.begin(); protoServer.setNoDelay(true); protoUp = true; }
    if (!mdnsUp) {
      if (MDNS.begin(HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        // _lumenc3._tcp lets Home Assistant discover the board (zeroconf);
        // "id" is the MAC, the same as the USB serial number, so HA knows it's one board
        MDNS.addService("lumenc3", "tcp", PROTO_TCP_PORT);
        MDNS.addServiceTxt("lumenc3", "tcp", "id", WiFi.macAddress().c_str());
        MDNS.addServiceTxt("lumenc3", "tcp", "proto", String(PROTO_VERSION).c_str());
      }
      Serial.print("Wi-Fi OK: "); Serial.println(WiFi.localIP());
      mdnsUp = true;
    }
  } else if (WiFi.status() != WL_CONNECTED && mdnsUp) {
    MDNS.end(); mdnsUp = false;
  }

  FastLED.setBrightness(st.on ? st.brightness : 0);
  if (millis() - lastFrame >= FRAME_MS) {
    lastFrame = millis();
    renderFrame();
  }
  saveIfNeeded();
}
