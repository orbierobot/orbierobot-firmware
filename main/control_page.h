#pragma once
/* The control panel served at "/".
 *
 * Kept out of app_main.cpp because it had grown to a few hundred characters
 * per line of unreadable C string literal wedged between the HTTP handlers.
 *
 * Push-to-talk is deliberately NOT here. getUserMedia() needs a secure
 * context, and this page is served over plain HTTP from a LAN address, which
 * browsers do not accept as secure - iOS Safari strictly so. The microphone
 * lives on the HTTPS page hosted at apis.orbierobot.com/talk; this page only
 * links to it. See wifi_portal.h and the orbie_apis README.
 */

static const char CONTROL_PAGE_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
"<title>Orbie</title><style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{background:#0e0e0e;color:#eee;font:15px -apple-system,system-ui,Arial;padding-bottom:30px}"
".wrap{max-width:560px;margin:0 auto;padding:12px 14px}"
"h2{font-size:11px;text-transform:uppercase;letter-spacing:1.4px;color:#6a6a6a;margin:22px 0 10px}"
"#stream{width:100%;border-radius:12px;display:block;background:#000;min-height:150px}"
"#info{font-size:12px;color:#8a8a8a;margin:8px 2px;line-height:1.6}"
"#info b{color:#ccc;font-weight:600}"
".card{background:#161616;border:1px solid #242424;border-radius:12px;padding:14px;margin-bottom:10px}"
".row{display:flex;align-items:center;gap:12px}"
".row+.row{margin-top:14px}"
".lbl{font-size:10px;text-transform:uppercase;letter-spacing:1.2px;color:#777;width:52px;flex:none}"
"input[type=range]{flex:1;accent-color:#e94560}"
".val{font-size:12px;color:#999;width:42px;text-align:right;flex:none}"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}"
"button{border:0;border-radius:10px;background:#242424;color:#eee;font-size:14px;padding:13px 10px;font-family:inherit}"
"button:active{transform:scale(.97)}"
"button.p{background:#e94560;font-weight:600}"
"button.g{background:#1f6f43}"
"button.w{background:#7a5c14}"
"a.link{display:block;text-align:center;padding:13px;border:1px solid #2a2a2a;border-radius:10px;color:#9ab;text-decoration:none;font-size:14px;margin-bottom:8px}"
"#res{margin-top:10px;font-size:13px;color:#7ee2a8;min-height:18px}"
"#res.err{color:#ff8f9c}"
".joys{display:flex;justify-content:space-between;align-items:center;gap:14px;margin-top:6px}"
".joy{width:96px;height:96px;border-radius:50%;background:rgba(255,255,255,.05);border:2px solid rgba(255,255,255,.12);position:relative;touch-action:none;flex:none}"
".joy .knob{width:36px;height:36px;border-radius:50%;background:#e94560;position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);pointer-events:none}"
".joy .lbl2{position:absolute;bottom:-15px;left:0;width:100%;text-align:center;font-size:9px;color:rgba(255,255,255,.3);text-transform:uppercase;letter-spacing:1px}"
"</style></head><body><div class='wrap'>"

"<img id='stream' src='/stream81'>"
"<div id='info'>Connecting…</div>"

"<h2>Drive</h2>"
"<div class='card'>"
"<div class='joys'>"
"<div class='joy' id='dj'><div class='knob' id='dk'></div><div class='lbl2'>Drive</div></div>"
"<button class='p' style='flex:1' onclick='stop()'>STOP</button>"
"<div class='joy' id='tj'><div class='knob' id='tk'></div><div class='lbl2'>Turn</div></div>"
"</div></div>"

"<h2>Adjust</h2>"
"<div class='card'>"
"<div class='row'><span class='lbl'>Head</span>"
"<input id='head' type='range' min='0' max='100' value='90' oninput='hd(this.value)'>"
"<span class='val' id='headv'>90</span></div>"
"<div class='row'><span class='lbl'>Volume</span>"
"<input id='vol' type='range' min='0' max='100' value='8' oninput='vl(this.value)'>"
"<span class='val' id='volv'>8%</span></div>"
"</div>"

"<h2>Test</h2>"
"<div class='card'>"
"<div class='grid'>"
"<button class='g' onclick=\"t('voice')\">Test voice</button>"
"<button onclick=\"t('beep')\">Test beep</button>"
"<button onclick=\"t('eyes')\">Test eyes</button>"
"<button onclick=\"t('head')\">Test head</button>"
"<button class='w' onclick=\"t('motors')\">Test motors</button>"
"<button onclick='laser()'>Toggle laser</button>"
"</div>"
"<div id='res'></div>"
"</div>"

"<h2>More</h2>"
"<a class='link' id='talk' href='#'>Talk to Orbie (push to talk)</a>"
"<a class='link' href='/portal?setup=1'>Wi-Fi setup</a>"
"<a class='link' href='/update'>Firmware update</a>"

"</div><script>"
/* The stream lives on port 81; build that from wherever this page was served. */
"document.getElementById('stream').src='http://'+location.hostname+':81/stream';"

"function show(t,bad){var r=document.getElementById('res');r.textContent=t;r.className=bad?'err':'';}"
"function t(w){show('Running '+w+'…');fetch('/test?run='+w).then(r=>r.json())"
".then(d=>show(d.msg)).catch(()=>show('Request failed',1));}"
"function laser(){fetch('/laser').then(r=>r.text()).then(x=>show('Laser '+x)).catch(()=>show('Request failed',1));}"

"var hdT=0;function hd(v){document.getElementById('headv').textContent=v;"
"clearTimeout(hdT);hdT=setTimeout(function(){fetch('/head?pos='+v).catch(function(){});},60);}"
"var vlT=0;function vl(v){document.getElementById('volv').textContent=v+'%';"
"clearTimeout(vlT);vlT=setTimeout(function(){fetch('/volume?pct='+v).catch(function(){});},60);}"

/* Show the stored volume rather than a hardcoded default - it persists in NVS. */
"fetch('/volume').then(r=>r.json()).then(function(d){"
"document.getElementById('vol').value=d.pct;document.getElementById('volv').textContent=d.pct+'%';}).catch(function(){});"
"fetch('/head').then(r=>r.json()).then(function(d){"
"document.getElementById('head').value=d.pos;"
"document.getElementById('headv').textContent=d.enabled?d.pos:'off';"
"document.getElementById('head').disabled=!d.enabled;}).catch(function(){});"

/* Push-to-talk is hosted over HTTPS elsewhere; pass the robot id along. */
"fetch('/api/whoami').then(r=>r.json()).then(function(d){"
"document.getElementById('talk').href=d.api+'/talk?robot='+encodeURIComponent(d.robot);"
"}).catch(function(){});"

"function go(d,t){fetch('/motor?drive='+Math.round(d*255)+'&turn='+Math.round(t*255)).catch(function(){});}"
"var dv=0,tv=0;"
"function stop(){dv=0;tv=0;go(0,0);var a=document.getElementById('dk'),b=document.getElementById('tk');"
"a.style.top='50%';a.style.left='50%';b.style.top='50%';b.style.left='50%';}"
"function setupV(el,knob,cb){var sy=null;"
"function mv(e){var y=e.touches?e.touches[0].clientY:e.clientY;var dy=y-sy;var v=-dy/34;"
"if(v>1)v=1;if(v<-1)v=-1;knob.style.top=(50+dy)+'px';cb(v);}"
"function up(){sy=null;knob.style.top='50%';cb(0);}"
"el.addEventListener('touchstart',function(e){sy=e.touches[0].clientY;mv(e);},{passive:true});"
"el.addEventListener('touchmove',function(e){if(sy!==null)mv(e);},{passive:true});"
"el.addEventListener('touchend',up,{passive:true});"
"el.addEventListener('mousedown',function(e){sy=e.clientY;mv(e);});"
"document.addEventListener('mousemove',function(e){if(sy!==null)mv(e);});"
"document.addEventListener('mouseup',function(){if(sy!==null)up();});}"
"function setupH(el,knob,cb){var sx=null;"
"function mv(e){var x=e.touches?e.touches[0].clientX:e.clientX;var dx=x-sx;var v=dx/34;"
"if(v>1)v=1;if(v<-1)v=-1;knob.style.left='calc(50% + '+dx+'px)';cb(v);}"
"function up(){sx=null;knob.style.left='50%';cb(0);}"
"el.addEventListener('touchstart',function(e){sx=e.touches[0].clientX;mv(e);},{passive:true});"
"el.addEventListener('touchmove',function(e){if(sx!==null)mv(e);},{passive:true});"
"el.addEventListener('touchend',up,{passive:true});"
"el.addEventListener('mousedown',function(e){sx=e.clientX;mv(e);});"
"document.addEventListener('mousemove',function(e){if(sx!==null)mv(e);});"
"document.addEventListener('mouseup',function(){if(sx!==null)up();});}"
"setupV(document.getElementById('dj'),document.getElementById('dk'),function(v){dv=v;go(dv,tv);});"
"setupH(document.getElementById('tj'),document.getElementById('tk'),function(v){tv=v;go(dv,tv);});"

"setInterval(function(){fetch('/status').then(r=>r.json()).then(function(d){"
"document.getElementById('info').innerHTML="
"'Distance <b>'+(d.distance>0?d.distance+' mm':'—')+'</b> &nbsp; '+"
"'Ambient <b>'+d.temp_ambient.toFixed(1)+'°C</b> &nbsp; '+"
"'Object <b>'+d.temp_object.toFixed(1)+'°C</b><br>'+"
"'Laser <b>'+(d.laser?'on':'off')+'</b> &nbsp; Drive <b>'+d.drive+'</b> &nbsp; Turn <b>'+d.turn+'</b>'+"
"(d.wifi_ssid?' &nbsp; Wi-Fi <b>'+d.wifi_ssid+'</b> '+(d.wifi_ip||''):'');"
"}).catch(function(){});},700);"
"</script></body></html>";
