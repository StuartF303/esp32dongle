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
// ---- THE FIRMWARE UPLOAD CARD (POST /api/ota) ----------------------------
//
// The endpoint (mod_http.cpp handleOta, otaupload.cpp) had no client at all: a
// phone browser cannot POST a binary body without something to drive it, and
// this machine has no Wi-Fi adapter to POST from. This card IS the test rig.
//
// TWO things about it are not obvious and are the reason it looks the way it
// does:
//
//   1. crypto.subtle DOES NOT EXIST HERE. The page is served over plain HTTP at
//      192.168.4.1, and every browser gates SubtleCrypto on a SECURE CONTEXT
//      (HTTPS or localhost). So window.crypto.subtle is undefined on the only
//      device that matters, and a design that hashed the image client-side
//      would fail as "undefined is not an object" at the moment of use. It is
//      therefore FEATURE-DETECTED: present -> the digest is computed and sent
//      as ?sha256 (which is optional in the API); absent -> the parameter is
//      OMITTED ENTIRELY rather than sent empty or wrong, and the page says so.
//      Either way the DEVICE's own digest of what it received is displayed in a
//      readonly, tap-to-select field, because that is the actual verification
//      path: compare it with `sha256sum firmware.bin` on the host. No SHA-256
//      is hand-rolled in JS — this file is .rodata and that is not worth 2 KB.
//
//   2. XMLHttpRequest, not fetch. fetch() cannot report UPLOAD progress at all
//      (no request stream progress event, and ReadableStream request bodies are
//      HTTP/2-only in Chrome and absent in Safari). ~1.25 MB over the SoftAP
//      takes several seconds during which the HTTP task is blocked, so with no
//      progress the page looks hung. xhr.upload.onprogress is the only API that
//      reports it. An XHR to /api/ota is same-origin, so `connect-src 'self'`
//      in the CSP covers it exactly as it covers fetch and the WebSocket.
//
// Every documented failure code from handleOta is rendered as `e.code: e.msg`
// VERBATIM, with a short separate note on what to do about it. EPENDING is the
// one that will be met in practice — the first ~30 s after an OTA boot the
// running image is still PENDING_VERIFY and IDF refuses to begin another OTA —
// so the card also warns about it BEFORE the upload, from `ota`'s own state.
//
// A rejection may arrive as NO RESPONSE AT ALL. handleOta answers and then
// returns ESP_FAIL on every failure path, deliberately, so esp_http_server
// closes the socket instead of draining a rejected 1.25 MB body on its own
// task — and a browser that is still uploading when that happens can surface
// it as a bare network error with status 0. So xhr.onerror does not guess: it
// asks `ota`, whose d.upload carries the last upload's own code and msg, and
// distinguishes "the device recorded a failure" from "the device has no record
// of an upload this boot", which means the request was rejected before its body
// was read (auth, ?len vs Content-Length, a bad query).
//
// The card never offers a reboot button: `reboot` is AUTH_PHYSICAL and a
// button that can only ever produce EAUTH is worse than no button.
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
progress{width:100%;height:12px}
#ores div{margin:4px 0}
#msg{min-height:1.2em}
.err{color:#dc2626}
</style></head><body>
<h1 id="hd">T-Dongle-S3</h1>
<div id="login">
<p class="mut" id="who"></p>
<!-- NO maxlength HERE, ON PURPOSE. It is set from GET /api/status's pin_len
     when the status arrives. A literal in this attribute is exactly how the
     wrong number survives a change: the 8 that used to sit here outlived the
     move to a 4-digit PIN, in a file nothing recompiles when AuthFmt::PIN_LEN
     moves. pin_len was added to that endpoint for this and had no consumer. -->
<div class="row"><input id="pin" type="password" inputmode="numeric" autocomplete="one-time-code" placeholder="PIN">
<button class="p" id="go">Unlock</button></div>
<p id="msg" class="mut"></p>
<p id="pinnote" class="mut"></p>
</div>
<div id="app" hidden>
<div class="row"><button id="rf">Refresh</button><button id="lo">Log out</button><span class="mut" id="conn"></span><span class="mut" id="st"></span></div>
<div class="card" id="ota">
<div class="row"><strong>Firmware update</strong><code class="mut">POST /api/ota</code></div>
<div class="mut" id="ost">reading OTA state...</div>
<div class="mut" id="ocry"></div>
<div class="row"><input id="ofile" type="file" accept=".bin,application/octet-stream"></div>
<div class="mut" id="oinf"></div>
<div class="fld"><label><input id="osel" type="checkbox"><span>select this image to boot next &mdash; the device runs it after the NEXT reboot. Leave off to park it in the slot.</span></label></div>
<div class="row"><button class="p" id="oup">Upload</button><span class="mut" id="opct"></span></div>
<progress id="obar" value="0" max="100" hidden></progress>
<div id="ores"></div>
</div>
<div id="mods"></div>
<h2>Events</h2><pre id="log"></pre>
</div>
<script>
// ---- ARRIVING WITH A PIN IN THE PATH -----------------------------------
// THE FIRST THING THIS SCRIPT DOES, and the order inside it is the point:
// read the path, STRIP IT, and only then consider using it.
//
// The pair QR on the LCD encodes HTTP://192.168.4.1/4821 (ARCHITECTURE.md
// "QR pairing on the LCD"), so the common way to reach this page is already
// holding the PIN. Landing already-pairing is the happy path -- the user
// should watch it complete, not be handed an empty box.
//
// replaceState RUNS BEFORE ANYTHING ELSE CAN THROW OR AWAIT. If the PIN
// survived in the URL, a reload -- or a phone restoring the tab, or the back
// button -- would re-POST it. The PIN is single-use (mod_http.cpp mint trigger
// 2) so the retry would fail, spend an attempt against the rate limiter, and
// land the user on an error for doing nothing but refreshing. After the strip,
// a reload lands on /, which is ordinary PIN entry.
//
// The device serves the page for ANY path of pin_len digits and never
// compares -- see handleWildcard() in mod_http.cpp for why that is deliberate
// and not laziness -- so the digits here are a claim, not a credential. They
// go to POST /api/session and take the limiter like anything else.
var ARRIVED=(function(){
  var m=/^\/(\d+)$/.exec(location.pathname);
  if(!m)return "";
  try{history.replaceState(null,"","/")}catch(e){}
  return m[1]})();
// ---- localStorage, NOT sessionStorage ----------------------------------
// ARCHITECTURE.md's grace-window story says localStorage, and the difference
// is exactly where the window matters. sessionStorage survives a screen lock
// but NOT a tab close and not an iOS tab eviction under memory pressure. After
// one of those the token is gone from the page while the DEVICE still holds
// the session for up to 90 s -- and because the device is not unpaired, there
// is no PIN on the LCD to re-pair with. The user is locked out of their own
// device until the grace window closes, which is precisely the lockout the
// whole single-session design exists to prevent, arriving through the other
// door. localStorage survives both, so the page comes back holding the token
// the device is still honouring.
//
// WHAT IT COSTS IS RECORDED AS BACKLOG S12, not left implicit: localStorage is
// keyed by ORIGIN and is durable, and this origin is http://192.168.4.1 -- the
// SoftAP default here and on a long tail of other consumer devices. A token
// that survives a tab close also survives the phone joining somebody else's
// 192.168.4.1. Accepted trade, bounded by the idle and absolute session caps;
// read S12 before changing either side of it.
var T=localStorage.getItem("tk"),ws=null;
// From GET /api/status. 0 until it answers; nothing hardcodes 4.
var PINLEN=0;
// ---- the WebSocket keepalive -------------------------------------------
// The page used to send the auth frame on open and NOTHING afterwards.
// esp_http_server answers a browser's ping/pong itself, on its own task, and
// that reply never reaches our code -- so a paired phone that is only WATCHING
// events (which is the normal state) never refreshed its session and was
// reaped at SESSION_IDLE_MS, 15 minutes, mid-use. Only an inbound frame
// refreshes, by design: mod_http.cpp's sessionLiveLocked() note explains why
// receiving a push deliberately does not count as activity.
//
// So the page sends a real command frame. `uptime` is the cheapest built-in at
// AUTH_TOKEN (cmdauth.h) and its response is a few dozen bytes.
//
// WHY THIS IS NOT JUST DEFEATING THE IDLE TIMEOUT, which is the honest
// question to ask of any keepalive: a locked or backgrounded phone SUSPENDS
// JS timers. So a phone in a pocket stops sending these and still ages out at
// 15 minutes, which is the case the timeout was written for; what survives is
// a page someone is actually looking at on a bench, which is the case it was
// never meant to kill. And both are still bounded by SESSION_ABSOLUTE_MS -- 4
// hours, refreshed by nothing -- so no amount of keepalive holds a session
// open indefinitely.
//
// 60 s is comfortably inside 15 minutes with room for a phone that suspends
// timers for a minute or two and comes back.
var KA=null,KA_MS=60000;
// ---- reconnect state ---------------------------------------------------
// GRACE is ApGrace::GRACE_MS, learned from the device (`http status`.grace_ms)
// rather than assumed; 90000 is only the value to use before the first
// successful read.
var RC=null,RCN=0,DOWN=0,GRACE=90000;
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
      // A 401 from ANY endpoint means the token is dead: the grace window is
      // irrelevant once the device has actually rejected it.
      if(r.status===401){sessionOver("the session token was rejected -- pair again");throw new Error("401")}
      if(!j)throw new Error("HTTP "+r.status);
      return j})})}
function cmd(body){return api("/api/cmd",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)})}
function errText(j){return j&&j.e?(j.e.code+": "+j.e.msg):"failed"}
function conn(s,bad){var c=$("conn");c.textContent=s;c.className=bad?"err":"mut"}
function pinNote(s){$("pinnote").textContent=s||""}
// The PIN the user is holding -- typed, or scanned off the LCD -- has been
// spent or replaced. Stop offering it. Every path that learns this learns it
// from the device: `pin_rotated` on the /api/session response, on the DELETE
// response, and on the http.auth event.
function pinRotated(why){
  $("pin").value="";
  ARRIVED="";
  pinNote(why||"the PIN on the device's screen has changed -- read the new one")}
function logout(why){
  T=null;localStorage.removeItem("tk");
  kaStop();rcStop();conn("");
  if(ws){try{ws.close()}catch(e){}ws=null}
  $("app").hidden=true;$("login").hidden=false;say(why||"","")}
// The grace window has closed (or the token was rejected outright). This is
// design/BRIEF.md 4.2's "session over": say so plainly and route to pairing.
// Distinct from logout() only in what it tells the user and in clearing the
// PIN, because a session ending on the device ALWAYS rotates the PIN
// (mod_http.cpp rotateAfterRevocation), so whatever they were holding is dead.
function sessionOver(why){
  logout(why);
  pinRotated("the session ended, so the device has put a NEW PIN on its screen")}
function unlock(auto){
  var p=$("pin").value;say(auto?"pairing...":"checking...");pinNote("");
  fetch("/api/session",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({pin:p})})
  .then(function(r){return r.json().then(function(j){return{s:r.status,j:j}})})
  .then(function(x){
    if(x.s===200&&x.j.ok){
      T=x.j.d.token;localStorage.setItem("tk",T);$("pin").value="";ARRIVED="";say("");pinNote("");start();return}
    var d=x.j.d||{},extra="";
    if(d.attempts_remaining!==undefined)extra=" ("+d.attempts_remaining+" attempts left)";
    if(d.retry_after_ms!==undefined)extra=" (wait "+Math.ceil(d.retry_after_ms/1000)+"s)";
    say(errText(x.j)+extra,1);
    // ELOCKED and the tenth failure both rotate. The device says which.
    if(d.pin_rotated)pinRotated();
    // A PIN that came from the URL is not something the user typed, so leaving
    // it in the box for them to correct helps nobody -- it is spent or wrong.
    else if(auto)$("pin").value=""})
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
// ---- firmware upload (POST /api/ota) ------------------------------------
// See the crypto.subtle / XMLHttpRequest note at the top of this file before
// changing anything here.
var SUB=!!(window.crypto&&window.crypto.subtle&&window.crypto.subtle.digest);
var oSlot=0,oTgt="",oBusy=false,MINB=4096,MAXB=4194304;
// Not paraphrases of e.msg -- that is printed verbatim above these. This is
// what to DO about each code.
var HINT={
EPENDING:"the running image is still PENDING_VERIFY, so IDF refuses to begin another OTA. Run `ota confirm` on the USB console, or wait for the health check (~30s after an OTA boot), then upload again",
EARGS:"the query string was rejected before anything was erased",
ELENGTH:"the body needs a Content-Length equal to ?len; chunked encoding is refused",
ETOOSMALL:"far too small to be an ESP32-S3 app image",
ETOOBIG:"bigger than the target slot",
EBUSY:"another upload is already running; wait for it to finish",
ETIMEOUT:"the transfer stalled or ran past the deadline; the slot was aborted and nothing was selected",
ECONN:"the link failed mid-transfer; the slot was aborted",
ESHORT:"the body ended before ?len bytes arrived; the slot was aborted",
ESHA256:"what arrived does not match the digest that was sent; the slot was aborted. Retry",
EIMAGE:"esp_ota_end() rejected it: not a bootable image. Check you picked firmware.bin, not the .elf and not a merged/factory image",
EMAGIC:"the first byte is not 0xE9, so this is not an app image at all",
EAUTH:"the session token was rejected; unlock again",
ENOSLOT:"this partition table has no second app slot",
ECONFLICT:"the target slot is the one currently running",
ENOMEM:"not enough heap to start an OTA; disable a module and retry",
EOTADATA:"the otadata partition holds invalid data",
EOTABEGIN:"esp_ota_begin() refused",
EOTAWRITE:"a flash write failed; the slot was aborted",
EOTAEND:"finalising the image failed; nothing was selected",
ESELECT:"the image was written and validated but could NOT be selected; the device still boots the old one",
ESTOPPING:"the Wi-Fi transport is shutting down"};
function fmtB(n){return n>=1048576?(n/1048576).toFixed(2)+" MiB":(n/1024).toFixed(1)+" KiB"}
function oRes(){var r=$("ores");r.textContent="";return r}
function oLine(r,c,t){r.appendChild(el("div",c,t));return r}
function oBad(t){oLine(oRes(),"err",t)}
function oLock(on){oBusy=on;$("oup").disabled=on}
function oPct(p,t){var b=$("obar");b.hidden=false;b.value=p;$("opct").textContent=t}
function oHex(buf){var v=new Uint8Array(buf),s="",i;for(i=0;i<v.length;i++)s+=(v[i]<16?"0":"")+v[i].toString(16);return s}
// Readonly input, not text: the only comfortable way to select 64 hex
// characters on a phone.
function oSha(r,hex){
  var i=el("input");i.className="raw";i.readOnly=true;i.value=hex;
  i.onclick=function(){this.select()};
  r.appendChild(i);return r}
// `ota` (read-only, AUTH_TOKEN) names the target slot and says whether this
// image is still PENDING_VERIFY -- i.e. EPENDING, before it happens. `parts`
// then gives that slot's size, once per session.
function oState(){
  cmd({act:"ota"}).then(function(j){
    if(!j.ok)return;
    var d=j.d||{},t=d.rollback_target||{},e=$("ost");
    oTgt=t.label||"";
    var s="running "+d.running+" · next boot "+d.boot+" · "+d.ota_state;
    if(oTgt)s+=" · target "+oTgt+(oSlot?" ("+fmtB(oSlot)+")":"");
    if(d.pending)s+=" — still PENDING_VERIFY: an upload now is refused with EPENDING for up to "+d.window_remaining_s+"s. `ota confirm` over USB, or wait.";
    e.textContent=s;e.className=d.pending?"err":"mut";
    if(oTgt&&!oSlot)oParts()}).catch(function(){})}
function oParts(){
  cmd({act:"parts"}).then(function(j){
    if(!j.ok)return;
    (j.d.partitions||[]).forEach(function(p){if(p.label===oTgt)oSlot=p.size});
    if(oSlot)oState()}).catch(function(){})}
// otaupload.cpp's own bounds, checked here so a doomed image does not cost a
// multi-second upload first.
function oCheck(f){
  if(f.size<MINB)return "only "+f.size+" B: anything under "+MINB+" is refused (ETOOSMALL)";
  if(oSlot&&f.size>oSlot)return f.size+" B will not fit slot "+oTgt+" ("+oSlot+" B): ETOOBIG";
  if(!oSlot&&f.size>MAXB)return f.size+" B is over the "+fmtB(MAXB)+" bound this page applies while the slot size is unknown";
  return ""}
function oPick(){
  var f=this.files&&this.files[0],i=$("oinf");
  $("obar").hidden=true;$("opct").textContent="";oRes();
  if(!f){i.textContent="";i.className="mut";return}
  var bad=oCheck(f);
  i.textContent=f.name+" · "+f.size+" B ("+fmtB(f.size)+")"+
    (bad?" — "+bad:(/\.bin$/i.test(f.name)?"":" — not a .bin; only a raw firmware.bin is a valid image"));
  i.className=bad?"err":"mut"}
function oGo(){
  if(oBusy)return;
  var f=$("ofile").files&&$("ofile").files[0];
  oRes();
  if(!f){oBad("choose a firmware .bin first");return}
  var bad=oCheck(f);
  if(bad){oBad(bad);return}
  oLock(true);
  if(!SUB){oSend(f,"");return}
  $("opct").textContent="hashing locally (crypto.subtle)...";
  var fr=new FileReader();
  fr.onerror=function(){oLock(false);$("opct").textContent="";oBad("the browser could not read the file")};
  fr.onload=function(){
    window.crypto.subtle.digest("SHA-256",fr.result).then(function(h){oSend(f,oHex(h))})
    .catch(function(e){oLock(false);$("opct").textContent="";
      oBad("crypto.subtle.digest failed: "+e.message+" -- not retrying without a digest")})};
  fr.readAsArrayBuffer(f)}
function oSend(f,sha){
  var sel=$("osel").checked;
  // ?sha256 is OMITTED, not blanked, when there is no digest to send.
  var q="/api/ota?len="+f.size+(sha?"&sha256="+sha:"")+(sel?"&select=1":"");
  var x=new XMLHttpRequest();
  x.open("POST",q,true);
  x.setRequestHeader("Authorization","Bearer "+T);
  x.setRequestHeader("Content-Type","application/octet-stream");
  x.upload.onprogress=function(e){
    if(e.lengthComputable)oPct(Math.floor(e.loaded*100/e.total),e.loaded+" / "+e.total+" B")};
  // The device validates AFTER the last byte, with its HTTP task blocked --
  // without this the bar sits at 100% looking stuck.
  x.upload.onload=function(){oPct(100,"all "+f.size+" B sent — the device is validating the image")};
  x.onload=function(){oLock(false);oDone(x)};
  x.onerror=function(){oLock(false);oLost("the connection dropped")};
  x.onabort=function(){oLock(false);oLost("the upload was cancelled")};
  oPct(0,"uploading...");
  log("ota upload "+f.size+" B"+(sha?" sha256="+sha.slice(0,12)+"...":" (no client digest)")+(sel?" select=1":""));
  x.send(f)}
function oDone(x){
  var j=null;try{j=JSON.parse(x.responseText)}catch(e){}
  if(x.status===401){sessionOver("the session token was rejected -- pair again");return}
  var r=oRes();
  if(!j){oLine(r,"err","HTTP "+x.status+" and the body was not JSON: "+(x.responseText||"").slice(0,120));oState();return}
  log("ota -> HTTP "+x.status+" "+(j.ok?"ok":JSON.stringify(j.e||{})));
  if(j.ok)oOk(r,j.d||{});else oFail(r,j.e||{},j.d||{});
  oState()}
function oOk(r,d){
  oLine(r,"on","wrote "+d.written+" of "+d.len+" B to "+d.target+" in "+((d.elapsed_ms||0)/1000).toFixed(1)+"s");
  if(d.build)oLine(r,"mut","now in that slot: "+(d.project||"?")+" "+(d.app_version||"?")+" · built "+d.build+" · IDF "+(d.idf_version||"?"));
  oLine(r,"mut","sha256 the DEVICE computed over what it received — compare with: sha256sum firmware.bin");
  oSha(r,d.sha256||"");
  oLine(r,d.sha256_checked?(d.sha256_ok?"mut":"err"):"err",
    d.sha256_checked?(d.sha256_ok?"digest was sent with the request and verified on the device before install"
                                :"digest MISMATCH reported by the device")
                    :"no digest was sent with the request: integrity NOT verified in transit — compare the digest above by hand");
  if(d.selected)oLine(r,"err","selected: otadata now boots "+d.boot+". A REBOOT IS STILL REQUIRED and this page cannot do it — `reboot` is AUTH_PHYSICAL, i.e. the USB cable: run it on the serial console, or unplug and replug.");
  else oLine(r,"mut","not selected: otadata still boots "+d.boot+", so this image will not run. Upload again with the box ticked, or `ota boot "+d.target+"` over USB.");
  if(d.msg)oLine(r,"mut",d.msg)}
function oFail(r,e,d){
  oLine(r,"err",(e.code||"?")+": "+(e.msg||""));
  if(HINT[e.code])oLine(r,"mut",HINT[e.code]);
  if(d.written!==undefined)
    oLine(r,"mut","got "+d.written+" of "+d.len+" B into "+(d.target||"?")+" in "+((d.elapsed_ms||0)/1000).toFixed(1)+
      "s · otadata still boots "+(d.boot||"?")+" · selected="+(d.selected?"yes":"no"));
  if(d.sha256){oLine(r,"mut","sha256 of what did arrive:");oSha(r,d.sha256)}}
// A rejection can reach us as a bare network error: handleOta closes the socket
// rather than draining a rejected body. `ota` recorded the code -- ask it.
function oLost(why){
  var r=oRes();
  oLine(r,"err",why+" — no HTTP response reached the browser. Asking the device what it recorded...");
  cmd({act:"ota"}).then(function(j){
    var u=(j&&j.ok&&j.d&&j.d.upload)||{};
    if(u.phase==="running"){oLine(r,"mut","the device still reports an upload in flight ("+u.written+" of "+u.len+" B); wait, then press Refresh");return}
    if(u.phase==="ok"||u.phase==="failed"){
      oLine(r,u.phase==="ok"?"on":"err","the device recorded: "+u.phase+(u.code?" "+u.code:"")+(u.msg?" — "+u.msg:""));
      oLine(r,"mut",u.written+" of "+u.len+" B, "+u.finished_ms_ago+" ms ago");
      if(HINT[u.code])oLine(r,"mut",HINT[u.code]);
      return}
    oLine(r,"mut","the device has no record of an upload this boot: the request was rejected before the body was read (auth, ?len vs Content-Length, or a bad query) and the socket closed. The reply is in the USB console log.")})
  .catch(function(e){oLine(r,"err","and /api/cmd failed too: "+e.message)});
  oState()}
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
// ---- keepalive ----------------------------------------------------------
// A distinctive id so the response can be dropped from the event log: fifteen
// lines an hour of "uptime -> ok" would bury the events the log exists for.
var KA_ID=9901;
function kaStop(){if(KA){clearInterval(KA);KA=null}}
function kaStart(){
  kaStop();
  KA=setInterval(function(){
    if(ws&&ws.readyState===1){try{ws.send(JSON.stringify({id:KA_ID,act:"uptime"}))}catch(e){}}},KA_MS)}
// ---- the reconnecting state (design/BRIEF.md 4.2) ----------------------
// THREE STATES, AND THE MIDDLE ONE IS NOT AN ERROR:
//   live          - socket open, events arriving
//   reconnecting  - inside the grace window, token still good, retrying. The
//                   user is NOT thrown back to a PIN box here. A phone drops a
//                   Wi-Fi network without saying so, and this is the state the
//                   user will actually hit, repeatedly, without knowing why.
//   session over  - the window closed. The token in localStorage is worthless;
//                   say so and route to pairing.
//
// HOW THE THIRD IS DISTINGUISHED FROM THE SECOND, which is the whole problem:
// the socket dying tells us nothing on its own. So each retry first asks the
// DEVICE over REST (`http status`, which is the only thing that carries
// grace_ms / grace_active / grace_ms_left):
//   * it answers      -> the AP is reachable and the token is alive. Whatever
//                        killed the socket, the session is fine. Keep retrying
//                        quietly and show the real countdown if one is armed.
//   * 401             -> the token is dead. api() routes that to sessionOver().
//   * unreachable     -> the phone is off the AP. Count down from GRACE, which
//                        was learned from the device rather than assumed, and
//                        call it over when it runs out.
// The last case is a local clock rather than the device's, necessarily: we
// cannot ask a device we cannot reach. It is the same 90 s the device is
// counting, started at most one retry interval later, so it errs towards
// saying "over" slightly after the device does -- which is the safe direction,
// because the alternative is telling the user to go and read a PIN that is not
// on the screen yet.
function rcStop(){if(RC){clearTimeout(RC);RC=null}RCN=0;DOWN=0}
function rcArm(){
  if(RC||!T)return;
  if(!DOWN)DOWN=Date.now();
  // 1, 2, 4, then 5 s. Fast enough that a screen-unlock reconnects while the
  // user is still looking at the page, slow enough not to hammer a device that
  // is not there.
  var d=Math.min(1000*Math.pow(2,RCN),5000);RCN++;
  RC=setTimeout(rcTick,d)}
function rcTick(){
  RC=null;
  if(!T)return;
  cmd({mod:"http",act:"status"}).then(function(j){
    var d=(j&&j.ok&&j.d)||null;
    if(d){
      if(d.grace_ms)GRACE=d.grace_ms;
      if(d.grace_active&&d.grace_ms_left!==undefined)
        conn("reconnecting - "+Math.ceil(d.grace_ms_left/1000)+"s of grace left");
      else conn("reconnecting - device reachable")}
    openWs()})
  .catch(function(){
    // api() turned a 401 into sessionOver(), which cleared T. Nothing to add.
    if(!T)return;
    var left=GRACE-(Date.now()-DOWN);
    if(left<=0){
      sessionOver("the session is over: the device was unreachable for longer than its "+
        Math.round(GRACE/1000)+"s grace window");return}
    conn("reconnecting - "+Math.ceil(left/1000)+"s of grace left");
    rcArm()})}
function openWs(){
  if(!T)return;
  if(ws&&(ws.readyState===0||ws.readyState===1))return;
  try{ws=new WebSocket((location.protocol==="https:"?"wss://":"ws://")+location.host+"/ws")}
  catch(e){log("ws: "+e.message);ws=null;rcArm();return}
  ws.onopen=function(){ws.send(JSON.stringify({id:1,act:"auth",p:{token:T}}));log("ws open")};
  ws.onmessage=function(ev){
    var j=null;try{j=JSON.parse(ev.data)}catch(e){log("ws: "+ev.data);return}
    // The auth frame's own reply. A rejection here means the token is dead --
    // the device closes the socket straight after -- so resolve it now instead
    // of waiting for onclose to discover it the long way round.
    if(j.id===1&&j.ok===false&&j.e&&j.e.code==="EAUTH"){
      sessionOver("this session is no longer valid: "+j.e.msg);return}
    if(j.id===1&&j.ok){rcStop();conn("live");return}
    if(j.id===KA_ID)return;  // keepalive round trip, deliberately silent
    if(j.ev){
      // The device says the PIN has been replaced. Whatever the page or the
      // user is still holding -- a typed value, a scanned one -- is void.
      if(j.d&&j.d.pin_rotated)pinRotated();
      log("ev "+j.ev+" "+JSON.stringify(j.d||{}));return}
    log("ws "+ev.data)};
  ws.onclose=function(){
    ws=null;log("ws closed");
    if(T){conn("reconnecting...");rcArm()}};
  ws.onerror=function(){log("ws error")}}
function start(){
  $("login").hidden=true;$("app").hidden=false;
  rcStop();conn("connecting...");
  load();oState();openWs();kaStart()}
$("go").onclick=function(){unlock(false)};
$("pin").addEventListener("keydown",function(e){if(e.key==="Enter")unlock(false)});
$("rf").onclick=function(){load();oState()};
$("ofile").onchange=oPick;
$("oup").onclick=oGo;
$("ocry").textContent=SUB
 ?"crypto.subtle is available: a SHA-256 is computed here and sent as ?sha256 for the device to verify before it installs the image."
 :"crypto.subtle is NOT available — a plain-HTTP page is not a secure context — so ?sha256 is omitted: integrity is not verified in transit. Compare the digest the device reports below with `sha256sum firmware.bin`.";
// UNPAIR, and it is honest about what it does: DELETE /api/session revokes the
// session AND rotates the PIN (mod_http.cpp mint trigger 3), so the device puts
// a new one on its screen. The response says whether it actually did.
$("lo").onclick=function(){
  api("/api/session",{method:"DELETE"}).then(function(j){
    var rot=j&&j.d&&j.d.pin_rotated;
    logout("logged out");
    if(rot)pinRotated("unpaired: a NEW PIN is on the device's screen")})
   .catch(function(){logout("logged out")})};
fetch("/api/status").then(function(r){return r.json()}).then(function(j){
  if(!j.ok)return;
  $("hd").textContent=j.d.name;$("who").textContent=j.d.fw+" · built "+j.d.build;
  // THE PIN LENGTH COMES FROM THE DEVICE. pin_len was added to this endpoint
  // (2026-08-24) for exactly this and had no consumer until now; the input
  // carries no maxlength in the markup, so there is nowhere for a stale 4 -- or
  // a stale 8 -- to hide. The endpoint is unauthenticated on purpose: the
  // pairing page needs this before there is any session to authenticate with,
  // and the FORMAT of a credential is public while its VALUE is not.
  if(j.d.pin_len){
    PINLEN=j.d.pin_len;
    var i=$("pin");i.maxLength=PINLEN;i.placeholder=PINLEN+"-digit PIN"}})
 .catch(function(){});
// The three ways this page starts, in priority order.
if(T)start();
// Arrived from the pair QR with a PIN in the path (already stripped from the
// URL at the top of this script). This is the happy path: pair without asking
// the user for anything.
else if(ARRIVED){$("pin").value=ARRIVED;unlock(true)}
</script></body></html>
)HTML";
