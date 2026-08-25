const char DASHBOARD_HTML[] PROGMEM = R"HTMLEOF(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>Bots Bangla · GPS Tracker</title>
<style>
:root{
  --bg:#0c0e13;--panel:#141821;--panel2:#181d28;--line:#252b38;
  --txt:#e8eaf0;--dim:#8b93a5;--faint:#5b6273;
  --accent:#ff5a2c;--accent-dim:#c23f1c;--ok:#3ecf8e;--warn:#f5a623;--err:#ff4d5e;
  --blue:#4a9eff;--mono:ui-monospace,'SF Mono',Menlo,Consolas,monospace;
  --sans:-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;
}
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{background:var(--bg);color:var(--txt);font-family:var(--sans);min-height:100vh}
header{background:var(--panel);border-bottom:1px solid var(--line);
  padding:14px 20px;display:flex;align-items:center;justify-content:space-between;
  position:sticky;top:0;z-index:100}
.brand{display:flex;align-items:center;gap:10px}
.dot{width:9px;height:9px;border-radius:50%;background:var(--accent);
  box-shadow:0 0 10px var(--accent);animation:pulse 2s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}
.brand b{font-size:14px;letter-spacing:.12em;text-transform:uppercase}
nav{display:none;gap:4px;overflow-x:auto;padding:0 20px;
  background:var(--panel);border-bottom:1px solid var(--line)}
nav.unlocked{display:flex}
nav button{background:none;border:none;color:var(--dim);font-size:12px;
  padding:10px 14px;cursor:pointer;white-space:nowrap;border-bottom:2px solid transparent;
  font-family:var(--sans);letter-spacing:.08em;transition:.15s}
nav button.active{color:var(--accent);border-bottom-color:var(--accent)}
.page{display:none;padding:16px 20px 60px;max-width:520px;margin:0 auto}
.page.active{display:block}
.card{background:var(--panel);border:1px solid var(--line);border-radius:14px;
  padding:16px;margin-bottom:14px}
.card h3{font-size:11px;letter-spacing:.14em;text-transform:uppercase;
  color:var(--faint);margin-bottom:12px}
.kv{display:flex;justify-content:space-between;align-items:center;
  padding:6px 0;border-bottom:1px solid var(--line)}
.kv:last-child{border-bottom:none}
.kv .k{color:var(--dim);font-size:13px}
.kv .v{font-family:var(--mono);font-size:13px;text-align:right}
.ok{color:var(--ok)}.warn{color:var(--warn)}.err{color:var(--err)}.blue{color:var(--blue)}
label{display:block;font-size:12px;color:var(--dim);margin:10px 0 5px}
input,select{width:100%;background:var(--bg);border:1px solid var(--line);
  border-radius:8px;color:var(--txt);font-size:14px;font-family:var(--mono);
  padding:10px 12px;outline:none;transition:.15s}
input:focus,select:focus{border-color:var(--accent)}
input[type=checkbox]{width:auto}
input[type=range]{padding:0;height:28px;accent-color:var(--accent)}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.btn{padding:11px 20px;border:none;border-radius:9px;font-size:13px;
  font-weight:600;cursor:pointer;transition:.15s;font-family:var(--sans)}
.btn-primary{background:var(--accent);color:#fff}
.btn-ghost{background:transparent;border:1px solid var(--line);color:var(--dim)}
.btn-danger{background:rgba(255,77,94,.15);border:1px solid var(--err);color:var(--err)}
.btn-ok{background:rgba(62,207,142,.15);border:1px solid var(--ok);color:var(--ok)}
.btn:active{transform:scale(.97)}
.btn:disabled{opacity:.4}
.full{width:100%;margin-top:8px}
.lockwrap{display:flex;flex-direction:column;align-items:center;padding:30px 0}
.lock-title{font-size:18px;font-weight:700;margin-bottom:4px}
.lock-sub{color:var(--dim);font-size:13px;margin-bottom:24px}
.pin-dots{display:flex;gap:14px;margin-bottom:24px}
.pd{width:13px;height:13px;border-radius:50%;border:1.5px solid var(--faint);transition:.15s}
.pd.on{background:var(--accent);border-color:var(--accent);box-shadow:0 0 10px rgba(255,90,44,.5)}
.pad{display:grid;grid-template-columns:repeat(3,72px);gap:12px}
.key{height:72px;border-radius:50%;border:1px solid var(--line);background:var(--panel);
  color:var(--txt);font-size:22px;font-family:var(--mono);cursor:pointer;transition:.12s}
.key:active{background:var(--panel2);border-color:var(--accent);transform:scale(.93)}
.key.util{font-size:12px;color:var(--dim)}
.lockmsg{height:18px;margin-top:16px;font-size:12px;color:var(--err);font-family:var(--mono)}
#map,#pbmap{height:320px;border-radius:12px;overflow:hidden;border:1px solid var(--line);
  margin-bottom:14px;background:var(--panel)}
.map-placeholder{display:flex;flex-direction:column;align-items:center;justify-content:center;
  height:100%;color:var(--faint);font-size:13px;gap:8px}
.progbar{background:var(--line);border-radius:4px;height:8px;overflow:hidden;margin:6px 0}
.progbar-fill{height:100%;border-radius:4px;background:var(--accent);transition:width .3s}
.progbar-fill.ok{background:var(--ok)}
.status-chip{display:inline-block;font-size:11px;font-family:var(--mono);
  padding:3px 9px;border-radius:5px;border:1px solid currentColor}
.batt-bar{display:flex;gap:3px;align-items:flex-end;height:20px}
.batt-seg{width:8px;border-radius:2px;background:var(--line)}
.batt-seg.on{background:var(--ok)}
.batt-seg.warn{background:var(--warn)}
.batt-seg.err{background:var(--err)}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);
  background:var(--panel2);border:1px solid var(--line);border-radius:10px;
  padding:10px 18px;font-size:13px;font-family:var(--mono);
  opacity:0;transition:.25s;pointer-events:none;z-index:999;max-width:88%}
.toast.show{opacity:1}.toast.err{border-color:var(--err);color:var(--err)}
.log-entry{font-family:var(--mono);font-size:11px;padding:8px 10px;
  border-bottom:1px solid var(--line);cursor:pointer;transition:.12s}
.log-entry:hover{background:var(--panel2)}
.log-entry .ts{color:var(--dim);font-size:10px}
.net{display:flex;justify-content:space-between;align-items:center;padding:10px 12px;
  border:1px solid var(--line);border-radius:8px;margin-bottom:8px;cursor:pointer;transition:.12s}
.net:active,.net.sel{border-color:var(--accent);background:rgba(255,90,44,.06)}
.net .nm{font-family:var(--mono);font-size:13px}
.net .meta{font-size:11px;color:var(--faint);font-family:var(--mono)}
.scanmsg{color:var(--faint);font-size:13px;font-family:var(--mono);padding:6px 2px}
.mode-tag{display:inline-block;font-family:var(--mono);font-size:10px;letter-spacing:.14em;
  text-transform:uppercase;color:var(--accent);border:1px solid var(--accent-dim);
  border-radius:5px;padding:3px 8px;margin-bottom:10px}
.pb-controls{display:flex;gap:8px;align-items:center;margin-top:10px}
.pb-time{font-family:var(--mono);font-size:11px;color:var(--dim);text-align:center;margin-top:6px}
.pb-stats{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:12px}
.pb-stat{background:var(--bg);border:1px solid var(--line);border-radius:9px;padding:10px;text-align:center}
.pb-stat .n{font-family:var(--mono);font-size:15px;color:var(--txt)}
.pb-stat .l{font-size:10px;letter-spacing:.1em;text-transform:uppercase;color:var(--faint);margin-top:3px}
.switchrow{display:flex;justify-content:space-between;align-items:center;padding:8px 0}
.switchrow .k{font-size:13px;color:var(--dim)}
.sw{position:relative;width:44px;height:24px;background:var(--line);border-radius:12px;
  cursor:pointer;transition:.2s;flex-shrink:0}
.sw::after{content:'';position:absolute;top:3px;left:3px;width:18px;height:18px;
  border-radius:50%;background:var(--faint);transition:.2s}
.sw.on{background:var(--accent)}
.sw.on::after{left:23px;background:#fff}
</style>
</head>
<body>
<header>
  <div class="brand">
    <div class="dot"></div>
    <div><b>Bots Bangla</b></div>
  </div>
  <div style="display:flex;gap:8px;align-items:center">
    <span id="hdr-bat" style="font-family:var(--mono);font-size:11px;color:var(--ok)">🔋—</span>
    <span id="hdr-net" style="font-family:var(--mono);font-size:11px;color:var(--dim)">—</span>
    <span id="hdr-dev" style="font-family:var(--mono);font-size:11px;color:var(--faint)">—</span>
  </div>
</header>

<!-- Tabs stay hidden until a valid PIN is entered. Which tabs appear depends on the role. -->
<nav id="nav">
  <button onclick="showPage('p-live')"  id="nb-live">📍 Live</button>
  <button onclick="showPage('p-play')"  id="nb-play">▶️ Playback</button>
  <button onclick="showPage('p-hist')"  id="nb-hist">📜 History</button>
  <button onclick="showPage('p-sync')"  id="nb-sync">☁️ Sync</button>
  <button onclick="showPage('p-store')" id="nb-store">💾 Storage</button>
  <button onclick="showPage('p-net')"   id="nb-net">📡 Network</button>
  <button onclick="showPage('p-logs')"  id="nb-logs" style="display:none">📋 Logs</button>
  <button onclick="showPage('p-cfg')"   id="nb-cfg">⚙️ Config</button>
  <button onclick="showPage('p-dev')"   id="nb-dev" style="display:none">🛠️ Dev</button>
  <button onclick="lockNow()"           id="nb-relock">🔒 Lock</button>
</nav>

<div id="p-lock" class="page active">
  <div class="lockwrap">
    <div class="lock-title">Device Locked</div>
    <div class="lock-sub">Enter PIN to access dashboard</div>
    <div class="pin-dots">
      <i class="pd"></i><i class="pd"></i><i class="pd"></i><i class="pd"></i>
    </div>
    <div class="pad">
      <button class="key">1</button><button class="key">2</button><button class="key">3</button>
      <button class="key">4</button><button class="key">5</button><button class="key">6</button>
      <button class="key">7</button><button class="key">8</button><button class="key">9</button>
      <button class="key util" id="k-clr">CLR</button>
      <button class="key">0</button>
      <button class="key util" id="k-del">⌫</button>
    </div>
    <div class="lockmsg" id="lockmsg"></div>
  </div>
</div>

<div id="p-live" class="page">
  <div id="map">
    <div class="map-placeholder">
      <span style="font-size:32px">📍</span>
      <span>Map requires internet &amp; GPS fix</span>
      <span id="map-status" style="font-size:11px"></span>
    </div>
  </div>
  <div class="card">
    <h3>GPS Fix</h3>
    <div class="kv"><span class="k">Status</span><span class="v" id="l-fix">—</span></div>
    <div class="kv"><span class="k">Latitude</span><span class="v" id="l-lat">—</span></div>
    <div class="kv"><span class="k">Longitude</span><span class="v" id="l-lng">—</span></div>
    <div class="kv"><span class="k">Altitude</span><span class="v" id="l-alt">—</span></div>
    <div class="kv"><span class="k">Speed</span><span class="v" id="l-spd">—</span></div>
    <div class="kv"><span class="k">Heading</span><span class="v" id="l-hdg">—</span></div>
    <div class="kv"><span class="k">Satellites</span><span class="v" id="l-sats">—</span></div>
    <div class="kv"><span class="k">Accuracy (HDOP)</span><span class="v" id="l-hdop">—</span></div>
  </div>
  <div class="card">
    <h3>Battery</h3>
    <div class="kv"><span class="k">Voltage</span><span class="v" id="l-batv">—</span></div>
    <div class="kv"><span class="k">Level</span>
      <span class="v">
        <span id="l-batp">—</span>
        <div class="batt-bar" id="l-batbar" style="display:inline-flex;margin-left:8px"></div>
      </span>
    </div>
  </div>
</div>

<div id="p-play" class="page">
  <div class="card">
    <h3>Track Source</h3>
    <div class="row2">
      <div>
        <label>Track</label>
        <select id="pb-file"></select>
      </div>
      <div>
        <label>&nbsp;</label>
        <button class="btn btn-primary" style="width:100%" onclick="loadTrack()">Load track</button>
      </div>
    </div>
    <div class="scanmsg" id="pb-loadmsg"></div>
  </div>

  <div id="pbmap">
    <div class="map-placeholder">
      <span style="font-size:32px">▶️</span>
      <span>Load a track to play it back</span>
    </div>
  </div>

  <div class="card">
    <h3>Playback</h3>
    <input type="range" id="pb-scrub" min="0" max="0" value="0" oninput="scrubTo(this.value)">
    <div class="pb-time" id="pb-time">—</div>
    <div class="pb-controls">
      <button class="btn btn-primary" id="pb-playbtn" onclick="togglePlay()" style="flex:1">▶ Play</button>
      <select id="pb-speed" style="width:90px" onchange="setPlaySpeed()">
        <option value="1">1×</option>
        <option value="5" selected>5×</option>
        <option value="20">20×</option>
        <option value="60">60×</option>
      </select>
    </div>
    <div class="pb-stats">
      <div class="pb-stat"><div class="n" id="pb-dist">—</div><div class="l">Distance</div></div>
      <div class="pb-stat"><div class="n" id="pb-dur">—</div><div class="l">Duration</div></div>
      <div class="pb-stat"><div class="n" id="pb-max">—</div><div class="l">Max speed</div></div>
    </div>
    <div class="kv" style="margin-top:10px"><span class="k">Point</span><span class="v" id="pb-point">—</span></div>
    <div class="kv"><span class="k">Position</span><span class="v" id="pb-pos">—</span></div>
    <div class="kv"><span class="k">Speed here</span><span class="v" id="pb-spdhere">—</span></div>
  </div>

  <div class="card" id="pb-dev-card" style="display:none">
    <h3>Track Storage (Developer)</h3>
    <div class="switchrow">
      <span class="k">Auto-delete oldest logs when SD is full</span>
      <div class="sw" id="sw-autodel" onclick="toggleAutoDel()"></div>
    </div>
    <label>Auto-delete threshold — max SD usage (%)</label>
    <input type="number" id="pb-maxstore" min="10" max="99" value="80">
    <button class="btn btn-primary full" onclick="saveAutoDel()">Save auto-delete settings</button>
    <div style="display:flex;gap:8px;margin-top:12px">
      <button class="btn btn-danger full" onclick="deletePlaybackFile()">🗑 Delete this track</button>
      <button class="btn btn-danger full" onclick="deleteAllLogs()">🗑 Delete all tracks</button>
    </div>
  </div>
</div>

<div id="p-hist" class="page">
  <div class="card">
    <h3>Date Filter</h3>
    <div class="row2">
      <div>
        <label>Log File</label>
        <select id="hist-file" onchange="loadHistPage()"></select>
      </div>
      <div>
        <label>Page Size</label>
        <select id="hist-size" onchange="loadHistPage()">
          <option value="20">20</option>
          <option value="50">50</option>
          <option value="100">100</option>
        </select>
      </div>
    </div>
    <label>Search (substring, e.g. a date/time fragment)</label>
    <input id="hist-search" placeholder="e.g. 2026-07-18 14" oninput="debounceHist()">
    <label><input type="checkbox" id="hist-newest" onchange="loadHistPage()" checked> Newest first</label>
  </div>
  <div class="card">
    <h3>Records <span id="hist-count" style="color:var(--faint);font-weight:400"></span></h3>
    <div id="hist-list"><div class="scanmsg">Select a file above</div></div>
    <div style="display:flex;gap:8px;margin-top:12px">
      <button class="btn btn-ghost" onclick="histPrev()">← Prev</button>
      <button class="btn btn-ghost" onclick="histNext()">Next →</button>
      <span id="hist-page-info" style="font-size:11px;color:var(--dim);line-height:36px">—</span>
    </div>
  </div>
</div>

<div id="p-sync" class="page">
  <div class="card">
    <h3>Sync Status</h3>
    <div class="kv"><span class="k">Status</span>
      <span class="v"><span id="s-status" class="status-chip">—</span></span></div>
    <div class="kv"><span class="k">Pending Records</span><span class="v ok" id="s-pending">—</span></div>
    <div class="kv"><span class="k">Synced Records</span><span class="v" id="s-synced">—</span></div>
    <div class="kv"><span class="k">Batch Size</span><span class="v" id="s-batch">—</span></div>
    <div class="kv"><span class="k">Upload Speed</span><span class="v" id="s-speed">—</span></div>
    <div class="kv"><span class="k">Est. Remaining</span><span class="v" id="s-est">—</span></div>
    <div class="kv"><span class="k">Last Success</span><span class="v" id="s-ok">—</span></div>
    <div class="kv"><span class="k">Last Failure</span><span class="v" id="s-fail">—</span></div>
    <div class="kv"><span class="k">Last Error</span><span class="v err" id="s-err">—</span></div>
  </div>
  <div class="card" id="sync-dev-card" style="display:none">
    <h3>Sync Controls (Developer)</h3>
    <label>Batch Size (1–100)</label>
    <input type="number" id="s-bsize" min="1" max="100" value="30">
    <div style="display:flex;gap:8px;margin-top:10px">
      <button class="btn btn-ok full" onclick="syncNow()">⬆ Sync Now</button>
      <button class="btn btn-ghost full" onclick="pauseSync()">⏸ Pause</button>
      <button class="btn btn-ghost full" onclick="resumeSync()">▶️ Resume</button>
    </div>
    <button class="btn btn-primary full" onclick="saveBatchSize()">Save Batch Size</button>
  </div>
</div>

<div id="p-store" class="page">
  <div class="card">
    <h3>SD Card</h3>
    <div class="kv"><span class="k">Status</span><span class="v" id="st-ok">—</span></div>
    <div class="kv"><span class="k">Total</span><span class="v" id="st-total">—</span></div>
    <div class="kv"><span class="k">Used</span><span class="v" id="st-used">—</span></div>
    <div class="kv"><span class="k">Free</span><span class="v ok" id="st-free">—</span></div>
    <div class="kv"><span class="k">Used %</span>
      <span class="v">
        <span id="st-pct">—</span>
        <div class="progbar" style="width:120px;display:inline-block;vertical-align:middle;margin-left:8px">
          <div class="progbar-fill" id="st-pctbar" style="width:0%"></div>
        </div>
      </span>
    </div>
  </div>
  <div class="card">
    <h3>Log Statistics</h3>
    <div class="kv"><span class="k">Log Files</span><span class="v" id="st-files">—</span></div>
    <div class="kv"><span class="k">Total Records</span><span class="v" id="st-lines">—</span></div>
    <div class="kv"><span class="k">Log Size</span><span class="v" id="st-logmb">—</span></div>
    <div class="kv"><span class="k">Oldest Log</span><span class="v" id="st-old">—</span></div>
    <div class="kv"><span class="k">Newest Log</span><span class="v" id="st-new">—</span></div>
  </div>
</div>

<div id="p-net" class="page">
  <div class="card">
    <h3>WiFi</h3>
    <div class="kv"><span class="k">SSID</span><span class="v" id="n-wssid">—</span></div>
    <div class="kv"><span class="k">RSSI</span><span class="v" id="n-wrssi">—</span></div>
    <div class="kv"><span class="k">Signal</span><span class="v" id="n-wsig">—</span></div>
    <div class="kv"><span class="k">IP Address</span><span class="v" id="n-wip">—</span></div>
    <div class="kv"><span class="k">Gateway</span><span class="v" id="n-wgw">—</span></div>
    <div class="kv"><span class="k">DNS</span><span class="v" id="n-wdns">—</span></div>
  </div>
  <div class="card">
    <h3>SIM / Cellular</h3>
    <div class="kv"><span class="k">Operator</span><span class="v" id="n-sop">—</span></div>
    <div class="kv"><span class="k">Network Type</span><span class="v" id="n-stype">—</span></div>
    <div class="kv"><span class="k">RSSI</span><span class="v" id="n-srssi">—</span></div>
    <div class="kv"><span class="k">Signal</span><span class="v" id="n-ssig">—</span></div>
    <div class="kv"><span class="k">IMEI</span><span class="v" id="n-imei">—</span></div>
    <div class="kv"><span class="k">ICCID</span><span class="v" id="n-iccid">—</span></div>
    <div class="kv"><span class="k">IP</span><span class="v" id="n-sip">—</span></div>
    <div class="kv"><span class="k">Roaming</span><span class="v" id="n-roam">—</span></div>
    <div class="kv"><span class="k">Registration</span><span class="v" id="n-reg">—</span></div>
    <div class="kv"><span class="k">Balance</span><span class="v blue" id="n-bal">—</span></div>
    <div class="kv"><span class="k">Data Remaining</span><span class="v blue" id="n-data">—</span></div>
  </div>
  <div class="card">
    <h3>Connection</h3>
    <div class="kv"><span class="k">Active Network</span><span class="v" id="n-active">—</span></div>
    <div class="kv"><span class="k">Reconnects</span><span class="v" id="n-recon">—</span></div>
    <div class="kv"><span class="k">MQTT Status</span><span class="v" id="n-mqtt">—</span></div>
    <div class="kv"><span class="k">Connected For</span><span class="v" id="n-uptime">—</span></div>
  </div>
</div>

<div id="p-logs" class="page">
  <div class="card">
    <h3>Log Viewer</h3>
    <div class="row2">
      <div>
        <label>File</label>
        <select id="log-file" onchange="loadLogsPage()"></select>
      </div>
      <div>
        <label>Per page</label>
        <select id="log-size" onchange="loadLogsPage()">
          <option>20</option><option>50</option><option>100</option>
        </select>
      </div>
    </div>
    <label>Search</label>
    <input id="log-search" placeholder="substring search" oninput="debounceLog()">
    <label><input type="checkbox" id="log-newest" onchange="loadLogsPage()" checked> Newest first</label>
  </div>
  <div class="card">
    <h3 id="log-count-title">Records</h3>
    <div id="log-list"><div class="scanmsg">Select a log file</div></div>
    <div style="display:flex;gap:8px;margin-top:10px">
      <button class="btn btn-ghost" onclick="logPrev()">← Prev</button>
      <button class="btn btn-ghost" onclick="logNext()">Next →</button>
      <span id="log-page-info" style="font-size:11px;color:var(--dim);line-height:36px"></span>
    </div>
  </div>
  <div class="card" id="log-dev-card" style="display:none">
    <h3>Log Actions (Developer)</h3>
    <div style="display:flex;gap:8px;flex-wrap:wrap">
      <button class="btn btn-ghost" onclick="exportCSV()">⬇ CSV</button>
      <button class="btn btn-ghost" onclick="exportJSON()">⬇ JSON</button>
      <button class="btn btn-danger" onclick="deleteSelectedLog()">🗑 Del File</button>
      <button class="btn btn-danger" onclick="deleteAllLogs()">🗑 Del All</button>
    </div>
  </div>
</div>

<div id="p-cfg" class="page">
  <span class="mode-tag" id="cfg-mode-tag">User Mode</span>
  <div class="card">
    <h3>WiFi</h3>
    <div id="cfg-scanlist"><div class="scanmsg">Scanning…</div></div>
    <button class="btn btn-ghost full" onclick="doScan()" style="margin-top:8px">↺ Rescan</button>
    <label>SSID</label><input id="c-ssid">
    <label>Password</label><input id="c-wpass" type="text">
    <div class="scanmsg" id="cfg-ap-note" style="margin-top:8px"></div>
  </div>
  <div class="card" id="cfg-dev-fields" style="display:none">
    <h3>MQTT Broker</h3>
    <label>Broker Host</label><input id="c-broker">
    <div class="row2">
      <div><label>Port</label><input id="c-port" type="number"></div>
      <div><label>Publish Interval (ms)</label><input id="c-intv" type="number"></div>
    </div>
    <label>Topic</label><input id="c-topic">
    <label>Device ID</label><input id="c-devid">
  </div>
  <div class="card" id="cfg-dev-fields2" style="display:none">
    <h3>Cellular &amp; Sync</h3>
    <label>APN</label><input id="c-apn">
    <label>Hotspot Password (8+ chars)</label><input id="c-appass">
    <label>Batch Size (1–100)</label><input id="c-bsize" type="number" min="1" max="100">
    <label>Max SD Usage (%)</label><input id="c-maxstore" type="number" min="10" max="99">
    <div class="switchrow">
      <span class="k">Auto-delete oldest logs above limit</span>
      <div class="sw" id="sw-autodel-cfg" onclick="toggleAutoDelCfg()"></div>
    </div>
  </div>
  <div class="card" id="cfg-dev-fields3" style="display:none">
    <h3>Access PINs</h3>
    <label>Developer PIN (4 digits)</label><input id="c-devpin" maxlength="4" inputmode="numeric">
    <label>User PIN (4 digits)</label><input id="c-userpin" maxlength="4" inputmode="numeric">
  </div>
  <button class="btn btn-primary full" onclick="saveCfg()">💾 Save &amp; Restart</button>
</div>

<div id="p-dev" class="page">
  <span class="mode-tag">Developer Mode</span>
  <div class="card">
    <h3>Device Control</h3>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px">
      <button class="btn btn-ghost" onclick="devAction('/api/restart','Restart?')">↺ Restart</button>
      <button class="btn btn-danger" onclick="devAction('/api/factory_reset','FACTORY RESET — are you sure?')">⚠ Reset</button>
      <button class="btn btn-ok" onclick="syncNow()">⬆ Sync Now</button>
      <button class="btn btn-ghost" onclick="pauseSync()">⏸ Pause Sync</button>
      <button class="btn btn-ghost" onclick="resumeSync()">▶️ Resume Sync</button>
      <button class="btn btn-ghost" onclick="exportCSV()">⬇ Export CSV</button>
    </div>
  </div>
  <div class="card">
    <h3>SD Card</h3>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px">
      <button class="btn btn-danger" onclick="deleteAllLogs()">🗑 Delete All Logs</button>
      <button class="btn btn-danger" onclick="devAction('/api/format_sd','Format SD card?')">🗑 Format SD</button>
    </div>
  </div>
</div>

<div class="toast" id="toast"></div>

<script>
let token = '', mode = '', pin = '';
let histOffset = 0, logOffset = 0;
let histSearchTimer, logSearchTimer;
const $ = id => document.getElementById(id);
let statusTimer;
let autoDelOn = false;

/* ------------------------------------------------------------------ */
/* Navigation / role gating                                            */
/* ------------------------------------------------------------------ */

function showPage(id) {
  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('nav button').forEach(b => b.classList.remove('active'));
  $(id).classList.add('active');
  const nb = id.replace('p-', 'nb-');
  if ($(nb)) $(nb).classList.add('active');

  if (id === 'p-sync')  loadSyncStatus();
  if (id === 'p-store') loadStorage();
  if (id === 'p-net')   loadNetDiag();
  if (id === 'p-hist')  loadFileList('hist');
  if (id === 'p-logs')  loadFileList('log');
  if (id === 'p-play')  loadFileList('pb');
}

function applyRole() {
  const devOnly = mode === 'dev';
  $('nav').classList.add('unlocked');
  // (14) USER sessions only get tabs whose endpoints they are actually
  // allowed to call (Playback + History use /api/log_files + /api/log_page,
  // Config uses /api/config + /api/save + /api/scan). Live/Sync/Storage/
  // Network sit on DEV-only routes and are hidden for users — no more 403
  // spam and no dead pages.
  ['nb-live', 'nb-sync', 'nb-store', 'nb-net', 'nb-logs', 'nb-dev'].forEach(id =>
    $(id).style.display = devOnly ? '' : 'none');
  // Dev-only cards inside shared pages.
  $('sync-dev-card').style.display   = devOnly ? '' : 'none';
  $('log-dev-card').style.display    = devOnly ? '' : 'none';
  $('pb-dev-card').style.display     = devOnly ? '' : 'none';
  $('cfg-dev-fields').style.display  = devOnly ? '' : 'none';
  $('cfg-dev-fields2').style.display = devOnly ? '' : 'none';
  $('cfg-dev-fields3').style.display = devOnly ? '' : 'none';
  $('cfg-mode-tag').textContent = devOnly ? 'Developer Mode' : 'User Mode';
}

function lockNow() {
  token = ''; mode = ''; pin = '';
  clearInterval(statusTimer);
  stopPlayback();
  $('nav').classList.remove('unlocked');
  document.querySelectorAll('nav button').forEach(b => b.classList.remove('active'));
  pinDots();
  showPageRaw('p-lock');
}

function showPageRaw(id) {
  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
  $(id).classList.add('active');
}

function toast(msg, err) {
  const t = $('toast');
  t.textContent = msg;
  t.className = 'toast show' + (err ? ' err' : '');
  setTimeout(() => t.className = 'toast', 2800);
}

/* ------------------------------------------------------------------ */
/* PIN pad                                                             */
/* ------------------------------------------------------------------ */

function pinDots() {
  document.querySelectorAll('.pd').forEach((d, i) => d.classList.toggle('on', i < pin.length));
}
document.querySelectorAll('.key').forEach(k => {
  k.addEventListener('click', () => {
    if (k.id === 'k-clr') { pin = ''; pinDots(); return; }
    if (k.id === 'k-del') { pin = pin.slice(0, -1); pinDots(); return; }
    if (pin.length < 4) {
      pin += k.textContent;
      pinDots();
      if (pin.length === 4) doUnlock();
    }
  });
});

// Every fetch below attaches the session token — the backend independently
// re-validates it (and role, for dev-only routes) on every single request.
async function doUnlock() {
  try {
    const r = await fetch('/api/unlock', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'pin=' + encodeURIComponent(pin)
    });
    const j = await r.json();
    if (!j.ok) {
      $('lockmsg').textContent = j.locked
        ? ('Locked — wait ' + j.waitSec + 's')
        : 'Wrong PIN';
      pin = ''; pinDots();
      setTimeout(() => $('lockmsg').textContent = '', 2200);
      return;
    }
    token = j.token; mode = j.mode;
    pin = ''; pinDots();
    applyRole();
    await loadConfig();
    // (14) Land users on a page their role can actually use.
    showPage(mode === 'dev' ? 'p-live' : 'p-hist');
    startStatusPolling();
  } catch (e) { toast('Device not responding', 1); }
}

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

async function loadConfig() {
  const r = await fetch('/api/config?token=' + token);
  if (r.status === 401) { toast('Session expired', 1); return; }
  const j = await r.json();
  $('hdr-dev').textContent = 'ID ' + (j.deviceId || '—');
  $('c-ssid').value  = j.wifiSsid || '';
  $('c-wpass').value = j.wifiPass || '';
  // [K] The device's own hotspot never goes away — say so where the user
  // is editing WiFi, so they always know the fallback path in.
  $('cfg-ap-note').textContent = 'Hotspot BB-TRACKER-' + (j.deviceId || '') +
    ' → http://192.168.4.1 stays ON even while the device is online.';
  if (mode === 'dev') {
    $('c-broker').value   = j.broker || '';
    $('c-port').value     = j.port || 1883;
    $('c-topic').value    = j.topic || '';
    $('c-devid').value    = j.deviceId || '';
    $('c-apn').value      = j.apn || '';
    $('c-intv').value     = j.intervalMs || 5000;
    $('c-appass').value   = j.apPass || '';
    $('c-bsize').value    = j.batchSize || 30;
    $('c-maxstore').value = j.maxStorage || 80;
    $('c-devpin').value   = j.devPin || '';
    $('c-userpin').value  = j.userPin || '';
    $('s-bsize').value    = j.batchSize || 30;
    $('pb-maxstore').value = j.maxStorage || 80;
    autoDelOn = !!j.autoDelete;
    syncAutoDelSwitches();
  }
  doScan();
}

function syncAutoDelSwitches() {
  $('sw-autodel').classList.toggle('on', autoDelOn);
  $('sw-autodel-cfg').classList.toggle('on', autoDelOn);
}
function toggleAutoDel()    { autoDelOn = !autoDelOn; syncAutoDelSwitches(); }
function toggleAutoDelCfg() { autoDelOn = !autoDelOn; syncAutoDelSwitches(); }

async function saveAutoDel() {
  const body = {
    autoDelete: autoDelOn ? 1 : 0,
    maxStorage: $('pb-maxstore').value
  };
  const r = await post('/api/save?token=' + token, body);
  if (r.status === 401) { toast('Developer PIN required', 1); return; }
  const j = await r.json();
  j.ok ? toast('Auto-delete settings saved') : toast(j.msg || 'Save failed', 1);
}

async function doScan() {
  $('cfg-scanlist').innerHTML = '<div class="scanmsg">Scanning…</div>';
  try {
    const r = await fetch('/api/scan?token=' + token);
    const j = await r.json();
    if (!j.networks || !j.networks.length) {
      $('cfg-scanlist').innerHTML = '<div class="scanmsg">None found — rescan</div>'; return;
    }
    $('cfg-scanlist').innerHTML = '';
    j.networks.forEach(n => {
      const d = document.createElement('div');
      d.className = 'net';
      d.innerHTML = '<span class="nm">' + n.ssid.replace(/</g,'&lt;') + '</span>' +
        '<span class="meta">' + (n.open?'OPEN':'🔒') + ' ' + n.rssi + 'dBm</span>';
      d.onclick = () => {
        document.querySelectorAll('#cfg-scanlist .net').forEach(x => x.classList.remove('sel'));
        d.classList.add('sel');
        $('c-ssid').value = n.ssid;
        $('c-wpass').focus();
      };
      $('cfg-scanlist').appendChild(d);
    });
  } catch (e) { $('cfg-scanlist').innerHTML = '<div class="scanmsg">Scan failed</div>'; }
}

async function saveCfg() {
  const body = { wifiSsid: $('c-ssid').value, wifiPass: $('c-wpass').value }; 
  if (mode === 'dev') {
    Object.assign(body, {
      broker: $('c-broker').value.trim(), port: $('c-port').value,
      topic: $('c-topic').value.trim(), deviceId: $('c-devid').value.trim(),
      apn: $('c-apn').value.trim(), intervalMs: $('c-intv').value,
      apPass: $('c-appass').value, batchSize: $('c-bsize').value,
      maxStorage: $('c-maxstore').value,
      autoDelete: autoDelOn ? 1 : 0,
      devPin: $('c-devpin').value.trim(), userPin: $('c-userpin').value.trim()
    });
  }
  const r = await post('/api/save?token=' + token, body);
  if (r.status === 401) { toast('Unauthorized', 1); return; }
  const j = await r.json();
  j.ok ? toast('Saved — restarting…') : toast(j.msg || 'Save failed', 1);
}

/* ------------------------------------------------------------------ */
/* Live status                                                         */
/* ------------------------------------------------------------------ */

function startStatusPolling() {
  clearInterval(statusTimer);
  // (14) DEV polls the full /api/status; USER polls the shared /api/basic
  // so the header (battery / net) works without touching dev routes.
  const poll = (mode === 'dev') ? loadStatus : loadBasic;
  statusTimer = setInterval(poll, 3000);
  poll();
}

// (14) Lightweight shared status — available to BOTH roles.
async function loadBasic() {
  try {
    const r = await fetch('/api/basic?token=' + token);
    if (r.status === 401) return;
    const j = await r.json();
    $('hdr-bat').textContent = '🔋 ' + j.batPct + '%' + (j.charging ? ' ⚡' : '');
    $('hdr-net').textContent = j.net;
  } catch (e) { /* silent */ }
}

// Session trail: every live fix is remembered so it can be replayed
// from Playback → "Live session" without touching the SD card.
let liveTrail = [];

async function loadStatus() {
  try {
    const r = await fetch('/api/status?token=' + token);
    if (r.status === 401) return;
    const j = await r.json();
    $('hdr-bat').textContent = '🔋 ' + j.batPct + '%';
    $('hdr-net').textContent = j.net;
    $('l-fix').textContent  = j.fix ? '✅ Fixed' : '❌ No Fix';
    $('l-fix').className    = 'v ' + (j.fix ? 'ok' : 'err');
    $('l-lat').textContent  = j.fix ? j.lat.toFixed(7) : '—';
    $('l-lng').textContent  = j.fix ? j.lng.toFixed(7) : '—';
    $('l-alt').textContent  = j.alt.toFixed(1) + ' m';
    $('l-spd').textContent  = j.spd.toFixed(1) + ' km/h';
    $('l-hdg').textContent  = j.hdg.toFixed(1) + '°';
    $('l-sats').textContent = j.sats;
    $('l-hdop').textContent = j.hdop.toFixed(2);
    $('l-batv').textContent = j.batV + ' V';
    $('l-batp').textContent = j.batPct + '%';
    buildBattBar(j.batPct);
    updateSyncUI(j.sync);
    if (j.fix) {
      updateMap(j.lat, j.lng);
      const last = liveTrail[liveTrail.length - 1];
      if (!last || last.lat !== j.lat || last.lng !== j.lng) {
        liveTrail.push({ ts: new Date().toISOString().replace('T',' ').slice(0,19),
                         lat: j.lat, lng: j.lng, spd: j.spd, alt: j.alt, hdg: j.hdg,
                         sats: j.sats, bat: j.batPct });
        if (liveTrail.length > 5000) liveTrail.shift();
      }
    }
  } catch (e) { /* silent */ }
}

const SYNC_LABELS = ['Idle','Preparing','Uploading','Waiting ACK','Retrying','Completed','Failed','Paused'];
const SYNC_COLORS = ['var(--dim)','var(--warn)','var(--blue)','var(--warn)','var(--warn)','var(--ok)','var(--err)','var(--faint)'];

function updateSyncUI(s) {
  if (!s) return;
  const chip = $('s-status');
  chip.textContent = SYNC_LABELS[s.status] || '—';
  chip.style.color = SYNC_COLORS[s.status] || 'var(--dim)';
  $('s-pending').textContent = s.pending + ' records';
  $('s-synced').textContent  = s.synced  + ' records';
  $('s-batch').textContent   = s.batch   + ' records/batch';
  $('s-speed').textContent   = s.speed.toFixed(1)  + ' rec/s';
  $('s-est').textContent     = s.estSec  > 0 ? fmtDuration(s.estSec) : '—';
  $('s-ok').textContent      = s.lastOk  > 0 ? fmtAge(s.lastOk)   : 'Never';
  $('s-fail').textContent    = s.lastFail > 0 ? fmtAge(s.lastFail) : 'Never';
  $('s-err').textContent     = s.lastError || '—';
}

async function loadSyncStatus() {
  const r = await fetch('/api/status?token=' + token);
  if (r.status === 401) return;
  const j = await r.json();
  updateSyncUI(j.sync);
}

/* ------------------------------------------------------------------ */
/* Storage / Network pages                                             */
/* ------------------------------------------------------------------ */

async function loadStorage() {
  try {
    const r = await fetch('/api/storage?token=' + token);
    if (r.status === 401) return;
    const j = await r.json();
    $('st-ok').textContent    = j.ok ? '✅ Mounted' : '❌ Failed';
    $('st-ok').className      = 'v ' + (j.ok ? 'ok' : 'err');
    $('st-total').textContent = j.totalMB + ' MB';
    $('st-used').textContent  = j.usedMB  + ' MB';
    $('st-free').textContent  = j.freeMB  + ' MB';
    $('st-pct').textContent   = j.usedPct + '%';
    $('st-pctbar').style.width = j.usedPct + '%';
    $('st-pctbar').className  = 'progbar-fill' + (j.usedPct > 80 ? ' err' : j.usedPct > 60 ? '' : ' ok');
    $('st-files').textContent = j.logFiles;
    $('st-lines').textContent = j.logLines;
    $('st-logmb').textContent = j.logMB + ' MB';
    $('st-old').textContent   = j.oldest || '—';
    $('st-new').textContent   = j.newest || '—';
  } catch (e) { toast('Storage read failed', 1); }
}

async function loadNetDiag() {
  try {
    const r = await fetch('/api/netdiag?token=' + token);
    if (r.status === 401) return;
    const j = await r.json();
    $('n-wssid').textContent = j.wifi.ssid  || '—';
    $('n-wrssi').textContent = j.wifi.rssi  + ' dBm';
    $('n-wsig').textContent  = j.wifi.signal + '%';
    $('n-wip').textContent   = j.wifi.ip    || '—';
    $('n-wgw').textContent   = j.wifi.gateway || '—';
    $('n-wdns').textContent  = j.wifi.dns   || '—';
    $('n-sop').textContent   = j.sim.operator_  || '—';
    $('n-stype').textContent = j.sim.networkType || '—';
    $('n-srssi').textContent = j.sim.rssi   + ' dBm';
    $('n-ssig').textContent  = j.sim.signal + '%';
    $('n-imei').textContent  = j.sim.imei   || '—';
    $('n-iccid').textContent = j.sim.iccid  || '—';
    $('n-sip').textContent   = j.sim.ip     || '—';
    $('n-roam').textContent  = j.sim.roaming ? 'Yes' : 'No';
    $('n-reg').textContent   = j.sim.regStatus || '—';
    $('n-bal').textContent   = j.sim.balance  || 'Not Supported by Carrier';
    $('n-data').textContent  = j.sim.dataRemain || 'Not Supported by Carrier';
    $('n-active').textContent = j.wifi.ip ? 'WiFi' : 'SIM';
    $('n-recon').textContent  = j.reconnects;
    $('n-mqtt').textContent   = j.mqttStatus;
    $('n-uptime').textContent = fmtDuration(j.connectedMs / 1000);
  } catch (e) { toast('Network diag failed', 1); }
}

/* ------------------------------------------------------------------ */
/* File list (shared by History / Logs / Playback)                     */
/* ------------------------------------------------------------------ */

async function loadFileList(prefix) {
  try {
    const r = await fetch('/api/log_files?token=' + token);
    if (r.status === 401) return;
    const j = await r.json();
    const sel = $(prefix + '-file');
    sel.innerHTML = '';
    if (prefix === 'pb') {
      const live = document.createElement('option');
      live.value = '__live__';
      live.textContent = '⚡ Live session (' + liveTrail.length + ' pts)';
      sel.appendChild(live);
    }
    (j.files || []).forEach(f => {
      const opt = document.createElement('option');
      opt.value = f.file; opt.textContent = f.name + ' (' + f.records + ' rec)';
      sel.appendChild(opt);
    });
    if (prefix === 'hist')     { histOffset = 0; loadHistPage(); }
    else if (prefix === 'log') { logOffset  = 0; loadLogsPage(); }
  } catch (e) {}
}

function debounceHist() { clearTimeout(histSearchTimer); histSearchTimer = setTimeout(() => { histOffset = 0; loadHistPage(); }, 350); }
function debounceLog()  { clearTimeout(logSearchTimer);  logSearchTimer  = setTimeout(() => { logOffset  = 0; loadLogsPage();  }, 350); }

async function loadHistPage() {
  const file = $('hist-file').value;
  const size = parseInt($('hist-size').value);
  const newest = $('hist-newest').checked ? 1 : 0;
  const search = encodeURIComponent($('hist-search').value || '');
  if (!file || !token) return;
  const url = `/api/log_page?token=${token}&file=${encodeURIComponent(file)}&offset=${histOffset}&size=${size}&newest=${newest}&search=${search}`;
  try {
    const r = await fetch(url);
    if (r.status === 401) return;
    const records = await r.json();
    const el = $('hist-list');
    el.innerHTML = '';
    if (!records.length) { el.innerHTML = '<div class="scanmsg">No records</div>'; $('hist-count').textContent=''; return; }
    records.forEach(rec => {
      const d = document.createElement('div');
      d.className = 'log-entry';
      d.innerHTML = `<div class="ts">${rec.ts || '—'}</div>` +
        `<div>${(rec.lat||0).toFixed(6)}, ${(rec.lng||0).toFixed(6)} &nbsp; ` +
        `${(rec.spd||0).toFixed(1)} km/h &nbsp; alt ${(rec.alt||0).toFixed(1)}m &nbsp; hdg ${(rec.hdg||0).toFixed(0)}° &nbsp; ` +
        `🛰 ${rec.sats||0} &nbsp; hdop ${(rec.hdop||0).toFixed(1)} &nbsp; 🔋${rec.bat||0}% &nbsp; ${rec.net||''}</div>`;
      el.appendChild(d);
    });
    $('hist-count').textContent = '(' + records.length + ' shown)';
    $('hist-page-info').textContent = `Offset ${histOffset}`;
  } catch (e) { $('hist-list').innerHTML = '<div class="scanmsg">Load failed</div>'; }
}

function histPrev() { histOffset = Math.max(0, histOffset - parseInt($('hist-size').value)); loadHistPage(); }
function histNext() { histOffset += parseInt($('hist-size').value); loadHistPage(); }

async function loadLogsPage() {
  const file   = $('log-file').value;
  const size   = parseInt($('log-size').value);
  const newest = $('log-newest').checked ? 1 : 0;
  const search = encodeURIComponent($('log-search').value || '');
  if (!file || !token) return;
  const url = `/api/log_page?token=${token}&file=${encodeURIComponent(file)}&offset=${logOffset}&size=${size}&newest=${newest}&search=${search}`;
  try {
    const r = await fetch(url);
    if (r.status === 401) return;
    const records = await r.json();
    const el = $('log-list');
    el.innerHTML = '';
    records.forEach(rec => {
      const d = document.createElement('div');
      d.className = 'log-entry';
      d.innerHTML = `<div class="ts">${rec.ts || '—'}</div>` +
        `<div>${JSON.stringify(rec)}</div>`;
      el.appendChild(d);
    });
    $('log-count-title').textContent = 'Records (' + records.length + ' shown)';
    $('log-page-info').textContent = `Offset ${logOffset}`;
  } catch (e) {}
}
function logPrev() { logOffset = Math.max(0, logOffset - parseInt($('log-size').value)); loadLogsPage(); }
function logNext() { logOffset += parseInt($('log-size').value); loadLogsPage(); }

/* ------------------------------------------------------------------ */
/* Playback engine                                                     */
/* ------------------------------------------------------------------ */

let pbPoints = [];          // loaded track points, oldest → newest
let pbIndex = 0;
let pbPlaying = false;
let pbTimer = null;
let pbMap = null, pbMarker = null, pbLine = null, pbDoneLine = null;
let leafletReady = false;

function ensureLeaflet(cb) {
  if (window.L) { leafletReady = true; cb(); return; }
  if (leafletReady) { cb(); return; }
  const link = document.createElement('link');
  link.rel = 'stylesheet';
  link.href = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.css';
  document.head.appendChild(link);
  const scr = document.createElement('script');
  scr.src = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.js';
  scr.onload = () => { leafletReady = true; cb(); };
  scr.onerror = () => toast('Map library failed — internet needed', 1);
  document.head.appendChild(scr);
}

async function loadTrack() {
  const file = $('pb-file').value;
  if (!file) { toast('No track selected', 1); return; }
  stopPlayback();
  $('pb-loadmsg').textContent = 'Loading…';

  if (file === '__live__') {
    pbPoints = liveTrail.slice();
    finishTrackLoad();
    return;
  }

  // Page through the log via the existing /api/log_page endpoint —
  // no new firmware route needed. Oldest first (newest=0).
  const pageSize = 100;
  let offset = 0;
  const all = [];
  try {
    while (true) {
      const url = `/api/log_page?token=${token}&file=${encodeURIComponent(file)}&offset=${offset}&size=${pageSize}&newest=0&search=`;
      const r = await fetch(url);
      if (r.status === 401) { toast('Session expired', 1); return; }
      const recs = await r.json();
      if (!recs.length) break;
      recs.forEach(rec => {
        if (typeof rec.lat === 'number' && typeof rec.lng === 'number' &&
            (rec.lat !== 0 || rec.lng !== 0)) all.push(rec);
      });
      $('pb-loadmsg').textContent = 'Loading… ' + all.length + ' points';
      if (recs.length < pageSize) break;
      offset += pageSize;
      if (offset > 20000) break; // hard safety cap for huge files
    }
  } catch (e) { $('pb-loadmsg').textContent = 'Load failed'; return; }
  pbPoints = all;
  finishTrackLoad();
}

function finishTrackLoad() {
  if (!pbPoints.length) {
    $('pb-loadmsg').textContent = 'No GPS points in this track';
    return;
  }
  $('pb-loadmsg').textContent = pbPoints.length + ' points loaded';
  $('pb-scrub').max = pbPoints.length - 1;
  $('pb-scrub').value = 0;
  pbIndex = 0;

  // Track statistics
  let dist = 0, maxSpd = 0;
  for (let i = 1; i < pbPoints.length; i++) {
    dist += haversine(pbPoints[i-1].lat, pbPoints[i-1].lng, pbPoints[i].lat, pbPoints[i].lng);
    if ((pbPoints[i].spd || 0) > maxSpd) maxSpd = pbPoints[i].spd;
  }
  $('pb-dist').textContent = dist >= 1000 ? (dist/1000).toFixed(2) + ' km' : Math.round(dist) + ' m';
  $('pb-max').textContent  = maxSpd.toFixed(1) + ' km/h';
  const t0 = parseTs(pbPoints[0].ts), t1 = parseTs(pbPoints[pbPoints.length-1].ts);
  $('pb-dur').textContent  = (t0 && t1) ? fmtDuration((t1 - t0) / 1000) : '—';

  ensureLeaflet(() => {
    const el = $('pbmap');
    el.innerHTML = '';
    if (pbMap) { pbMap.remove(); pbMap = null; }
    const latlngs = pbPoints.map(p => [p.lat, p.lng]);
    pbMap = L.map('pbmap');
    L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png').addTo(pbMap);
    pbLine = L.polyline(latlngs, { color: '#5b6273', weight: 3, opacity: .6 }).addTo(pbMap);
    pbDoneLine = L.polyline([latlngs[0]], { color: '#ff5a2c', weight: 4 }).addTo(pbMap);
    pbMarker = L.circleMarker(latlngs[0], {
      radius: 8, color: '#ff5a2c', fillColor: '#ff5a2c', fillOpacity: 1
    }).addTo(pbMap);
    pbMap.fitBounds(pbLine.getBounds(), { padding: [24, 24] });
    renderPbFrame();
  });
}

function renderPbFrame() {
  const p = pbPoints[pbIndex];
  if (!p) return;
  $('pb-time').textContent  = p.ts || '—';
  $('pb-point').textContent = (pbIndex + 1) + ' / ' + pbPoints.length;
  $('pb-pos').textContent   = p.lat.toFixed(6) + ', ' + p.lng.toFixed(6);
  $('pb-spdhere').textContent = (p.spd || 0).toFixed(1) + ' km/h';
  $('pb-scrub').value = pbIndex;
  if (pbMarker) {
    pbMarker.setLatLng([p.lat, p.lng]);
    pbDoneLine.setLatLngs(pbPoints.slice(0, pbIndex + 1).map(x => [x.lat, x.lng]));
    pbMap.panTo([p.lat, p.lng], { animate: true, duration: .25 });
  }
}

function togglePlay() {
  if (!pbPoints.length) { toast('Load a track first', 1); return; }
  pbPlaying ? stopPlayback() : startPlayback();
}

function startPlayback() {
  pbPlaying = true;
  $('pb-playbtn').textContent = '⏸ Pause';
  scheduleNextFrame();
}

function stopPlayback() {
  pbPlaying = false;
  clearTimeout(pbTimer);
  const b = $('pb-playbtn');
  if (b) b.textContent = '▶ Play';
}

function scheduleNextFrame() {
  if (!pbPlaying) return;
  if (pbIndex >= pbPoints.length - 1) { stopPlayback(); return; }
  const speed = parseInt($('pb-speed').value);
  const tNow  = parseTs(pbPoints[pbIndex].ts);
  const tNext = parseTs(pbPoints[pbIndex + 1].ts);
  // Real gap between fixes divided by playback speed; clamped to keep it watchable.
  let delay = (tNow && tNext) ? (tNext - tNow) / speed : 400 / speed;
  delay = Math.min(Math.max(delay, 40), 2000);
  pbTimer = setTimeout(() => {
    pbIndex++;
    renderPbFrame();
    scheduleNextFrame();
  }, delay);
}

function setPlaySpeed() { /* picked up automatically on the next frame */ }

function scrubTo(v) {
  pbIndex = parseInt(v);
  renderPbFrame();
}

function parseTs(ts) {
  if (!ts) return 0;
  const t = Date.parse(ts.replace(' ', 'T'));
  return isNaN(t) ? 0 : t;
}

function haversine(la1, lo1, la2, lo2) {
  const R = 6371000, rad = Math.PI / 180;
  const dLa = (la2 - la1) * rad, dLo = (lo2 - lo1) * rad;
  const a = Math.sin(dLa/2)**2 + Math.cos(la1*rad) * Math.cos(la2*rad) * Math.sin(dLo/2)**2;
  return 2 * R * Math.asin(Math.sqrt(a));
}

async function deletePlaybackFile() {
  const file = $('pb-file').value;
  if (!file || file === '__live__') { toast('Select an SD track', 1); return; }
  if (!confirm('Delete ' + file + ' from the SD card?')) return;
  const r = await fetch('/api/delete_log?token=' + token + '&file=' + encodeURIComponent(file), { method: 'DELETE' });
  if (r.status === 401) { toast('Developer PIN required', 1); return; }
  const j = await r.json();
  toast(j.ok ? 'Track deleted' : 'Failed', !j.ok);
  loadFileList('pb');
}

/* ------------------------------------------------------------------ */
/* Dev actions                                                         */
/* ------------------------------------------------------------------ */

async function devAction(endpoint, confirm_msg) {
  if (!confirm(confirm_msg)) return;
  const r = await post(endpoint + '?token=' + token, {});
  if (r.status === 401) { toast('Developer PIN required', 1); return; }
  const j = await r.json();
  toast(j.ok ? 'Done' : 'Failed', !j.ok);
}

async function syncNow()    { const r = await post('/api/sync?token='        + token, {}); toast(r.status===401?'Developer PIN required':'Sync triggered', r.status===401); }
async function pauseSync()  { const r = await post('/api/pause_sync?token='  + token, {}); toast(r.status===401?'Developer PIN required':'Sync paused', r.status===401); }
async function resumeSync() { const r = await post('/api/resume_sync?token=' + token, {}); toast(r.status===401?'Developer PIN required':'Sync resumed', r.status===401); }

async function deleteAllLogs() {
  if (!confirm('Delete ALL logs?')) return;
  const r = await fetch('/api/delete_all_logs?token=' + token, { method: 'DELETE' });
  if (r.status === 401) { toast('Developer PIN required', 1); return; }
  const j = await r.json();
  toast(j.ok ? 'Deleted' : 'Failed', !j.ok);
  loadStorage();
}

async function deleteSelectedLog() {
  const file = $('log-file').value;
  if (!file || !confirm('Delete ' + file + '?')) return;
  const r = await fetch('/api/delete_log?token=' + token + '&file=' + encodeURIComponent(file), { method: 'DELETE' });
  if (r.status === 401) { toast('Developer PIN required', 1); return; }
  const j = await r.json();
  toast(j.ok ? 'Deleted' : 'Failed', !j.ok);
  loadFileList('log');
}

function exportCSV()  { window.open('/api/download_csv?token='  + token + '&file=' + encodeURIComponent($('log-file') ? $('log-file').value : '')); }
function exportJSON() { window.open('/api/download_json?token=' + token + '&file=' + encodeURIComponent($('log-file') ? $('log-file').value : '')); }

async function saveBatchSize() {
  const sz = $('s-bsize').value;
  const r = await post('/api/set_batch_size?token=' + token, { size: sz });
  if (r.status === 401) { toast('Developer PIN required', 1); return; }
  const j = await r.json();
  toast(j.ok ? 'Batch size: ' + j.batchSize : 'Failed', !j.ok);
}

/* ------------------------------------------------------------------ */
/* Live map                                                            */
/* ------------------------------------------------------------------ */

let mapLoaded = false, leafletMap, leafletMarker, liveLine;
function updateMap(lat, lng) {
  if (!mapLoaded) {
    mapLoaded = true;
    ensureLeaflet(() => {
      const el = $('map');
      el.innerHTML = '';
      leafletMap = L.map('map').setView([lat, lng], 15);
      L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png').addTo(leafletMap);
      leafletMarker = L.marker([lat, lng]).addTo(leafletMap);
      liveLine = L.polyline(liveTrail.map(p => [p.lat, p.lng]), { color: '#ff5a2c', weight: 3 }).addTo(leafletMap);
    });
  } else if (leafletMap && leafletMarker) {
    leafletMarker.setLatLng([lat, lng]);
    leafletMap.setView([lat, lng]);
    if (liveLine) liveLine.setLatLngs(liveTrail.map(p => [p.lat, p.lng]));
  }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

function post(url, obj) {
  return fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: Object.entries(obj).map(([k, v]) => k + '=' + encodeURIComponent(v)).join('&')
  });
}

function buildBattBar(pct) {
  const segs = 10;
  let h = '';
  const filled = Math.round(pct / 10);
  for (let i = 0; i < segs; i++) {
    const cls = i < filled
      ? (pct > 50 ? 'batt-seg on' : pct > 20 ? 'batt-seg warn' : 'batt-seg err')
      : 'batt-seg';
    h += `<div class="${cls}" style="height:${8+i}px"></div>`;
  }
  $('l-batbar').innerHTML = h;
}

function fmtDuration(secs) {
  if (secs < 60)  return Math.floor(secs) + 's';
  if (secs < 3600) return Math.floor(secs/60) + 'm ' + (Math.floor(secs)%60) + 's';
  return Math.floor(secs/3600) + 'h ' + Math.floor((secs%3600)/60) + 'm';
}

function fmtAge(ms) {
  const secs = (Date.now() - ms) / 1000;
  return fmtDuration(secs) + ' ago';
}
</script>
</body>
</html>

)HTMLEOF";