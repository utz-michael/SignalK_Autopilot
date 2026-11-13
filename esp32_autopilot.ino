// ============================================================================
//  ESP32 Kurs-Autopilot  (Schrittmotor + Reibrad am Lenkrad, Steuerung via Signal K)
//
//  - Liest von Signal K (WebSocket): Kurs (mag), ROT, Ruderwinkel, SOG
//  - Regler: Kurs-PD  ->  Soll-Ruderwinkel  ->  Ruder-P  ->  Schrittmotor-Drehzahl
//  - WebSocket-Server (Port 81) fuer das Plugin "signalk-esp-autopilot-provider"
//  - Konfigurations-Webseite (Port 80), OTA
//
//  Winkel intern in GRAD, Signal K liefert Radiant (wird beim Einlesen gewandelt).
//  Vorzeichen: Kurs steigt = Drehung nach Steuerbord; Ruderwinkel + = Steuerbord.
// ============================================================================

struct Sig;
struct Cmd;

#include <Arduino.h>
#include <WiFi.h>
//#include <ESPmDNS.h>
//#include <ArduinoOTA.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WebSocketsClient.h>
#include <FastAccelStepper.h>

// ---------------------------- Hardware / Defaults ---------------------------
#define PIN_STEP 25
#define PIN_DIR  26
#define PIN_EN   27           // Pololu ENABLE (active low)
#define DEFAULT_SSID "Connection2"
#define DEFAULT_PASS ".Sonja00"
#define CONTROL_PERIOD_MS 50  // 20 Hz

// ------------------------------- Konfiguration ------------------------------
struct Config {
  String ssid = DEFAULT_SSID, pass = DEFAULT_PASS, host = "esp-autopilot";
  String skHost = "10.10.1.42"; int skPort = 3000; String skToken = "";
  float kp = 1.0f;        // Grad Ruder pro Grad Kursfehler
  float kd = 1.0f;        // Grad Ruder pro (Grad/s) Drehrate
  float hdgDb = 1.0f;     // Totband Kursfehler [Grad]
  float hdgTau = 0.5f;    // Tiefpass Kursfehler [s]
  float rudLim = 30.0f;   // max. Soll-Ruderwinkel [Grad]
  float rudDb = 1.0f;     // Totband Ruder [Grad]
  float kv = 100.0f;      // Motor-Hz pro Grad Ruderfehler
  float minHz = 80.0f, maxHz = 4000.0f, accel = 6000.0f, jogHz = 800.0f;
  float maxKmh = 15.0f;   // darueber: Standby
  int   staleMs = 1000;   // Sensordaten aelter -> Standby
  float maxErr = 60.0f;   // Kursabweichung [Grad] ...
  float maxErrT = 10.0f;  // ... laenger als [s] -> Standby
  bool  invMot = false, invRud = false, invRot = false;
} cfg;

struct Field { const char* key; const char* label; char type; void* ptr; bool secret; };
Field fields[] = {
  {"ssid",   "WLAN SSID (Neustart)",              's', &cfg.ssid,    false},
  {"pass",   "WLAN Passwort (Neustart)",          's', &cfg.pass,    true},
  {"host",   "Hostname (Neustart)",               's', &cfg.host,    false},
  {"skhost", "Signal-K Host (Neustart)",          's', &cfg.skHost,  false},
  {"skport", "Signal-K Port (Neustart)",          'i', &cfg.skPort,  false},
  {"sktoken","Signal-K Token (Neustart)",         's', &cfg.skToken, true},
  {"kp",     "Kp [Grad Ruder / Grad Fehler]",     'f', &cfg.kp,      false},
  {"kd",     "Kd [Grad Ruder / (Grad/s)]",        'f', &cfg.kd,      false},
  {"hdgdb",  "Totband Kurs [Grad]",               'f', &cfg.hdgDb,   false},
  {"hdgtau", "Tiefpass Kursfehler [s]",           'f', &cfg.hdgTau,  false},
  {"rudlim", "Max. Soll-Ruder [Grad]",            'f', &cfg.rudLim,  false},
  {"ruddb",  "Totband Ruder [Grad]",              'f', &cfg.rudDb,   false},
  {"kv",     "Motor-Hz pro Grad Ruderfehler",     'f', &cfg.kv,      false},
  {"minhz",  "Motor min. Hz",                     'f', &cfg.minHz,   false},
  {"maxhz",  "Motor max. Hz",                     'f', &cfg.maxHz,   false},
  {"accel",  "Motor Beschleunigung [Hz/s]",       'f', &cfg.accel,   false},
  {"joghz",  "Jog-Geschwindigkeit [Hz]",          'f', &cfg.jogHz,   false},
  {"maxkmh", "Max. Geschwindigkeit [km/h]",       'f', &cfg.maxKmh,  false},
  {"stale",  "Sensor-Timeout [ms]",               'i', &cfg.staleMs, false},
  {"maxerr", "Max. Kursabweichung [Grad]",        'f', &cfg.maxErr,  false},
  {"maxerrt","... laenger als [s]",               'f', &cfg.maxErrT, false},
  {"invmot", "Motor-Drehrichtung invertieren",    'b', &cfg.invMot,  false},
  {"invrud", "Ruderwinkel-Vorzeichen invertieren",'b', &cfg.invRud,  false},
  {"invrot", "ROT-Vorzeichen invertieren",        'b', &cfg.invRot,  false},
};
const size_t NFIELDS = sizeof(fields) / sizeof(fields[0]);

void setField(Field& f, const String& v) {
  switch (f.type) {
    case 's': *(String*)f.ptr = v; break;
    case 'f': *(float*)f.ptr = v.toFloat(); break;
    case 'i': *(int*)f.ptr = v.toInt(); break;
    case 'b': *(bool*)f.ptr = (v == "1" || v == "true" || v == "on"); break;
  }
}
String getField(const Field& f) {
  switch (f.type) {
    case 's': return *(String*)f.ptr;
    case 'f': return String(*(float*)f.ptr, 3);
    case 'i': return String(*(int*)f.ptr);
    default:  return *(bool*)f.ptr ? "1" : "0";
  }
}
void loadConfig() {
  Preferences p; p.begin("ap", false);
  for (size_t i = 0; i < NFIELDS; i++) {
    Field& f = fields[i];
    if (!p.isKey(f.key)) continue;
    switch (f.type) {
      case 's': *(String*)f.ptr = p.getString(f.key); break;
      case 'f': *(float*)f.ptr = p.getFloat(f.key); break;
      case 'i': *(int*)f.ptr = p.getInt(f.key); break;
      case 'b': *(bool*)f.ptr = p.getBool(f.key); break;
    }
  }
  p.end();
}
void saveConfig() {
  Preferences p; p.begin("ap", false);
  for (size_t i = 0; i < NFIELDS; i++) {
    Field& f = fields[i];
    switch (f.type) {
      case 's': p.putString(f.key, *(String*)f.ptr); break;
      case 'f': p.putFloat(f.key, *(float*)f.ptr); break;
      case 'i': p.putInt(f.key, *(int*)f.ptr); break;
      case 'b': p.putBool(f.key, *(bool*)f.ptr); break;
    }
  }
  p.end();
}

// --------------------------------- Signale ----------------------------------
struct Sig { float v = NAN; uint32_t t = 0; };
Sig sHdg, sRot, sRud, sSog;   // Grad, Grad/s, Grad, km/h
bool fresh(const Sig & s, uint32_t ms) { return s.t != 0 && (millis() - s.t) <= ms; }

float wrap360(float a) { a = fmodf(a, 360.0f); if (a < 0) a += 360.0f; return a; }
float wrap180(float a) { return wrap360(a + 180.0f) - 180.0f; }
float applyDb(float e, float db) { if (e > db) return e - db; if (e < -db) return e + db; return 0; }

// ---------------------------------- Zustand ---------------------------------
enum ApState { AP_STANDBY, AP_AUTO };
ApState apState = AP_STANDBY;
char  alarmText[56] = "";
float targetDeg = 0; bool targetPreset = false;
float rudCmd = 0, rotUsed = 0, errFilt = 0;
uint32_t errBadSince = 0, stallRefT = 0; float stallRefRud = 0;
int jogDir = 0; uint32_t jogUntil = 0;
bool statusNow = true; uint32_t restartAt = 0;
bool skConnected = false;

enum CmdType : uint8_t { C_ENGAGE, C_STANDBY, C_TARGET, C_ADJUST, C_JOG };
struct Cmd { CmdType t; float v; };
QueueHandle_t cmdQ;
void enqueue(CmdType t, float v = 0) { Cmd c{t, v}; xQueueSend(cmdQ, &c, 0); }

// -------------------------------- Schrittmotor ------------------------------
FastAccelStepperEngine engine;
FastAccelStepper* stepper = nullptr;

// dir: +1 = Ruder nach Steuerbord, -1 = nach Backbord; hz = Betrag
void drive(float hz, int dir) {
  if (!stepper) return;
  if (hz < 1.0f || dir == 0) { if (stepper->isRunning()) stepper->stopMove(); return; }
  if (cfg.invMot) dir = -dir;
  stepper->setSpeedInHz((uint32_t)min(hz, cfg.maxHz));
  stepper->setAcceleration((int32_t)cfg.accel);
  if (dir > 0) stepper->runForward(); else stepper->runBackward();
}

// -------------------------------- Zustandswechsel ---------------------------
void setAlarm(const char* txt) { strlcpy(alarmText, txt, sizeof(alarmText)); statusNow = true; }

void goStandby(const char* alarm) {
  apState = AP_STANDBY; targetPreset = false;
  drive(0, 0);
  setAlarm(alarm);
}

void doEngage() {
  if (!fresh(sHdg, cfg.staleMs) || !fresh(sRud, cfg.staleMs)) { setAlarm("Kurs-/Ruderdaten fehlen"); return; }
  if (fresh(sSog, 3000) && sSog.v > cfg.maxKmh) { setAlarm("Zu schnell fuer Autopilot"); return; }
  if (!targetPreset) targetDeg = sHdg.v;
  apState = AP_AUTO; errFilt = 0; errBadSince = 0; stallRefT = 0;
  setAlarm("");
}

void handleCmd(const Cmd& c) {
  switch (c.t) {
    case C_ENGAGE:  doEngage(); break;
    case C_STANDBY: goStandby(""); break;
    case C_TARGET:
      targetDeg = wrap360(c.v);
      if (apState == AP_STANDBY) targetPreset = true;
      break;
    case C_ADJUST:
      if (apState == AP_AUTO) targetDeg = wrap360(targetDeg + c.v);
      break;
    case C_JOG:
      if (apState == AP_STANDBY) { jogDir = c.v > 0 ? 1 : -1; jogUntil = millis() + 400; }
      break;
  }
  statusNow = true;
}

// ----------------------------------- Regler ---------------------------------
void controlStep(uint32_t now, float dt) {
  // ROT: Sensorwert, sonst aus Kursaenderung abgeleitet
  static uint32_t lastHdgT = 0; static float lastHdg = 0, rotEst = 0;
  if (sHdg.t != lastHdgT) {
    float dtH = (sHdg.t - lastHdgT) / 1000.0f;
    if (lastHdgT && dtH > 0.02f && dtH < 1.0f) rotEst += 0.3f * (wrap180(sHdg.v - lastHdg) / dtH - rotEst);
    lastHdg = sHdg.v; lastHdgT = sHdg.t;
  }
  rotUsed = fresh(sRot, 500) ? sRot.v : rotEst;

  if (apState == AP_STANDBY) {
    rudCmd = 0;
    if (jogDir != 0 && now < jogUntil) {
      bool stop = fresh(sRud, cfg.staleMs) &&
                  ((jogDir > 0 && sRud.v >= cfg.rudLim + 5) || (jogDir < 0 && sRud.v <= -(cfg.rudLim + 5)));
      if (stop) drive(0, 0); else drive(cfg.jogHz, jogDir);
    } else { jogDir = 0; drive(0, 0); }
    return;
  }

  // ---- AUTO: Sicherheitspruefungen ----
  if (!fresh(sHdg, cfg.staleMs) || !fresh(sRud, cfg.staleMs)) { goStandby("Sensordaten fehlen"); return; }
  if (fresh(sSog, 3000) && sSog.v > cfg.maxKmh)              { goStandby("Geschwindigkeit ueber Limit"); return; }
  if (fabsf(sRud.v) > 45.0f)                                 { goStandby("Ruderwinkel unplausibel"); return; }

  // ---- aeussere Schleife: Kurs -> Soll-Ruder ----
  float e = wrap180(targetDeg - sHdg.v);
  errFilt += (dt / (cfg.hdgTau + dt)) * (e - errFilt);
  float eDb = applyDb(errFilt, cfg.hdgDb);
  rudCmd = cfg.kp * eDb - cfg.kd * rotUsed;
  rudCmd = constrain(rudCmd, -cfg.rudLim, cfg.rudLim);

  // Dauerhaft grosse Abweichung -> Standby
  if (fabsf(e) > cfg.maxErr) {
    if (!errBadSince) errBadSince = now;
    if (now - errBadSince > (uint32_t)(cfg.maxErrT * 1000)) { goStandby("Kursabweichung zu gross"); return; }
  } else errBadSince = 0;

  // ---- innere Schleife: Soll-Ruder -> Motor-Drehzahl ----
  float rudErr = rudCmd - sRud.v;
  float hardLim = cfg.rudLim + 5.0f;
  bool blocked = (rudErr > 0 && sRud.v >= hardLim) || (rudErr < 0 && sRud.v <= -hardLim);
  if (fabsf(rudErr) <= cfg.rudDb || blocked) {
    drive(0, 0); stallRefT = 0;
  } else {
    float hz = max(cfg.minHz, cfg.kv * fabsf(rudErr));
    drive(hz, rudErr > 0 ? 1 : -1);
    // Blockade / Reibrad rutscht: Motor laeuft, Ruder bewegt sich nicht
    if (!stallRefT || fabsf(sRud.v - stallRefRud) >= 0.5f) { stallRefT = now; stallRefRud = sRud.v; }
    else if (now - stallRefT > 3000) { goStandby("Ruder reagiert nicht (Reibrad/Blockade)"); return; }
  }
}

// ---------------------------------- Status ----------------------------------
String statusJson() {
  JsonDocument d;
  d["state"] = apState == AP_AUTO ? "auto" : "standby";
  d["mode"] = "compass";
  d["target"] = targetDeg;
  d["heading"] = fresh(sHdg, cfg.staleMs) ? sHdg.v : NAN;
  d["rudder"] = fresh(sRud, cfg.staleMs) ? sRud.v : NAN;
  d["rudderCmd"] = rudCmd;
  d["rot"] = rotUsed;
  d["sog"] = fresh(sSog, 3000) ? sSog.v : NAN;
  d["sk"] = skConnected;
  d["alarm"] = alarmText;
  String s; serializeJson(d, s); return s;
}

// -------------------------------- Signal K Client ---------------------------
WebSocketsClient skWs;

void handleSkDelta(uint8_t* payload, size_t len) {
  static JsonDocument filter;
  static bool init = false;
  if (!init) {
    filter["updates"][0]["values"][0]["path"] = true;
    filter["updates"][0]["values"][0]["value"] = true;
    init = true;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload, len, DeserializationOption::Filter(filter))) return;
  uint32_t now = millis();
  for (JsonObject u : doc["updates"].as<JsonArray>()) {
    for (JsonObject v : u["values"].as<JsonArray>()) {
      const char* path = v["path"];
      if (!path || !v["value"].is<float>()) continue;
      float val = v["value"].as<float>();
      if (!strcmp(path, "navigation.headingMagnetic"))     { sHdg.v = wrap360(val * RAD_TO_DEG); sHdg.t = now; }
      else if (!strcmp(path, "navigation.rateOfTurn"))     { sRot.v = val * RAD_TO_DEG * (cfg.invRot ? -1 : 1); sRot.t = now; }
      else if (!strcmp(path, "steering.rudderAngle"))      { sRud.v = val * RAD_TO_DEG * (cfg.invRud ? -1 : 1); sRud.t = now; }
      else if (!strcmp(path, "navigation.speedOverGround")){ sSog.v = val * 3.6f; sSog.t = now; }
    }
  }
}

void skEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      skConnected = true;
      skWs.sendTXT(
        "{\"context\":\"vessels.self\",\"subscribe\":["
        "{\"path\":\"navigation.headingMagnetic\",\"policy\":\"instant\",\"minPeriod\":50},"
        "{\"path\":\"navigation.rateOfTurn\",\"policy\":\"instant\",\"minPeriod\":50},"
        "{\"path\":\"steering.rudderAngle\",\"policy\":\"instant\",\"minPeriod\":50},"
        "{\"path\":\"navigation.speedOverGround\",\"policy\":\"instant\",\"minPeriod\":500}]}");
      break;
    case WStype_DISCONNECTED: skConnected = false; break;
    case WStype_TEXT: handleSkDelta(payload, length); break;
    default: break;
  }
}

// ------------------------- WebSocket-Server fuer das Plugin -----------------
AsyncWebServer wsServer(81);
AsyncWebSocket ws("/");

void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    client->text(statusJson());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)) return;
    JsonDocument d;
    if (deserializeJson(d, data, len)) return;
    const char* cmd = d["cmd"];
    if (!cmd) return;
    float v = d["value"] | 0.0f;
    if      (!strcmp(cmd, "engage"))    enqueue(C_ENGAGE);
    else if (!strcmp(cmd, "disengage")) enqueue(C_STANDBY);
    else if (!strcmp(cmd, "target"))    enqueue(C_TARGET, v);
    else if (!strcmp(cmd, "adjust"))    enqueue(C_ADJUST, v);
    // "mode": nur "compass" vorhanden, nichts zu tun
  }
}

// ------------------------------- Konfig-Webseite ----------------------------
AsyncWebServer server(80);

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html><html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Autopilot</title>
<style>body{font-family:sans-serif;max-width:640px;margin:1em auto;padding:0 1em}
label{display:flex;justify-content:space-between;align-items:center;margin:.3em 0}
input{width:13em}input[type=checkbox]{width:auto}
#st{background:#eee;padding:.5em;border-radius:6px;font-family:monospace;white-space:pre;min-height:9em}
button{margin:.2em;padding:.5em 1em}</style></head><body>
<h2>Autopilot</h2><div id="st">...</div>
<p><button onclick="post('/api/standby')">STANDBY</button>
Jog (nur Standby): <button onclick="post('/api/jog?dir=-1')">&#9664; Backbord</button>
<button onclick="post('/api/jog?dir=1')">Steuerbord &#9654;</button></p>
<h3>Konfiguration</h3><div id="f"></div>
<button onclick="save()">Speichern</button> <button onclick="if(confirm('Neustart?'))post('/api/reboot')">Neustart</button>
<span id="m"></span>
<script>
function post(u){fetch(u,{method:'POST'})}
async function load(){const a=await (await fetch('/api/config')).json();const f=document.getElementById('f');f.innerHTML='';
for(const x of a){const l=document.createElement('label');l.textContent=x.label;const i=document.createElement('input');i.name=x.key;
if(x.type=='b'){i.type='checkbox';i.checked=x.value=='1'}else{i.type=x.secret?'password':'text';i.value=x.value;if(x.secret)i.placeholder='(unveraendert)'}
l.appendChild(i);f.appendChild(l)}}
async function save(){const p=new URLSearchParams();for(const i of document.querySelectorAll('#f input'))p.append(i.name,i.type=='checkbox'?(i.checked?'1':'0'):i.value);
await fetch('/api/config',{method:'POST',body:p});document.getElementById('m').textContent='Gespeichert';setTimeout(()=>document.getElementById('m').textContent='',2000)}
setInterval(async()=>{try{const s=await (await fetch('/api/status')).json();const n=v=>v===null?'-':v.toFixed(1);
document.getElementById('st').textContent='Zustand:  '+s.state+(s.alarm?'  ALARM: '+s.alarm:'')+'\nSoll-Kurs: '+n(s.target)+'\nKurs:      '+n(s.heading)+'\nROT:       '+n(s.rot)+' Grad/s\nRuder ist: '+n(s.rudder)+'\nRuder soll:'+n(s.rudderCmd)+'\nSOG:       '+n(s.sog)+' km/h\nSignal K:  '+(s.sk?'verbunden':'GETRENNT')}catch(e){}},500);
load();
</script></body></html>)rawliteral";

void setupWeb() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/html; charset=utf-8", INDEX_HTML); });
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "application/json", statusJson()); });
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d; JsonArray a = d.to<JsonArray>();
    for (size_t i = 0; i < NFIELDS; i++) {
      JsonObject o = a.add<JsonObject>();
      o["key"] = fields[i].key; o["label"] = fields[i].label;
      o["type"] = String(fields[i].type); o["secret"] = fields[i].secret;
      o["value"] = fields[i].secret ? String("") : getField(fields[i]);
    }
    String s; serializeJson(d, s); r->send(200, "application/json", s);
  });
  server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest* r) {
    for (size_t i = 0; i < NFIELDS; i++) {
      if (!r->hasParam(fields[i].key, true)) continue;
      String v = r->getParam(fields[i].key, true)->value();
      if (fields[i].secret && v.length() == 0) continue;
      setField(fields[i], v);
    }
    saveConfig();
    r->send(200, "text/plain", "OK");
  });
  server.on("/api/standby", HTTP_POST, [](AsyncWebServerRequest* r) { enqueue(C_STANDBY); r->send(200, "text/plain", "OK"); });
  server.on("/api/jog", HTTP_POST, [](AsyncWebServerRequest* r) {
    int dir = r->hasParam("dir") ? r->getParam("dir")->value().toInt() : 0;
    if (dir != 0) enqueue(C_JOG, (float)dir);
    r->send(200, "text/plain", "OK");
  });
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* r) { restartAt = millis() + 500; r->send(200, "text/plain", "OK"); });
  server.begin();

  ws.onEvent(onWsEvent);
  wsServer.addHandler(&ws);
  wsServer.begin();
}

// ------------------------------------ Setup ---------------------------------
void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.host.c_str());
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
  if (WiFi.status() != WL_CONNECTED) {            // Notfall: eigenes Netz zur Konfiguration
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Autopilot-Setup");
    Serial.println("Kein WLAN - AP 'Autopilot-Setup' aktiv (192.168.4.1)");
  } else {
    Serial.printf("WLAN verbunden, IP %s\n", WiFi.localIP().toString().c_str());
  }
//  MDNS.begin(cfg.host.c_str());
//  MDNS.addService("http", "tcp", 80);
// ArduinoOTA.setHostname(cfg.host.c_str());
// ArduinoOTA.onStart([]() { goStandby("OTA-Update"); });
// ArduinoOTA.begin();
}

void setup() {
  Serial.begin(115200);
  cmdQ = xQueueCreate(16, sizeof(Cmd));
  loadConfig();

  engine.init();
  stepper = engine.stepperConnectToPin(PIN_STEP);
  if (stepper) {
    stepper->setDirectionPin(PIN_DIR);
    stepper->setEnablePin(PIN_EN);        // active low
    stepper->setAutoEnable(true);
    stepper->setDelayToDisable(500);
    stepper->setAcceleration((int32_t)cfg.accel);
  } else {
    Serial.println("FEHLER: Stepper konnte nicht initialisiert werden");
  }

  setupWifi();
  setupWeb();

  String path = "/signalk/v1/stream?subscribe=none";
  if (cfg.skToken.length()) path += "&token=" + cfg.skToken;
  skWs.begin(cfg.skHost.c_str(), cfg.skPort, path.c_str());
  skWs.onEvent(skEvent);
  skWs.setReconnectInterval(2000);
  skWs.enableHeartbeat(15000, 3000, 2);
}

// ------------------------------------ Loop ----------------------------------
void loop() {
  static uint32_t lastCtl = 0, lastStatus = 0;
  //ArduinoOTA.handle();
  skWs.loop();

  Cmd c;
  while (xQueueReceive(cmdQ, &c, 0) == pdTRUE) handleCmd(c);

  uint32_t now = millis();
  if (now - lastCtl >= CONTROL_PERIOD_MS) {
    float dt = (now - lastCtl) / 1000.0f;
    lastCtl = now;
    controlStep(now, dt);
  }
  if (statusNow || now - lastStatus >= 500) {
    statusNow = false; lastStatus = now;
    ws.textAll(statusJson());
    ws.cleanupClients();
  }
  if (restartAt && now > restartAt) ESP.restart();
}
