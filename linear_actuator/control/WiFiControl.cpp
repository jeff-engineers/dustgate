// =============================================================================
// WiFiControl.cpp — Web-based control interface
// =============================================================================

#include "WiFiControl.h"

#ifdef CONTROL_WIFI

#include <Preferences.h>

WiFiControl* WiFiControl::_instance = nullptr;

// =============================================================================
// Embedded HTML — served at GET /
// Single-page app; JS polls /api/status every second and posts to /api/cmd.
// =============================================================================
static const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Dust Gate</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:#ebebeb;max-width:500px;margin:0 auto}
header{background:#1c1c1c;color:#fff;padding:14px 16px;font-size:17px;font-weight:600;letter-spacing:.3px}
.card{background:#fff;margin:12px;padding:16px;border-radius:10px}
h2{font-size:11px;font-weight:700;color:#999;text-transform:uppercase;letter-spacing:.9px;margin-bottom:14px}
p{font-size:14px;color:#555;line-height:1.5;margin-bottom:10px}
pre{font-size:13px;background:#f5f5f5;padding:12px;border-radius:6px;border:1px solid #ddd;white-space:pre-wrap;line-height:1.6;margin-bottom:14px}
label{font-size:14px;color:#444;display:block;margin-bottom:6px}
input[type=range]{width:100%;margin:6px 0 16px;accent-color:#1c1c1c;height:5px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(88px,1fr));gap:10px}
btn,button{border:none;border-radius:8px;padding:16px 6px;font-size:15px;font-weight:600;cursor:pointer;width:100%;display:block}
button:active{filter:brightness(.82)}
button:disabled{opacity:.35;pointer-events:none}
.dn{background:#e2e2e2;color:#222}
.da{background:#246224;color:#fff}
.dh{background:#2c2c2c;color:#fff}
.ds{background:#bb0000;color:#fff;font-size:17px}
.de{background:#246224;color:#fff}
.dd{background:#777;color:#fff}
.dp{background:#1a5aa8;color:#fff}
.dt{background:#6030a8;color:#fff}
.dw{background:#964800;color:#fff}
.row{display:flex;gap:10px;margin-bottom:10px}
.row button{flex:1;margin-bottom:0}
.badges{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:14px;min-height:26px}
.badge{padding:4px 11px;border-radius:20px;font-size:12px;font-weight:700;background:#e2e2e2;color:#555}
.bg{background:#ceedce;color:#174a17}
.br{background:#fdd;color:#800}
.bb{background:#cee0f5;color:#0a3860}
.spin{display:inline-block;width:13px;height:13px;border:2px solid #bbb;border-top-color:#444;border-radius:50%;animation:sp .7s linear infinite;vertical-align:middle;margin-right:6px}
@keyframes sp{to{transform:rotate(360deg)}}
.hidden{display:none!important}
.warn-text{color:#964800;font-weight:600;margin-bottom:10px;font-size:14px}
</style>
</head>
<body>
<header>Dust Gate Controller</header>

<!-- Connecting splash -->
<div id="v-wait">
  <div class="card" style="margin-top:20px">
    <h2><span class="spin"></span>Connecting…</h2>
  </div>
</div>

<!-- Setup view -->
<div id="v-setup" class="hidden">
  <div class="card">
    <h2>Setup</h2>
    <label>Number of gates: <strong id="gv">4</strong></label>
    <input type="range" id="ng" min="1" max="7" value="4"
           oninput="document.getElementById('gv').textContent=this.value">
    <div class="row" style="margin-bottom:0">
      <button class="dt" onclick="startTune()">Run Autotune</button>
      <button class="dd" onclick="skipTune()">Skip / use defaults</button>
    </div>
  </div>
  <div id="c-run" class="card hidden">
    <h2><span class="spin"></span>Autotune running…</h2>
    <p>Motor is binary-searching for the optimal StallGuard threshold.<br>Do not move the machine.</p>
  </div>
  <div id="c-done" class="card hidden">
    <h2>Autotune complete</h2>
    <pre id="tune-out"></pre>
    <button class="dp" onclick="doSave()">Save and open controls →</button>
  </div>
</div>

<!-- State-machine is autotuning (motor running) -->
<div id="v-tune" class="hidden">
  <div class="card" style="margin-top:20px">
    <h2><span class="spin"></span>Autotune in progress…</h2>
    <p>Motor is searching for the optimal StallGuard threshold.<br>Do not move the machine.</p>
  </div>
</div>

<!-- Control view -->
<div id="v-ctrl" class="hidden">
  <div class="card">
    <div class="badges" id="badges"></div>
    <p id="no-home" class="warn-text hidden">Not homed — press Home before selecting a gate.</p>
    <div class="grid" id="gates"></div>
  </div>
  <div class="card">
    <div class="row">
      <button class="dh" onclick="c('home')">⌂ Home</button>
      <button class="ds" onclick="c('estop')">■ STOP</button>
    </div>
    <div class="row" style="margin-bottom:0">
      <button class="de" onclick="c('enable')">Enable</button>
      <button class="dd" onclick="c('disable')">Disable</button>
    </div>
  </div>
  <div class="card">
    <button class="dw" onclick="doReconfig()">⚙ Reconfigure / Re-tune</button>
  </div>
</div>

<script>
let prevTuning=false, tuneDone=false, maxGates=7, firstPoll=true;

function show(id){
  ['v-wait','v-setup','v-tune','v-ctrl'].forEach(v=>{
    const el=document.getElementById(v);
    if(el) el.className=v===id?'':'hidden';
  });
}

function c(cmd,ex={}){
  fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({cmd,...ex})}).catch(()=>{});
}

function startTune(){
  const n=parseInt(document.getElementById('ng').value);
  c('setgates',{count:n});
  setTimeout(()=>c('autotune'),200);
  document.getElementById('c-run').className='card';
  document.getElementById('c-done').className='card hidden';
  tuneDone=false; prevTuning=false;
}

function skipTune(){
  const n=parseInt(document.getElementById('ng').value);
  c('setgates',{count:n});
  setTimeout(()=>c('save'),200);
}

function doSave(){ c('save'); }

function doReconfig(){
  c('reconfigure');
  show('v-setup');
  document.getElementById('c-run').className='card hidden';
  document.getElementById('c-done').className='card hidden';
  tuneDone=false; prevTuning=false;
}

function buildGates(n,cur,homed){
  const g=document.getElementById('gates');
  g.innerHTML='';
  document.getElementById('no-home').className=homed?'warn-text hidden':'warn-text';
  const mk=(lbl,stop)=>{
    const b=document.createElement('button');
    b.className=stop===cur?'da':'dn';
    b.textContent=lbl;
    if(!homed&&stop>0) b.disabled=true;
    b.onclick=()=>c('goto',{stop});
    g.appendChild(b);
  };
  mk('Home',0);
  for(let i=1;i<=n;i++) mk('Gate '+i,i);
}

function bdg(t,cls){
  const s=document.createElement('span');
  s.className='badge '+(cls||'');
  s.textContent=t;
  return s;
}

function poll(){
  fetch('/api/status').then(r=>r.json()).then(s=>{
    if(firstPoll){
      firstPoll=false;
      // Set slider max from firmware compile-time NUM_STOPS
      const sl=document.getElementById('ng');
      maxGates=s.maxGates||7;
      sl.max=maxGates;
      if(parseInt(sl.value)>maxGates) sl.value=maxGates;
      document.getElementById('gv').textContent=sl.value;
    }

    // State machine is running autotune — dedicated view
    if(s.state==='AUTOTUNING'){
      show('v-tune');
      prevTuning=true;
      return;
    }

    // Not yet configured (first run or after reconfigure)
    if(!s.configured){
      show('v-setup');
      // Detect autotune completion
      if(prevTuning && !tuneDone){
        tuneDone=true;
        document.getElementById('c-run').className='card hidden';
        let out='';
        if(s.recSGTHRS>=0){
          out='Recommended settings:\n\n'
            +'#define TMC2209_STALL_THRESHOLD     '+s.recSGTHRS+'\n'
            +'#define HOMING_SPEED_STEPS_PER_SEC  '+Math.round(s.recSpeed)+'f\n\n'
            +'Values saved to flash — no need to update config.h.';
        } else {
          out='No reliable threshold found.\n'
            +'Try increasing motor current or re-run autotune.\n'
            +'You can still save and use config.h defaults.';
        }
        document.getElementById('tune-out').textContent=out;
        document.getElementById('c-done').className='card';
      }
      return;
    }

    // Normal control view
    show('v-ctrl');
    const homed=s.currentStop>=0;
    buildGates(s.numGates,s.currentStop,homed);

    const bar=document.getElementById('badges');
    bar.innerHTML='';
    const sc={AT_STOP:'bg',ERROR:'br',HOMING:'bb',MOVING:'bb',AUTOTUNING:'bb'}[s.state]||'';
    bar.appendChild(bdg(s.state,sc));
    if(homed) bar.appendChild(bdg(s.currentStop===0?'Home':'Gate '+s.currentStop));
    bar.appendChild(bdg(s.enabled?'Enabled':'Disabled',s.enabled?'bg':''));
    bar.appendChild(bdg('Relay '+(s.relayOn?'ON':'off'),s.relayOn?'bg':''));
    if(s.eStop) bar.appendChild(bdg('E-STOP','br'));
  }).catch(()=>{});
}

poll();
setInterval(poll,1000);
</script>
</body>
</html>
)rawhtml";


// =============================================================================
// Constructor
// =============================================================================
WiFiControl::WiFiControl()
    : _server(WIFI_PORT),
      _requestedStop(0),
      _enabled(false),
      _eStopPending(false),
      _homePending(false),
      _autotunePending(false),
      _savePending(false),
      _reconfigPending(false),
      _pendingGateCount(-1),
      _configured(false),
      _savedSGTHRS(-1),
      _savedSpeed(-1.0f)
{
    memset(&_snap, 0, sizeof(_snap));
    strncpy(_snap.state, "STARTUP", sizeof(_snap.state));
    _snap.numGates   = NUM_STOPS;
    _snap.maxGates   = NUM_STOPS;
    _snap.currentStop = -1;
}

// =============================================================================
// begin()
// =============================================================================
bool WiFiControl::begin() {
    _instance = this;
    loadPreferences();

    // WiFi setup
#ifdef WIFI_STA_SSID
    DEBUG_PRINT(F("[WiFi] Connecting to ")); DEBUG_PRINTLN(F(WIFI_STA_SSID));
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 12000) {
        delay(200);
        DEBUG_PRINT(F("."));
    }
    if (WiFi.status() == WL_CONNECTED) {
        DEBUG_PRINTLN(F(""));
        DEBUG_PRINT(F("[WiFi] Connected. IP: "));
        DEBUG_PRINTLN(WiFi.localIP().toString());
    } else {
        DEBUG_PRINTLN(F(""));
        DEBUG_PRINTLN(F("[WiFi] Station connect failed — falling back to AP mode."));
        startAP();
    }
#else
    startAP();
#endif

    // Register routes
    _server.on("/",           HTTP_GET,  []() { _instance->handleRoot();    });
    _server.on("/api/status", HTTP_GET,  []() { _instance->handleStatus();  });
    _server.on("/api/cmd",    HTTP_POST, []() { _instance->handleCommand(); });
    _server.onNotFound(              []() { _instance->handleNotFound(); });
    _server.begin();

    DEBUG_PRINT(F("[WiFi] HTTP server started on port ")); DEBUG_PRINTLN(WIFI_PORT);
    return true;
}

void WiFiControl::startAP() {
    WiFi.mode(WIFI_AP);
    const char* pass = (strlen(WIFI_AP_PASS) > 0) ? WIFI_AP_PASS : nullptr;
    WiFi.softAP(WIFI_AP_SSID, pass);
    DEBUG_PRINT(F("[WiFi] Access point: ")); DEBUG_PRINTLN(WIFI_AP_SSID);
    DEBUG_PRINT(F("[WiFi] Connect to: http://"));
    DEBUG_PRINTLN(WiFi.softAPIP().toString());
}

// =============================================================================
// update() — must be called every loop()
// =============================================================================
void WiFiControl::update() {
    _server.handleClient();
}

// =============================================================================
// ControlInput interface
// =============================================================================
int  WiFiControl::readRequestedStop() { return _requestedStop; }
bool WiFiControl::isEnabled()         { return _enabled; }

// =============================================================================
// Consume methods
// =============================================================================
bool WiFiControl::consumeEStop() {
    if (_eStopPending) { _eStopPending = false; return true; } return false;
}
bool WiFiControl::consumeHomeRequest() {
    if (_homePending) { _homePending = false; return true; } return false;
}
bool WiFiControl::consumeAutotuneRequest() {
    if (_autotunePending) { _autotunePending = false; return true; } return false;
}
bool WiFiControl::consumeSaveRequest() {
    if (_savePending) { _savePending = false; return true; } return false;
}
bool WiFiControl::consumeReconfigureRequest() {
    if (_reconfigPending) { _reconfigPending = false; return true; } return false;
}

// =============================================================================
// pushStatus() — main.ino calls this every loop()
// =============================================================================
void WiFiControl::pushStatus(const char* state, int numGates, int currentStop,
                              bool relayOn, bool enabled, bool eStop,
                              bool autotuneRunning, bool autotuneDone,
                              int recSGTHRS, float recSpeed) {
    strncpy(_snap.state, state, sizeof(_snap.state) - 1);
    _snap.numGates       = numGates;
    _snap.maxGates       = NUM_STOPS;
    _snap.currentStop    = currentStop;
    _snap.relayOn        = relayOn;
    _snap.enabled        = enabled;
    _snap.eStop          = eStop;
    _snap.configured     = _configured;
    _snap.autotuneRunning = autotuneRunning;
    _snap.autotuneDone   = autotuneDone;
    _snap.recSGTHRS      = recSGTHRS;
    _snap.recSpeed       = recSpeed;
}

// =============================================================================
// performSave() — persists tuning results to NVS flash
// =============================================================================
void WiFiControl::performSave(int numGates, int sgthrs, float speed) {
    _configured   = true;
    _savedSGTHRS  = sgthrs;
    _savedSpeed   = speed;
    _snap.configured = true;

    Preferences prefs;
    prefs.begin("dustgate", false);
    prefs.putBool ("configured", true);
    prefs.putInt  ("numGates",   numGates);
    prefs.putInt  ("sgthrs",     sgthrs);
    prefs.putFloat("speed",      speed);
    prefs.end();

    DEBUG_PRINT(F("[WiFi] Settings saved. gates=")); DEBUG_PRINT(numGates);
    DEBUG_PRINT(F(" sgthrs=")); DEBUG_PRINT(sgthrs);
    DEBUG_PRINT(F(" speed=")); DEBUG_PRINTLN(speed, 0);
}

// =============================================================================
// clearConfiguration() — wipes saved config, returns to setup view
// =============================================================================
void WiFiControl::clearConfiguration() {
    _configured  = false;
    _savedSGTHRS = -1;
    _savedSpeed  = -1.0f;
    _snap.configured = false;

    Preferences prefs;
    prefs.begin("dustgate", false);
    prefs.clear();
    prefs.end();

    DEBUG_PRINTLN(F("[WiFi] Configuration cleared."));
}

// =============================================================================
// loadPreferences()
// =============================================================================
void WiFiControl::loadPreferences() {
    Preferences prefs;
    prefs.begin("dustgate", true); // read-only
    _configured  = prefs.getBool ("configured", false);
    _savedSGTHRS = prefs.getInt  ("sgthrs",     -1);
    _savedSpeed  = prefs.getFloat("speed",      -1.0f);
    int savedGates = prefs.getInt("numGates", NUM_STOPS);
    prefs.end();

    _snap.configured = _configured;
    _snap.numGates   = savedGates;

    if (_configured) {
        DEBUG_PRINT(F("[WiFi] Loaded saved config: gates=")); DEBUG_PRINT(savedGates);
        DEBUG_PRINT(F(" sgthrs=")); DEBUG_PRINT(_savedSGTHRS);
        DEBUG_PRINT(F(" speed=")); DEBUG_PRINTLN(_savedSpeed, 0);
    } else {
        DEBUG_PRINTLN(F("[WiFi] No saved config — setup required."));
    }
}

// =============================================================================
// HTTP handlers
// =============================================================================
void WiFiControl::handleRoot() {
    _server.send_P(200, "text/html", INDEX_HTML);
}

void WiFiControl::handleStatus() {
    // Build JSON manually — no library needed for this simple structure
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{"
        "\"state\":\"%s\","
        "\"numGates\":%d,"
        "\"maxGates\":%d,"
        "\"currentStop\":%d,"
        "\"relayOn\":%s,"
        "\"enabled\":%s,"
        "\"eStop\":%s,"
        "\"configured\":%s,"
        "\"autotuneRunning\":%s,"
        "\"autotuneDone\":%s,"
        "\"recSGTHRS\":%d,"
        "\"recSpeed\":%.1f"
        "}",
        _snap.state,
        _snap.numGates,
        _snap.maxGates,
        _snap.currentStop,
        _snap.relayOn        ? "true" : "false",
        _snap.enabled        ? "true" : "false",
        _snap.eStop          ? "true" : "false",
        _snap.configured     ? "true" : "false",
        _snap.autotuneRunning ? "true" : "false",
        _snap.autotuneDone   ? "true" : "false",
        _snap.recSGTHRS,
        (double)_snap.recSpeed
    );

    _server.sendHeader("Cache-Control", "no-cache");
    _server.send(200, "application/json", buf);
}

void WiFiControl::handleCommand() {
    String body = _server.arg("plain");
    String cmd  = extractStr(body, "cmd");

    if      (cmd == "enable")      { _enabled = true; }
    else if (cmd == "disable")     { _enabled = false; }
    else if (cmd == "estop")       { _enabled = false; _eStopPending = true; }
    else if (cmd == "home")        { _enabled = false; _eStopPending = false; _homePending = true; }
    else if (cmd == "goto") {
        int stop = extractInt(body, "stop");
        if (stop >= 0 && stop <= _snap.numGates) {
            _requestedStop = stop;
            _enabled = true;
        }
    }
    else if (cmd == "autotune")    { _autotunePending = true; }
    else if (cmd == "setgates") {
        int n = extractInt(body, "count");
        if (n >= 1 && n <= NUM_STOPS) _pendingGateCount = n;
    }
    else if (cmd == "save")        { _savePending = true; }
    else if (cmd == "reconfigure") { _reconfigPending = true; }

    _server.sendHeader("Cache-Control", "no-cache");
    _server.send(200, "application/json", "{\"ok\":true}");
}

void WiFiControl::handleNotFound() {
    _server.send(404, "text/plain", "Not found");
}

// =============================================================================
// JSON helpers — minimal, for our controlled API only
// =============================================================================
String WiFiControl::extractStr(const String& json, const char* key) {
    String search = "\""; search += key; search += "\":\"";
    int i = json.indexOf(search);
    if (i < 0) return "";
    i += search.length();
    int j = json.indexOf('"', i);
    return (j < 0) ? "" : json.substring(i, j);
}

int WiFiControl::extractInt(const String& json, const char* key, int def) {
    String search = "\""; search += key; search += "\":";
    int i = json.indexOf(search);
    if (i < 0) return def;
    return json.substring(i + search.length()).toInt();
}

#endif // CONTROL_WIFI
