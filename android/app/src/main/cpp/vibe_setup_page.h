// The VibeServer SETUP page — served at GET / while the server is not yet configured.
//
// ★★★ WHY A SEPARATE PAGE. The receiver client (vibe_web_page.h, ~600 KB, generated) is for
// LISTENING. This is for a person who has just typed `sudo apt install vibeserver`, run the
// wizard, and been given an IP address. They are not a listener yet and must not be shown a
// spectrum: the server has a radio and a password but no policy, and until the owner sets one it
// should not be serving strangers.
//
// ★★ THE BAR IS THE ANDROID SETUP FLOW. Stuart, 2026-08-04: *"browser page configures everything,
// as having it visually is much easier — that is why the Android setup works so well, people love
// how easy it is."* So: grouped, plain words, sensible defaults already filled in, a short line of
// explanation under anything that needs one, and no concept the user has to carry from one screen
// to the next. A list of every option is NOT a design — that is exactly what made the TUI it
// replaces unusable.
//
// ★ Hand-written and small enough to keep as a raw string (no NUL bytes, so unlike the receiver
// client it needs no base64). It talks to GET/POST /vibeserver/config and nothing else, which is
// the point: it is one CLIENT of that API, and the VibeSDR app will be another.
#pragma once

#include <string>

static const char* const kVibeSetupPage = R"HTML(<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>VibeServer setup</title>
<style>
  :root{--bg:#0b0906;--panel:#151009;--line:#3a2c17;--ink:#f2e4cf;--dim:#a08e73;
        --amber:#ffb833;--good:#7ddc6a;--bad:#ff6b5e}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--ink);
       font:15px/1.55 ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
  .wrap{max-width:760px;margin:0 auto;padding:32px 20px 80px}
  h1{font-size:26px;margin:0 0 4px;color:var(--amber);letter-spacing:.3px}
  .sub{color:var(--dim);margin:0 0 28px}
  .card{background:var(--panel);border:1px solid var(--line);border-radius:12px;
        padding:20px 22px;margin:0 0 18px}
  .card h2{font-size:15px;margin:0 0 4px;color:var(--amber);text-transform:uppercase;
           letter-spacing:.09em}
  .card .why{color:var(--dim);font-size:13.5px;margin:0 0 16px}
  label{display:block;margin:14px 0 0}
  .lbl{display:block;font-size:13.5px;margin-bottom:5px}
  .hint{color:var(--dim);font-size:12.5px;margin-top:4px}
  input[type=text],input[type=password],input[type=number],select{
      width:100%;padding:9px 11px;background:#0a0704;color:var(--ink);
      border:1px solid var(--line);border-radius:7px;font:inherit}
  input:focus,select:focus{outline:none;border-color:var(--amber)}
  .row{display:flex;gap:14px;flex-wrap:wrap}
  .row>label{flex:1 1 200px}
  button{background:var(--amber);color:#1a1200;border:0;border-radius:8px;
         padding:11px 20px;font:600 15px/1 inherit;cursor:pointer}
  button:disabled{opacity:.5;cursor:default}
  button.ghost{background:transparent;color:var(--ink);border:1px solid var(--line)}
  .modes{display:grid;gap:14px;grid-template-columns:1fr}
  @media(min-width:620px){.modes{grid-template-columns:1fr 1fr}}
  .mode{background:#0a0704;border:1px solid var(--line);border-radius:10px;padding:16px;
        cursor:pointer;text-align:left}
  .mode.sel{border-color:var(--amber);box-shadow:0 0 0 1px var(--amber) inset}
  .mode b{display:block;color:var(--amber);margin-bottom:6px;font-size:15px}
  .mode span{color:var(--dim);font-size:13px;display:block}
  .checks{display:grid;gap:8px 18px;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));
          margin-top:12px}
  .checks label{display:flex;align-items:center;gap:8px;margin:0;font-size:14px}
  .checks input{accent-color:var(--amber);width:16px;height:16px}
  .note{border-left:3px solid var(--amber);padding:8px 0 8px 12px;margin:14px 0;
        color:var(--dim);font-size:13px}
  .err{color:var(--bad);margin-top:10px;min-height:20px}
  .ok{color:var(--good)}
  .addr{font-family:ui-monospace,monospace;color:var(--amber)}
  .bar{position:fixed;left:0;right:0;bottom:0;background:#12100c;border-top:1px solid var(--line);
       padding:14px 20px;display:flex;gap:14px;align-items:center;justify-content:flex-end}
  .bar .spacer{flex:1;color:var(--dim);font-size:13px}
  .hide{display:none}
</style>
<div class="wrap">
  <h1>VibeServer</h1>

  <!-- ── 1. SIGN IN ──────────────────────────────────────────────────────── -->
  <div id="signin">
    <p class="sub" id="signinSub">Sign in to set up this server.</p>
    <div class="card">
      <h2>Sign in</h2>
      <p class="why">Use the admin password you set when you ran <code>vibeserver</code> on the
         machine itself.</p>
      <label><span class="lbl">Admin password</span>
        <input type="password" id="pass" autocomplete="current-password" autofocus></label>
      <div class="err" id="signinErr"></div>
      <p style="margin-top:16px"><button id="signinBtn">Continue</button></p>
    </div>
  </div>

  <!-- ── 2. SETUP ────────────────────────────────────────────────────────── -->
  <div id="setup" class="hide">
    <p class="sub">Set this receiver up. You can change any of it later from Admin.</p>

    <div class="card">
      <h2>On your network</h2>
      <p class="why">How people find this server once it is running.</p>
      <label style="display:flex;align-items:center;gap:10px;margin:0">
        <input type="checkbox" id="mdns" checked style="width:16px;height:16px;accent-color:var(--amber)">
        <span>Let VibeSDR apps on this network discover this server automatically</span>
      </label>
      <label><span class="lbl">Name</span>
        <input type="text" id="name" placeholder="VibeServer: Pi500"></label>
      <div class="hint" id="addrLine"></div>
    </div>

    <div class="card">
      <h2>Where this receiver is</h2>
      <p class="why">Published to listeners. It sets the flag and the ITU band plan, centres the
         map, and is what lets RDS name a station's country — with no location set, every station
         shows a blank country.</p>
      <div class="row">
        <label><span class="lbl">Town or area</span>
          <input type="text" id="place" placeholder="Northampton"></label>
        <label><span class="lbl">Country code</span>
          <input type="text" id="country" maxlength="2" placeholder="GB"></label>
      </div>
      <label><span class="lbl">Maidenhead locator</span>
        <input type="text" id="locator" maxlength="8" placeholder="IO92nh">
        <div class="hint">Deliberately coarse — about 4 km. A receiver's position is published, so
          this is usually the right amount to give away.</div></label>
      <div class="row">
        <label><span class="lbl">Latitude (optional)</span>
          <input type="text" id="lat" placeholder="52.24"></label>
        <label><span class="lbl">Longitude (optional)</span>
          <input type="text" id="lon" placeholder="-0.90"></label>
      </div>
      <div class="hint">Exact coordinates win over the locator. Leave them blank to publish only
        the square.</div>
    </div>

    <div class="card">
      <h2>How will it be used?</h2>
      <p class="why">This decides what listeners are allowed to change.</p>
      <div class="modes">
        <div class="mode" id="modeSingle" tabindex="0">
          <b>One user at a time</b>
          <span>The listener has the whole radio and every control, as if it were plugged into
                their own machine. Best for your own use from elsewhere in the house.</span>
        </div>
        <div class="mode" id="modeLocked" tabindex="0">
          <b>Shared, locked range</b>
          <span>You choose the frequency range and the rules; listeners tune freely inside it but
                cannot move the radio for everybody. Best for a receiver other people use.</span>
        </div>
      </div>
    </div>

    <!-- Shared-mode only -->
    <div id="lockedOnly" class="hide">
      <div class="card">
        <h2>Range</h2>
        <p class="why">The window everyone listens inside. The radio stays here; listeners pan and
           zoom within it.</p>
        <div class="row">
          <label><span class="lbl">Centre frequency (kHz)</span>
            <input type="number" id="lockFreq" step="1"></label>
          <label><span class="lbl">Sample rate (Hz)</span>
            <input type="number" id="rate" step="1"></label>
        </div>
        <div class="hint" id="coverage"></div>
        <label style="display:flex;align-items:center;gap:10px;margin-top:16px">
          <input type="checkbox" id="zoomSpectrum" checked
                 style="width:16px;height:16px;accent-color:var(--amber)">
          <span>Keep the spectrum sharp when zoomed in</span></label>
        <div class="hint">Recomputes real detail as listeners zoom, instead of magnifying what is
          already on screen. Without it a close-in view goes blocky. Costs a little CPU and is
          what makes a shared receiver worth zooming into.</div>
      </div>

      <div class="card">
        <h2>Where new listeners start</h2>
        <p class="why">What someone sees the moment they connect.</p>
        <div class="row">
          <label><span class="lbl">Frequency (kHz)</span>
            <input type="number" id="landingFreq" step="0.1"></label>
          <label><span class="lbl">Mode</span>
            <select id="demodMode">
              <option value="am">AM</option><option value="lsb">LSB</option>
              <option value="usb">USB</option><option value="nfm">NFM</option>
              <option value="wfm">WFM</option><option value="cw">CW</option>
            </select></label>
        </div>
      </div>

      <div class="card">
        <h2>Listeners</h2>
        <p class="why">How many people at once, and for how long.</p>
        <div class="row">
          <label><span class="lbl">Maximum listeners</span>
            <input type="number" id="users" min="1" max="50"></label>
          <label><span class="lbl">Time limit (minutes, 0 = none)</span>
            <input type="number" id="sessionLimit" min="0"></label>
        </div>
        <div class="note" id="bwNote"></div>
      </div>

      <div class="card">
        <h2>Radio</h2>
        <p class="why">Listeners cannot change these in shared mode, so they are set here or not
           at all.</p>
        <div id="hw"></div>
      </div>
    </div>

    <div class="card">
      <h2>Processor</h2>
      <p class="why">How hard this machine is allowed to run.</p>
      <label><span class="lbl">CPU governor</span>
        <select id="cpuGovernor">
          <option value="performance">Full speed — recommended for a receiver</option>
          <option value="ondemand">On demand — saves a little power</option>
          <option value="default">Leave the system setting alone</option>
        </select>
        <div class="hint">A Raspberry Pi normally decides its speed from how busy each core looks.
          VibeServer spreads its work across every core, so they all look half-idle and the Pi
          clocks itself <em>down</em> — measured at 1.9&nbsp;GHz instead of 2.4, cool and
          un-throttled, while the audio broke up. Full speed costs a couple of watts.
          <br><b>Choose full speed for a shared receiver</b>, where several people are each being
          given their own tuner. <b>On demand suits a battery or solar host</b> serving one
          listener from a dongle — far less work, and the watts matter more than the headroom.</div></label>
      <div class="hint" id="govNow"></div>
    </div>

    <div class="card">
      <h2>Audio and power</h2>
      <p class="why">Applies in both modes.</p>
      <label><span class="lbl">Uncompressed audio</span>
        <select id="uncompressed">
          <option value="0">Off — everyone gets Opus</option>
          <option value="1">Listener's choice</option>
          <option value="2">Only as a fallback for old browsers</option>
        </select>
        <div class="hint">Raw audio is about twenty times the bandwidth of Opus, out of your
          upload.</div></label>
      <label style="display:flex;align-items:center;gap:10px;margin-top:16px">
        <input type="checkbox" id="forceIdle" style="width:16px;height:16px;accent-color:var(--amber)">
        <span>Listeners may not switch off idle power saving</span></label>
    </div>

    <div class="err" id="saveErr"></div>
  </div>
</div>

<div class="bar hide" id="bar">
  <span class="spacer" id="barMsg"></span>
  <button id="saveBtn">Save and start</button>
</div>

<script>
"use strict";
const $ = id => document.getElementById(id);
let AUTH = "";          // vs_admin_nonce=…&vs_admin_auth=… for the current request
let PASS = "";
let cfg  = null;

// ── ★★★ PURE-JS HMAC-SHA256. crypto.subtle IS NOT AVAILABLE HERE AND MUST NOT BE USED.
//    A VibeServer is plain http:// on a LAN IP, which is NOT a secure context, so crypto.subtle
//    is undefined there — but localhost IS a secure context, so a version using it works
//    perfectly on the dev machine and fails on every real Pi, which is the worst shape a bug can
//    have. scripts/build-web.mjs BANS the API outright for exactly this reason and says so at
//    length; the first draft of this page reached for it anyway.
//    Same algorithm as src/services/vibeAuth.ts, which exists for this same reason.
const K=new Uint32Array([0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
const rotr=(x,n)=>(x>>>n)|(x<<(32-n));
function sha256(msg){
  const H=new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
  const ml=msg.length*8, blocks=((msg.length+8)>>6)+1, buf=new Uint8Array(blocks*64);
  buf.set(msg); buf[msg.length]=0x80;
  const dv=new DataView(buf.buffer);
  dv.setUint32(buf.length-4, ml>>>0);
  dv.setUint32(buf.length-8, Math.floor(ml/0x100000000));
  const w=new Uint32Array(64);
  for(let off=0; off<buf.length; off+=64){
    for(let i=0;i<16;i++) w[i]=dv.getUint32(off+i*4);
    for(let i=16;i<64;i++){
      const s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>>3);
      const s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>>10);
      w[i]=(w[i-16]+s0+w[i-7]+s1)>>>0;
    }
    let a=H[0],b=H[1],c=H[2],d=H[3],e=H[4],f=H[5],g=H[6],h=H[7];
    for(let i=0;i<64;i++){
      const S1=rotr(e,6)^rotr(e,11)^rotr(e,25), ch=(e&f)^(~e&g);
      const t1=(h+S1+ch+K[i]+w[i])>>>0;
      const S0=rotr(a,2)^rotr(a,13)^rotr(a,22), maj=(a&b)^(a&c)^(b&c);
      const t2=(S0+maj)>>>0;
      h=g;g=f;f=e;e=(d+t1)>>>0;d=c;c=b;b=a;a=(t1+t2)>>>0;
    }
    H[0]=(H[0]+a)>>>0;H[1]=(H[1]+b)>>>0;H[2]=(H[2]+c)>>>0;H[3]=(H[3]+d)>>>0;
    H[4]=(H[4]+e)>>>0;H[5]=(H[5]+f)>>>0;H[6]=(H[6]+g)>>>0;H[7]=(H[7]+h)>>>0;
  }
  const out=new Uint8Array(32), odv=new DataView(out.buffer);
  for(let i=0;i<8;i++) odv.setUint32(i*4,H[i]);
  return out;
}
const cat=(a,b)=>{const o=new Uint8Array(a.length+b.length);o.set(a);o.set(b,a.length);return o;};
function hmacSha256(key,msg){
  const k=key.length>64?sha256(key):key;
  const blk=new Uint8Array(64); blk.set(k);
  const ip=new Uint8Array(64), op=new Uint8Array(64);
  for(let i=0;i<64;i++){ip[i]=blk[i]^0x36;op[i]=blk[i]^0x5c;}
  return sha256(cat(op, sha256(cat(ip,msg))));
}
const bytesOf=s=>new Uint8Array(Array.from(s, c=>c.charCodeAt(0)&0xff));
const toHex=b=>Array.from(b, x=>x.toString(16).padStart(2,"0")).join("");

// The server issues a single-use nonce; we return HMAC-SHA256(password, nonce). The password
// itself never crosses the wire, so each request fetches a fresh nonce.
async function authQuery() {
  const r = await fetch("/vibeserver/auth", {cache:"no-store"});
  const nonce = (await r.json()).nonce;
  const tok = toHex(hmacSha256(bytesOf(PASS), bytesOf(nonce)));
  return `vs_admin_nonce=${encodeURIComponent(nonce)}&vs_admin_auth=${tok}`;
}

// ── The .local address, DERIVED THE SAME WAY THE SERVER DOES IT. Shown live as the user types so
//    the address they are given is the address that works — see mdnsLabel() in vibeserver_config.
//    ★ This is a PREVIEW. The name actually held is confirmed by the server after it restarts,
//      because two VibeServers on one network cannot both hold the same label.
function mdnsLabel(s) {
  let out = "", dash = false;
  for (const ch of (s || "")) {
    if (/[a-z0-9]/i.test(ch)) { out += ch.toLowerCase(); dash = false; }
    else if (out && !dash) { out += "-"; dash = true; }
  }
  out = out.replace(/-+$/, "");
  return (out || "vibeserver").slice(0, 63).replace(/-+$/, "");
}

function coverage() {
  const f = parseFloat($("lockFreq").value) * 1e3, r = parseFloat($("rate").value);
  if (!(f > 0 && r > 0)) { $("coverage").textContent = ""; return; }
  const lo = (f - r/2) / 1e6, hi = (f + r/2) / 1e6;
  $("coverage").textContent = `Listeners will be able to tune ${lo.toFixed(3)} – ${hi.toFixed(3)} MHz.`;
}

// ★ Say what the listener cap COSTS, because the number is meaningless on its own. Measured on a
//   Pi: ~0.2 Mb/s per listener at the default width, and roughly four times that if uncompressed
//   audio is enabled — which is why the two are shown together.
function bwNote() {
  const n = parseInt($("users").value || "1", 10);
  const raw = $("uncompressed").value !== "0";
  const per = raw ? 2.0 : 0.2;
  $("bwNote").innerHTML = `About <b>${(n*per).toFixed(1)} Mb/s</b> of your upload with ${n} `
    + `listener${n===1?"":"s"} connected` + (raw
      ? `, because uncompressed audio is switched on. That is roughly ten times the compressed cost.`
      : `.`);
}

function setMode(locked) {
  cfg.mode = locked ? "locked" : "single";
  $("modeLocked").classList.toggle("sel", locked);
  $("modeSingle").classList.toggle("sel", !locked);
  $("lockedOnly").classList.toggle("hide", !locked);
}

// ── The hardware panel, BRANCHED ON THE DRIVER ────────────────────────────────────────────────
// ★★★ The three supported radios do not share a gain model, so there is no "gain" control that is
//     honest on all of them: RTL has a discrete gain LIST, the Airspy HF+ has no variable gain at
//     all (an attenuator and a preamp), the SDRplay RSP works in IF gain REDUCTION. Draw the right
//     one, or draw NONE — never a control that quietly does nothing, because a user reading a dead
//     slider concludes the FEATURE is broken, not that it is the wrong control for their radio.
// ★★ And if we cannot tell what is plugged in, we say exactly that and offer nothing. A guess here
//    is worse than a blank: it would be a control that appears to work.
async function renderHw() {
  let hw = null;
  try { hw = await (await fetch("/vibeserver/hardware", {cache:"no-store"})).json(); } catch (e) {}
  const el = $("hw");
  // ★ Show what the processor is ACTUALLY doing, next to the control that asks for it.
  const gv = document.getElementById("govNow");
  if (gv && hw && hw.governor) {
    const mhz = hw.cpuKHz ? ` at ${(hw.cpuKHz/1000).toFixed(0)} MHz` : "";
    gv.textContent = `Currently: ${hw.governor}${mhz}.`;
    gv.style.color = hw.governor === "performance" ? "var(--good)" : "var(--dim)";
  }
  if (!hw || !hw.present) {
    el.innerHTML = `<p class="hint">No radio detected, so there is nothing to set here.
      Plug one in and reload this page.</p>`;
    return;
  }
  if (hw.driver === "sdrplay") {
    el.innerHTML = `
      <label style="display:flex;align-items:center;gap:10px;margin:0">
        <input type="checkbox" id="rfNotch" style="width:16px;height:16px;accent-color:var(--amber)">
        <span>Broadcast notch</span></label>
      <div class="hint">Removes the MW <em>and</em> FM broadcast bands before the tuner. Use it on
        HF or airband if a local transmitter is overloading the front end.
        <b>Never use it to listen to FM</b> &mdash; it removes exactly that.</div>
      <label style="display:flex;align-items:center;gap:10px;margin-top:14px">
        <input type="checkbox" id="dabNotch" style="width:16px;height:16px;accent-color:var(--amber)">
        <span>DAB notch</span></label>
      <div class="hint">Same idea, for the DAB band.</div>
      <div class="note">This RSP sets its own gain automatically, and listeners can see it but not
        change it while the range is locked.</div>`;
  } else if (hw.driver === "airspyhf") {
    el.innerHTML = `<p class="hint">The Airspy HF+ has no variable gain &mdash; it manages its own
      attenuator and preamp &mdash; so there is nothing to set here.</p>`;
  } else {
    const opts = (hw.gains || []).map(g =>
      `<option value="${g}">${(g/10).toFixed(1)} dB</option>`).join("");
    el.innerHTML = `
      <label><span class="lbl">Tuner gain</span>
        <select id="gain"><option value="-1">Automatic</option>${opts}</select>
        <div class="hint">Automatic suits most aerials. Fix it only if you know you need to.</div>
      </label>`;
  }
  // Restore stored values into whichever controls we just drew.
  if ($("rfNotch")) $("rfNotch").checked = !!cfg.rfNotch;
  if ($("dabNotch")) $("dabNotch").checked = !!cfg.dabNotch;
  if ($("gain")) $("gain").value = String(cfg.gain != null ? cfg.gain : -1);
}

function fill() {
  $("name").value = cfg.name || "";
  $("place").value = cfg.place || "";
  $("country").value = cfg.country || "";
  $("locator").value = cfg.locator || "";
  $("lat").value = cfg.lat || "";
  $("lon").value = cfg.lon || "";
  // ★ Default ON for a shared receiver. The 1024-bin window only stays sharp BECAUSE of the zoom
  //   resampling — without it, deep zoom interpolates and looks blocky, which is what a listener
  //   reads as a poor receiver. Off by default was right when this was experimental; it is not
  //   right for the model a shared server is built on.
  $("zoomSpectrum").checked = cfg.zoomSpectrum !== false;
  $("mdns").checked = cfg.mdnsAdvertise !== false;
  $("lockFreq").value = Math.round((cfg.lockFreq || cfg.freq || 0) / 1e3);
  $("rate").value = cfg.rate || 2400000;
  $("landingFreq").value = ((cfg.landingFreq || cfg.freq || 0) / 1e3).toFixed(1);
  $("demodMode").value = cfg.demodMode || "am";
  $("users").value = cfg.users || 1;
  $("sessionLimit").value = cfg.sessionLimitMin || 0;
  $("uncompressed").value = String(cfg.uncompressed || 0);
  $("cpuGovernor").value = cfg.cpuGovernor || "performance";
  $("forceIdle").checked = !!cfg.forceIdleSaver;
  setMode((cfg.mode || "single") === "locked");
  addr(); coverage(); bwNote(); renderHw();
}

function addr() {
  const on = $("mdns").checked;
  const label = mdnsLabel($("name").value);
  $("addrLine").innerHTML = on
    ? `You will be able to reach this server at <span class="addr">${label}.local</span>`
      + ` &mdash; and always at <span class="addr">${location.host}</span>.`
    : `Discovery is off, so this server will only be reachable at`
      + ` <span class="addr">${location.host}</span>.`;
}

function collect() {
  const locked = cfg.mode === "locked";
  return {
    mode: cfg.mode,
    name: $("name").value.trim(),
    place: $("place").value.trim(),
    country: $("country").value.trim().toUpperCase(),
    locator: $("locator").value.trim(),
    lat: $("lat").value.trim(),
    lon: $("lon").value.trim(),
    zoomSpectrum: $("zoomSpectrum").checked,
    mdnsAdvertise: $("mdns").checked,
    mdnsName: $("name").value.trim(),
    lockFreq: locked ? Math.round(parseFloat($("lockFreq").value || "0") * 1e3) : 0,
    rate: parseFloat($("rate").value || "2400000"),
    landingFreq: locked ? Math.round(parseFloat($("landingFreq").value || "0") * 1e3) : 0,
    demodMode: $("demodMode").value,
    users: locked ? parseInt($("users").value || "1", 10) : 1,
    sessionLimitMin: locked ? parseInt($("sessionLimit").value || "0", 10) : 0,
    uncompressed: parseInt($("uncompressed").value, 10),
    cpuGovernor: $("cpuGovernor").value,
    forceIdleSaver: $("forceIdle").checked,
    // ★ Only send what this radio actually has a control for. Posting rfNotch for an Airspy would
    //   be storing a setting that can never apply — the config would describe a radio we are not.
    ...($("rfNotch")  ? {rfNotch:  $("rfNotch").checked}  : {}),
    ...($("dabNotch") ? {dabNotch: $("dabNotch").checked} : {}),
    ...($("gain")     ? {gain: parseInt($("gain").value, 10)} : {})
  };
}

$("signinBtn").onclick = async () => {
  PASS = $("pass").value;
  $("signinErr").textContent = "";
  if (!PASS) { $("signinErr").textContent = "Enter the admin password."; return; }
  try {
    const r = await fetch("/vibeserver/config?" + await authQuery(), {cache:"no-store"});
    if (r.status === 401) { $("signinErr").textContent = "That password was not accepted."; return; }
    if (!r.ok) { $("signinErr").textContent = "Server error (" + r.status + ")."; return; }
    cfg = await r.json();
    // ★ Same page, two jobs. Say which one you are doing — "Save and start" on a receiver that
    //   is already running would read as if it were about to do something drastic.
    if (cfg.configured) {
      document.querySelector("#setup .sub").textContent =
        "Change how this receiver is set up. Saving restarts it, so listeners will reconnect.";
      $("saveBtn").textContent = "Save changes";
    }
    $("signin").classList.add("hide");
    $("setup").classList.remove("hide");
    $("bar").classList.remove("hide");
    fill();
  } catch (e) { $("signinErr").textContent = "Could not reach the server."; }
};
$("pass").addEventListener("keydown", e => { if (e.key === "Enter") $("signinBtn").click(); });

$("modeSingle").onclick = () => setMode(false);
$("modeLocked").onclick = () => setMode(true);
$("name").addEventListener("input", addr);
$("mdns").addEventListener("change", addr);
for (const id of ["lockFreq","rate"]) $(id).addEventListener("input", coverage);
for (const id of ["users","uncompressed"]) $(id).addEventListener("input", bwNote);

$("saveBtn").onclick = async () => {
  $("saveErr").textContent = "";
  $("saveBtn").disabled = true;
  $("barMsg").textContent = "Saving…";
  try {
    const r = await fetch("/vibeserver/config?" + await authQuery(),
                          {method:"POST", body: JSON.stringify(collect())});
    const j = await r.json().catch(() => ({}));
    if (!r.ok) {
      $("saveErr").textContent = j.error || ("Save failed (" + r.status + ").");
      $("barMsg").textContent = "";
      $("saveBtn").disabled = false;
      return;
    }
    // ★ The server restarts to apply this, so the honest thing is to say so and then WAIT for it
    //   to come back rather than reloading into a connection error.
    $("barMsg").innerHTML = '<span class="ok">Saved. Restarting the receiver…</span>';
    const waitBack = async () => {
      for (let i = 0; i < 60; i++) {
        await new Promise(r => setTimeout(r, 1000));
        try {
          const s = await fetch("/vibeserver.json", {cache:"no-store"});
          if (s.ok && (await s.json()).configured) { location.reload(); return; }
        } catch (e) { /* still down — expected */ }
      }
      $("barMsg").textContent = "Saved, but the server has not come back. Check it on the machine.";
      $("saveBtn").disabled = false;
    };
    waitBack();
  } catch (e) {
    $("saveErr").textContent = "Could not reach the server.";
    $("saveBtn").disabled = false;
    $("barMsg").textContent = "";
  }
};
</script>
)HTML";
