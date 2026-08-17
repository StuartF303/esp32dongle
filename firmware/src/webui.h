// usbdongle W2 — the embedded bring-up UI, served at GET /.
//
// ONE self-contained page in .rodata: no LittleFS, no CDN, no external fetch of
// any kind. That is a deliberate scope decision (stuart, 2026-08-17): the
// filesystem-backed asset pipeline in ARCHITECTURE.md section 2 is W4's job,
// and the HTTP transport must be testable before it exists. The cost is ~5 KB
// of flash and a UI with no build step.
//
// It renders itself from GET /api/modules — the descriptor's own action table
// (registry.h ModuleAction) supplies the buttons, the help text and now a REAL
// PER-PARAMETER FORM. NOTHING here names a module: adding `wifiscan` in W3 must
// cost zero front-end changes, and the only way to keep that true is to never
// mention `led` or `storage` in this file. Grep it: there are no module ids in
// here — and now there are no per-module parameter names either.
//
// ---- WHAT REPLACED THE JSON TEXTBOX -------------------------------------
//
// Until 2026-08-17 this page had ONE input per action and demanded raw JSON in
// it, while its placeholder showed the descriptor's PROSE params hint. It
// advertised rgb:"rrggbb"|"off" and answered "bad JSON for led.set" — the field
// advertised one syntax and rejected it for not being another. A heuristic
// ("take the first identifier out of the prose, allow a bare value") papered
// over the single-parameter case and did nothing for the rest.
//
// The descriptor is now machine-readable (modparam.h), so each parameter gets
// its own labelled control:
//   string     -> text input
//   int        -> number input carrying the dispatch's own min/max
//   bool       -> checkbox when required; a (omit)/true/false select when
//                 optional, because a checkbox cannot say "leave it out" and
//                 "false" is a meaningful value for several of them
//   enum       -> select, with an "(omit)" entry when optional
//   enum_list  -> a checkbox per value, sent as a JSON array
// An optional field left empty is OMITTED from `p` rather than sent as "".
//
// The raw-JSON box SURVIVES, one per action, collapsed behind a toggle, and is
// MERGED OVER the built object. It is the escape hatch for what the schema
// cannot express (a session id that must be an unquoted number, say) — but it
// is no longer the only way to send a parameter, which is the whole point.
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
.fld{margin:6px 0 8px}
.fld label{display:flex;gap:6px;align-items:center;flex-wrap:wrap}
.fld input[type=text],.fld input[type=number],.fld select{flex:1;min-width:130px}
.fld input[type=checkbox]{flex:none;width:1.1rem;height:1.1rem}
.req{color:#dc2626;font-weight:600}
.raw{width:100%;box-sizing:border-box;font-family:ui-monospace,monospace}
button.lnk{background:transparent;border:0;text-decoration:underline;padding:8px 4px;opacity:.8}
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
function opt(s,v,t){var o=document.createElement("option");o.value=v;o.textContent=t;s.appendChild(o);return o}
// One labelled control for one descriptor parameter. Returns {p,ctl,boxes}.
function field(p){
  var wrap=el("div","fld"),lab=el("label"),ctl=null,boxes=[];
  lab.appendChild(el("code",null,p.name));
  lab.appendChild(el("span",p.required?"req":"mut",p.required?"required":"optional"));
  lab.appendChild(el("span","mut",p.type));
  if(p.type==="bool"&&p.required){ctl=el("input");ctl.type="checkbox"}
  else if(p.type==="bool"){
    // Optional bool: a checkbox has two states and this parameter has three.
    // "unchecked" would have to mean either "false" or "leave it out", and both
    // are real -- p.on:false turns the backlight off, p.cancel absent is not the
    // same request as p.cancel:false.
    ctl=el("select");opt(ctl,"","(omit)");opt(ctl,"true","true");opt(ctl,"false","false")}
  else if(p.type==="enum"){
    ctl=el("select");if(!p.required)opt(ctl,"","(omit)");
    (p["enum"]||[]).forEach(function(v){opt(ctl,v,v)})}
  else if(p.type==="enum_list"){
    ctl=el("div","row");
    (p["enum"]||[]).forEach(function(v){
      var l=el("label"),b=el("input");b.type="checkbox";b.value=v;
      l.appendChild(b);l.appendChild(el("span",null,v));ctl.appendChild(l);boxes.push(b)})}
  else{
    ctl=el("input");
    if(p.type==="int"){
      ctl.type="number";ctl.inputMode="numeric";
      if(p.min!==undefined)ctl.min=p.min;
      if(p.max!==undefined)ctl.max=p.max;
      ctl.placeholder=(p.min!==undefined?p.min:"")+".."+(p.max!==undefined?p.max:"")}
    else{ctl.type="text";ctl.placeholder=p.type==="string"?"":p.type}}
  // enum_list holds its OWN labels, one per checkbox, so it must not be nested
  // inside this one -- nested labels are invalid and a tap on the name would
  // toggle the first value.
  if(p.type==="enum_list"){wrap.appendChild(lab);wrap.appendChild(ctl)}
  else{lab.appendChild(ctl);wrap.appendChild(lab)}
  if(p.help)wrap.appendChild(el("div","mut",p.help));
  return{p:p,ctl:ctl,boxes:boxes,el:wrap}}
// Reads one field. {has:false} == leave it out; {err:...} == refuse to send.
function readField(f){
  var p=f.p,c=f.ctl;
  if(p.type==="bool")return p.required?{has:true,val:c.checked}:(c.value===""?{has:false}:{has:true,val:c.value==="true"});
  if(p.type==="enum_list"){
    var a=[];f.boxes.forEach(function(b){if(b.checked)a.push(b.value)});
    return a.length?{has:true,val:a}:(p.required?{err:"pick at least one"}:{has:false})}
  var v=(c.value||"").trim();
  if(v==="")return p.required?{err:"required"}:{has:false};
  if(p.type==="int"){
    if(!/^-?\d+$/.test(v))return{err:"must be a whole number"};
    var n=Number(v);
    if(p.min!==undefined&&n<p.min)return{err:"minimum is "+p.min};
    if(p.max!==undefined&&n>p.max)return{err:"maximum is "+p.max};
    return{has:true,val:n}}
  return{has:true,val:v}}
function actionRow(mid,a){
  var w=el("div","act"),head=el("div","row");
  head.appendChild(el("code",null,a.act));
  var b=el("button",null,"Send");head.appendChild(b);
  var rawBtn=el("button","lnk","raw JSON");head.appendChild(rawBtn);
  w.appendChild(head);
  if(a.help)w.appendChild(el("div","mut",a.help));
  // The form is built from the descriptor's own parameter table -- no module
  // name, no parameter name and no type list is hardcoded in this file.
  var fs=(a.params||[]).map(function(p){var f=field(p);w.appendChild(f.el);return f});
  var raw=el("input");raw.className="raw";raw.hidden=true;
  raw.placeholder='{"k":v}  merged over the fields above';
  rawBtn.onclick=function(){raw.hidden=!raw.hidden;if(!raw.hidden)raw.focus()};
  w.appendChild(raw);
  b.onclick=function(){
    var p={},bad=null;
    fs.forEach(function(f){
      if(bad)return;
      var r=readField(f);
      if(r.err){bad=f.p.name+": "+r.err;return}
      if(r.has)p[f.p.name]=r.val});
    if(bad){log(mid+"."+a.act+" not sent -- "+bad);return}
    var rv=(raw.value||"").trim();
    if(rv){
      var o=null;try{o=JSON.parse(rv)}catch(e){log("raw JSON for "+mid+"."+a.act+": "+e.message);return}
      if(!o||typeof o!=="object"||o instanceof Array){log("raw JSON must be an object, e.g. {\"k\":v}");return}
      for(var k in o)p[k]=o[k]}
    cmd({id:Date.now()%100000,mod:mid,act:a.act,p:p}).then(function(j){
      log(mid+"."+a.act+" -> "+JSON.stringify(j.ok?(j.d||{}):j.e))}).catch(function(e){log(e.message)})};
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
