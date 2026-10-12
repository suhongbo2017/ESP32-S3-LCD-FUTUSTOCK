/* 自动生成/维护：Web 控制台首页（纯内嵌，无外部依赖，体积小） */
#pragma once

static const char WEB_CONSOLE_INDEX_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 网关控制台</title>
<style>
  :root{--bg:#0f1419;--card:#1a2029;--fg:#e6edf3;--mut:#8b949e;--acc:#1c7ed6;--ok:#2f9e44;--warn:#e8590c;--err:#e03131}
  *{box-sizing:border-box}
  body{margin:0;font:15px/1.5 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif;background:var(--bg);color:var(--fg);padding:16px}
  .wrap{max-width:760px;margin:0 auto}
  h1{font-size:20px;margin:0 0 4px}
  .sub{color:var(--mut);font-size:13px;margin-bottom:16px}
  .card{background:var(--card);border:1px solid #263041;border-radius:12px;padding:14px 16px;margin-bottom:14px}
  .card h2{font-size:15px;margin:0 0 10px;color:var(--acc);font-weight:600}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}
  .kv{background:#131922;border-radius:8px;padding:10px 12px}
  .kv .k{color:var(--mut);font-size:12px}
  .kv .v{font-size:16px;font-weight:600;font-family:Consolas,monospace;word-break:break-all}
  .ok{color:var(--ok)} .bad{color:var(--err)} .warn{color:var(--warn)}
  .up{color:#ff5252;font-weight:600} .down{color:#69f0ae;font-weight:600}
  button{background:var(--acc);color:#fff;border:0;border-radius:8px;padding:9px 14px;font-size:14px;cursor:pointer}
  button.sec{background:#2b3648}
  button:hover{filter:brightness(1.1)}
  select,input{background:#131922;color:var(--fg);border:1px solid #2b3648;border-radius:8px;padding:8px 10px;font-size:14px}
  .row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:8px}
  .msg{font-size:13px;color:var(--mut);min-height:18px}
  table{width:100%;border-collapse:collapse;font-size:13px}
  td,th{text-align:left;padding:6px 4px;border-bottom:1px solid #263041}
  a{color:var(--acc);text-decoration:none}
  code{background:#131922;padding:2px 6px;border-radius:6px;font-size:12px}
</style>
</head>
<body>
<div class="wrap">
  <h1>ESP32-S3 双网网关 · 控制台</h1>
  <div class="sub" id="host">—</div>

  <div class="card">
    <h2>访问鉴权 <span class="msg" id="amsg"></span></h2>
    <div class="row">
      <input id="token" type="password" placeholder="输入 Web Token" autocomplete="off" style="flex:1">
      <button onclick="saveToken()">保存 Token</button>
      <button class="sec" onclick="clearToken()">清除</button>
    </div>
    <div class="msg">Token 仅保存在当前浏览器，用于访问状态、日志和控制接口。</div>
  </div>

  <div class="card">
    <h2>运行状态 <span class="msg" id="ts"></span></h2>
    <div class="grid" id="status"><div class="kv"><div class="k">加载中…</div><div class="v">—</div></div></div>
  </div>

  <div class="card">
    <h2>行情看板 <span class="msg" id="mmsg"></span></h2>
    <div class="row">
      <button id="mrefresh" onclick="marketRefresh()">立即同步并刷新</button>
      <span class="msg" id="mmeta">等待行情数据</span>
    </div>
    <table id="market"><tbody><tr><td>加载中…</td></tr></tbody></table>
  </div>

  <div class="card">
    <h2>GPIO 控制 <span class="msg" id="gmsg"></span></h2>
    <div class="row">
      <span style="color:var(--mut)">引脚</span>
      <select id="pin">
        <option>1</option><option>2</option><option>3</option><option>4</option><option>5</option>
        <option>6</option><option>7</option><option>8</option><option>9</option>
        <option>39</option><option>40</option><option>41</option><option>42</option><option>47</option><option>48</option>
      </select>
      <button onclick="gpioRead()">读取</button>
      <button onclick="gpioSet(1)">置高</button>
      <button class="sec" onclick="gpioSet(0)">置低</button>
    </div>
    <div class="msg">可用引脚：GPIO1–9、39–42、47–48（其余为板载占用，已屏蔽）</div>
  </div>

  <div class="card">
    <h2>TF 卡日志</h2>
    <table id="logs"><tbody><tr><td>加载中…</td></tr></tbody></table>
  </div>

  <div class="card">
    <h2>温湿度曲线 <span class="msg" id="hmsg"></span></h2>
    <div class="row">
      <button onclick="drawChart()">刷新曲线</button>
      <span class="msg">已采样 <span id="hcnt">0</span> 条（约 15s/条，最多 240 条 ≈ 1 小时）</span>
    </div>
    <div id="chart"><div class="msg">加载中…</div></div>
  </div>

  <div class="card">
    <h2>参数配置 <span class="msg" id="cmsg"></span></h2>
    <div class="row"><span style="color:var(--mut);width:112px">WiFi SSID</span><input id="c_ssid" style="flex:1"></div>
    <div class="row"><span style="color:var(--mut);width:112px">WiFi 密码</span><input id="c_pass" type="password" placeholder="(不改请留空)" style="flex:1"></div>
    <div class="row"><span style="color:var(--mut);width:112px">MQTT 主机</span><input id="c_host" style="flex:1"></div>
    <div class="row"><span style="color:var(--mut);width:112px">MQTT 端口</span><input id="c_port" style="width:90px"></div>
    <div class="row"><span style="color:var(--mut);width:112px">通知主题</span><input id="c_tn" style="flex:1"></div>
    <div class="row"><span style="color:var(--mut);width:112px">传感器主题</span><input id="c_ts" style="flex:1"></div>
    <div class="row">
      <button onclick="cfgSave()">保存配置</button>
      <button onclick="saveAndReboot()">保存并重启设备</button>
      <button class="sec" onclick="cfgReset()">恢复默认</button>
      <button class="sec" onclick="reboot(false)">仅重启</button>
    </div>
    <div class="msg">保存写入 NVS，重启后生效</div>
  </div>

  <div class="card">
    <h2>OTA 固件升级 <span class="msg" id="omsg"></span></h2>
    <div class="row">
      <input type="file" id="fw" accept=".bin" style="flex:1;padding:6px">
      <button onclick="ota()">上传升级</button>
    </div>
    <div class="msg">选 <code>build/esp32s3_gateway.bin</code>；升级成功后自动重启</div>
  </div>

  <div class="sub">
    只读接口：<code>/api/status</code> <code>/api/logs</code> ｜ 行情控制：<code>POST /api/market/refresh</code> ｜ 其它控制接口需 Token
  </div>
</div>

<script>
const $ = id => document.getElementById(id);
/* 鉴权默认关闭（CONFIG_WEB_CONSOLE_TOKEN 为空）；若启用，可从本机 localStorage 自动带上 */
let token = localStorage.getItem('gw_token') || '';
$('token').value = token;
function saveToken(){
  token = $('token').value.trim();
  localStorage.setItem('gw_token', token);
  $('amsg').textContent = token ? 'Token 已保存' : 'Token 为空';
  refresh(); loadLogs(); drawChart();
}
function clearToken(){
  token = '';
  localStorage.removeItem('gw_token');
  $('token').value = '';
  $('amsg').textContent = 'Token 已清除';
}
function authHeaders(extra){
  const headers = new Headers(extra || {});
  if(token) headers.set('X-Auth-Token', token);
  return headers;
}
async function apiFetch(path, options){
  const opts = Object.assign({}, options || {}, {headers: authHeaders(options && options.headers)});
  const response = await fetch(path, opts);
  if(response.status === 401 || response.status === 403){
    $('amsg').textContent = 'Token 无效或未提供';
  }
  return response;
}

async function refresh(){
  try{
    const r = await apiFetch('/api/status');
    const s = await r.json();
    $('host').textContent = location.host + ' · ' + (s.mdns ? ('http://' + s.mdns + '.local') : '');
    const up = (s.uptime|0);
    const hh = String(Math.floor(up/3600)).padStart(2,'0');
    const mm = String(Math.floor(up%3600/60)).padStart(2,'0');
    const ss = String(up%60).padStart(2,'0');
    const sensor = s.sensor_valid && !s.sensor_stale
        ? s.sensor_temp.toFixed(1) + '℃ / ' + s.sensor_hum.toFixed(1) + '%'
        : '-- / --';
    const items = [
      ['上行链路', s.uplink, s.uplink && s.uplink !== 'None' ? 'ok' : 'warn'],
      ['IP 地址', s.ip || '-', ''],
      ['温湿度', sensor, s.sensor_stale ? 'warn' : 'ok'],
      ['MQTT', s.mqtt ? '已连接' : '未连接', s.mqtt ? 'ok' : 'bad'],
      ['TF 卡', s.tf_ready ? ('已挂载 · ' + s.tf_written + ' 条') : '未挂载', s.tf_ready ? 'ok' : 'warn'],
      ['运行时长', hh + ':' + mm + ':' + ss, ''],
    ];
    $('status').innerHTML = items.map(([k,v,c]) =>
      `<div class="kv"><div class="k">${k}</div><div class="v ${c}">${v}</div></div>`).join('');
    renderMarket(s.market);
    $('ts').textContent = new Date().toLocaleTimeString();
  }catch(e){ $('ts').textContent = '刷新失败: ' + e; }
}

async function marketRefresh(){
  const button = $('mrefresh');
  button.disabled = true;
  $('mmsg').textContent = '已请求后台刷新…';
  try{
    const r = await apiFetch('/api/market/refresh', {method:'POST'});
    const j = await r.json();
    $('mmsg').textContent = j.ok ? '刷新任务已开始' : ('失败: ' + (j.message || j.error || r.status));
  }catch(e){ $('mmsg').textContent = '失败: ' + e; }
  setTimeout(() => { button.disabled = false; }, 1500);
}

function renderMarket(m){
  const esc = value => String(value == null ? '-' : value)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;')
      .replace(/>/g, '&gt;').replace(/\"/g, '&quot;').replace(/'/g, '&#39;');
  if(!m){ $('market').innerHTML = '<tbody><tr><td>暂无行情状态</td></tr></tbody>'; return; }
  const items = m.items || [];
  const state = m.refreshing ? '刷新中…' : (m.ready ? ('已更新 ' + m.quote_ok + '/' + m.holdings) : (m.error || '等待数据'));
  $('mmeta').textContent = state;
  if(!items.length){
    $('market').innerHTML = '<tbody><tr><td>暂无持仓行情</td></tr></tbody>';
    return;
  }
  $('market').innerHTML = '<tbody><tr><th>代码</th><th>名称</th><th>现价</th><th>涨跌</th><th>涨跌幅</th></tr>' +
    items.map(x => {
      const valid = x.valid;
      const cls = valid ? (x.percent >= 0 ? 'up' : 'down') : 'warn';
      const price = valid ? Number(x.last).toFixed(2) : '--';
      const change = valid ? ((x.change >= 0 ? '+' : '') + Number(x.change).toFixed(2)) : '--';
      const percent = valid ? ((x.percent >= 0 ? '+' : '') + Number(x.percent).toFixed(2) + '%') : '--';
      return `<tr><td>${esc(x.symbol)}</td><td>${esc(x.name)}</td><td>${price}</td><td class="${cls}">${change}</td><td class="${cls}">${percent}</td></tr>`;
    }).join('') + '</tbody>';
}

async function downloadLog(encodedName){
  try{
    const r = await apiFetch('/api/logs/' + encodedName);
    if(!r.ok){ throw new Error('HTTP ' + r.status); }
    const blob = await r.blob();
    const link = document.createElement('a');
    link.href = URL.createObjectURL(blob);
    link.download = decodeURIComponent(encodedName);
    link.click();
    URL.revokeObjectURL(link.href);
  }catch(e){ $('amsg').textContent = '日志下载失败: ' + e; }
}

async function loadLogs(){
  try{
    const r = await apiFetch('/api/logs'); const j = await r.json();
    if(!j.files || !j.files.length){ $('logs').innerHTML = '<tbody><tr><td>（无文件）</td></tr></tbody>'; return; }
    $('logs').innerHTML = '<tbody><tr><th>文件</th><th>大小</th><th></th></tr>' +
      j.files.map(f => `<tr><td>${f.name}</td><td>${(f.size/1024).toFixed(1)} KB</td>
        <td><button class="sec" data-file="${encodeURIComponent(f.name)}" onclick="downloadLog(this.dataset.file)">下载</button></td></tr>`).join('') +
      '</tbody>';
  }catch(e){ $('logs').innerHTML = '<tbody><tr><td>获取失败</td></tr></tbody>'; }
}

async function gpioRead(){
  try{
    const r = await apiFetch('/api/gpio?pin=' + $('pin').value);
    const j = await r.json();
    $('gmsg').textContent = j.ok ? ('GPIO' + j.pin + ' 电平=' + j.level) : ('失败: ' + j.error);
  }catch(e){ $('gmsg').textContent = '失败: ' + e; }
}
async function gpioSet(v){
  try{
    const r = await apiFetch('/api/gpio?pin=' + $('pin').value + '&value=' + v, {method:'POST'});
    const j = await r.json();
    $('gmsg').textContent = j.ok ? ('GPIO' + j.pin + ' 已置' + (v ? '高' : '低')) : ('失败: ' + (j.error||r.status));
  }catch(e){ $('gmsg').textContent = '失败: ' + e; }
}

refresh(); loadLogs();
setInterval(refresh, 2000);

/* ---- 温湿度曲线（纯 SVG，无外部库） ---- */
async function drawChart(){
  try{
    const j = await (await apiFetch('/api/history')).json();
    const it = j.items || [];
    $('hcnt').textContent = j.count || 0;
    if (it.length < 2){
      $('chart').innerHTML = '<div class="msg">数据不足（等待传感器上报，约 15s 一条）</div>';
      $('hmsg').textContent = '';
      return;
    }
    const W = 700, H = 240, P = 42;
    const ts = it.map(x => x.t), temps = it.map(x => x.temp), hums = it.map(x => x.hum);
    const pad = (a, b) => a === b ? [a - 1, b + 1] : [a - (b-a)*0.1, b + (b-a)*0.1];
    const [t0, t1] = pad(Math.min(...temps), Math.max(...temps));
    const [h0, h1] = pad(Math.min(...hums),  Math.max(...hums));
    const N = it.length;
    const X  = i => P + (W - 2*P) * i / (N - 1);
    const Yt = v => P + (H - 2*P) * (1 - (v - t0) / (t1 - t0));
    const Yh = v => P + (H - 2*P) * (1 - (v - h0) / (h1 - h0));
    const path = (arr, Y) => arr.map((v, i) => (i ? 'L' : 'M') + X(i).toFixed(1) + ' ' + Y(v).toFixed(1)).join(' ');
    const fmt = t => j.wall_clock ? new Date(t * 1000).toLocaleTimeString() : ('+' + Math.round(t/60) + 'min');
    let grid = '';
    for (let k = 0; k <= 4; k++){
      const y = P + (H - 2*P) * k / 4;
      grid += `<line x1="${P}" y1="${y}" x2="${W-P}" y2="${y}" stroke="#263041" stroke-width="1"/>`;
      grid += `<text x="${P-6}" y="${y+4}" fill="#e8590c" font-size="11" text-anchor="end">${(t1-(t1-t0)*k/4).toFixed(1)}</text>`;
      grid += `<text x="${W-P+6}" y="${y+4}" fill="#4dabf7" font-size="11">${(h1-(h1-h0)*k/4).toFixed(1)}</text>`;
    }
    $('chart').innerHTML =
      `<svg viewBox="0 0 ${W} ${H}" style="width:100%;height:auto">${grid}
        <path d="${path(temps, Yt)}" fill="none" stroke="#e8590c" stroke-width="2"/>
        <path d="${path(hums,  Yh)}" fill="none" stroke="#4dabf7" stroke-width="2"/>
        <text x="${P}" y="${H-8}" fill="#8b949e" font-size="11">${fmt(ts[0])}</text>
        <text x="${W-P}" y="${H-8}" fill="#8b949e" font-size="11" text-anchor="end">${fmt(ts[N-1])}</text>
        <text x="${P+6}" y="${P-16}" fill="#e8590c" font-size="12">温度 ℃</text>
        <text x="${W-P-6}" y="${P-16}" fill="#4dabf7" font-size="12" text-anchor="end">湿度 %</text>
      </svg>`;
    $('hmsg').textContent = '温度 ' + temps[N-1].toFixed(1) + '℃ · 湿度 ' + hums[N-1].toFixed(1) + '%';
  }catch(e){ $('chart').innerHTML = '<div class="msg">获取失败: ' + e + '</div>'; }
}

/* ---- 参数配置 ---- */
async function cfgLoad(){
  try{
    const c = await (await apiFetch('/api/config')).json();
    $('c_ssid').value = c.wifi_ssid || '';
    $('c_host').value = c.mqtt_host || '';
    $('c_port').value = c.mqtt_port || '';
    $('c_tn').value   = c.topic_notify || '';
    $('c_ts').value   = c.topic_sensor || '';
    $('cmsg').textContent = c.from_nvs ? '（含 NVS 自定义值）' : '（当前为默认值）';
  }catch(e){}
}
async function cfgSave(){
  const p = new URLSearchParams();
  const put = (k,id) => { const v = $(id).value.trim(); if (v) p.set(k, v); };
  put('wifi_ssid','c_ssid'); put('wifi_pass','c_pass'); put('mqtt_host','c_host');
  put('mqtt_port','c_port'); put('topic_notify','c_tn'); put('topic_sensor','c_ts');
  try{
    const j = await (await apiFetch('/api/config', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:p.toString()
    })).json();
    $('cmsg').textContent = j.ok ? ('已保存 ' + j.applied + ' 项，重启生效') : ('失败: ' + (j.error || j.message));
  }catch(e){ $('cmsg').textContent = '失败: ' + e; }
}
async function cfgReset(){
  try{
    const j = await (await apiFetch('/api/config?action=reset', {method:'POST'})).json();
    $('cmsg').textContent = j.ok ? '已恢复默认，重启生效' : '失败';
  }catch(e){ $('cmsg').textContent = '失败: ' + e; }
}

/* ---- 重启 ---- */
async function reboot(skipConfirm){
  if (!skipConfirm && !confirm('确定重启设备？')) return;
  $('cmsg').textContent = '设备重启中…（约 20 秒后恢复）';
  try{ await apiFetch('/api/reboot', {method:'POST'}); }catch(e){ /* 重启会断开连接，属正常 */ }
}
async function saveAndReboot(){
  await cfgSave();
  setTimeout(() => reboot(true), 800);
}

/* ---- OTA ---- */
async function ota(){
  const f = $('fw').files[0];
  if (!f) { $('omsg').textContent = '请先选择固件文件'; return; }
  $('omsg').textContent = '上传中 ' + (f.size/1048576).toFixed(2) + ' MB …（约 1 分钟，请勿断电）';
  try{
    const j = await (await apiFetch('/api/ota', {method:'POST', body:f})).json();
    $('omsg').textContent = j.ok ? '升级成功，设备重启中…' : ('失败: ' + (j.error || j.message));
  }catch(e){ $('omsg').textContent = '连接中断（设备可能已升级并重启）'; }
}
cfgLoad(); drawChart();
setInterval(drawChart, 30000);
</script>
</body>
</html>)HTML";