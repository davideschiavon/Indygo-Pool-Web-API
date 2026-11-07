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

#define FIRMWARE_VERSION "0.2.0-m1a"
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
 input{padding:8px;border:1px solid #b9c7cd;border-radius:6px;width:100%;box-sizing:border-box;margin:4px 0 10px}
 .hide{display:none}
</style></head><body>
<header>Indygo Pool Gateway <span class="muted" id="ver" style="color:#cfe8f1"></span></header>
<nav><button id="t-status" class="on" onclick="tab('status')">Status</button>
<button id="t-ble" onclick="tab('ble')">Bluetooth devices</button>
<button id="t-wifi" onclick="tab('wifi')">Wi-Fi</button></nav>
<main>
<section id="status" class="card"><h3>Selected pool device</h3><div id="sel">Loading...</div>
<p class="muted">Open the <b>Bluetooth devices</b> menu to find and select your device.</p>
<h3>Network</h3><div id="net">Loading...</div></section>

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
const $=id=>document.getElementById(id);let poll,wpoll;const T=['status','ble','wifi'];
function tab(t){for(const s of T){$(s).classList.toggle('hide',s!==t);$('t-'+s).classList.toggle('on',s===t)}if(t==='status'){loadSel();loadNet()}if(t==='wifi')loadNet()}
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
j('/api/v1/gateway').then(g=>$('ver').textContent='v'+g.firmware);loadSel();loadNet();
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
  doc["address"] = selAddress;
  doc["name"] = selName;
  doc["connected"] = false;
  sendJson(200, doc);
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
