// Indygo-Pool-Web-API gateway - milestone M1a:
//   * first-connection access point + captive portal, then join your home Wi-Fi from the web UI
//   * BLE device discovery menu, classification of Indygo/Solem pool equipment, selection saved
//   * REST API (see api/openapi.yaml)
// It does NOT connect to the pool device yet (protocol work is tracked in docs/ROADMAP.md).
//
// Board: ESP32 (Arduino core 3.x), partition scheme "Minimal SPIFFS" (the BLE+Wi-Fi image is
// larger than the default 1.3 MB app partition). Libraries: built-in BLE, WiFi, WebServer,
// DNSServer, ESPmDNS, Preferences, plus ArduinoJson 7.

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define FIRMWARE_VERSION "0.4.0-m1a"
#define HOSTNAME "indygo-gateway"
#define AP_PASSWORD "indygo-setup"  // default password of the first-connection access point
#define STA_CONNECT_TIMEOUT_MS 15000
#define AP_RETRY_STA_MS 60000
#define MAX_DEVICES 40
#define DEFAULT_SCAN_SECONDS 8
#define MAX_SCAN_SECONDS 30

struct BleEntry {
  String address;
  String name;
  int rssi;
  String family;        // pooltech | sterilor_evo | solem_ipx | unknown
  String services;      // comma separated service UUIDs seen in the advertisement
  String manufacturer;  // manufacturer data as hex
};

static BleEntry devices[MAX_DEVICES];
static int deviceCount = 0;
static volatile bool scanning = false;
static BLEScan *bleScan = nullptr;
static WebServer server(80);
static DNSServer dnsServer;
static Preferences prefs;
static String selAddress, selName;

static bool apMode = false;
static String apSsid;
static String savedSsid;           // home network stored in flash ("" = none)
static uint32_t restartAtMs = 0;   // non-zero: restart when millis() reaches it
static uint32_t lastStaRetryMs = 0;

// ---------- demo mode ----------
// Simulated pool device so the full interface can be explored without real equipment. ALL values are
// invented; the field set is illustrative (modelled on what the vendor app exposes for treatment units),
// NOT a verified protocol. Nothing is read from or sent to real equipment while it is on.
static bool demoMode = false;
static struct {
  float phSetpoint = 7.2f;
  int orpSetpoint = 670;
  int productionPercent = 50;
  String mode = "auto";  // off | auto | on
  bool boost = false;
  uint32_t boostEndMs = 0;
  bool winter = false;
} demo;
#define DEMO_BOOST_MINUTES 120

// ---------- classification (patterns from docs/protocol-ble.md) ----------
static String classify(const String &name, const String &services) {
  if (name.startsWith("GEN_")) return "pooltech";
  if (name.startsWith("EVO_")) return "sterilor_evo";
  String s = services;
  s.toLowerCase();
  // 84A4000x-C06B-4D49-87BC-EDBBAE1F9EAB = generic Solem IPX service (x = variant index)
  if (s.indexOf("84a4000") >= 0 && s.indexOf("-c06b-4d49-87bc-edbbae1f9eab") >= 0) return "solem_ipx";
  return "unknown";
}

static String toHex(const String &raw) {
  String out;
  char buf[3];
  for (size_t i = 0; i < raw.length(); i++) {
    snprintf(buf, sizeof(buf), "%02x", (uint8_t)raw[i]);
    out += buf;
  }
  return out;
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    String addr = dev.getAddress().toString();
    int idx = -1;
    for (int i = 0; i < deviceCount; i++)
      if (devices[i].address == addr) { idx = i; break; }
    if (idx < 0) {
      if (deviceCount >= MAX_DEVICES) return;
      idx = deviceCount++;
      devices[idx].address = addr;
    }
    BleEntry &e = devices[idx];
    e.rssi = dev.getRSSI();
    if (dev.haveName()) e.name = dev.getName();
    String svc;
    for (int i = 0; i < dev.getServiceUUIDCount(); i++) {
      if (i) svc += ",";
      svc += dev.getServiceUUID(i).toString();
    }
    if (svc.length()) e.services = svc;
    if (dev.haveManufacturerData()) e.manufacturer = toHex(dev.getManufacturerData());
    e.family = classify(e.name, e.services);
  }
};
static ScanCallbacks scanCallbacks;

static void onScanDone(BLEScanResults) { scanning = false; }

static bool startScan(int seconds) {
  if (scanning) return false;
  deviceCount = 0;
  bleScan->clearResults();
  scanning = true;
  if (!bleScan->start(seconds, onScanDone, false)) {
    scanning = false;
    return false;
  }
  return true;
}

// ---------- web UI ----------
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Indygo Pool Gateway</title>
<style>
 body{font-family:system-ui,sans-serif;margin:0;background:#f3f6f8;color:#14232b}
 header{background:#0b5d7a;color:#fff;padding:14px 18px;font-size:18px}
 nav{display:flex;gap:4px;background:#0a4f67;padding:0 12px;overflow-x:auto}
 nav button{background:none;border:0;color:#cfe8f1;padding:12px 14px;font-size:15px;cursor:pointer;white-space:nowrap}
 nav button.on{color:#fff;border-bottom:3px solid #6fd3f0}
 main{padding:16px;max-width:900px;margin:auto}
 .card{background:#fff;border-radius:10px;padding:14px;margin-bottom:14px;box-shadow:0 1px 3px #0002}
 table{width:100%;border-collapse:collapse;font-size:14px}
 th,td{text-align:left;padding:8px 6px;border-bottom:1px solid #e3e9ec}
 .pool{background:#e4f6ec}.btn{background:#0b5d7a;color:#fff;border:0;border-radius:6px;padding:8px 14px;cursor:pointer}
 .btn.alt{background:#8a3b3b}.btn[disabled]{opacity:.5}.tag{font-size:12px;padding:2px 7px;border-radius:10px;background:#dfe7ea}
 .tag.pooltech,.tag.sterilor_evo,.tag.solem_ipx{background:#2e9e5b;color:#fff}.muted{color:#5a6c75;font-size:13px}
 input,select{padding:8px;border:1px solid #b9c7cd;border-radius:6px;width:100%;box-sizing:border-box;margin:4px 0 10px}
 .hide{display:none}
</style></head><body>
<header>Indygo Pool Gateway <span class="muted" id="ver" style="color:#cfe8f1"></span></header>
<div id="demo" class="hide" style="background:#b36b00;color:#fff;padding:8px 18px;font-size:14px">DEMO MODE - simulated device and invented data. Nothing is read from or sent to real equipment.</div>
<nav><button id="t-status" class="on" onclick="tab('status')">Status</button>
<button id="t-ble" onclick="tab('ble')">Bluetooth devices</button>
<button id="t-products" onclick="tab('products')">Products</button>
<button id="t-pool" onclick="tab('pool')">Pool</button>
<button id="t-lighting" onclick="tab('lighting')">Lighting</button>
<button id="t-settings" onclick="tab('settings')">Settings</button>
<button id="t-wifi" onclick="tab('wifi')">Wi-Fi</button></nav>
<main>
<section id="status" class="card"><h3>Selected pool device</h3><div id="sel">Loading...</div>
<p class="muted">Open the <b>Bluetooth devices</b> menu to find and select your device.</p>
<label><input type="checkbox" id="demoSw" onchange="setDemo(this.checked)" style="width:auto"> Demo mode (simulated device, to preview the interface)</label>
<h3>Live measurements</h3><table><tbody id="live"><tr><td class="muted">No device connected.</td></tr></tbody></table>
<h3>Network</h3><div id="net">Loading...</div></section>

<section id="products" class="hide"><div class="card"><h3>Products</h3>
 <p class="muted">The product families offered by the vendor app. Entries marked <b>Preview</b> have a demo panel (turn on Demo mode on the Status page); the others are listed but not supported yet.</p>
 <div id="cat" class="muted">Loading...</div></div></section>

<section id="pool" class="hide"><div class="card"><h3>Pool filtration</h3>
 <div id="nodemo-pool" class="muted hide">Demo mode only for now: turn it on from the Status page.</div>
 <div id="pool-body">
 <div id="pl-state" class="muted">Loading...</div>
 <label>Filtration rule</label><select id="pl-rule" onchange="setRule()"><option value="0">Time</option><option value="1">Thermo-adaptive</option><option value="2">Variable speed pump</option></select>
 <label>Duration (minutes)</label><input id="pl-min" type="number" value="60" min="1" max="1440">
 <label>Speed (variable-speed pumps)</label><select id="pl-speed"><option>1</option><option selected>2</option><option>3</option></select>
 <label>Pause (days)</label><input id="pl-days" type="number" value="1" min="1" max="30">
 <p><button class="btn" onclick="poolCmd('manualOn')">Manual ON</button>
 <button class="btn" onclick="poolCmd('boost')">Boost 24 h</button>
 <button class="btn" onclick="poolCmd('pause')">Pause</button>
 <button class="btn" onclick="poolCmd('backwash')">Backwash 2 min</button>
 <button class="btn alt" onclick="poolCmd('stopManualCommand')">Stop / back to auto</button></p>
 <p class="muted">Same commands as the vendor app: manualOn, boost, pause, backwash, stopManualCommand. Durations and limits are assumptions in this preview.</p></div></div></section>

<section id="lighting" class="hide"><div class="card"><h3>Lighting</h3>
 <div id="nodemo-lighting" class="muted hide">Demo mode only for now: turn it on from the Status page.</div>
 <div id="light-body">
 <div id="lt-state" class="muted">Loading...</div>
 <p><button class="btn" onclick="lightCmd({command:'manualOn',index:1,duration:3600})">Light ON (1 h)</button>
 <button class="btn alt" onclick="lightCmd({command:'stopManualCommand',index:1})">Light OFF</button>
 <button class="btn" onclick="lightCmd({command:'colorChange',index:1})">Next colour (pulse)</button></p>
 <label>Spotlight type</label><select id="lt-type" onchange="drawPalette()"><option>CCEI</option><option>Seamaid</option></select>
 <div id="lt-pal"></div>
 <p class="muted">CCEI values and Seamaid modes are those used by the vendor app. Grouping of the CCEI entries (colours, levels, programs, speeds) is my reading of the app and may change.</p></div></div></section>

<section id="settings" class="hide"><div class="card"><h3>Settings</h3>
 <div id="cfgmsg" class="muted">No device connected. Select a device, or enable demo mode on the Status page.</div>
 <div id="cfgform" class="hide">
 <label>pH setpoint (6.8 - 7.8)</label><input id="c-ph" type="number" step="0.1" min="6.8" max="7.8">
 <label>ORP setpoint, mV (400 - 900)</label><input id="c-orp" type="number" step="10" min="400" max="900">
 <label>Chlorine production, % (0 - 100)</label><input id="c-pct" type="number" min="0" max="100">
 <label>Mode</label><select id="c-mode"><option>off</option><option>auto</option><option>on</option></select>
 <label><input type="checkbox" id="c-boost" style="width:auto"> Boost</label>
 <label><input type="checkbox" id="c-winter" style="width:auto"> Winter mode</label>
 <button class="btn" onclick="saveCfg()">Apply</button> <span class="muted" id="cfgstate"></span></div></div></section>

<section id="ble" class="hide">
 <div class="card"><button class="btn" id="scan" onclick="scan()">Scan nearby Bluetooth devices</button>
 <span class="muted" id="state"></span>
 <p class="muted">Devices recognised as Indygo / Solem pool equipment are highlighted. Keep the ESP32 close to the device.</p></div>
 <div class="card"><table><thead><tr><th>Name</th><th>Address</th><th>RSSI</th><th>Type</th><th></th></tr></thead>
 <tbody id="rows"><tr><td colspan="5" class="muted">No scan yet.</td></tr></tbody></table></div>
</section>

<section id="wifi" class="hide">
 <div class="card"><h3>Connect to your home Wi-Fi</h3>
 <p class="muted" id="wmode"></p>
 <button class="btn" id="wscan" onclick="wscan()">Scan Wi-Fi networks</button> <span class="muted" id="wstate"></span>
 <table><tbody id="wrows"></tbody></table>
 <label>Network name (SSID, 2.4 GHz only)</label><input id="ssid" autocomplete="off" maxlength="32">
 <label>Password</label><input id="pass" type="password" autocomplete="off" maxlength="63">
 <button class="btn" onclick="wsave()">Save and connect</button>
 <button class="btn alt" onclick="wforget()">Forget saved network</button>
 <p class="muted">After saving, the gateway restarts and joins your network. Reconnect your phone/PC to your home Wi-Fi and open
 <b>http://indygo-gateway.local</b> (or the IP shown by your router). If it cannot connect within about 15 seconds, it
 brings its own access point back so you can try again.</p></div>
</section></main>
<script>
const $=id=>document.getElementById(id);let poll,wpoll;const T=['status','products','pool','lighting','ble','settings','wifi'];
function tab(t){for(const s of T){$(s).classList.toggle('hide',s!==t);$('t-'+s).classList.toggle('on',s===t)}if(t==='status'){loadSel();loadNet();live()}if(t==='wifi')loadNet();if(t==='settings')loadCfg();if(t==='products')loadCat();if(t==='pool'||t==='lighting')loadSim()}
async function j(u,o){const r=await fetch(u,o);if(!r.ok)throw new Error(r.status);return r.status===204?null:r.json()}
async function loadSel(){const d=await j('/api/v1/device');$('sel').textContent=d.address?((d.name||'(no name)')+' - '+d.address):'None selected yet.'}
async function loadNet(){const w=await j('/api/v1/wifi');
 const t=w.mode==='ap'?'First-connection access point "'+w.apSsid+'" ('+w.ip+'). No home network joined yet.':'Connected to "'+w.ssid+'" - '+w.ip+' ('+w.hostname+')';
 $('net').textContent=t;$('wmode').textContent=t;if(w.savedSsid&&!$('ssid').value)$('ssid').value=w.savedSsid}
async function scan(){$('scan').disabled=true;$('state').textContent='Scanning...';await j('/api/v1/ble/scan',{method:'POST'});poll=setInterval(refresh,1000)}
async function refresh(){const d=await j('/api/v1/ble/scan');$('state').textContent=d.scanning?'Scanning...':'Scan finished: '+d.devices.length+' device(s).';
 const rows=d.devices.slice().sort((a,b)=>(b.family!=='unknown')-(a.family!=='unknown')||b.rssi-a.rssi);
 $('rows').innerHTML=rows.length?'':'<tr><td colspan="5" class="muted">Nothing found.</td></tr>';
 for(const x of rows){const tr=document.createElement('tr');if(x.family!=='unknown')tr.className='pool';
  tr.innerHTML='<td></td><td></td><td></td><td><span class="tag"></span></td><td><button class="btn">Select</button></td>';
  const c=tr.children;c[0].textContent=x.name||'(no name)';c[1].textContent=x.address;c[2].textContent=x.rssi;
  c[3].firstChild.textContent=x.family;c[3].firstChild.classList.add(x.family);
  c[4].firstChild.onclick=async()=>{await j('/api/v1/device',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({address:x.address,name:x.name})});tab('status')};
  $('rows').appendChild(tr)}
 if(!d.scanning){clearInterval(poll);$('scan').disabled=false}}
async function wscan(){$('wscan').disabled=true;$('wstate').textContent='Scanning...';await j('/api/v1/wifi/scan',{method:'POST'});wpoll=setInterval(wrefresh,1500)}
async function wrefresh(){const d=await j('/api/v1/wifi/scan');if(d.scanning)return;clearInterval(wpoll);$('wscan').disabled=false;
 $('wstate').textContent=d.networks.length+' network(s) found. Tap one to use it.';$('wrows').innerHTML='';
 for(const n of d.networks.sort((a,b)=>b.rssi-a.rssi)){const tr=document.createElement('tr');tr.style.cursor='pointer';
  tr.innerHTML='<td></td><td></td><td></td>';tr.children[0].textContent=n.ssid;tr.children[1].textContent=n.rssi+' dBm';tr.children[2].textContent=n.secure?'secured':'open';
  tr.onclick=()=>{$('ssid').value=n.ssid;$('pass').focus()};$('wrows').appendChild(tr)}}
async function wsave(){const s=$('ssid').value;if(!s){alert('Enter the network name');return}
 try{await j('/api/v1/wifi',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,password:$('pass').value})});
  $('wmode').textContent='Saved. The gateway is restarting - reconnect to your home Wi-Fi and open http://indygo-gateway.local'}
 catch(e){alert('Could not save (password must be empty or 8-63 characters).')}}
async function wforget(){await j('/api/v1/wifi',{method:'DELETE'});$('wmode').textContent='Network forgotten. Restarting in access point mode...'}
let SIM=null;const CSS={'Hot white':'#ffd9a0','White':'#ffffff','Blue':'#1f4fff','Lagoon':'#1ec3b0','Cyan':'#19d3f0','Purple':'#7a2ee0','Magenta':'#e22bd0','Pink':'#ff8fc0','Red':'#e02424','Orange':'#ff8a1f','Green':'#25b24a','Favorite':'#f2c500'};
function fmt(s){if(!s)return'';const m=Math.floor(s/60);return m>=60?Math.floor(m/60)+' h '+(m%60)+' min':m+' min '+(s%60)+' s'}
async function loadCat(){const d=await j('/api/v1/catalog');const box=$('cat');box.innerHTML='';
 for(const g of['pool','relay']){const h=document.createElement('h4');h.textContent=g==='pool'?'Pool':'Relay / valve controllers';box.appendChild(h);
  for(const p of d.products.filter(x=>x.group===g)){const r=document.createElement('div');r.style.cssText='padding:8px 0;border-bottom:1px solid #e3e9ec';
   const b=document.createElement('b');b.textContent=p.name+' ';r.appendChild(b);
   if(p.demoPanel){const t=document.createElement('span');t.className='tag solem_ipx';t.textContent='Preview';r.appendChild(t);
    const bt=document.createElement('button');bt.className='btn';bt.style.marginLeft='8px';bt.textContent='Open';bt.onclick=()=>tab(p.demoPanel);r.appendChild(bt)}
   const ds=document.createElement('div');ds.className='muted';ds.textContent=p.description;r.appendChild(ds);box.appendChild(r)}}}
async function loadSim(){let d=null;try{d=await j('/api/v1/sim')}catch(e){}SIM=d;
 for(const k of['pool','lighting']){$('nodemo-'+k).classList.toggle('hide',!!d);$(k+'-body').classList.toggle('hide',!d)}
 if(!d)return;const p=d.pool,l=d.lighting;
 $('pl-state').textContent='State: '+p.state+(p.remainingSeconds?' ('+fmt(p.remainingSeconds)+' left)':'')+' - rule: '+p.ruleName;$('pl-rule').value=p.rule;
 $('lt-state').textContent='Light: '+(l.on?'on':'off')+' - '+l.spotlight+' - '+l.color+(l.on&&l.remainingSeconds?' ('+fmt(l.remainingSeconds)+' left)':'');
 $('lt-type').value=l.spotlight;drawPalette()}
async function sendCmd(o){try{await j('/api/v1/commands',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)})}catch(e){alert('Command rejected')}loadSim()}
function poolCmd(c){const o={command:c,index:0};
 if(c==='manualOn'){o.duration=parseInt($('pl-min').value)*60;o.speed=parseInt($('pl-speed').value)}
 if(c==='pause')o.pauseDurationInDays=parseInt($('pl-days').value);
 sendCmd(o)}
async function setRule(){try{await j('/api/v1/pool',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({rule:parseInt($('pl-rule').value)})})}catch(e){}loadSim()}
function lightCmd(o){sendCmd(o)}
function drawPalette(){const box=$('lt-pal');box.innerHTML='';if(!SIM)return;
 if($('lt-type').value==='Seamaid'){for(const [n,v] of[['Change colour',16],['Save colour',48],['Reset',32]]){const b=document.createElement('button');b.className='btn';b.style.margin='3px';b.textContent=n;b.onclick=()=>lightCmd({command:'colorChangeSeamaid',index:1,mode:v});box.appendChild(b)}return}
 let last='';for(const c of SIM.lighting.palette){if(c.kind!==last){last=c.kind;const h=document.createElement('div');h.className='muted';h.style.marginTop='8px';h.textContent=c.kind;box.appendChild(h)}
  const b=document.createElement('button');b.className='btn';b.style.cssText='margin:3px;'+(CSS[c.name]?'background:'+CSS[c.name]+';color:'+(['White','Hot white','Cyan','Pink','Favorite','Lagoon'].includes(c.name)?'#14232b':'#fff')+';border:1px solid #0003':'');
  b.textContent=c.name;b.onclick=()=>lightCmd({command:'colorChangeCCEI',index:1,mode:c.value});box.appendChild(b)}}
async function loadDemo(){const d=await j('/api/v1/demo');$('demoSw').checked=d.enabled;$('demo').classList.toggle('hide',!d.enabled)}
async function setDemo(on){await j('/api/v1/demo',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled:on})});loadDemo();loadSel();live();loadCfg()}
const LBL=[['ph','pH',''],['orp','ORP',' mV'],['saltGramsPerLiter','Salt',' g/L'],['waterTemperatureC','Water temperature',' °C'],['productionPercent','Chlorine production',' %'],['mode','Mode',''],['boostRemainingMinutes','Boost remaining',' min'],['winterMode','Winter mode','']];
async function live(){let d=null;try{d=await j('/api/v1/status')}catch(e){}
 $('live').innerHTML='';if(!d){$('live').innerHTML='<tr><td class="muted">No device connected.</td></tr>';return}
 for(const [k,n,u] of LBL){if(d[k]===undefined)continue;const tr=document.createElement('tr');tr.innerHTML='<td></td><td></td>';
  tr.children[0].textContent=n;let v=d[k];if(typeof v==='boolean')v=v?'on':'off';else if(typeof v==='number')v=(Number.isInteger(v)?v:v.toFixed(2));tr.children[1].textContent=v+u;$('live').appendChild(tr)}
 const a=document.createElement('tr');a.innerHTML='<td></td><td></td>';a.children[0].textContent='Alarms';a.children[1].textContent=d.alarms&&d.alarms.length?d.alarms.join(', '):'none';$('live').appendChild(a)}
async function loadCfg(){let c=null;try{c=await j('/api/v1/config')}catch(e){}
 $('cfgform').classList.toggle('hide',!c);$('cfgmsg').classList.toggle('hide',!!c);if(!c)return;
 $('c-ph').value=c.phSetpoint;$('c-orp').value=c.orpSetpoint;$('c-pct').value=c.productionPercent;$('c-mode').value=c.mode;$('c-boost').checked=c.boost;$('c-winter').checked=c.winterMode}
async function saveCfg(){try{await j('/api/v1/config',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({phSetpoint:parseFloat($('c-ph').value),orpSetpoint:parseInt($('c-orp').value),productionPercent:parseInt($('c-pct').value),mode:$('c-mode').value,boost:$('c-boost').checked,winterMode:$('c-winter').checked})});$('cfgstate').textContent='Applied.'}
 catch(e){$('cfgstate').textContent='Rejected: value outside the allowed range.'}setTimeout(()=>$('cfgstate').textContent='',3000);live()}
j('/api/v1/gateway').then(g=>$('ver').textContent='v'+g.firmware);loadSel();loadNet();loadDemo();live();setInterval(()=>{if(!$('status').classList.contains('hide'))live()},3000);
</script></body></html>)HTML";

static void sendJson(int code, JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

static void handleGateway() {
  JsonDocument doc;
  doc["firmware"] = FIRMWARE_VERSION;
  doc["uptimeSeconds"] = (uint32_t)(millis() / 1000);
  doc["wifiRssi"] = apMode ? 0 : WiFi.RSSI();
  doc["bleConnected"] = false;  // connection to the pool device is not implemented yet
  doc["demo"] = demoMode;
  doc["scanning"] = (bool)scanning;
  sendJson(200, doc);
}

// ----- BLE -----
static void handleScanStart() {
  int secs = server.hasArg("seconds") ? server.arg("seconds").toInt() : DEFAULT_SCAN_SECONDS;
  secs = constrain(secs, 1, MAX_SCAN_SECONDS);
  if (!startScan(secs)) {
    JsonDocument doc;
    doc["error"] = "scan already running or could not start";
    sendJson(409, doc);
    return;
  }
  server.send(202, "application/json", "{\"scanning\":true}");
}

static void handleScanResults() {
  JsonDocument doc;
  doc["scanning"] = (bool)scanning;
  JsonArray arr = doc["devices"].to<JsonArray>();
  for (int i = 0; i < deviceCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["address"] = devices[i].address;
    o["name"] = devices[i].name;
    o["rssi"] = devices[i].rssi;
    o["family"] = devices[i].family;
    o["services"] = devices[i].services;
    o["manufacturerData"] = devices[i].manufacturer;
  }
  sendJson(200, doc);
}

static void handleDeviceGet() {
  JsonDocument doc;
  if (demoMode) {
    doc["address"] = "DE:MO:00:00:00:01";  // invented
    doc["name"] = "DEMO Pool Controller (simulated)";
    doc["family"] = "demo";
    doc["connected"] = true;
    doc["demo"] = true;
  } else {
    doc["address"] = selAddress;
    doc["name"] = selName;
    doc["connected"] = false;  // connection to the pool device is not implemented yet
    doc["demo"] = false;
  }
  sendJson(200, doc);
}

// ----- demo mode (simulated device) -----
static void demoTick() {  // expire the simulated boost
  if (demo.boost && (int32_t)(millis() - demo.boostEndMs) >= 0) demo.boost = false;
}

static void handleDemoGet() {
  JsonDocument doc;
  doc["enabled"] = demoMode;
  sendJson(200, doc);
}

static void handleDemoPut() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain")) || !in["enabled"].is<bool>()) {
    server.send(400, "application/json", "{\"error\":\"body must be {\\\"enabled\\\":true|false}\"}");
    return;
  }
  demoMode = in["enabled"].as<bool>();
  prefs.putBool("demo", demoMode);
  server.send(204);
}

static void noDevice() {
  server.send(503, "application/json", "{\"error\":\"no device connected (enable demo mode to preview)\"}");
}

static void handleStatus() {
  if (!demoMode) return noDevice();
  demoTick();
  float t = millis() / 1000.0f;
  JsonDocument doc;
  doc["demo"] = true;
  doc["uptimeSeconds"] = (uint32_t)t;
  doc["ph"] = serialized(String(demo.phSetpoint + 0.06f * sinf(t / 37.0f), 2));
  doc["orp"] = (int)(demo.orpSetpoint + 18.0f * sinf(t / 53.0f));
  doc["saltGramsPerLiter"] = serialized(String(4.7f + 0.04f * sinf(t / 211.0f), 2));
  doc["waterTemperatureC"] = serialized(String(27.0f + 0.5f * sinf(t / 389.0f), 1));
  doc["mode"] = demo.mode;
  doc["productionPercent"] = demo.mode == "off" ? 0 : (demo.mode == "on" || demo.boost ? 100 : demo.productionPercent);
  doc["boostRemainingMinutes"] = demo.boost ? (int)((demo.boostEndMs - millis()) / 60000UL) + 1 : 0;
  doc["winterMode"] = demo.winter;
  doc["alarms"].to<JsonArray>();  // none in the demo
  sendJson(200, doc);
}

static void handleConfigGet() {
  if (!demoMode) return noDevice();
  demoTick();
  JsonDocument doc;
  doc["demo"] = true;
  doc["phSetpoint"] = serialized(String(demo.phSetpoint, 1));
  doc["orpSetpoint"] = demo.orpSetpoint;
  doc["productionPercent"] = demo.productionPercent;
  doc["mode"] = demo.mode;
  doc["boost"] = demo.boost;
  doc["winterMode"] = demo.winter;
  sendJson(200, doc);
}

static void handleConfigPut() {
  if (!demoMode) return noDevice();
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  // Validate everything first, apply only if all fields are inside the allowed ranges.
  float ph = in["phSetpoint"] | demo.phSetpoint;
  int orp = in["orpSetpoint"] | demo.orpSetpoint;
  int pct = in["productionPercent"] | demo.productionPercent;
  String mode = in["mode"] | demo.mode.c_str();
  if (ph < 6.8f || ph > 7.8f || orp < 400 || orp > 900 || pct < 0 || pct > 100 ||
      !(mode == "off" || mode == "auto" || mode == "on")) {
    server.send(422, "application/json", "{\"error\":\"value outside allowed range\"}");
    return;
  }
  demo.phSetpoint = ph;
  demo.orpSetpoint = orp;
  demo.productionPercent = pct;
  demo.mode = mode;
  if (in["winterMode"].is<bool>()) demo.winter = in["winterMode"].as<bool>();
  if (in["boost"].is<bool>()) {
    bool b = in["boost"].as<bool>();
    if (b && !demo.boost) demo.boostEndMs = millis() + DEMO_BOOST_MINUTES * 60000UL;
    demo.boost = b;
  }
  server.send(204);
}

static void handleDevicePut() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain")) || !in["address"].is<const char *>()) {
    server.send(400, "application/json", "{\"error\":\"body must be {\\\"address\\\":\\\"..\\\"}\"}");
    return;
  }
  selAddress = in["address"].as<String>();
  selName = in["name"] | "";
  prefs.putString("addr", selAddress);
  prefs.putString("name", selName);
  server.send(204);
}

// ----- Wi-Fi -----
// ---------- product catalog (families offered by the vendor app, MyIndygo build) ----------
struct CatalogItem {
  const char *id, *name, *group, *description, *demoPanel;  // demoPanel: "" or pool|lighting|heatpump|settings
};
static const CatalogItem CATALOG[] = {
    {"lrpc", "BPC", "pool", "Connected pool controller installed in the pool's electrical box: filtration, lighting outputs, programs, manual commands.", "pool"},
    {"lrpb", "Pool Box / Pool Box LT / Pool Box VS", "pool", "Pool electrical box with connected controller (VS = variable-speed pump).", "pool"},
    {"lrpg", "Pool Guard", "pool", "Module for pool probes and sensors.", ""},
    {"lrmas", "AC1", "pool", "Connected / floating analyzer (probes, batteries, wintering).", ""},
    {"lrpr", "LR-PR", "pool", "Listed in the app; purpose not identified from its strings yet.", ""},
    {"ipx_treatment", "IPX treatment device", "pool", "Water treatment units (PoolTech, Sterilor EVO): pH, ORP, salt, setpoints, boost, winter mode.", "settings"},
    {"polytropic", "Polytropic heat pump", "pool", "Not supported yet (set aside for now; the app controls it through the vendor cloud).", ""},
    {"sweeler", "Sweeler robot", "pool", "Robotic pool cleaner (IPX device).", ""},
    {"lrniv", "Water level regulator (LR-NIV)", "pool", "Water level regulator; water consumption data.", ""},
    {"waterlevel", "Water level sensor", "pool", "Available when the app's lrNivReno feature is on.", ""},
    {"emec", "Emec Board", "pool", "Listed in the app.", ""},
    {"lrcan", "Can sensor module", "pool", "Module reading can sensors (pH/ORP etc.).", ""},
    {"poolpower", "Pool Power", "pool", "Listed in the app (power related).", ""},
    {"distboard", "Distribution board", "pool", "Listed in the app.", ""},
    {"poolextender", "Pool Extender", "pool", "Range extender for the pool system.", ""},
    {"flood", "Flood sensor", "pool", "Water leak / flood sensor.", ""},
    {"lrmb10", "LR-MB-10", "relay", "Relay / valve controller family (name from the app).", ""},
    {"lrmb30", "LR-MB-30", "relay", "Relay / valve controller family (name from the app).", ""},
    {"bst", "LR-BST (compact, autonomous, 4G, React)", "relay", "Relay controller family with several variants (name from the app).", ""},
};

static void handleCatalog() {
  JsonDocument doc;
  doc["source"] = "Families offered by the vendor app (MyIndygo build). Names/groups come from the app; descriptions are paraphrased from its strings. Only entries with a demoPanel can be previewed.";
  JsonArray arr = doc["products"].to<JsonArray>();
  for (const CatalogItem &c : CATALOG) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = c.id;
    o["name"] = c.name;
    o["group"] = c.group;
    o["description"] = c.description;
    o["demoPanel"] = c.demoPanel;
  }
  sendJson(200, doc);
}

// ---------- simulated pool equipment (demo mode only) ----------
// Command names, action semantics and colour values come from the vendor app (see docs/app-commands.md).
// The mapping index 0 = filtration pump, index 1 = lighting, durations in seconds, and the limits below are
// ASSUMPTIONS for the demo, not verified.
struct CceiColor { const char *name; int value; const char *kind; };
static const CceiColor CCEI_COLORS[] = {
    {"Hot white", 227, "white"}, {"White", 195, "white"},
    {"Blue", 35, "color"}, {"Lagoon", 243, "color"}, {"Cyan", 115, "color"}, {"Purple", 51, "color"},
    {"Magenta", 251, "color"}, {"Pink", 123, "color"}, {"Red", 59, "color"}, {"Orange", 225, "color"},
    {"Green", 97, "color"}, {"Favorite", 33, "color"},
    {"Light 25%", 241, "level"}, {"Light 50%", 113, "level"}, {"Light 75%", 49, "level"},
    {"Gradient", 121, "program"}, {"Rainbow", 57, "program"}, {"Parade", 224, "program"}, {"Techno", 96, "program"},
    {"Horizon", 32, "program"}, {"Random", 240, "program"}, {"Magic", 112, "program"},
    {"Speed 50", 120, "speed"}, {"Speed 100", 248, "speed"}, {"Speed 200", 48, "speed"},
    {"Save color", 56, "save"},
};
static const int CCEI_COUNT = sizeof(CCEI_COLORS) / sizeof(CCEI_COLORS[0]);

static struct {
  int rule = 0;  // 0 time, 1 thermo-adaptive, 2 variable-speed pump
  String pump = "auto";  // auto | manualOn | boost | paused | backwash
  uint32_t pumpEnd = 0;
  int pumpSpeed = 2;
  bool lightOn = false;
  uint32_t lightEnd = 0;
  String spotlight = "CCEI";  // CCEI | Seamaid
  String lightName = "Blue";
  int lightValue = 35;
  int cycle = 2;
} sim;

static const char *RULE_NAMES[] = {"time", "thermo-adaptive", "variable speed pump"};

static int remainingS(uint32_t endMs) {
  int32_t d = (int32_t)(endMs - millis());
  return d > 0 ? d / 1000 : 0;
}

static void simTick() {
  if (sim.pump != "auto" && (int32_t)(millis() - sim.pumpEnd) >= 0) sim.pump = "auto";
  if (sim.lightOn && sim.lightEnd && (int32_t)(millis() - sim.lightEnd) >= 0) sim.lightOn = false;
}

static void handleSim() {
  if (!demoMode) return noDevice();
  simTick();
  JsonDocument doc;
  doc["demo"] = true;
  JsonObject pool = doc["pool"].to<JsonObject>();
  pool["rule"] = sim.rule;
  pool["ruleName"] = RULE_NAMES[sim.rule];
  pool["state"] = sim.pump;
  pool["remainingSeconds"] = sim.pump == "auto" ? 0 : remainingS(sim.pumpEnd);
  pool["speed"] = sim.pumpSpeed;
  JsonObject l = doc["lighting"].to<JsonObject>();
  l["on"] = sim.lightOn;
  l["spotlight"] = sim.spotlight;
  l["color"] = sim.lightName;
  l["colorValue"] = sim.lightValue;
  l["remainingSeconds"] = sim.lightOn ? remainingS(sim.lightEnd) : 0;
  JsonArray pal = l["palette"].to<JsonArray>();
  for (int i = 0; i < CCEI_COUNT; i++) {
    JsonObject p = pal.add<JsonObject>();
    p["name"] = CCEI_COLORS[i].name;
    p["value"] = CCEI_COLORS[i].value;
    p["kind"] = CCEI_COLORS[i].kind;
  }
  sendJson(200, doc);
}

// POST /api/v1/commands: same JSON shape the vendor app passes to its sendManualCommand().
static void handleCommand() {
  if (!demoMode) return noDevice();
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain")) || !in["command"].is<const char *>()) {
    server.send(400, "application/json", "{\"error\":\"body must contain a \\\"command\\\"\"}");
    return;
  }
  simTick();
  String cmd = in["command"].as<String>();
  int index = in["index"] | 0;
  bool isPump = (index == 0);
  uint32_t now = millis();
  auto reject = []() { server.send(422, "application/json", "{\"error\":\"unknown command or value out of range\"}"); };

  if (cmd == "manualOn") {
    int dur = in["duration"] | 3600;
    int speed = in["speed"] | 2;
    if (dur < 1 || dur > 86400 || speed < 1 || speed > 3) return reject();
    if (isPump) { sim.pump = "manualOn"; sim.pumpEnd = now + dur * 1000UL; sim.pumpSpeed = speed; }
    else { sim.lightOn = true; sim.lightEnd = now + dur * 1000UL; }
  } else if (cmd == "stopManualCommand") {
    if (isPump) sim.pump = "auto"; else sim.lightOn = false;
  } else if (cmd == "boost") {
    int dur = in["boostDuration"] | 86400;
    if (dur < 60 || dur > 604800) return reject();
    sim.pump = "boost"; sim.pumpEnd = now + dur * 1000UL;
  } else if (cmd == "pause") {
    int days = in["pauseDurationInDays"] | 1;
    if (days < 1 || days > 30) return reject();
    sim.pump = "paused"; sim.pumpEnd = now + days * 86400000UL;
  } else if (cmd == "backwash") {
    int dur = in["duration"] | 120;
    if (dur < 10 || dur > 1800) return reject();
    sim.pump = "backwash"; sim.pumpEnd = now + dur * 1000UL;
  } else if (cmd == "colorChange") {  // pulse the light supply: the lamp steps to its next colour
    do { sim.cycle = (sim.cycle + 1) % CCEI_COUNT; } while (String(CCEI_COLORS[sim.cycle].kind) != "color");
    sim.lightOn = true; sim.lightEnd = 0; sim.lightName = CCEI_COLORS[sim.cycle].name; sim.lightValue = CCEI_COLORS[sim.cycle].value;
  } else if (cmd == "colorChangeCCEI") {
    int mode = in["mode"] | -1;
    int found = -1;
    for (int i = 0; i < CCEI_COUNT; i++) if (CCEI_COLORS[i].value == mode) found = i;
    if (found < 0) return reject();
    sim.spotlight = "CCEI"; sim.lightOn = true; sim.lightEnd = 0;
    sim.lightName = CCEI_COLORS[found].name; sim.lightValue = mode;
  } else if (cmd == "colorChangeSeamaid") {
    int mode = in["mode"] | -1;  // 16 change colour, 48 save colour, 32 reset
    if (mode != 16 && mode != 48 && mode != 32) return reject();
    sim.spotlight = "Seamaid"; sim.lightOn = true; sim.lightEnd = 0; sim.lightValue = mode;
    sim.lightName = mode == 16 ? "Change colour" : (mode == 48 ? "Save colour" : "Reset");
  } else {
    return reject();
  }
  server.send(204);
}

static void handlePoolPut() {
  if (!demoMode) return noDevice();
  JsonDocument in;
  int rule = -1;
  if (!deserializeJson(in, server.arg("plain"))) rule = in["rule"] | -1;
  if (rule < 0 || rule > 2) {
    server.send(422, "application/json", "{\"error\":\"rule must be 0 (time), 1 (thermo-adaptive) or 2 (variable speed pump)\"}");
    return;
  }
  sim.rule = rule;
  server.send(204);
}

static void handleWifiGet() {
  JsonDocument doc;
  doc["mode"] = apMode ? "ap" : "sta";
  doc["connected"] = !apMode && WiFi.status() == WL_CONNECTED;
  doc["ssid"] = apMode ? "" : WiFi.SSID();
  doc["savedSsid"] = savedSsid;  // the password is never returned
  doc["ip"] = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  doc["apSsid"] = apMode ? apSsid : "";
  doc["hostname"] = String(HOSTNAME) + ".local";
  sendJson(200, doc);
}

static void handleWifiScanStart() {
  WiFi.scanDelete();
  WiFi.scanNetworks(true);  // asynchronous
  server.send(202, "application/json", "{\"scanning\":true}");
}

static void handleWifiScanResults() {
  int n = WiFi.scanComplete();
  JsonDocument doc;
  doc["scanning"] = (n == WIFI_SCAN_RUNNING);
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  sendJson(200, doc);
}

static void handleWifiPut() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain")) || !in["ssid"].is<const char *>()) {
    server.send(400, "application/json", "{\"error\":\"body must be {\\\"ssid\\\":\\\"..\\\",\\\"password\\\":\\\"..\\\"}\"}");
    return;
  }
  String ssid = in["ssid"].as<String>();
  String pass = in["password"] | "";
  if (ssid.length() < 1 || ssid.length() > 32 || (pass.length() != 0 && (pass.length() < 8 || pass.length() > 63))) {
    server.send(422, "application/json", "{\"error\":\"ssid 1-32 chars; password empty or 8-63 chars\"}");
    return;
  }
  prefs.putString("wssid", ssid);
  prefs.putString("wpass", pass);
  server.send(202, "application/json", "{\"restarting\":true}");
  restartAtMs = millis() + 1500;
}

static void handleWifiDelete() {
  prefs.remove("wssid");
  prefs.remove("wpass");
  server.send(202, "application/json", "{\"restarting\":true}");
  restartAtMs = millis() + 1500;
}

static void handleNotFound() {
  if (apMode) {  // captive portal: send any unknown URL (OS connectivity checks) to the UI
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "application/json", "{\"error\":\"not found\"}");
  }
}

// ---------- Wi-Fi bring-up ----------
static bool connectSta(const String &ssid, const String &pass) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < STA_CONNECT_TIMEOUT_MS) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

static void startAp() {
  // Use the chip's eFuse MAC: WiFi.macAddress() can still read zeros here on boards whose radio
  // is a separate co-processor (e.g. ESP32-P4 + ESP32-C6), which made every name end in 0000.
  uint64_t efuse = ESP.getEfuseMac();
  char name[32];
  snprintf(name, sizeof(name), "Indygo-Gateway-%02X%02X", (uint8_t)(efuse >> 32), (uint8_t)(efuse >> 40));
  apSsid = name;
  WiFi.mode(WIFI_AP_STA);  // AP for the first connection + STA radio so Wi-Fi scans work
  WiFi.softAP(apSsid.c_str(), AP_PASSWORD);
  dnsServer.start(53, "*", WiFi.softAPIP());  // captive portal
  apMode = true;
  Serial.printf("Access point '%s' (password '%s') -> http://%s\n", apSsid.c_str(), AP_PASSWORD,
                WiFi.softAPIP().toString().c_str());
}

static void setupWifi() {
  savedSsid = prefs.getString("wssid", "");
  String pass = prefs.getString("wpass", "");
  if (savedSsid.length() && connectSta(savedSsid, pass)) {
    apMode = false;
    Serial.printf("Connected to '%s' -> http://%s\n", savedSsid.c_str(), WiFi.localIP().toString().c_str());
  } else {
    startAp();
  }
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  lastStaRetryMs = millis();
}

void setup() {
  Serial.begin(115200);
  prefs.begin("indygo", false);
  selAddress = prefs.getString("addr", "");
  selName = prefs.getString("name", "");
  demoMode = prefs.getBool("demo", false);

  BLEDevice::init("indygo-gateway");
  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(&scanCallbacks, false);
  bleScan->setActiveScan(true);  // active scan to also receive names / scan responses
  bleScan->setInterval(100);
  bleScan->setWindow(99);

  setupWifi();

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/v1/gateway", HTTP_GET, handleGateway);
  server.on("/api/v1/ble/scan", HTTP_POST, handleScanStart);
  server.on("/api/v1/ble/scan", HTTP_GET, handleScanResults);
  server.on("/api/v1/device", HTTP_GET, handleDeviceGet);
  server.on("/api/v1/device", HTTP_PUT, handleDevicePut);
  server.on("/api/v1/demo", HTTP_GET, handleDemoGet);
  server.on("/api/v1/demo", HTTP_PUT, handleDemoPut);
  server.on("/api/v1/status", HTTP_GET, handleStatus);
  server.on("/api/v1/config", HTTP_GET, handleConfigGet);
  server.on("/api/v1/config", HTTP_PUT, handleConfigPut);
  server.on("/api/v1/catalog", HTTP_GET, handleCatalog);
  server.on("/api/v1/sim", HTTP_GET, handleSim);
  server.on("/api/v1/commands", HTTP_POST, handleCommand);
  server.on("/api/v1/pool", HTTP_PUT, handlePoolPut);
  server.on("/api/v1/wifi", HTTP_GET, handleWifiGet);
  server.on("/api/v1/wifi", HTTP_PUT, handleWifiPut);
  server.on("/api/v1/wifi", HTTP_DELETE, handleWifiDelete);
  server.on("/api/v1/wifi/scan", HTTP_POST, handleWifiScanStart);
  server.on("/api/v1/wifi/scan", HTTP_GET, handleWifiScanResults);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  if (apMode) dnsServer.processNextRequest();
  server.handleClient();

  if (restartAtMs && (int32_t)(millis() - restartAtMs) >= 0) ESP.restart();

  // Home network saved but unreachable at boot (router still starting, power cut...): while nobody
  // is using the access point, retry the home network periodically.
  if (apMode && savedSsid.length() && WiFi.softAPgetStationNum() == 0 &&
      millis() - lastStaRetryMs > AP_RETRY_STA_MS) {
    ESP.restart();
  }
}
