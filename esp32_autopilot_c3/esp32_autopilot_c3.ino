// ============================================================================
//  ESP32-C3 Kurs-Autopilot  (Schrittmotor + Reibrad am Lenkrad)
//
//  Bedienung ausschliesslich ueber die Webseite auf dem ESP (Port 80).
//  Signal K wird nur als Datenquelle gelesen (Kurs, ROT, Ruderwinkel, SOG).
//
//  Regler: Kurs-PD -> Soll-Ruderwinkel -> Ruder-P -> Schrittmotor-Drehzahl
//  Winkel intern in GRAD. Kurs steigt = Drehung nach Steuerbord,
//  Ruderwinkel + = Steuerbord.
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <FastAccelStepper.h>

// ---------------------------- Hardware / Defaults ---------------------------
#define STEP_PIN   2
#define DIR_PIN    1
#define ENABLE_PIN 3          // Pololu ENABLE (active low)
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
bool fresh(const Sig& s, uint32_t ms) { return s.t != 0 && (millis() - s.t) <= ms; }

float wrap360(float a) { a = fmodf(a, 360.0f); if (a < 0) a += 360.0f; return a; }
float wrap180(float a) { return wrap360(a + 180.0f) - 180.0f; }
float applyDb(float e, float db) { if (e > db) return e - db; if (e < -db) return e + db; return 0; }

// ---------------------------------- Zustand ---------------------------------
enum ApState { AP_STANDBY, AP_AUTO };
ApState apState = AP_STANDBY;
char  alarmText[56] = "";
float targetDeg = 0, rudCmd = 0, rotUsed = 0, errFilt = 0;
uint32_t errBadSince = 0, stallRefT = 0; float stallRefRud = 0;
int jogDir = 0; uint32_t jogUntil = 0;
uint32_t restartAt = 0;
bool skConnected = false;

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
void goStandby(const char* alarm) {
  apState = AP_STANDBY;
  drive(0, 0);
  strlcpy(alarmText, alarm, sizeof(alarmText));
}

void doEngage() {
  if (!fresh(sHdg, cfg.staleMs) || !fresh(sRud, cfg.staleMs)) { strlcpy(alarmText, "Kurs-/Ruderdaten fehlen", sizeof(alarmText)); return; }
  if (fresh(sSog, 3000) && sSog.v > cfg.maxKmh) { strlcpy(alarmText, "Zu schnell fuer Autopilot", sizeof(alarmText)); return; }
  targetDeg = sHdg.v;                       // aktuellen Kurs halten
  errFilt = 0; errBadSince = 0; stallRefT = 0;
  alarmText[0] = 0;
  apState = AP_AUTO;
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
  rudCmd = constrain(cfg.kp * eDb - cfg.kd * rotUsed, -cfg.rudLim, cfg.rudLim);

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
    drive(max(cfg.minHz, cfg.kv * fabsf(rudErr)), rudErr > 0 ? 1 : -1);
    // Blockade / Reibrad rutscht: Motor laeuft, Ruder bewegt sich nicht
    if (!stallRefT || fabsf(sRud.v - stallRefRud) >= 0.5f) { stallRefT = now; stallRefRud = sRud.v; }
    else if (now - stallRefT > 3000) { goStandby("Ruder reagiert nicht (Reibrad/Blockade)"); return; }
  }
}

// ---------------------------------- Status ----------------------------------
String statusJson() {
  JsonDocument d;
  d["state"] = apState == AP_AUTO ? "auto" : "standby";
  d["target"] = targetDeg;
  d["heading"] = fresh(sHdg, cfg.staleMs) ? sHdg.v : NAN;
  d["rudder"] = fresh(sRud, cfg.staleMs) ? sRud.v : NAN;
  d["rudderCmd"] = rudCmd;
  d["lim"] = cfg.rudLim;
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
      if (!strcmp(path, "navigation.headingMagnetic"))      { sHdg.v = wrap360(val * RAD_TO_DEG); sHdg.t = now; }
      else if (!strcmp(path, "navigation.rateOfTurn"))      { sRot.v = val * RAD_TO_DEG * (cfg.invRot ? -1 : 1); sRot.t = now; }
      else if (!strcmp(path, "steering.rudderAngle"))       { sRud.v = val * RAD_TO_DEG * (cfg.invRud ? -1 : 1); sRud.t = now; }
      else if (!strcmp(path, "navigation.speedOverGround")) { sSog.v = val * 3.6f; sSog.t = now; }
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

// ------------------------------------ Webseite ------------------------------
WebServer server(80);

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html><html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no"><meta name="theme-color" content="#1e2327">
<title>Autopilot</title>
<style>
:root{--bg:#1e2327;--pn:#2a3238;--ln:#3b464e;--tx:#e8edf0;--mu:#8a98a3;--ac:#2ec4ff;--ok:#38d27a;--wa:#ffb020;--er:#ff4d4f}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;user-select:none}
html,body{margin:0;background:var(--bg);color:var(--tx);font-family:system-ui,-apple-system,sans-serif;min-height:100%}
#app{max-width:480px;margin:0 auto;padding:10px;display:flex;flex-direction:column;gap:10px}
.top{display:flex;justify-content:space-between;align-items:center}
.pill{padding:5px 16px;border-radius:99px;font-weight:700;letter-spacing:.1em;background:var(--ln)}
.pill.auto{background:var(--ok);color:#06210f}
.st{color:var(--mu);font-size:13px}.dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:var(--er);margin:0 10px 0 4px}.dot.on{background:var(--ok)}
.gear{background:none;border:0;color:var(--mu);font-size:22px;padding:0 4px}
.al{background:var(--er);color:#fff;font-weight:700;padding:10px;border-radius:10px;text-align:center;animation:bl 1s infinite}
@keyframes bl{50%{opacity:.55}}
svg{display:block;width:100%}
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.box{background:var(--pn);border-radius:10px;padding:8px;text-align:center}
.box small{display:block;color:var(--mu);font-size:11px;letter-spacing:.08em}.box b{font-size:20px}
.row{display:grid;grid-template-columns:1fr 1fr 1.5fr 1fr 1fr;gap:8px}
button.b{height:64px;border:0;border-radius:12px;background:var(--pn);color:var(--tx);font-size:18px;font-weight:700}
button.b:active{background:var(--ln)}button.b:disabled{opacity:.3}
button.m{background:var(--ok);color:#06210f;font-size:20px}button.m.s{background:var(--er);color:#fff}
#off{position:fixed;inset:0;background:#000c;display:flex;align-items:center;justify-content:center;font-size:20px;font-weight:700;color:var(--wa)}
#cfg label{display:flex;justify-content:space-between;align-items:center;margin:6px 0;font-size:14px;gap:8px}
#cfg input{width:9em;background:var(--pn);border:1px solid var(--ln);color:var(--tx);border-radius:6px;padding:6px}
#cfg input[type=checkbox]{width:auto}
.bar{display:flex;gap:8px;margin:10px 0;flex-wrap:wrap}.bar button{flex:1;min-width:30%}
[hidden]{display:none!important}
</style></head><body><div id="app">
<div class="top"><span id="pill" class="pill">STANDBY</span>
<span class="st">Signal K<span id="sk" class="dot"></span><button class="gear" onclick="tg()">&#9881;</button></span></div>

<div id="main" style="display:flex;flex-direction:column;gap:10px">
<div id="al" class="al" hidden></div>
<svg id="dial" viewBox="-170 -170 340 340">
<circle r="140" fill="#242b30" stroke="#3b464e" stroke-width="3"/>
<g id="card"><g id="ticks"></g><g id="tg"><rect x="-7" y="-146" width="14" height="16" rx="3" fill="#2ec4ff"/></g></g>
<path d="M0,-146 L-10,-168 H10 Z" fill="#ffb020"/>
<text y="-30" text-anchor="middle" font-size="14" fill="#8a98a3" letter-spacing="2">MAG</text>
<text id="hd" y="22" text-anchor="middle" font-size="58" font-weight="700" fill="#e8edf0">---</text>
<text id="tv" y="56" text-anchor="middle" font-size="20" font-weight="700" fill="#2ec4ff"></text>
</svg>
<svg viewBox="-150 -24 300 48">
<rect x="-140" y="-7" width="140" height="14" fill="#ff4d4f33"/><rect x="0" y="-7" width="140" height="14" fill="#38d27a33"/>
<g stroke="#8a98a3" stroke-width="1.5"><line x1="-140" y1="-9" x2="-140" y2="9"/><line x1="-70" y1="-9" x2="-70" y2="9"/><line x1="0" y1="-11" x2="0" y2="11" stroke="#e8edf0" stroke-width="2.5"/><line x1="70" y1="-9" x2="70" y2="9"/><line x1="140" y1="-9" x2="140" y2="9"/></g>
<g id="rc"><path d="M0,5 L-6,19 H6 Z" fill="#2ec4ff"/></g>
<g id="rn"><path d="M0,-5 L-8,-21 H8 Z" fill="#e8edf0"/></g>
</svg>
<div class="stats"><div class="box"><small>RUDER</small><b id="rv">-</b></div><div class="box"><small>SOG km/h</small><b id="sg">-</b></div><div class="box"><small>ROT &deg;/s</small><b id="rt">-</b></div></div>
<div class="row">
<button class="b a" onclick="c('adj',-10)">&#9664;&#9664;10</button><button class="b a" onclick="c('adj',-1)">&#9664;1</button>
<button id="mb" class="b m" onclick="mb()">AUTO</button>
<button class="b a" onclick="c('adj',1)">1&#9654;</button><button class="b a" onclick="c('adj',10)">10&#9654;&#9654;</button>
</div></div>

<div id="cfg" hidden>
<b>Ruder-Test (nur Standby, gedr&uuml;ckt halten)</b>
<div class="bar"><button class="b" id="jm">&#9664; Backbord</button><button class="b" id="jp">Steuerbord &#9654;</button></div>
<b>Konfiguration</b><div id="f"></div>
<div class="bar"><button class="b" onclick="save()">Speichern</button><button class="b" onclick="if(confirm('Neustart?'))post('/api/reboot')">Neustart</button><button class="b" onclick="tg()">Zur&uuml;ck</button></div>
<span id="m" class="st"></span>
</div></div>
<div id="off" hidden>ESP nicht erreichbar</div>
<script>
const $=i=>document.getElementById(i),w180=a=>((a+180)%360+360)%360-180,pad=n=>String(Math.round(n)%360).padStart(3,'0');
let au=false,cont=null,disp=0,fails=0;
(()=>{let g='';for(let a=0;a<360;a+=5){const L=a%30==0,M=a%10==0,r=L?118:M?124:129;
g+=`<line y1="-${r}" y2="-136" transform="rotate(${a})" stroke="${L?'#e8edf0':'#6b7a85'}" stroke-width="${L?3:1.5}"/>`;
if(L){const t=a==0?'N':a==90?'E':a==180?'S':a==270?'W':a/10;
g+=`<text transform="rotate(${a}) translate(0,-98)" text-anchor="middle" dominant-baseline="middle" font-size="${a%90?16:24}" fill="${a?'#e8edf0':'#ff6b6b'}">${t}</text>`}}
$('ticks').innerHTML=g})();
(function an(){if(cont!==null){disp+=(cont-disp)*.2;$('card').setAttribute('transform','rotate('+(-disp).toFixed(2)+')')}requestAnimationFrame(an)})();
function post(u){return fetch(u,{method:'POST'})}
function c(k,v){post('/api/cmd?c='+k+(v?'&v='+v:'')).then(poll)}
function mb(){c(au?'standby':'engage')}
function render(s){
 au=s.state=='auto';
 $('pill').textContent=au?'AUTO':'STANDBY';$('pill').className='pill'+(au?' auto':'');
 $('sk').className='dot'+(s.sk?' on':'');
 $('al').hidden=!s.alarm;$('al').textContent=s.alarm||'';
 if(s.heading!==null){if(cont===null){cont=s.heading;disp=cont}else cont+=w180(s.heading-cont);$('hd').textContent=pad(s.heading)+'\u00b0'}else $('hd').textContent='---';
 $('tg').setAttribute('transform','rotate('+s.target+')');$('tg').style.display=au?'':'none';
 $('tv').textContent=au?'SOLL '+pad(s.target)+'\u00b0':'';
 const cl=x=>Math.max(-40,Math.min(40,x))*3.5;
 if(s.rudder!==null){$('rn').setAttribute('transform','translate('+cl(s.rudder)+',0)');$('rv').textContent=(s.rudder>=0?'S ':'P ')+Math.abs(s.rudder).toFixed(1)+'\u00b0'}else $('rv').textContent='-';
 $('rc').style.display=au?'':'none';$('rc').setAttribute('transform','translate('+cl(s.rudderCmd)+',0)');
 $('sg').textContent=s.sog===null?'-':s.sog.toFixed(1);$('rt').textContent=s.rot===null?'-':s.rot.toFixed(1);
 $('mb').textContent=au?'STANDBY':'AUTO';$('mb').className='b m'+(au?' s':'');
 document.querySelectorAll('.a').forEach(b=>b.disabled=!au);
}
async function poll(){try{render(await (await fetch('/api/status',{cache:'no-store'})).json());fails=0;$('off').hidden=true}catch(e){if(++fails>2)$('off').hidden=false}}
setInterval(poll,400);poll();
function tg(){const h=$('cfg').hidden;$('cfg').hidden=!h;$('main').hidden=h;if(h)load()}
async function load(){const a=await (await fetch('/api/config')).json(),f=$('f');f.innerHTML='';
 for(const x of a){const l=document.createElement('label');l.textContent=x.label;const i=document.createElement('input');i.name=x.key;
 if(x.type=='b'){i.type='checkbox';i.checked=x.value=='1'}else{i.type=x.secret?'password':'text';i.value=x.value;if(x.secret)i.placeholder='(unver\u00e4ndert)'}
 l.appendChild(i);f.appendChild(l)}}
async function save(){const p=new URLSearchParams();for(const i of document.querySelectorAll('#f input'))p.append(i.name,i.type=='checkbox'?(i.checked?'1':'0'):i.value);
 await fetch('/api/config',{method:'POST',body:p});$('m').textContent='Gespeichert';setTimeout(()=>$('m').textContent='',2000)}
function jog(id,d){let t;const go=()=>post('/api/cmd?c=jog&v='+d),st=()=>clearInterval(t),b=$(id);
 b.onpointerdown=()=>{go();t=setInterval(go,250)};b.onpointerup=b.onpointerleave=b.onpointercancel=st}
jog('jm',-1);jog('jp',1);
</script></body></html>)rawliteral";

void setupWeb() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
  server.on("/api/status", HTTP_GET, []() { server.send(200, "application/json", statusJson()); });

  server.on("/api/cmd", HTTP_POST, []() {
    String k = server.arg("c"); float v = server.arg("v").toFloat();
    if (k == "engage") doEngage();
    else if (k == "standby") { alarmText[0] = 0; goStandby(""); }
    else if (k == "adj") { if (apState == AP_AUTO && fabsf(v) <= 30.0f) targetDeg = wrap360(targetDeg + v); }
    else if (k == "jog") { if (apState == AP_STANDBY && v != 0) { jogDir = v > 0 ? 1 : -1; jogUntil = millis() + 400; } }
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/config", HTTP_GET, []() {
    JsonDocument d; JsonArray a = d.to<JsonArray>();
    for (size_t i = 0; i < NFIELDS; i++) {
      JsonObject o = a.add<JsonObject>();
      o["key"] = fields[i].key; o["label"] = fields[i].label;
      o["type"] = String(fields[i].type); o["secret"] = fields[i].secret;
      o["value"] = fields[i].secret ? String("") : getField(fields[i]);
    }
    String s; serializeJson(d, s); server.send(200, "application/json", s);
  });
  server.on("/api/config", HTTP_POST, []() {
    for (size_t i = 0; i < NFIELDS; i++) {
      if (!server.hasArg(fields[i].key)) continue;
      String v = server.arg(fields[i].key);
      if (fields[i].secret && v.length() == 0) continue;
      setField(fields[i], v);
    }
    saveConfig();
    server.send(200, "text/plain", "OK");
  });
  server.on("/api/reboot", HTTP_POST, []() { restartAt = millis() + 500; server.send(200, "text/plain", "OK"); });
  server.begin();
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
}

void setup() {
  Serial.begin(115200);
  loadConfig();

  engine.init();
  stepper = engine.stepperConnectToPin(STEP_PIN);
  if (stepper) {
    stepper->setDirectionPin(DIR_PIN);
    stepper->setEnablePin(ENABLE_PIN);    // active low
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
  static uint32_t lastCtl = 0;
  skWs.loop();
  server.handleClient();

  uint32_t now = millis();
  if (now - lastCtl >= CONTROL_PERIOD_MS) {
    float dt = (now - lastCtl) / 1000.0f;
    lastCtl = now;
    controlStep(now, dt);
  }
  if (restartAt && now > restartAt) ESP.restart();
}
