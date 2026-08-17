// usbdongle W2 — the embedded bring-up UI, served at GET /.
//
// ONE self-contained page in .rodata: no LittleFS, no CDN, no external fetch of
// any kind. That is a deliberate scope decision (stuart, 2026-08-17): the
// filesystem-backed asset pipeline in ARCHITECTURE.md section 2 is W4's job,
// and the HTTP transport must be testable before it exists. The cost is ~5 KB
// of flash and a UI with no build step.
//
// It renders itself from GET /api/modules — the descriptor's own action table
// (registry.h ModuleAction) supplies the buttons, the help text and the params
// sketch. NOTHING here names a module: adding `wifiscan` in W3 must cost zero
// front-end changes, and the only way to keep that true is to never mention
// `led` or `storage` in this file. Grep it: there are no module ids in here.
//
// Deliberately NOT here, because this is a bring-up console and not the product:
// no framework, no offline caching, no styling beyond what makes it legible on
// a phone, no per-module panels. W4 replaces it wholesale.
//
// Content-Security-Policy is set on the response (mod_http.cpp), not in a meta
// tag, so it also covers a response served from a cache. `unsafe-inline` is
// unavoidable for a single-file page with no asset partition to serve a .js
// from; connect-src 'self' still confines fetch/WebSocket to this device.
//
// DOM is built with createElement/textContent, never innerHTML. The strings
// being rendered are our own descriptors today, but a module's `help` string is
// one careless edit away from carrying a '<', and an XSS in the page that holds
// the session token is worth avoiding for the price of three lines.

#pragma once

// PROGMEM is a no-op on ESP32 (flash is memory-mapped), so this is a plain
// const array in .rodata — it costs flash, not RAM, and is served straight out
// of the mapping without ever being copied into a buffer.
static const char WEBUI_HTML[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>T-Dongle-S3</title>
<style>
:root{color-scheme:dark light}
body{font:15px/1.4 system-ui,sans-serif;margin:0;padding:12px;max-width:760px;margin:auto}
h1{font-size:1.2rem;margin:.2rem 0 .8rem}
h2{font-size:1rem;margin:1rem 0 .4rem}
input,button,select{font:inherit;padding:8px;border-radius:6px;border:1px solid #888;background:transparent;color:inherit}
button{cursor:pointer}
button.p{background:#2563eb;color:#fff;border-color:#2563eb}
.card{border:1px solid #888;border-radius:8px;padding:10px;margin:8px 0}
.row{display:flex;flex-wrap:wrap;gap:6px;align-items:center}
.mut{opacity:.7;font-size:.85rem}
.on{color:#16a34a;font-weight:600}
.off{opacity:.6}
.act{border-top:1px solid #8884;padding-top:6px;margin-top:6px}
.act input{flex:1;min-width:140px}
pre{background:#8881;padding:8px;border-radius:6px;max-height:11rem;overflow:auto;white-space:pre-wrap;word-break:break-all}
#msg{min-height:1.2em}
.err{color:#dc2626}
</style></head><body>
<h1 id="hd">T-Dongle-S3</h1>
<div id="login">
<p class="mut" id="who"></p>
<div class="row"><input id="pin" type="password" inputmode="numeric" autocomplete="one-time-code" placeholder="PIN" maxlength="8">
<button class="p" id="go">Unlock</button></div>
<p id="msg" class="mut"></p>
</div>
<div id="app" hidden>
<div class="row"><button id="rf">Refresh</button><button id="lo">Log out</button><span class="mut" id="st"></span></div>
<div id="mods"></div>
<h2>Events</h2><pre id="log"></pre>
</div>
<script>
var T=sessionStorage.getItem("tk"),ws=null;
var $=function(i){return document.getElementById(i)};
function el(t,c,x){var e=document.createElement(t);if(c)e.className=c;if(x!==undefined)e.textContent=x;return e}
function log(s){var p=$("log");p.textContent=(new Date().toLocaleTimeString()+" "+s+"\n")+p.textContent.slice(0,4000)}
function say(s,bad){var m=$("msg");m.textContent=s;m.className=bad?"err":"mut"}
function api(path,opt){
  opt=opt||{};opt.headers=opt.headers||{};
  if(T)opt.headers["Authorization"]="Bearer "+T;
  return fetch(path,opt).then(function(r){
    return r.text().then(function(t){
      var j=null;try{j=JSON.parse(t)}catch(e){}
      if(r.status===401){logout("session expired or rejected");throw new Error("401")}
      if(!j)throw new Error("HTTP "+r.status);
      return j})})}
function cmd(body){return api("/api/cmd",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)})}
function errText(j){return j&&j.e?(j.e.code+": "+j.e.msg):"failed"}
function logout(why){
  T=null;sessionStorage.removeItem("tk");
  if(ws){try{ws.close()}catch(e){}ws=null}
  $("app").hidden=true;$("login").hidden=false;say(why||"","")}
function unlock(){
  var p=$("pin").value;say("checking...");
  fetch("/api/session",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({pin:p})})
  .then(function(r){return r.json().then(function(j){return{s:r.status,j:j}})})
  .then(function(x){
    if(x.s===200&&x.j.ok){T=x.j.d.token;sessionStorage.setItem("tk",T);$("pin").value="";say("");start();return}
    var d=x.j.d||{},extra="";
    if(d.attempts_remaining!==undefined)extra=" ("+d.attempts_remaining+" attempts left)";
    if(d.retry_after_ms!==undefined)extra=" (wait "+Math.ceil(d.retry_after_ms/1000)+"s)";
    say(errText(x.j)+extra,1)})
  .catch(function(e){say("network error: "+e.message,1)})}
function actionRow(mid,a){
  var w=el("div","row act");
  w.appendChild(el("code",null,a.act));
  // ModuleAction.params is PROSE (e.g. rgb:"rrggbb"|"off"), not a schema, so it
  // cannot drive a real form. Until it is machine-readable, take the first
  // identifier from it as the sole parameter name and let a bare value be typed
  // -- "ff0000" instead of {"rgb":"ff0000"}. Anything starting with { is still
  // parsed as JSON, so multi-parameter actions keep working.
  var key=(a.params||"").match(/^\s*([A-Za-z_][A-Za-z0-9_]*)/);key=key?key[1]:null;
  var i=el("input");
  i.placeholder=a.params?(key?key+"  (or {\"k\":v} JSON)":"{\"k\":v} JSON"):"no params";
  i.value="";
  w.appendChild(i);
  var b=el("button",null,"Send");
  b.onclick=function(){
    var p={},v=i.value.trim();
    if(v){
      if(v.charAt(0)==="{"){
        try{p=JSON.parse(v)}catch(e){log("bad JSON for "+mid+"."+a.act+": "+e.message);return}
      }else if(key){
        // Bare value. Numbers and true/false/null go through as themselves;
        // everything else stays a string, which is what rgb/path/text all want.
        p[key]=(/^-?\d+(\.\d+)?$/.test(v))?Number(v):(v==="true"?true:(v==="false"?false:(v==="null"?null:v)));
      }else{
        log(mid+"."+a.act+" takes a JSON object, e.g. {\"k\":v}");return;
      }
    }
    cmd({id:Date.now()%100000,mod:mid,act:a.act,p:p}).then(function(j){
      log(mid+"."+a.act+" -> "+JSON.stringify(j.ok?(j.d||{}):j.e))}).catch(function(e){log(e.message)})};
  w.appendChild(b);
  if(a.help){var h=el("div","mut",a.help);h.style.flexBasis="100%";w.appendChild(h)}
  return w}
function toggle(id,on,force){
  cmd({act:on?"disable":"enable",p:force?{id:id,force:true}:{id:id}}).then(function(j){
    log((on?"disable ":"enable ")+id+" -> "+(j.ok?(j.d&&j.d.msg||"ok"):errText(j)));load()})
   .catch(function(e){log(e.message)})}
function card(m){
  var c=el("div","card");
  var h=el("div","row");
  h.appendChild(el("strong",null,m.name));
  h.appendChild(el("code","mut",m.id));
  h.appendChild(el("span",m.enabled?"on":"off",m.enabled?"enabled":"disabled"));
  if(m.pending_restart)h.appendChild(el("span","mut","reboot to apply"));
  c.appendChild(h);
  var meta=[];
  if(m.category)meta.push(m.category);
  for(var k in m.claims||{})meta.push(k+":"+m.claims[k]);
  if(m.blocked_by&&m.blocked_by.length)meta.push("blocked by "+m.blocked_by.join(", "));
  c.appendChild(el("div","mut",meta.join(" · ")));
  var r=el("div","row");
  var b=el("button",m.enabled?null:"p",m.enabled?"Disable":"Enable");
  b.disabled=!!m.essential&&m.enabled;
  b.onclick=function(){toggle(m.id,m.enabled,false)};
  r.appendChild(b);
  if(!m.enabled&&m.blocked_by&&m.blocked_by.length){
    var f=el("button",null,"Force enable");f.onclick=function(){toggle(m.id,false,true)};r.appendChild(f)}
  c.appendChild(r);
  (m.actions||[]).forEach(function(a){c.appendChild(actionRow(m.id,a))});
  if(m.status){var s=el("pre",null,JSON.stringify(m.status,null,1));c.appendChild(s)}
  return c}
function load(){
  api("/api/modules").then(function(j){
    var box=$("mods");box.textContent="";
    if(!j.ok){box.appendChild(el("p","err",errText(j)));return}
    (j.d.modules||[]).forEach(function(m){box.appendChild(card(m))});
    var b=j.d.boot||{};
    $("st").textContent="nvs:"+b.nvs+" restored:"+((b.restored||[]).length)+" skipped:"+((b.skipped||[]).length)})
   .catch(function(e){log(e.message)})}
function openWs(){
  if(!T)return;
  try{ws=new WebSocket((location.protocol==="https:"?"wss://":"ws://")+location.host+"/ws")}catch(e){log("ws: "+e.message);return}
  ws.onopen=function(){ws.send(JSON.stringify({id:1,act:"auth",p:{token:T}}));log("ws open")};
  ws.onmessage=function(ev){
    var j=null;try{j=JSON.parse(ev.data)}catch(e){log("ws: "+ev.data);return}
    if(j.ev)log("ev "+j.ev+" "+JSON.stringify(j.d||{}));else log("ws "+ev.data)};
  ws.onclose=function(){log("ws closed");ws=null};
  ws.onerror=function(){log("ws error")}}
function start(){$("login").hidden=true;$("app").hidden=false;load();openWs()}
$("go").onclick=unlock;
$("pin").addEventListener("keydown",function(e){if(e.key==="Enter")unlock()});
$("rf").onclick=load;
$("lo").onclick=function(){api("/api/session",{method:"DELETE"}).catch(function(){}).then(function(){logout("logged out")})};
fetch("/api/status").then(function(r){return r.json()}).then(function(j){
  if(j.ok){$("hd").textContent=j.d.name;$("who").textContent=j.d.fw+" · built "+j.d.build}})
 .catch(function(){});
if(T)start();
</script></body></html>
)HTML";
