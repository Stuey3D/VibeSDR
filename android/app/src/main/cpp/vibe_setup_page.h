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
    <p class="sub">This server is not configured yet.</p>
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
        <h2>Available modes</h2>
        <p class="why">Untick anything you do not want offered. There is no point in wide FM on an
           HF receiver, and WFM stereo is the most expensive thing a listener can switch on.</p>
        <div class="checks" id="modeChecks"></div>
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

// ── Auth. The server issues a nonce; we return HMAC-SHA256(password, nonce) in hex. The password
//    itself never crosses the wire, and a nonce is single-use — so each request fetches a fresh one.
async function authQuery() {
  const r = await fetch("/vibeserver/auth", {cache:"no-store"});
  const nonce = (await r.json()).nonce;
  const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(PASS),
                {name:"HMAC", hash:"SHA-256"}, false, ["sign"]);
  const sig = await crypto.subtle.sign("HMAC", key, new TextEncoder().encode(nonce));
  const hex = [...new Uint8Array(sig)].map(b => b.toString(16).padStart(2,"0")).join("");
  return `vs_admin_nonce=${encodeURIComponent(nonce)}&vs_admin_auth=${hex}`;
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

const DEMODS = [["am","AM"],["lsb","LSB"],["usb","USB"],["nfm","NFM"],["wfm","WFM (wide)"],
                ["cw","CW"],["rdsx","Advanced RDS"],["ft8","FT8"],["wefax","WEFAX"],
                ["sstv","SSTV"],["rtty","RTTY"]];

function renderChecks() {
  $("modeChecks").innerHTML = DEMODS.map(([id,label]) =>
    `<label><input type="checkbox" data-demod="${id}" checked><span>${label}</span></label>`).join("");
  // ★ Advanced RDS rides on WFM. Unticking WFM must take RDS with it rather than leaving an
  //   orphan that can never fire — an option that cannot work is worse than one that is absent.
  const wfm = document.querySelector('[data-demod="wfm"]');
  const rds = document.querySelector('[data-demod="rdsx"]');
  const sync = () => { if (!wfm.checked) { rds.checked = false; } rds.disabled = !wfm.checked; };
  wfm.addEventListener("change", sync);
  sync();
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
  $("mdns").checked = cfg.mdnsAdvertise !== false;
  $("lockFreq").value = Math.round((cfg.lockFreq || cfg.freq || 0) / 1e3);
  $("rate").value = cfg.rate || 2400000;
  $("landingFreq").value = ((cfg.landingFreq || cfg.freq || 0) / 1e3).toFixed(1);
  $("demodMode").value = cfg.demodMode || "am";
  $("users").value = cfg.users || 1;
  $("sessionLimit").value = cfg.sessionLimitMin || 0;
  $("uncompressed").value = String(cfg.uncompressed || 0);
  $("forceIdle").checked = !!cfg.forceIdleSaver;
  renderChecks();
  for (const b of (cfg.blocked || [])) {
    const el = document.querySelector(`[data-demod="${b}"]`);
    if (el) el.checked = false;
  }
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
  const blocked = DEMODS.map(([id]) => id)
    .filter(id => !document.querySelector(`[data-demod="${id}"]`).checked);
  const locked = cfg.mode === "locked";
  return {
    mode: cfg.mode,
    name: $("name").value.trim(),
    mdnsAdvertise: $("mdns").checked,
    mdnsName: $("name").value.trim(),
    lockFreq: locked ? Math.round(parseFloat($("lockFreq").value || "0") * 1e3) : 0,
    rate: parseFloat($("rate").value || "2400000"),
    landingFreq: locked ? Math.round(parseFloat($("landingFreq").value || "0") * 1e3) : 0,
    demodMode: $("demodMode").value,
    users: locked ? parseInt($("users").value || "1", 10) : 1,
    sessionLimitMin: locked ? parseInt($("sessionLimit").value || "0", 10) : 0,
    uncompressed: parseInt($("uncompressed").value, 10),
    forceIdleSaver: $("forceIdle").checked,
    blocked: locked ? blocked : [],
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
