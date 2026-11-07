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
#include <Update.h>
#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>

#if __has_include("ble_secrets.h")
#include "ble_secrets.h"
#endif
#ifndef BLE_AUTH_KEY_HEX
#define BLE_AUTH_KEY_HEX ""
#endif

#define FIRMWARE_VERSION "0.7.0-m1a"
#define HOSTNAME "indygo-gateway"
#define AP_PASSWORD "indygo-setup"  // default password of the first-connection access point
#define STA_CONNECT_TIMEOUT_MS 15000
#define AP_RETRY_STA_MS 60000
#define MAX_DEVICES 40
#define DEFAULT_SCAN_SECONDS 8
#define MAX_SCAN_SECONDS 30

#define MAX_RANGES 8
struct Range {
  bool used = false;
  uint8_t days = 127;  // bit 0 = Monday ... bit 6 = Sunday
  uint16_t startMin = 480, endMin = 1020;
  uint8_t speed = 2;
};

struct BleEntry {
  uint8_t addrType = 0;  // BLE address type (0 public, 1 random)
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
    e.addrType = (uint8_t)dev.getAddressType();
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
<button id="t-pool" onclick="tab('pool')">Filtration</button>
<button id="t-aux" onclick="tab('aux')">Auxiliaries</button>
<button id="t-lighting" onclick="tab('lighting')">Lighting</button>
<button id="t-settings" onclick="tab('settings')">Settings</button>
<button id="t-wifi" onclick="tab('wifi')">Wi-Fi</button>
<button id="t-dev" onclick="tab('dev')">Developer</button></nav>
<main>
<section id="status" class="card"><h3>Selected pool device</h3><div id="sel">Loading...</div>
<p class="muted">Open the <b>Bluetooth devices</b> menu to find and select your device.</p>
<label><input type="checkbox" id="demoSw" onchange="setDemo(this.checked)" style="width:auto"> Demo mode (simulated device, to preview the interface)</label>
<h3>Live measurements</h3><table><tbody id="live"><tr><td class="muted">No device connected.</td></tr></tbody></table>
<h3>Network</h3><div id="net">Loading...</div></section>

<section id="products" class="hide"><div class="card"><h3>Products</h3>
 <p class="muted">The product families offered by the vendor app. Entries marked <b>Preview</b> have a demo panel (turn on Demo mode on the Status page); the others are listed but not supported yet.</p>
 <div id="cat" class="muted">Loading...</div></div></section>

<section id="pool" class="hide"><div class="card"><h3>Filtration</h3>
 <div id="nodemo-pool" class="muted hide">Demo mode only for now: turn it on from the Status page.</div>
 <div id="pool-body">
 <div id="pl-state" class="muted">Loading...</div>
 <p><button class="btn alt" onclick="filt({mode:'off'})">OFF</button>
 <button class="btn" onclick="filt({mode:'on'})">ON (forced)</button>
 <button class="btn" onclick="filt({mode:'auto'})">AUTO</button></p>
 <label>Speed (3-speed pump contacts)</label><select id="pl-speed" onchange="filt({speed:+this.value})"><option>1</option><option>2</option><option>3</option></select>
 <label>AUTO type</label><select id="pl-kind" onchange="filt({autoKind:this.value})">
  <option value="variable">Variable speed - up to 8 daily ranges, each with its speed</option>
  <option value="thermo">Thermo-adaptive - duration follows the water temperature</option>
  <option value="ranges">Time ranges - single-speed pumps</option></select>
 <div id="pl-adj-wrap"><label>Daily adjustment, hours (-4 to +4)</label><input id="pl-adj" type="number" min="-4" max="4" value="0" onchange="filt({dailyAdjustHours:+this.value})"></div>
 <p>Boost (AUTO only): <button class="btn" onclick="filt({boost:24})">24 h</button>
 <button class="btn" onclick="filt({boost:48})">48 h</button>
 <button class="btn alt" onclick="filt({boost:0})">Stop boost</button></p>
 <div id="pl-ranges-wrap"><h4>Time ranges (up to 8)</h4><div id="pl-ranges"></div></div>
 <p class="muted">Frost protection is automatic in the equipment: water below 4 C runs 15 min every 2 h, below 1 C runs continuously.
 Modes follow the manufacturer's manual. Schedules are stored but not evaluated in this preview.</p></div></div></section>

<section id="aux" class="hide"><div class="card"><h3>Auxiliaries (AUX1 / AUX2)</h3>
 <div id="nodemo-aux" class="muted hide">Demo mode only for now: turn it on from the Status page.</div>
 <div id="aux-body"></div>
 <p class="muted">Each auxiliary has its own modes by equipment type. Starting a robot forces filtration; treatment equipment only runs while filtration runs. The heat pump is not supported yet.</p></div></section>

<section id="lighting" class="hide"><div class="card"><h3>Lighting</h3>
 <div id="nodemo-lighting" class="muted hide">Demo mode only for now: turn it on from the Status page.</div>
 <div id="light-body">
 <div id="lt-state" class="muted">Loading...</div>
 <p><button class="btn" onclick="lightCmd('manualOn',{duration:3600})">Light ON (1 h)</button>
 <button class="btn alt" onclick="lightCmd('stopManualCommand',{})">Light OFF</button>
 <button class="btn" onclick="lightCmd('colorChange',{})">Next colour (pulse)</button></p>
 <label>Spotlight type</label><select id="lt-type" onchange="drawPalette()"><option>CCEI</option><option>Seamaid</option></select>
 <div id="lt-pal"></div>
 <p class="muted">CCEI values and Seamaid modes are those used by the vendor app. Grouping of the CCEI entries is my reading of the app and may change.</p></div></div></section>

<section id="dev" class="hide">
 <div class="card"><h3>Developer tools</h3>
  <p class="muted" id="dev-state">Loading...</p>
  <label>Developer password (kept only for this browser tab)</label>
  <input id="dev-pw" type="password" autocomplete="off"><button class="btn" onclick="savePw()">Use this password</button>
  <label>Set or change the password (8-64 characters; the current one is sent as proof)</label>
  <input id="dev-new" type="password" autocomplete="off"><button class="btn" onclick="setPw()">Save new password</button>
  <p class="muted" id="dev-msg"></p>
  <p class="muted">Anyone who knows this password can replace the firmware and talk to your equipment through this device. Use a strong one and do not expose the gateway to the internet.</p></div>
 <div class="card"><h3>Firmware update (over Wi-Fi)</h3>
  <p class="muted">Choose the application image <b>indygo_gateway.ino.bin</b> (not the merged or bootloader files). The gateway restarts when done. If a bad image is installed you will need a USB cable to recover.</p>
  <input id="ota-file" type="file" accept=".bin"><button class="btn" onclick="otaGo()">Upload and install</button></div>
 <div class="card"><h3>Bluetooth explorer</h3>
  <p class="muted">Connect to a device found by the Bluetooth scan (use <b>Explore</b> there). Demo mode must be off. Reading and listening are always possible; <b>writing is locked</b> until you enable it for a few minutes, and every write is logged.</p>
  <label>Address</label><input id="ble-addr" placeholder="aa:bb:cc:dd:ee:ff">
  <label>Address type</label><select id="ble-type"><option value="0">0 - public</option><option value="1">1 - random</option></select>
  <p><button class="btn" onclick="bleConn()">Connect</button> <button class="btn alt" onclick="bleDisc()">Disconnect</button>
  <button class="btn" onclick="bleSvc()">Load services</button></p>
  <div id="ble-svc" class="muted"></div>
  <label>Hex bytes to write</label><input id="ble-hex" placeholder="e.g. 0204 00">
  <p><button class="btn" onclick="bleWE(true)">Enable writes (10 min)</button> <button class="btn alt" onclick="bleWE(false)">Lock writes</button></p>
  <label>Log (reads, notifications, writes)</label>
  <textarea id="ble-log" rows="10" style="width:100%;box-sizing:border-box;font-family:monospace;font-size:12px" readonly></textarea></div>
</section>

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
const $=id=>document.getElementById(id);let poll,wpoll;const T=['status','products','pool','aux','lighting','ble','settings','wifi','dev'];
function tab(t){for(const s of T){$(s).classList.toggle('hide',s!==t);$('t-'+s).classList.toggle('on',s===t)}if(t==='status'){loadSel();loadNet();live()}if(t==='wifi')loadNet();if(t==='settings')loadCfg();if(t==='products')loadCat();if(t==='pool'||t==='aux'||t==='lighting')loadSim();if(t==='dev')loadDev()}
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
  tr.innerHTML='<td></td><td></td><td></td><td><span class="tag"></span></td><td><button class="btn">Select</button> <button class="btn alt">Explore</button></td>';
  const c=tr.children;c[0].textContent=x.name||'(no name)';c[1].textContent=x.address;c[2].textContent=x.rssi;
  c[3].firstChild.textContent=x.family;c[3].firstChild.classList.add(x.family);
  c[4].firstChild.onclick=async()=>{await j('/api/v1/device',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({address:x.address,name:x.name})});tab('status')};
  c[4].children[1].onclick=()=>{$('ble-addr').value=x.address;$('ble-type').value=x.addrType||0;tab('dev')};$('rows').appendChild(tr)}
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
const DAYS=['M','T','W','T','F','S','S'];
function fmt(s){if(!s)return'';const m=Math.floor(s/60);return m>=60?Math.floor(m/60)+' h '+(m%60)+' min':m+' min '+(s%60)+' s'}
function hm(m){return String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0')}
function toMin(v){const p=v.split(':');return (+p[0])*60+(+p[1])}
async function loadCat(){const d=await j('/api/v1/catalog');const box=$('cat');box.innerHTML='';
 for(const g of['pool','relay']){const h=document.createElement('h4');h.textContent=g==='pool'?'Pool':'Relay / valve controllers';box.appendChild(h);
  for(const p of d.products.filter(x=>x.group===g)){const r=document.createElement('div');r.style.cssText='padding:8px 0;border-bottom:1px solid #e3e9ec';
   const b=document.createElement('b');b.textContent=p.name+' ';r.appendChild(b);
   if(p.demoPanel){const t=document.createElement('span');t.className='tag solem_ipx';t.textContent='Preview';r.appendChild(t);
    const bt=document.createElement('button');bt.className='btn';bt.style.marginLeft='8px';bt.textContent='Open';bt.onclick=()=>tab(p.demoPanel);r.appendChild(bt)}
   const ds=document.createElement('div');ds.className='muted';ds.textContent=p.description;r.appendChild(ds);box.appendChild(r)}}}
async function put(u,o){try{await j(u,{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)})}catch(e){alert('Not allowed in the current state (check mode and values).')}loadSim()}
function filt(o){put('/api/v1/filtration',o)}
function auxPut(n,o){put('/api/v1/aux'+n,o)}
async function sendCmd(o){try{await j('/api/v1/commands',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)})}catch(e){alert('Command rejected')}loadSim()}
function lightCmd(c,extra){const i=SIM&&SIM.lighting.auxIndex;if(!i){alert('Set AUX1 or AUX2 to "lighting" first.');return}sendCmd(Object.assign({command:c,index:i},extra))}
function editRanges(box,ranges,withSpeed,save){const rows=ranges.map(r=>Object.assign({},r));
 function draw(){box.innerHTML='';
  rows.forEach((r,i)=>{const d=document.createElement('div');d.style.cssText='display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin:6px 0';
   for(let k=0;k<7;k++){const l=document.createElement('label');l.style.cssText='display:flex;align-items:center;gap:2px';const c=document.createElement('input');c.type='checkbox';c.style.cssText='width:auto;margin:0';c.checked=!!(r.days&(1<<k));c.onchange=()=>{r.days^=(1<<k)};l.appendChild(c);l.appendChild(document.createTextNode(DAYS[k]));d.appendChild(l)}
   const s=document.createElement('input');s.type='time';s.value=hm(r.start);s.style.cssText='width:auto;margin:0';s.onchange=()=>{r.start=toMin(s.value)};d.appendChild(s);
   const e=document.createElement('input');e.type='time';e.value=hm(r.end);e.style.cssText='width:auto;margin:0';e.onchange=()=>{r.end=toMin(e.value)};d.appendChild(e);
   if(withSpeed){const sp=document.createElement('select');sp.style.cssText='width:auto;margin:0';for(const v of[1,2,3]){const o=document.createElement('option');o.textContent='speed '+v;o.value=v;sp.appendChild(o)}sp.value=r.speed||2;sp.onchange=()=>{r.speed=+sp.value};d.appendChild(sp)}
   const x=document.createElement('button');x.className='btn alt';x.textContent='Remove';x.onclick=()=>{rows.splice(i,1);draw()};d.appendChild(x);box.appendChild(d)});
  const a=document.createElement('button');a.className='btn';a.textContent='Add range';a.disabled=rows.length>=8;a.onclick=()=>{rows.push({days:127,start:480,end:1020,speed:2});draw()};box.appendChild(a);
  const sv=document.createElement('button');sv.className='btn';sv.style.marginLeft='8px';sv.textContent='Save ranges';sv.onclick=()=>save(rows);box.appendChild(sv)}
 draw()}
function drawAux(d){const box=$('aux-body');box.innerHTML='';
 for(const a of d.aux){const n=a.index;const c=document.createElement('div');c.style.cssText='border-top:1px solid #e3e9ec;padding:10px 0';
  const h=document.createElement('h4');h.textContent='AUX'+n;c.appendChild(h);
  const st=document.createElement('div');st.className='muted';st.textContent='Mode: '+a.mode.toUpperCase()+(a.remainingSeconds?' ('+fmt(a.remainingSeconds)+' left)':'')+(a.note?' - '+a.note:'');c.appendChild(st);
  const lab=document.createElement('label');lab.textContent='Equipment type';c.appendChild(lab);
  const ty=document.createElement('select');for(const t of['lighting','treatment','robot','other']){const o=document.createElement('option');o.textContent=t;ty.appendChild(o)}
  const hp=document.createElement('option');hp.textContent='heat pump (not supported yet)';hp.disabled=true;ty.appendChild(hp);ty.value=a.type;ty.onchange=()=>auxPut(n,{type:ty.value});c.appendChild(ty);
  const p=document.createElement('p');for(const m of(a.type==='treatment'?['off','auto']:['off','on','auto'])){const b=document.createElement('button');b.className='btn'+(m==='off'?' alt':'');b.style.marginRight='6px';b.textContent=m.toUpperCase();b.onclick=()=>auxPut(n,{mode:m});p.appendChild(b)}c.appendChild(p);
  if(a.type==='treatment'){const w=document.createElement('div');const cb=document.createElement('input');cb.type='checkbox';cb.style.cssText='width:auto;margin:0 6px 0 0';cb.checked=a.shutterMode;cb.onchange=()=>auxPut(n,{shutterMode:cb.checked});
   w.appendChild(cb);w.appendChild(document.createTextNode('Cover mode: while the cover is closed, run at '));const r=document.createElement('input');r.type='number';r.min=0;r.max=60;r.value=a.shutterRate;r.style.cssText='width:70px;margin:0 6px';r.onchange=()=>auxPut(n,{shutterRate:+r.value});w.appendChild(r);w.appendChild(document.createTextNode('min per hour'));c.appendChild(w)}
  else if(a.mode==='auto'){const hh=document.createElement('div');hh.className='muted';hh.textContent='Time ranges (up to 8)';c.appendChild(hh);const rb=document.createElement('div');c.appendChild(rb);editRanges(rb,a.ranges,false,rows=>auxPut(n,{ranges:rows}))}
  box.appendChild(c)}}
async function loadSim(){let d=null;try{d=await j('/api/v1/sim')}catch(e){}SIM=d;
 for(const k of['pool','aux','lighting']){$('nodemo-'+k).classList.toggle('hide',!!d);if($(k+'-body'))$(k+'-body').classList.toggle('hide',!d)}
 if(!d)return;const p=d.pool;
 let st='Mode: '+p.mode.toUpperCase();if(p.mode==='on')st+=' at speed '+p.speed+(p.remainingSeconds?' ('+fmt(p.remainingSeconds)+' left)':'');
 if(p.mode==='auto')st+=' ('+p.autoKind+')'+(p.boost.active?' - BOOST, '+fmt(p.boost.remainingSeconds)+' left':'');
 st+=' - pump: '+(p.running===null?'by schedule (not simulated)':(p.running?'running':'stopped'))+(p.forcedBy?' (forced by '+p.forcedBy+')':'')+' - water '+d.water.temperatureC+' C';
 $('pl-state').textContent=st;$('pl-speed').value=p.speed;$('pl-kind').value=p.autoKind;$('pl-adj').value=p.dailyAdjustHours;
 $('pl-adj-wrap').classList.toggle('hide',p.autoKind!=='thermo');$('pl-ranges-wrap').classList.toggle('hide',p.autoKind==='thermo');
 editRanges($('pl-ranges'),p.ranges,p.autoKind==='variable',rows=>filt({ranges:rows}));
 drawAux(d);
 const l=d.lighting;$('lt-state').textContent=l.auxIndex?('Light on AUX'+l.auxIndex+': '+(l.on?'on':'off')+' - '+l.spotlight+' - '+l.color+(l.on&&l.remainingSeconds?' ('+fmt(l.remainingSeconds)+' left)':'')):'No auxiliary is set to "lighting". Choose it in the Auxiliaries menu.';
 $('lt-type').value=l.spotlight;drawPalette()}
function drawPalette(){const box=$('lt-pal');box.innerHTML='';if(!SIM||!SIM.lighting.auxIndex)return;
 if($('lt-type').value==='Seamaid'){for(const pair of[['Change colour',16],['Save colour',48],['Reset',32]]){const b=document.createElement('button');b.className='btn';b.style.margin='3px';b.textContent=pair[0];b.onclick=()=>lightCmd('colorChangeSeamaid',{mode:pair[1]});box.appendChild(b)}return}
 let last='';for(const c of SIM.lighting.palette){if(c.kind!==last){last=c.kind;const h=document.createElement('div');h.className='muted';h.style.marginTop='8px';h.textContent=c.kind;box.appendChild(h)}
  const b=document.createElement('button');b.className='btn';b.style.cssText='margin:3px;'+(CSS[c.name]?'background:'+CSS[c.name]+';color:'+(['White','Hot white','Cyan','Pink','Favorite','Lagoon'].includes(c.name)?'#14232b':'#fff')+';border:1px solid #0003':'');
  b.textContent=c.name;b.onclick=()=>lightCmd('colorChangeCCEI',{mode:c.value});box.appendChild(b)}}
let LOGSEQ=0,LOGTMR=null;
function PW(){try{return sessionStorage.getItem('devpw')||''}catch(e){return ''}}
async function dj(u,o){o=o||{};o.headers=Object.assign({'X-Dev-Password':PW()},o.headers||{});const r=await fetch(u,o);const t=await r.text();let b=null;try{b=JSON.parse(t)}catch(e){}if(!r.ok)throw new Error((b&&b.error)||r.status);return b}
async function loadDev(){const d=await j('/api/v1/dev');$('dev-state').textContent=(d.passwordSet?'Developer password is set.':'No developer password yet: choose one below to enable the tools.')+' BLE link: '+(d.bleLinked?'connected':'no')+' - writes: '+(d.writeEnabled?'ENABLED ('+d.writeSecondsLeft+' s left)':'locked');$('dev-pw').value=PW();
 if(!LOGTMR)LOGTMR=setInterval(()=>{if(!$('dev').classList.contains('hide'))pollLog()},1500)}
function savePw(){try{sessionStorage.setItem('devpw',$('dev-pw').value)}catch(e){}$('dev-msg').textContent='Password remembered for this tab.';loadDev()}
async function setPw(){const n=$('dev-new').value;try{await dj('/api/v1/dev/password',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:n,current:PW()})});try{sessionStorage.setItem('devpw',n)}catch(e){}$('dev-msg').textContent='Password saved.'}catch(e){$('dev-msg').textContent='Error: '+e.message}loadDev()}
async function otaGo(){const f=$('ota-file').files[0];if(!f){$('dev-msg').textContent='Choose the .bin file first.';return}
 const fd=new FormData();fd.append('file',f);$('dev-msg').textContent='Uploading '+f.size+' bytes...';
 try{await dj('/api/v1/ota',{method:'POST',body:fd});$('dev-msg').textContent='Update installed. The gateway is restarting: reload this page in about 30 seconds.'}catch(e){$('dev-msg').textContent='Update failed: '+e.message}}
function devPost(u,o){return dj(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)})}
async function bleConn(){$('dev-msg').textContent='Connecting...';try{const r=await devPost('/api/v1/dev/ble/connect',{address:$('ble-addr').value.trim(),addrType:+$('ble-type').value});$('dev-msg').textContent='Connected.';bleSvc()}catch(e){$('dev-msg').textContent='Connect failed: '+e.message}loadDev()}
async function bleDisc(){try{await devPost('/api/v1/dev/ble/disconnect',{})}catch(e){}$('ble-svc').textContent='';loadDev()}
async function bleSvc(){const box=$('ble-svc');box.textContent='';let d;try{d=await dj('/api/v1/dev/ble/services')}catch(e){box.textContent='Error: '+e.message;return}
 for(const s of d.services){const h=document.createElement('div');h.style.marginTop='8px';const b=document.createElement('b');b.textContent='Service '+s.uuid;h.appendChild(b);box.appendChild(h);
  for(const c of s.characteristics){const r=document.createElement('div');r.style.cssText='padding:4px 0 4px 12px';
   const p=[c.read&&'read',c.write&&'write',c.writeNoResponse&&'write-no-resp',c.notify&&'notify',c.indicate&&'indicate'].filter(Boolean).join(', ');
   r.appendChild(document.createTextNode(c.uuid+'  ['+p+']  '));
   const mk=(t,f)=>{const bt=document.createElement('button');bt.className='btn';bt.style.cssText='padding:3px 8px;margin-right:4px';bt.textContent=t;bt.onclick=f;r.appendChild(bt)};
   if(c.read)mk('Read',()=>devPost('/api/v1/dev/ble/read',{service:s.uuid,characteristic:c.uuid}).then(pollLog).catch(e=>$('dev-msg').textContent='Read failed: '+e.message));
   if(c.notify||c.indicate)mk('Listen',()=>devPost('/api/v1/dev/ble/subscribe',{service:s.uuid,characteristic:c.uuid}).then(()=>$('dev-msg').textContent='Listening on '+c.uuid).catch(e=>$('dev-msg').textContent='Subscribe failed: '+e.message));
   if(c.write||c.writeNoResponse)mk('Write hex',()=>devPost('/api/v1/dev/ble/write',{service:s.uuid,characteristic:c.uuid,hex:$('ble-hex').value.replace(/\s+/g,''),response:!!c.write}).then(()=>{$('dev-msg').textContent='Written.';pollLog()}).catch(e=>$('dev-msg').textContent='Write failed: '+e.message));
   box.appendChild(r)}}}
async function bleWE(on){try{await dj('/api/v1/dev/ble/write-enable',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled:on,minutes:10})})}catch(e){$('dev-msg').textContent='Error: '+e.message}loadDev()}
async function pollLog(){let d;try{d=await dj('/api/v1/dev/ble/log?since='+LOGSEQ)}catch(e){return}
 if(d.last<LOGSEQ){LOGSEQ=0;$('ble-log').value=''}
 for(const e of d.entries){$('ble-log').value+='['+e.ms+' ms] '+e.dir+' '+e.uuid+' '+e.hex+'\n';LOGSEQ=e.seq}
 $('ble-log').scrollTop=$('ble-log').scrollHeight}
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
    o["addrType"] = devices[i].addrType;
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
    {"lrpc", "POOL COMMAND VS2 (BPC / LRPC)", "pool", "Connected pool clock: filtration with 3-speed pump contacts, AUX1/AUX2 (lighting, treatment, robot, other), temperature probe, cover input.", "pool"},
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

// ---------- simulated POOL COMMAND VS2 (demo mode only) ----------
// Modes follow the manufacturer's manual (docs/pool-command-vs2.md); command names and colour values follow the
// vendor app (docs/app-commands.md). ASSUMPTIONS for the demo: output index 0 = filtration, 1 = AUX1, 2 = AUX2;
// durations in seconds; limits below. Schedules are stored but NOT evaluated (no clock): AUTO only reports that
// the schedule is in charge.
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

struct Aux {
  String type = "other";  // lighting | treatment | robot | other   (heat pump: set aside, not selectable)
  String mode = "off";    // off | on | auto   (treatment: off | auto)
  uint32_t onEnd = 0;     // timed manual ON
  bool shutterMode = false;
  int shutterRate = 30;  // minutes per hour while the cover is closed (treatment)
  Range ranges[MAX_RANGES];
};
static struct {
  String mode = "off";          // off | on | auto
  int speed = 2;                // 1..3 (ON mode)
  uint32_t onEnd = 0;           // timed manual ON
  String autoKind = "thermo";   // variable | thermo | ranges
  int dailyAdjust = 0;          // -4..+4 h (thermo-adaptive)
  bool boost = false;
  uint32_t boostEnd = 0;
  Range ranges[MAX_RANGES];
  Aux aux[2];
  String spotlight = "CCEI";
  String lightName = "Blue";
  int lightValue = 35;
  int cycle = 2;
} sim;

static void simInit() {
  sim.aux[0].type = "lighting";
  sim.aux[1].type = "other";
}

static int remainingS(uint32_t endMs) {
  int32_t d = (int32_t)(endMs - millis());
  return d > 0 ? d / 1000 : 0;
}

static void simTick() {
  uint32_t now = millis();
  if (sim.mode == "on" && sim.onEnd && (int32_t)(now - sim.onEnd) >= 0) { sim.mode = "auto"; sim.onEnd = 0; }
  if (sim.boost && (int32_t)(now - sim.boostEnd) >= 0) sim.boost = false;
  for (int i = 0; i < 2; i++) {
    Aux &a = sim.aux[i];
    if (a.mode == "on" && a.onEnd && (int32_t)(now - a.onEnd) >= 0) { a.mode = "off"; a.onEnd = 0; }
  }
}

// Filtration state. AUTO without boost/forcing: depends on the schedule, which the demo does not evaluate.
static int filtrationRunning(const char **forcedBy) {  // 1 yes, 0 no, -1 unknown (schedule)
  *forcedBy = nullptr;
  if (sim.mode == "off") return 0;
  if (sim.mode == "on" || sim.boost) return 1;
  for (int i = 0; i < 2; i++)
    if (sim.aux[i].type == "robot" && sim.aux[i].mode == "on") { *forcedBy = i == 0 ? "AUX1 robot" : "AUX2 robot"; return 1; }
  return -1;
}

static bool rangesFromJson(JsonArray arr, Range out[MAX_RANGES]) {
  if (arr.size() > MAX_RANGES) return false;
  Range tmp[MAX_RANGES];
  int n = 0;
  for (JsonObject o : arr) {
    int days = o["days"] | 0, st = o["start"] | -1, en = o["end"] | -1, sp = o["speed"] | 2;
    if (days < 1 || days > 127 || st < 0 || st > 1439 || en < 1 || en > 1440 || st >= en || sp < 1 || sp > 3) return false;
    tmp[n].used = true; tmp[n].days = days; tmp[n].startMin = st; tmp[n].endMin = en; tmp[n].speed = sp;
    n++;
  }
  for (int i = 0; i < MAX_RANGES; i++) out[i] = tmp[i];
  return true;
}

static void rangesToJson(JsonArray arr, const Range r[MAX_RANGES]) {
  for (int i = 0; i < MAX_RANGES; i++) {
    if (!r[i].used) continue;
    JsonObject o = arr.add<JsonObject>();
    o["days"] = r[i].days; o["start"] = r[i].startMin; o["end"] = r[i].endMin; o["speed"] = r[i].speed;
  }
}

static int lightingAux() {  // index (1/2) of the first lighting auxiliary, 0 if none
  for (int i = 0; i < 2; i++) if (sim.aux[i].type == "lighting") return i + 1;
  return 0;
}

static void handleSim() {
  if (!demoMode) return noDevice();
  simTick();
  JsonDocument doc;
  doc["demo"] = true;
  JsonObject pool = doc["pool"].to<JsonObject>();
  pool["mode"] = sim.mode;
  pool["speed"] = sim.speed;
  pool["autoKind"] = sim.autoKind;
  pool["dailyAdjustHours"] = sim.dailyAdjust;
  pool["remainingSeconds"] = (sim.mode == "on" && sim.onEnd) ? remainingS(sim.onEnd) : 0;
  JsonObject b = pool["boost"].to<JsonObject>();
  b["active"] = sim.boost;
  b["remainingSeconds"] = sim.boost ? remainingS(sim.boostEnd) : 0;
  const char *forcedBy;
  int run = filtrationRunning(&forcedBy);
  if (run < 0) pool["running"] = nullptr; else pool["running"] = (run == 1);
  pool["forcedBy"] = forcedBy ? forcedBy : "";
  rangesToJson(pool["ranges"].to<JsonArray>(), sim.ranges);

  JsonArray aux = doc["aux"].to<JsonArray>();
  for (int i = 0; i < 2; i++) {
    const Aux &a = sim.aux[i];
    JsonObject o = aux.add<JsonObject>();
    o["index"] = i + 1;
    o["type"] = a.type;
    o["mode"] = a.mode;
    o["remainingSeconds"] = (a.mode == "on" && a.onEnd) ? remainingS(a.onEnd) : 0;
    o["shutterMode"] = a.shutterMode;
    o["shutterRate"] = a.shutterRate;
    const char *note = "";
    if (a.type == "robot" && a.mode == "on") note = "forces filtration (unless filtration is OFF)";
    else if (a.type == "treatment") note = "runs only while filtration runs";
    else if (a.mode == "auto") note = "schedule not simulated in the demo";
    o["note"] = note;
    rangesToJson(o["ranges"].to<JsonArray>(), a.ranges);
  }

  JsonObject l = doc["lighting"].to<JsonObject>();
  int li = lightingAux();
  l["auxIndex"] = li;
  l["on"] = li ? sim.aux[li - 1].mode == "on" : false;
  l["spotlight"] = sim.spotlight;
  l["color"] = sim.lightName;
  l["colorValue"] = sim.lightValue;
  l["remainingSeconds"] = (li && sim.aux[li - 1].onEnd) ? remainingS(sim.aux[li - 1].onEnd) : 0;
  JsonArray pal = l["palette"].to<JsonArray>();
  for (int i = 0; i < CCEI_COUNT; i++) {
    JsonObject p = pal.add<JsonObject>();
    p["name"] = CCEI_COLORS[i].name; p["value"] = CCEI_COLORS[i].value; p["kind"] = CCEI_COLORS[i].kind;
  }
  doc["water"]["temperatureC"] = serialized(String(24.0f + 0.5f * sinf(millis() / 400000.0f), 1));
  sendJson(200, doc);
}

static void reject(const char *why) {
  server.send(422, "application/json", String("{\"error\":\"") + why + "\"}");
}

static void handleFiltrationPut() {
  if (!demoMode) return noDevice();
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) { server.send(400, "application/json", "{\"error\":\"invalid JSON\"}"); return; }
  simTick();
  String mode = in["mode"] | sim.mode.c_str();
  int speed = in["speed"] | sim.speed;
  String kind = in["autoKind"] | sim.autoKind.c_str();
  int adj = in["dailyAdjustHours"] | sim.dailyAdjust;
  bool boostGiven = in["boost"].is<int>();
  int boost = in["boost"] | 0;
  if (!(mode == "off" || mode == "on" || mode == "auto")) return reject("mode must be off, on or auto");
  if (speed < 1 || speed > 3) return reject("speed must be 1-3");
  if (!(kind == "variable" || kind == "thermo" || kind == "ranges")) return reject("autoKind must be variable, thermo or ranges");
  if (adj < -4 || adj > 4) return reject("dailyAdjustHours must be -4..4");
  if (boostGiven && boost != 0 && boost != 24 && boost != 48) return reject("boost must be 0, 24 or 48 hours");
  if (boostGiven && boost > 0 && mode != "auto") return reject("boost is only available in AUTO mode");
  Range newRanges[MAX_RANGES];
  bool haveRanges = false;
  if (in["ranges"].is<JsonArray>()) {
    if (!rangesFromJson(in["ranges"].as<JsonArray>(), newRanges)) return reject("invalid ranges (max 8; days 1-127; start<end; speed 1-3)");
    haveRanges = true;
  }
  if (mode != sim.mode) { sim.onEnd = 0; if (mode != "auto") sim.boost = false; }
  sim.mode = mode; sim.speed = speed; sim.autoKind = kind; sim.dailyAdjust = adj;
  if (boostGiven) {
    if (boost == 0) sim.boost = false;
    else { sim.boost = true; sim.boostEnd = millis() + boost * 3600000UL; }
  }
  if (haveRanges) for (int i = 0; i < MAX_RANGES; i++) sim.ranges[i] = newRanges[i];
  server.send(204);
}

static void handleAuxPut(int i) {
  if (!demoMode) return noDevice();
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) { server.send(400, "application/json", "{\"error\":\"invalid JSON\"}"); return; }
  simTick();
  Aux &a = sim.aux[i];
  String type = in["type"] | a.type.c_str();
  if (type == "heatpump") return reject("heat pump is not supported yet");
  if (!(type == "lighting" || type == "treatment" || type == "robot" || type == "other")) return reject("type must be lighting, treatment, robot or other");
  bool typeChanged = (type != a.type);
  String mode = in["mode"] | (typeChanged ? "off" : a.mode.c_str());
  if (!(mode == "off" || mode == "on" || mode == "auto")) return reject("mode must be off, on or auto");
  if (type == "treatment" && mode == "on") return reject("treatment equipment has OFF and AUTO only");
  int rate = in["shutterRate"] | a.shutterRate;
  if (rate < 0 || rate > 60) return reject("shutterRate must be 0-60 min/h");
  Range newRanges[MAX_RANGES];
  bool haveRanges = false;
  if (in["ranges"].is<JsonArray>()) {
    if (!rangesFromJson(in["ranges"].as<JsonArray>(), newRanges)) return reject("invalid ranges (max 8; days 1-127; start<end)");
    haveRanges = true;
  }
  if (typeChanged) { a.type = type; a.onEnd = 0; a.shutterMode = false; for (int k = 0; k < MAX_RANGES; k++) a.ranges[k] = Range(); }
  if (mode != a.mode) a.onEnd = 0;
  a.mode = mode;
  a.shutterRate = rate;
  if (in["shutterMode"].is<bool>()) a.shutterMode = in["shutterMode"].as<bool>();
  if (haveRanges) for (int k = 0; k < MAX_RANGES; k++) a.ranges[k] = newRanges[k];
  server.send(204);
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
  uint32_t now = millis();
  Aux *a = (index == 1 || index == 2) ? &sim.aux[index - 1] : nullptr;

  if (cmd == "manualOn") {
    int dur = in["duration"] | 3600;
    if (dur < 1 || dur > 86400) return reject("duration must be 1-86400 s");
    if (index == 0) {
      int speed = in["speed"] | sim.speed;
      if (speed < 1 || speed > 3) return reject("speed must be 1-3");
      sim.mode = "on"; sim.onEnd = now + dur * 1000UL; sim.speed = speed; sim.boost = false;
    } else if (a) {
      if (a->type == "treatment") return reject("treatment equipment has OFF and AUTO only");
      a->mode = "on"; a->onEnd = now + dur * 1000UL;
    } else return reject("index must be 0 (filtration), 1 (AUX1) or 2 (AUX2)");
  } else if (cmd == "stopManualCommand") {
    if (index == 0) { sim.mode = "auto"; sim.onEnd = 0; sim.boost = false; }
    else if (a) { a->mode = "off"; a->onEnd = 0; }
    else return reject("index must be 0, 1 or 2");
  } else if (cmd == "boost") {
    int dur = in["boostDuration"] | 86400;
    if (index != 0) return reject("boost applies to filtration (index 0)");
    if (sim.mode != "auto") return reject("boost is only available in AUTO mode");
    if (dur != 86400 && dur != 172800) return reject("boostDuration must be 86400 (24 h) or 172800 (48 h)");
    sim.boost = true; sim.boostEnd = now + dur * 1000UL;
  } else if (cmd == "colorChange" || cmd == "colorChangeCCEI" || cmd == "colorChangeSeamaid") {
    if (!a || a->type != "lighting") return reject("index must be an auxiliary configured as lighting");
    if (cmd == "colorChange") {  // pulse the light supply: the lamp steps to its next colour
      do { sim.cycle = (sim.cycle + 1) % CCEI_COUNT; } while (String(CCEI_COLORS[sim.cycle].kind) != "color");
      sim.lightName = CCEI_COLORS[sim.cycle].name; sim.lightValue = CCEI_COLORS[sim.cycle].value;
    } else if (cmd == "colorChangeCCEI") {
      int mode = in["mode"] | -1, found = -1;
      for (int i = 0; i < CCEI_COUNT; i++) if (CCEI_COLORS[i].value == mode) found = i;
      if (found < 0) return reject("unknown CCEI value");
      sim.spotlight = "CCEI"; sim.lightName = CCEI_COLORS[found].name; sim.lightValue = mode;
    } else {
      int mode = in["mode"] | -1;  // 16 change colour, 48 save colour, 32 reset
      if (mode != 16 && mode != 48 && mode != 32) return reject("Seamaid mode must be 16, 32 or 48");
      sim.spotlight = "Seamaid"; sim.lightValue = mode;
      sim.lightName = mode == 16 ? "Change colour" : (mode == 48 ? "Save colour" : "Reset");
    }
    a->mode = "on"; a->onEnd = 0;
  } else {
    return reject("command not part of the simulated POOL COMMAND VS2");
  }
  server.send(204);
}

// ---------- developer tools: password, OTA update, BLE explorer ----------
// Everything here needs the developer password (header X-Dev-Password). Until a password is set, the tools are
// disabled. BLE writes are locked by default and must be enabled for a limited time; every write is logged.
static String lastAuthAddress;          // BLE session auth (docs/ble-auth.md): last authenticated address...
static uint8_t lastAuthNonce[16];       // ...and the nonce used for it (RAM only, cleared on disconnect/reboot)
static bool lastAuthNonceValid = false;
static String devPwHash;                // SHA-256 (hex) of the developer password; empty = tools disabled
static uint32_t writeEnabledUntil = 0;  // BLE writes allowed until this millis() value (0 = locked)

static String sha256Hex(const String &s) {
  uint8_t h[32];
  mbedtls_sha256((const unsigned char *)s.c_str(), s.length(), h, 0);
  char b[65];
  for (int i = 0; i < 32; i++) snprintf(b + 2 * i, 3, "%02x", h[i]);
  return String(b);
}

static bool devAuthOk() { return devPwHash.length() && sha256Hex(server.header("X-Dev-Password")) == devPwHash; }

static bool devAuth() {
  if (!devPwHash.length()) {
    server.send(403, "application/json", "{\"error\":\"developer password not set (PUT /api/v1/dev/password)\"}");
    return false;
  }
  if (!devAuthOk()) {
    delay(1000);  // slow down guessing
    server.send(401, "application/json", "{\"error\":\"wrong or missing X-Dev-Password\"}");
    return false;
  }
  return true;
}

static bool writesAllowed() { return writeEnabledUntil && (int32_t)(writeEnabledUntil - millis()) > 0; }

#define DEV_LOG_SIZE 80
struct DevLogEntry { uint32_t seq = 0, ms = 0; char dir = ' '; String uuid, hex; };
static DevLogEntry devLog[DEV_LOG_SIZE];
static uint32_t devLogSeq = 0;
static BLEClient *devClient = nullptr;
static volatile bool devLinked = false;

static void devLogAdd(char dir, const String &uuid, const uint8_t *d, size_t n) {
  DevLogEntry &e = devLog[devLogSeq % DEV_LOG_SIZE];
  e.seq = ++devLogSeq; e.ms = millis(); e.dir = dir; e.uuid = uuid;
  String h;
  char b[3];
  for (size_t i = 0; i < n && i < 256; i++) { snprintf(b, sizeof(b), "%02x", d[i]); h += b; }
  e.hex = h;
}

class DevClientCb : public BLEClientCallbacks {
  void onConnect(BLEClient *) override {}
  void onDisconnect(BLEClient *) override { devLinked = false; }
};
static DevClientCb devClientCb;

static void handleDevStatus() {  // no password needed: says only whether the tools are enabled
  JsonDocument doc;
  doc["passwordSet"] = devPwHash.length() > 0;
  doc["bleLinked"] = (bool)devLinked;
  doc["writeEnabled"] = writesAllowed();
  doc["writeSecondsLeft"] = writesAllowed() ? (int)((writeEnabledUntil - millis()) / 1000) : 0;
  sendJson(200, doc);
}

static void handleDevPassword() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) { server.send(400, "application/json", "{\"error\":\"invalid JSON\"}"); return; }
  String pw = in["password"] | "";
  if (pw.length() < 8 || pw.length() > 64) { server.send(422, "application/json", "{\"error\":\"password must be 8-64 characters\"}"); return; }
  if (devPwHash.length() && sha256Hex(in["current"] | "") != devPwHash) {
    delay(1000);
    server.send(401, "application/json", "{\"error\":\"current password is wrong\"}");
    return;
  }
  devPwHash = sha256Hex(pw);
  prefs.putString("devpw", devPwHash);
  server.send(204);
}

// ----- OTA: POST /api/v1/ota (multipart/form-data, field "file" = the app .bin) -----
static bool otaAllowed = false, otaFailed = false;
static void otaUpload() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    otaAllowed = devAuthOk();
    otaFailed = false;
    if (otaAllowed && !Update.begin(UPDATE_SIZE_UNKNOWN)) otaFailed = true;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (otaAllowed && !otaFailed && Update.write(u.buf, u.currentSize) != u.currentSize) otaFailed = true;
  } else if (u.status == UPLOAD_FILE_END) {
    if (otaAllowed && !otaFailed && !Update.end(true)) otaFailed = true;
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (otaAllowed) Update.abort();
  }
}
static void otaDone() {
  if (!otaAllowed) {
    delay(1000);
    server.send(devPwHash.length() ? 401 : 403, "application/json", "{\"error\":\"developer password missing, wrong or not set\"}");
    return;
  }
  if (otaFailed || Update.hasError()) {
    server.send(500, "application/json", String("{\"error\":\"update failed: ") + Update.errorString() + "\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
  restartAtMs = millis() + 1500;
}

// ----- BLE explorer -----
static String hexOf(const String &v) {
  String h;
  char b[3];
  for (size_t i = 0; i < v.length(); i++) { snprintf(b, sizeof(b), "%02x", (uint8_t)v[i]); h += b; }
  return h;
}

static BLERemoteCharacteristic *devFindChar(JsonDocument &in) {
  if (!devClient || !devClient->isConnected()) return nullptr;
  String s = in["service"] | "", c = in["characteristic"] | "";
  BLERemoteService *svc = devClient->getService(BLEUUID(s));
  return svc ? svc->getCharacteristic(BLEUUID(c)) : nullptr;
}

static void handleDevBleConnect() {
  if (!devAuth()) return;
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain")) || !in["address"].is<const char *>()) {
    server.send(400, "application/json", "{\"error\":\"body must be {\\\"address\\\":\\\"..\\\",\\\"addrType\\\":0|1}\"}");
    return;
  }
  if (demoMode) { server.send(409, "application/json", "{\"error\":\"turn demo mode off first\"}"); return; }
  bleScan->stop();
  scanning = false;
  if (devClient && devClient->isConnected()) devClient->disconnect();
  if (!devClient) {
    devClient = BLEDevice::createClient();
    devClient->setClientCallbacks(&devClientCb);
  }
  String addr = in["address"].as<String>();
  int type = in["addrType"] | 0;
#if defined(CONFIG_BLUEDROID_ENABLED)
  bool ok = devClient->connect(BLEAddress(addr), (esp_ble_addr_type_t)type, 10000);  // classic ESP32 (Bluedroid), 10 s timeout
#else
  bool ok = devClient->connect(BLEAddress(addr), (uint8_t)type, 10000);  // NimBLE (e.g. ESP32-P4 + C6 over ESP-Hosted), 10 s timeout
#endif
  devLinked = ok;
  if (ok) devLogAdd('C', addr, nullptr, 0);
  JsonDocument out;
  out["connected"] = ok;
  sendJson(ok ? 200 : 502, out);
}

static void handleDevBleDisconnect() {
  if (!devAuth()) return;
  if (devClient && devClient->isConnected()) devClient->disconnect();
  devLinked = false;
  lastAuthNonceValid = false;
  server.send(204);
}

static void handleDevBleServices() {
  if (!devAuth()) return;
  if (!devClient || !devClient->isConnected()) { server.send(409, "application/json", "{\"error\":\"not connected\"}"); return; }
  JsonDocument doc;
  JsonArray arr = doc["services"].to<JsonArray>();
  auto *svcs = devClient->getServices();
  if (svcs) {
    for (auto &kv : *svcs) {
      JsonObject so = arr.add<JsonObject>();
      so["uuid"] = kv.second->getUUID().toString();
      JsonArray chs = so["characteristics"].to<JsonArray>();
      auto *cm = kv.second->getCharacteristics();
      if (!cm) continue;
      for (auto &ck : *cm) {
        BLERemoteCharacteristic *c = ck.second;
        JsonObject co = chs.add<JsonObject>();
        co["uuid"] = c->getUUID().toString();
        co["read"] = c->canRead();
        co["write"] = c->canWrite();
        co["writeNoResponse"] = c->canWriteNoResponse();
        co["notify"] = c->canNotify();
        co["indicate"] = c->canIndicate();
      }
    }
  }
  sendJson(200, doc);
}

static void handleDevBleRead() {
  if (!devAuth()) return;
  JsonDocument in;
  deserializeJson(in, server.arg("plain"));
  BLERemoteCharacteristic *c = devFindChar(in);
  if (!c) { server.send(404, "application/json", "{\"error\":\"not connected or characteristic not found\"}"); return; }
  if (!c->canRead()) { server.send(422, "application/json", "{\"error\":\"characteristic is not readable\"}"); return; }
  String v = c->readValue();
  devLogAdd('R', c->getUUID().toString(), (const uint8_t *)v.c_str(), v.length());
  JsonDocument out;
  out["hex"] = hexOf(v);
  sendJson(200, out);
}

static void handleDevBleSubscribe() {
  if (!devAuth()) return;
  JsonDocument in;
  deserializeJson(in, server.arg("plain"));
  BLERemoteCharacteristic *c = devFindChar(in);
  if (!c) { server.send(404, "application/json", "{\"error\":\"not connected or characteristic not found\"}"); return; }
  if (!c->canNotify() && !c->canIndicate()) { server.send(422, "application/json", "{\"error\":\"characteristic cannot notify\"}"); return; }
  c->registerForNotify([](BLERemoteCharacteristic *ch, uint8_t *d, size_t n, bool) { devLogAdd('N', ch->getUUID().toString(), d, n); });
  server.send(204);
}

static void handleDevBleWriteEnable() {
  if (!devAuth()) return;
  JsonDocument in;
  deserializeJson(in, server.arg("plain"));
  bool en = in["enabled"] | false;
  int minutes = in["minutes"] | 10;
  if (minutes < 1 || minutes > 30) { server.send(422, "application/json", "{\"error\":\"minutes must be 1-30\"}"); return; }
  writeEnabledUntil = en ? millis() + minutes * 60000UL : 0;
  server.send(204);
}

static void handleDevBleWrite() {
  if (!devAuth()) return;
  if (!writesAllowed()) { server.send(403, "application/json", "{\"error\":\"BLE writes are locked; enable them first (PUT /api/v1/dev/ble/write-enable)\"}"); return; }
  JsonDocument in;
  deserializeJson(in, server.arg("plain"));
  BLERemoteCharacteristic *c = devFindChar(in);
  if (!c) { server.send(404, "application/json", "{\"error\":\"not connected or characteristic not found\"}"); return; }
  String hex = in["hex"] | "";
  if (hex.length() == 0 || hex.length() % 2 || hex.length() > 488) { server.send(422, "application/json", "{\"error\":\"hex must be an even-length string, max 244 bytes\"}"); return; }
  uint8_t buf[244];
  size_t n = hex.length() / 2;
  for (size_t i = 0; i < n; i++) {
    char t[3] = {hex[2 * i], hex[2 * i + 1], 0};
    char *end;
    long v = strtol(t, &end, 16);
    if (*end) { server.send(422, "application/json", "{\"error\":\"hex contains non-hex characters\"}"); return; }
    buf[i] = (uint8_t)v;
  }
  bool resp = in["response"] | true;
  devLogAdd('W', c->getUUID().toString(), buf, n);
  c->writeValue(buf, n, resp);
  server.send(204);
}

static void handleDevBleLog() {
  if (!devAuth()) return;
  uint32_t since = server.hasArg("since") ? (uint32_t)server.arg("since").toInt() : 0;
  JsonDocument doc;
  doc["last"] = devLogSeq;
  JsonArray arr = doc["entries"].to<JsonArray>();
  uint32_t first = devLogSeq > DEV_LOG_SIZE ? devLogSeq - DEV_LOG_SIZE + 1 : 1;
  for (uint32_t s = max(first, since + 1); s <= devLogSeq; s++) {
    const DevLogEntry &e = devLog[(s - 1) % DEV_LOG_SIZE];
    JsonObject o = arr.add<JsonObject>();
    o["seq"] = e.seq; o["ms"] = e.ms; o["dir"] = String(e.dir); o["uuid"] = e.uuid; o["hex"] = e.hex;
  }
  sendJson(200, doc);
}

// ----- BLE session authentication (see docs/ble-auth.md). The key stays out of version control; it is
// supplied locally via ble_secrets.h (git-ignored) and is never echoed back by any endpoint. -----
#define AUTH_MARKER_LEN 14
static const uint8_t AUTH_MARKER[AUTH_MARKER_LEN] = {0x41, 0x55, 0x54, 0x48, 0x45, 0x4e, 0x54, 0x49, 0x43, 0x41, 0x54, 0x45, 0x44, 0x00};

static bool authKeyBytes(uint8_t out[16]) {
  String k = BLE_AUTH_KEY_HEX;
  if (k.length() != 32) return false;
  for (int i = 0; i < 16; i++) {
    char t[3] = {k[2 * i], k[2 * i + 1], 0};
    char *end;
    long v = strtol(t, &end, 16);
    if (*end) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

static void handleDevBleAuth() {
  if (!devAuth()) return;
  if (!devClient || !devClient->isConnected()) { server.send(409, "application/json", "{\"error\":\"not connected\"}"); return; }
  uint8_t key[16];
  if (!authKeyBytes(key)) {
    server.send(409, "application/json", "{\"error\":\"BLE_AUTH_KEY_HEX not configured (see docs/ble-auth.md and ble_secrets.h.example)\"}");
    return;
  }
  JsonDocument in;
  deserializeJson(in, server.arg("plain"));
  BLERemoteCharacteristic *c = devFindChar(in);
  if (!c || !c->canRead()) { server.send(404, "application/json", "{\"error\":\"not connected or auth characteristic not found/readable\"}"); return; }
  String v = c->readValue();
  devLogAdd('R', c->getUUID().toString(), (const uint8_t *)v.c_str(), v.length());

  JsonDocument out;
  String addr = devClient->getPeerAddress().toString();
  if (v.length() == AUTH_MARKER_LEN && memcmp(v.c_str(), AUTH_MARKER, AUTH_MARKER_LEN) == 0) {
    bool reused = lastAuthNonceValid && lastAuthAddress == addr;
    out["authenticated"] = true;
    out["reused"] = reused;
    if (!reused) out["note"] = "device reports already authenticated but this gateway has no cached nonce for it (new boot or different session)";
    sendJson(200, out);
    return;
  }
  if (v.length() != 16) {
    out["authenticated"] = false;
    out["error"] = "unexpected nonce length (" + String(v.length()) + " bytes, expected 16)";
    sendJson(502, out);
    return;
  }
  uint8_t resp[16];
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key, 128);
  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, (const uint8_t *)v.c_str(), resp);
  mbedtls_aes_free(&aes);
  if (!c->canWrite() && !c->canWriteNoResponse()) { server.send(422, "application/json", "{\"error\":\"auth characteristic is not writable\"}"); return; }
  if (!writesAllowed()) { server.send(403, "application/json", "{\"error\":\"BLE writes are locked; enable them first (PUT /api/v1/dev/ble/write-enable) before authenticating\"}"); return; }
  devLogAdd('W', c->getUUID().toString(), resp, 16);
  c->writeValue(resp, 16, c->canWrite());
  memcpy(lastAuthNonce, v.c_str(), 16);
  lastAuthAddress = addr;
  lastAuthNonceValid = true;
  out["authenticated"] = true;
  out["reused"] = false;
  sendJson(200, out);
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
  devPwHash = prefs.getString("devpw", "");
  simInit();

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
  server.on("/api/v1/filtration", HTTP_PUT, handleFiltrationPut);
  server.on("/api/v1/aux1", HTTP_PUT, []() { handleAuxPut(0); });
  server.on("/api/v1/aux2", HTTP_PUT, []() { handleAuxPut(1); });
  server.on("/api/v1/dev", HTTP_GET, handleDevStatus);
  server.on("/api/v1/dev/password", HTTP_PUT, handleDevPassword);
  server.on("/api/v1/ota", HTTP_POST, otaDone, otaUpload);
  server.on("/api/v1/dev/ble/connect", HTTP_POST, handleDevBleConnect);
  server.on("/api/v1/dev/ble/auth", HTTP_POST, handleDevBleAuth);
  server.on("/api/v1/dev/ble/disconnect", HTTP_POST, handleDevBleDisconnect);
  server.on("/api/v1/dev/ble/services", HTTP_GET, handleDevBleServices);
  server.on("/api/v1/dev/ble/read", HTTP_POST, handleDevBleRead);
  server.on("/api/v1/dev/ble/subscribe", HTTP_POST, handleDevBleSubscribe);
  server.on("/api/v1/dev/ble/write", HTTP_POST, handleDevBleWrite);
  server.on("/api/v1/dev/ble/write-enable", HTTP_PUT, handleDevBleWriteEnable);
  server.on("/api/v1/dev/ble/log", HTTP_GET, handleDevBleLog);
  server.on("/api/v1/wifi", HTTP_GET, handleWifiGet);
  server.on("/api/v1/wifi", HTTP_PUT, handleWifiPut);
  server.on("/api/v1/wifi", HTTP_DELETE, handleWifiDelete);
  server.on("/api/v1/wifi/scan", HTTP_POST, handleWifiScanStart);
  server.on("/api/v1/wifi/scan", HTTP_GET, handleWifiScanResults);
  server.onNotFound(handleNotFound);
  const char *hdrs[] = {"X-Dev-Password"};
  server.collectHeaders(hdrs, 1);
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
