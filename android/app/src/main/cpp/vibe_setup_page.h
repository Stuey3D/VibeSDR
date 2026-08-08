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
  /* ★ An ANCHOR styled as the primary button: it is a navigation, so it should be a real link
     (middle-click, open in a new tab, copy the address) rather than a button running location=. */
  a.gotoBtn{background:var(--amber);color:#1a1200;border-radius:8px;padding:9px 18px;
            font:600 14px/1 inherit;text-decoration:none;display:inline-block;white-space:nowrap}
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

    <!-- ★★★ ONE TAB PER RADIO. Hidden entirely when there is only one, so a single-radio server's
         setup page is exactly the page it has always been. Each tab is the whole settings form for
         that radio; what the MACHINE shares — where it is, its name, the admin password — is the
         same on every tab and stored once. -->
    <!-- ★ SAY WHAT THEY ARE. Three unlabelled buttons reading HF / FM / VHF look like BAND
         buttons, not radios — which is exactly how they were read (Stuart, 2026-08-08: "3 random
         buttons at the top ... nothing to do with the radios"). A heading costs one line. -->
    <div id="radioTabsWrap" class="hide" style="margin:16px 0 10px">
      <div style="font-weight:600;letter-spacing:.06em;font-size:12px;opacity:.8">RADIOS ON THIS MACHINE</div>
      <div id="radioTabHint" class="sub" style="margin:2px 0 8px;font-size:12px;opacity:.75"></div>
      <div id="radioTabs" style="display:flex;gap:6px;flex-wrap:wrap"></div>
      <div id="hwScope" class="hide sub" style="margin-top:8px;font-size:12px;opacity:.8"></div>
    </div>

    <div class="card">
      <h2>On your network</h2>
      <p class="why">How people find this server once it is running.</p>
      <label style="display:flex;align-items:center;gap:10px;margin:0">
        <input type="checkbox" id="mdns" checked style="width:16px;height:16px;accent-color:var(--amber)">
        <span>Let VibeSDR apps on this network discover this server automatically</span>
      </label>
      <!-- ★★ PLACEHOLDERS ARE EXAMPLES, NOT SOMEBODY'S ACTUAL RECEIVER. These carried the
           development Pi's real name, town and locator ("VibeServer: Pi500", "Northampton",
           "IO92nh") — so every new owner was shown the author's home location as the suggestion,
           and anyone who accepted the hint without thinking would publish it as their own
           (Stuart, 2026-08-06, before going live).
           ★ A placeholder still has to TEACH THE FORMAT — that a locator is six characters, that
             a name can describe the antenna — so these are plausible and clearly generic, not
             blanked out. -->
      <label><span class="lbl">Name</span>
        <input type="text" id="name" placeholder="e.g. Coastal SDR — 60 m vertical"></label>
      <div class="hint" id="addrLine"></div>
    </div>

    <div class="card">
      <h2>Where this receiver is</h2>
      <p class="why">Published to listeners. It sets the flag and the ITU band plan, centres the
         map, and is what lets RDS name a station's country — with no location set, every station
         shows a blank country.</p>
      <div class="row">
        <label><span class="lbl">Town or area</span>
          <input type="text" id="place" placeholder="e.g. Cardiff"></label>
        <label><span class="lbl">Country code</span>
          <input type="text" id="country" maxlength="2" placeholder="GB"></label>
      </div>
      <label><span class="lbl">Maidenhead locator</span>
        <input type="text" id="locator" maxlength="8" placeholder="e.g. IO81jm">
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

    <!-- ★★★ EVERY RADIO HAS THESE, whichever mode it is in. They used to sit inside the
         shared-mode block, which was right when a server had ONE radio: single-user mode means
         the listener owns the dial, so the owner set nothing. With a tab per radio it is wrong —
         an Airspy dedicated to FM needs a landing frequency of 96.6 and WFM whether or not its
         window is locked (Stuart, 2026-08-08: "no sample rate, no landing frequency/demodulator"). -->
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

    <!-- ★★★ THE PROTECTED CONTROLS, PER RADIO AND PER DRIVER.
         These are what the admin password exists to guard — the ones that leave a receiver broken
         for the next person. With several radios they must be settable on the radio you MEAN,
         not only on whichever happens to be running.
         ★★ Drawn per driver, never all at once: an Airspy has no bias-T and an RTL has no PPB, and
            offering a control whose every use is a no-op is the fault this project already has a
            rule about — either branch on the driver or leave it out. -->
    <div class="card" id="radioHwCard">
      <h2>This radio's hardware</h2>
      <p class="why">Settings that stay in the radio itself. Listeners never get these.</p>
      <!-- ★ EVERY radio has one, so it is not locked-mode-only. It was, which is why an Airspy
           being set up for FM had no way to choose 768 kSPS. -->
      <label><span class="lbl">Span (sample rate)</span>
        <select id="rate"></select></label>
      <div id="hwBiasT" class="hide">
        <label style="display:flex;gap:8px;align-items:center">
          <input type="checkbox" id="biasT" style="width:16px;height:16px;accent-color:var(--amber)">
          <span>Bias-T — put DC on the feedline to power an antenna amplifier</span></label>
        <p class="why" style="color:var(--warn)">Only switch this on if you know what is connected.
           It can damage equipment that is not expecting it.</p>
      </div>
      <div id="hwPpm" class="hide">
        <label><span class="lbl">Frequency correction (ppm)</span>
          <input type="number" id="ppm" step="1" placeholder="0"></label>
        <p class="why">Corrects a dongle whose crystal is slightly off. 0 unless you have measured it.</p>
      </div>
      <!-- ★★★ WHAT THE LANDING PAGE MEASURES FROM. Offered only to a radio with a FIXED window:
           a receiver a listener can retune contributes a smear with a hole in it every time
           somebody uses it, and a radio that releases when idle is letting go of the device at
           exactly the moment the picture would be drawn. -->
      <div id="hwSpectro" class="hide" style="margin-top:10px">
        <label style="display:flex;gap:8px;align-items:center">
          <input type="checkbox" id="spectrogram" style="width:16px;height:16px;accent-color:var(--amber)">
          <span>Use this radio for the spectrogram and band conditions</span></label>
        <p class="why" id="spectroWhy">The landing page's background, and the measured half of the
           band conditions table — both read the same window, so one radio provides both.</p>
      </div>
      <div id="hwRelease" style="margin-top:10px">
        <label style="display:flex;gap:8px;align-items:center">
          <input type="checkbox" id="releaseWhenIdle" style="width:16px;height:16px;accent-color:var(--amber)">
          <span>Let another program use this radio when nobody is listening</span></label>
        <p class="why">For a machine that shares one SDR with something else — OpenWebRX, a
           decoder. VibeServer keeps running and takes the radio back the moment a listener
           arrives; if the other program still has it, they are told so plainly.
           <strong>It stops this radio contributing the spectrogram and band conditions</strong>,
           because it spends its idle time letting the device go.</p>
      </div>
      <div id="hwPpb" class="hide">
        <label><span class="lbl">Calibration (ppb)</span>
          <input type="number" id="ppb" step="1" placeholder="0"></label>
        <p class="why">The Airspy HF+ is calibrated in parts per BILLION. 0 unless you have measured it.</p>
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
          <!-- moved out of the locked-only block: every radio has a sample rate -->
        </div>
        <div class="hint" id="coverage"></div>
        <div class="note" id="eibiNote" style="margin-top:18px">
          <b>Shortwave schedule (EiBi)</b>
          <div class="hint" id="eibiState">checking…</div>
          <div class="hint">Who is broadcasting, where and when — so the search box on this
            receiver finds stations by name instead of only by frequency. The list is fetched by
            the server, because a browser is not allowed to fetch it directly, and it refreshes
            itself once a day.</div>
          <button type="button" id="eibiGet" class="ghost" style="margin-top:10px">
            Download now</button>
        </div>
        <label style="display:flex;align-items:center;gap:10px;margin-top:16px">
          <input type="checkbox" id="zoomSpectrum" checked
                 style="width:16px;height:16px;accent-color:var(--amber)">
          <span>Keep the spectrum sharp when zoomed in</span></label>
        <div class="hint">Recomputes real detail as listeners zoom, instead of magnifying what is
          already on screen. Without it a close-in view goes blocky. Costs a little CPU and is
          what makes a shared receiver worth zooming into.</div>
      </div>

      

      <div class="card">
        <h2>Listeners</h2>
        <p class="why">How many people at once.</p>
        <div class="row">
          <label><span class="lbl">Maximum listeners</span>
            <input type="number" id="users" min="1" max="50"></label>
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

    <!-- ★★★ A TIME LIMIT IS NOT A SHARED-MODE IDEA. This lived inside the locked-only block with
         "maximum listeners", which is where it looks like it belongs — and it is exactly backwards.
         A shared radio gives everyone their own VFO, so a listener who stays all day costs the
         others nothing. A SINGLE-USER radio is the whole radio: one person holds the hardware and
         everybody else sits in the queue behind them. That is the case that needs a limit, and it
         was the one case where the control could not be reached (Stuart, 2026-08-08: "no time
         limits for the single user radios").
         ★ Maximum listeners stays locked-only: a single-user radio is one by definition. -->
    <div class="card">
      <h2>Time limit</h2>
      <p class="why">How long one listener may keep this radio.</p>
      <label><span class="lbl">Minutes (0 = no limit)</span>
        <input type="number" id="sessionLimit" min="0">
        <div class="hint">When the limit is reached the listener is warned, then disconnected, and
          the next person in the queue gets their turn. <b>On a single-user radio this is the only
          thing that keeps one listener from holding the hardware all day</b> — there is one tuner
          and whoever has it has all of it. On a shared radio it matters far less: everyone already
          has their own VFO, so a long session costs nobody else anything.
          <br>The owner is exempt, and so is anything connecting from this machine.</div></label>
    </div>

    <div class="card">
      <h2>Behind a reverse proxy</h2>
      <p class="why">Only fill this in if something sits in front of VibeServer.</p>
      <label><span class="lbl">Trusted proxy addresses</span>
        <input id="trustedProxies" placeholder="e.g. 127.0.0.1, 10.0.0.0/8">
        <div class="hint">If you run VibeServer behind nginx, Caddy or a tunnel, every listener
          arrives from the <em>proxy</em>, so they all share one address: a single ban blocks
          everybody, every visitor shows the same country, and one wrong admin password can lock
          out the world. Naming the proxy here lets the server read the real address from the
          <code>X-Forwarded-For</code> header instead.
          <br><b>Leave it empty unless you have a proxy.</b> That header is just text the client
          sends, so a server that believed it from anyone would let a stranger claim any address
          they liked and walk straight through the ban list. Nothing is read from it until you
          name the proxy you trust. Addresses or ranges, separated by commas.</div></label>
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

<!-- ★★ TWO ACTIONS, AND THEY ARE NOT THE SAME. Saving a radio's tab writes that radio's settings
     and leaves everyone listening exactly where they are. Restarting the server applies everything
     and drops every listener on every radio — so it is its own control, in its own row, and it
     says what it does. Stuart, 2026-08-07: "at the bottom of each tab is a save radio settings
     button. Underneath that in its own distinct footer section a save and reboot server button". -->
<div class="bar hide" id="bar" style="flex-wrap:wrap;row-gap:8px">
  <span class="spacer" id="barMsg"></span>
  <button id="saveRadioBtn" class="hide" style="background:transparent;border:1px solid var(--amber);color:var(--amber)">Save radio settings</button>
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
// ★★★ AN ADMIN WHO JUST SIGNED IN MUST NOT BE ASKED AGAIN. Arriving here from the landing page's
//     SETUP button is a fresh page load, so the password they typed a second ago is gone with the
//     page that held it — and they were made to type it twice (Stuart, 2026-08-08: "setup works
//     but I have to enter the admin password again"). The landing page passes a TICKET instead,
//     which every process on this machine accepts (vibe_admin_ticket.h).
// ★★ Taken from the URL and then REMOVED from it with replaceState: an admin credential has no
//    business sitting in the address bar, the history, or a copied link.
const TICKET = (() => {
  const t = new URLSearchParams(location.search).get("vs_admin_ticket") || "";
  if (t) history.replaceState(null, "", location.pathname);
  return t;
})();

async function authQuery() {
  if (TICKET) return `vs_admin_ticket=${encodeURIComponent(TICKET)}`;
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
  radio().mode = locked ? "locked" : "single";   // ★ the OPEN tab, not the machine
  $("modeLocked").classList.toggle("sel", locked);
  $("modeSingle").classList.toggle("sel", !locked);
  $("lockedOnly").classList.toggle("hide", !locked);
  syncSpectroOffer();
}

/** ★★★ THE OFFER FOLLOWS THE RULE, LIVE. A radio only qualifies while its window is FIXED and it
 *  is not letting the device go when idle — so the control appears and disappears as those two are
 *  changed, rather than being offered and then quietly ignored by the server.
 *  ★ Ticking it and having it silently not apply is the worst of the three options: the owner
 *    would believe they had a spectrogram and never know why the page was empty. */
function syncSpectroOffer() {
  const wrap = $("hwSpectro"); if (!wrap) return;
  const locked  = radio().mode === "locked";
  const release = $("releaseWhenIdle") && $("releaseWhenIdle").checked;
  const may = locked && !release;
  wrap.classList.toggle("hide", !may);
  if (!may && $("spectrogram")) $("spectrogram").checked = false;
  const why = $("spectroWhy");
  if (why && may) {
    why.textContent = "The landing page's background, and the measured half of the band conditions"
                    + " table — both read the same window, so one radio provides both.";
  }
}

// ── The hardware panel, BRANCHED ON THE DRIVER ────────────────────────────────────────────────
// ★★★ The three supported radios do not share a gain model, so there is no "gain" control that is
//     honest on all of them: RTL has a discrete gain LIST, the Airspy HF+ has no variable gain at
//     all (an attenuator and a preamp), the SDRplay RSP works in IF gain REDUCTION. Draw the right
//     one, or draw NONE — never a control that quietly does nothing, because a user reading a dead
//     slider concludes the FEATURE is broken, not that it is the wrong control for their radio.
// ★★ And if we cannot tell what is plugged in, we say exactly that and offer nothing. A guess here
//    is worse than a blank: it would be a control that appears to work.
/** ★ The schedule is the SERVER's job, so this only reports and triggers. Failure is reported in
 *  full rather than as "failed": the likely causes are no internet and a season file that has not
 *  been published yet, and those need different actions from the owner. */
/** The server instance this page was loaded from. Captured once, compared after a restart. */
let BOOT_ID = "";
(async () => {
  try { BOOT_ID = (await (await fetch("/vibeserver.json", {cache:"no-store"})).json()).instance || ""; }
  catch (e) { /* the compare simply falls back to the timed rule */ }
})();

async function eibiStatus() {
  const el = $("eibiState");
  if (!el) return;
  try {
    const r = await fetch("/vibeserver/eibi", { cache: "no-store" });
    const j = await r.json();
    el.textContent = j.entries
      ? `${j.entries.toLocaleString()} entries, updated ${j.updated || "unknown"}.`
      : "Not downloaded yet — this receiver's search will find frequencies but not station names.";
    el.style.color = j.entries ? "var(--good)" : "var(--dim)";
  } catch { el.textContent = "This server is too old to fetch the schedule."; }
}

async function eibiFetch() {
  const el = $("eibiState"), b = $("eibiGet");
  el.textContent = "downloading…"; el.style.color = "var(--dim)";
  b.disabled = true;
  try {
    // ★ Admin-gated: a download is CPU, bandwidth and a write to /var/lib on the owner's
    //   machine, triggerable by anyone who can reach the port if it were not.
    const r = await fetch("/vibeserver/eibi?refresh=1&" + (await authQuery()), { cache: "no-store" });
    const j = await r.json();
    if (j.entries) { el.textContent = `${j.entries.toLocaleString()} entries, updated ${j.updated}.`;
                     el.style.color = "var(--good)"; }
    else { el.textContent = j.error || "could not download the schedule"; el.style.color = "var(--bad)"; }
  } catch (e) { el.textContent = "could not reach the server"; el.style.color = "var(--bad)"; }
  b.disabled = false;
}

/** ★★★ WHAT THIS RADIO CAN DO — AND IT IS NOT ALWAYS THE ONE WE ARE TALKING TO.
 *
 *  /vibeserver/hardware answers for the radio THIS PROCESS serves. With a tab per radio that is
 *  wrong for every tab but one: opening the Airspy's tab on the RSP's process would offer the
 *  RSP's sample rates, and the owner would pick one the Airspy cannot do. It would then fail at
 *  the hardware and read as a broken receiver.
 *
 *  ★★ SO ASK THE RADIO ITSELF WHERE WE CAN, AND FALL BACK TO WHAT ITS DRIVER CAN DO WHERE WE
 *     CANNOT. A radio that is not configured yet is not running at all, so there is nobody to ask
 *     — and that is exactly when the owner is setting it up. The driver tables below are the
 *     honest answer for that case: they are properties of the MODEL, not of a live handle.
 *  ★ Rates measured from the radios themselves, not copied from a datasheet: an HF+ Discovery
 *    ADVERTISES seven rates and implements three (see the airspyhf notes in the shim).
 */
const DRIVER_HW = {
  rtl:      { rates: [250000, 1024000, 1536000, 1792000, 1920000, 2048000, 2160000, 2400000],
              biasT: true,  rfNotch: false, lnaState: false },
  rtlsdr:   { rates: [250000, 1024000, 1536000, 1792000, 1920000, 2048000, 2160000, 2400000],
              biasT: true,  rfNotch: false, lnaState: false },
  airspyhf: { rates: [768000, 456000, 228000],
              biasT: false, rfNotch: false, lnaState: false },
  sdrplay:  { rates: [2000000, 3000000, 4000000, 5000000, 6000000, 8000000, 10000000],
              biasT: true,  rfNotch: true,  lnaState: true },
};

async function renderHw() {
  let hw = null;
  const r = radio();
  // Is the tab we are looking at the radio this process is actually running?
  let mine = true;
  try {
    const dir = await (await fetch("/vibeserver/radios", {cache:"no-store"})).json();
    const me = (dir.radios || []).find(x => x.mine);
    if (me && r.serial) mine = (me.serial === r.serial);
  } catch (e) { /* single-radio server: it is always ours */ }

  if (mine) {
    try { hw = await (await fetch("/vibeserver/hardware", {cache:"no-store"})).json(); } catch (e) {}
  } else {
    // ★ Ask that radio's own process through the front door, if it is running. If it is not — the
    //   usual case while setting one up — fall back to what its driver can do.
    try {
      hw = await (await fetch(`/r/${encodeURIComponent(r.serial)}/vibeserver/hardware`,
                              {cache:"no-store"})).json();
    } catch (e) { hw = null; }
    if (!hw || !hw.rates || !hw.rates.length) {
      const d = DRIVER_HW[r.driver] || DRIVER_HW.rtl;
      hw = { driver: r.driver, present: false, rates: d.rates, gains: [],
             biasT: d.biasT, rfNotch: d.rfNotch, offline: true };
    }
  }
  const el = $("hw");
  // ★★ SAY WHICH RADIO THESE OPTIONS BELONG TO when it is not the one running. Otherwise the page
  //    quietly shows one radio's capabilities under another radio's name, and the owner has no way
  //    of telling — which is the same class of fault as a tour card pointing at a moved control.
  {
    const note = $("hwScope");
    if (note) {
      if (mine) { note.textContent = ""; note.classList.add("hide"); }
      else {
        note.classList.remove("hide");
        note.textContent = hw && hw.offline
          ? `${r.label || r.driver} is not running, so these are what this model supports. `
            + `Save its settings, then restart the server to bring it on air.`
          : `Settings for ${r.label || r.driver}, read from that radio.`;
      }
    }
  }

  // ★★ DRAW ONLY WHAT THIS RADIO HAS. An Airspy has no bias-T and no ppm; a dongle has no ppb.
  //    Showing a control whose every use is a no-op is the fault this project has a rule about:
  //    branch on the driver or leave it out, because a user concludes the FEATURE is broken, not
  //    the control.
  {
    const cap = DRIVER_HW[(hw && hw.driver) || r.driver] || {};
    const drv = (hw && hw.driver) || r.driver || "";
    const show = (id, yes) => { const e = $(id); if (e) e.classList.toggle("hide", !yes); };
    show("hwBiasT", !!cap.biasT);
    show("hwPpm",   drv === "rtl" || drv === "rtlsdr");
    show("hwPpb",   drv === "airspyhf");
  }

  // ★ Show what the processor is ACTUALLY doing, next to the control that asks for it.
  const gv = document.getElementById("govNow");
  if (gv && hw && hw.governor) {
    const mhz = hw.cpuKHz ? ` at ${(hw.cpuKHz/1000).toFixed(0)} MHz` : "";
    gv.textContent = `Currently: ${hw.governor}${mhz}.`;
    gv.style.color = hw.governor === "performance" ? "var(--good)" : "var(--dim)";
  }
  // ★★★ THE SPAN IS A LIST, NOT A TYPED NUMBER. It used to be a number box, which asks the owner
  //     for a figure most people do not have — and every radio refuses the ones it cannot do, so a
  //     wrong guess fails at the hardware and reads as a broken receiver. The rates come from the
  //     RADIO (GET /vibeserver/hardware), so the list is never wrong and never needs updating here
  //     when a fourth radio is added.
  //     ★ The stored value is kept even if it is not in the list — an owner who set 10 MSPS by
  //       hand on an older build must SEE that, not have the page silently pick something else and
  //       save it back. Marked so it is obvious.
  const rateSel = $("rate");
  if (rateSel && hw && hw.rates && hw.rates.length) {
    const want = String(radio().rate || hw.rates[0]);
    rateSel.innerHTML = hw.rates.map(r =>
      `<option value="${r}">${(r / 1e6).toFixed(3).replace(/0+$/, "").replace(/\.$/, "")} MHz` +
      ` &nbsp;(${(r / 1e6).toFixed(2)} MS/s)</option>`).join("");
    if (!hw.rates.some(r => String(r) === want))
      rateSel.insertAdjacentHTML("afterbegin",
        `<option value="${want}">${(+want / 1e6).toFixed(3)} MHz — not offered by this radio</option>`);
    rateSel.value = want;
    coverage();
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
      <div class="hint">The RSP manages its own gain by default. To set it by hand, see the note
        below.</div>`;
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
  // ★★★ SAY WHERE THE GAIN CONTROLS ACTUALLY ARE. They are deliberately NOT on this page: gain
  //     is the one setting you cannot choose sensibly in the abstract — it depends on your aerial,
  //     your noise floor and what is on the band right now — so it belongs where you can watch the
  //     waterfall while you move it, not in a wizard you fill in before the radio is even
  //     listening (Stuart, 2026-08-05). Without this note the omission reads as a missing feature.
  //     ★ And the answer to "how?" has to be here too, or we have only told them what is absent.
  // ★★★ SAY THAT A QUIET WATERFALL IS DELIBERATE. On an RSP the tuner starts at 59 dB of IF
  //     reduction — the least gain it can produce — because approaching a gain target from the
  //     QUIET side is the only safe direction: the alternative is starting hot and clipping on the
  //     way down, into a front end whose aerial we know nothing about. The cost is that a
  //     brand-new receiver shows almost nothing until its owner sets a gain, and an owner who has
  //     not been told that will read it as broken hardware or broken software and stop there
  //     (Stuart, 2026-08-06). A deliberate behaviour nobody explained is indistinguishable from a
  //     fault.
  // ★★ PER RADIO, because it is only TRUE per radio. The RSP starts at minimum; an RTL starts in
  //    its own automatic mode; the HF+ has no variable gain to start anywhere. Printing the RSP's
  //    story for all three would be the same misdescription this project keeps having to fix —
  //    see the note on branching by driver in AGENTS.md.
  const startState = hw.driver === "sdrplay"
    ? `<b>Signals will look weak until you do.</b> This receiver starts with the tuner at
       <b>minimum gain and maximum attenuation</b>, deliberately: we know nothing about your
       aerial yet, and coming UP to a working gain is the only safe direction — the alternative
       risks overloading the front end on the way down. A near-empty waterfall on a brand-new
       server is this protection working, not a fault in the hardware or the software.`
    : hw.driver === "airspyhf"
    ? `The HF+ has no variable gain to set &mdash; it manages its own attenuator and preamp &mdash;
       so there is nothing to protect here and nothing to adjust.`
    : `<b>Signals will look weak until you do.</b> This receiver starts at the tuner's
       <b>lowest gain</b>, deliberately, and never uses the tuner's own automatic gain &mdash; that
       mode is unreliable across RTL tuners and is known to misbehave on the v4. We know nothing
       about your aerial yet, so coming UP to a working gain is the safe direction. A near-empty
       waterfall on a brand-new server is this protection working, not a fault.`;

  el.innerHTML += `<div class="note">${startState}</div>`;
  el.innerHTML += `<div class="note"><b>Gain is not set here.</b> Open this receiver in the client,
    unlock <b>Protected settings</b> with your admin password, and the gain controls appear in the
    menu &mdash; so you can set them against live signals instead of guessing. Whatever you set
    there is saved <b>the moment you change it</b> &mdash; there is nothing to press, and nothing
    is lost if you close the tab &mdash; and restored when the server restarts.
    <br><br><b>Do this on your first listen.</b> Finish this page, connect, and spend a minute on
    the gain while you can see the waterfall: it is the single setting that most decides how good
    this receiver sounds to everyone who visits it, and the default is only ever a starting point.</div>`;

  // Restore stored values into whichever controls we just drew.
  if ($("rfNotch")) $("rfNotch").checked = !!cfg.rfNotch;
  if ($("dabNotch")) $("dabNotch").checked = !!cfg.dabNotch;
  if ($("gain")) $("gain").value = String(radio().gain != null ? radio().gain : -1);
}

// ── Several radios ───────────────────────────────────────────────────────────────────────────
// ★★★ WHICH TAB IS OPEN. Everything below reads and writes cfg.radios[curRadio]; the shared
//     settings live on cfg itself and are the same whichever tab you are on.
let curRadio = 0;

/** The radios the owner ticked in the setup screen. A radio that was NOT ticked has no tab: it is
 *  not going to be served, and offering somewhere to configure it would say otherwise. */
function radioList() { return Array.isArray(cfg.radios) ? cfg.radios.filter(r => r.enabled !== false) : []; }
function radio()     { return radioList()[curRadio] || {}; }

function renderTabs() {
  const list = radioList();
  const tabs = $("radioTabs"), hint = $("radioTabHint"), wrap = $("radioTabsWrap");
  // ★ One radio is not a choice, so it does not get a chooser.
  if (list.length < 2) { wrap.classList.add("hide"); return; }
  wrap.classList.remove("hide");
  // ★★★ THREE STATES, THREE COLOURS — and they answer different questions.
  //     GREEN: set up, and will be served. RED: not set up, so it will NOT be served no matter
  //     how much you tick it elsewhere. AMBER: the tab you are editing right now.
  //     ★ The amber "you are here" has to win, or the tab you are working on becomes the one you
  //       cannot pick out — so the current tab is amber whatever its state, and its readiness is
  //       carried by the dot instead.
  tabs.innerHTML = list.map((r, i) => {
    const on = i === curRadio;
    const ready = !!r.configured;
    const dot = ready ? "" : " •";
    const colour = on     ? "background:var(--amber);color:#000;border-color:var(--amber)"
                 : ready  ? "background:rgba(60,200,90,.14);color:#6ede8a;border-color:#3c9a55"
                          : "background:rgba(230,80,80,.14);color:#ff9b9b;border-color:#b04a4a";
    return `<button type="button" data-i="${i}" title="${ready ? "Set up — will be served"
                                                              : "Not set up yet — will not be served"}"`
         + ` style="padding:6px 12px;border-radius:6px;border:1px solid;cursor:pointer;${colour}">`
         + `${(r.label || r.driver || "Radio " + (i + 1))}${dot}</button>`;
  }).join("");
  Array.from(tabs.querySelectorAll("button")).forEach(b => {
    b.onclick = () => {
      // ★ Switching tabs KEEPS what you typed, in memory, so flipping between two radios to
      //   compare them does not quietly discard the one you were editing.
      stashRadio();
      curRadio = parseInt(b.getAttribute("data-i"), 10);
      renderTabs(); fill(); renderHw();
    };
  });
  const unsaved = list.filter(r => !r.configured).length;
  hint.textContent = unsaved
    ? `Pick a radio to set it up. ${unsaved} still to do — a radio marked • is not on air yet.`
    : "Pick a radio to change how it is set up.";
}

/** Copy what is on screen into the radio this tab belongs to, without saving to the server. */
function stashRadio() {
  const list = radioList();
  if (!list.length) return;
  Object.assign(list[curRadio], collectRadio());
}

function fill() {
  // ★ The MACHINE — the same on every tab.
  $("name").value = cfg.name || "";
  $("place").value = cfg.place || "";
  $("country").value = cfg.country || "";
  $("locator").value = cfg.locator || "";
  $("lat").value = cfg.lat || "";
  $("lon").value = cfg.lon || "";
  $("mdns").checked = cfg.mdnsAdvertise !== false;

  $("cpuGovernor").value = cfg.cpuGovernor || "performance";
  $("trustedProxies").value = cfg.trustedProxies || "";

  // ★★ THIS RADIO. Read from the open tab, never from cfg — reading a radio setting off the
  //    machine is how every receiver would show the first one's frequency.
  const r = radio();
  // ★ Default ON for a shared receiver. The 1024-bin window only stays sharp BECAUSE of the zoom
  //   resampling — without it, deep zoom interpolates and looks blocky, which is what a listener
  //   reads as a poor receiver. Off by default was right when this was experimental; it is not
  //   right for the model a shared server is built on.
  $("zoomSpectrum").checked = r.zoomSpectrum !== false;
  $("lockFreq").value = Math.round((r.lockFreq || r.freq || 0) / 1e3);
  // ★ NOT set here: the options do not exist until renderHw() has heard back from the
  //   radio, and assigning a value to an empty <select> silently selects nothing.
  //   renderHw() applies the rate once it has built the list.
  $("landingFreq").value = ((r.landingFreq || r.freq || 0) / 1e3).toFixed(1);
  $("demodMode").value = r.demodMode || "am";
  $("users").value = r.users || 1;
  $("uncompressed").value = String(r.uncompressed || 0);
  $("forceIdle").checked = !!r.forceIdleSaver;
  if ($("biasT")) $("biasT").checked = !!r.biasT;
  if ($("ppm"))   $("ppm").value = r.ppm != null ? r.ppm : 0;
  if ($("ppb"))   $("ppb").value = r.ppb != null ? r.ppb : 0;
  if ($("releaseWhenIdle")) $("releaseWhenIdle").checked = !!r.releaseWhenIdle;
  $("sessionLimit").value = r.sessionLimitMin || 0;
  if ($("spectrogram"))     $("spectrogram").checked = !!r.spectrogram;
  setMode((r.mode || "single") === "locked");
  addr(); coverage(); bwNote(); renderHw(); eibiStatus();
  $("eibiGet").addEventListener("click", eibiFetch);
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

/** ★★ WHAT BELONGS TO THIS RADIO. Split from the machine's own settings deliberately: posting a
 *  radio's locked frequency as though it were a property of the SERVER is how three receivers
 *  would end up sharing one window. */
function collectRadio() {
  const locked = radio().mode === "locked";
  return {
    mode: radio().mode,
    zoomSpectrum: $("zoomSpectrum").checked,
    lockFreq: locked ? Math.round(parseFloat($("lockFreq").value || "0") * 1e3) : 0,
    rate: parseFloat($("rate").value || "2400000"),
    // ★★★ SAVED FOR EVERY RADIO, NOT ONLY A LOCKED ONE. The card above says so in its own comment
    //     ("EVERY RADIO HAS THESE, whichever mode it is in") and the input is drawn for every
    //     radio — but this line still threw the value away unless the window was locked. So an
    //     unlocked radio silently kept landingFreq: 0, which means "same as freq", and every
    //     listener landed on the capture centre no matter what the owner typed (Stuart,
    //     2026-08-08: "I set the RTL-SDR to 648AM but it always keeps going back to 145MHz" —
    //     the MODE stuck, because demodMode below was never gated, and the frequency did not).
    // ★ lockFreq and users stay gated: those genuinely only exist for a locked radio.
    landingFreq: Math.round(parseFloat($("landingFreq").value || "0") * 1e3),
    demodMode: $("demodMode").value,
    users: locked ? parseInt($("users").value || "1", 10) : 1,
    uncompressed: parseInt($("uncompressed").value, 10),
    forceIdleSaver: $("forceIdle").checked,
    // ★ Only send what this radio actually has a control for. Posting rfNotch for an Airspy would
    //   be storing a setting that can never apply — the config would describe a radio we are not.
    ...($("rfNotch")  ? {rfNotch:  $("rfNotch").checked}  : {}),
    ...($("dabNotch") ? {dabNotch: $("dabNotch").checked} : {}),
    ...($("gain")     ? {gain: parseInt($("gain").value, 10)} : {}),
    // ★ Only what this radio HAS. Sending ppb for a dongle would store a setting that can never
    //   apply — the config would then describe a radio we are not.
    ...($("hwBiasT").classList.contains("hide") ? {} : {biasT: $("biasT").checked}),
    ...($("hwPpm").classList.contains("hide")   ? {} : {ppm: parseInt($("ppm").value || "0", 10)}),
    ...($("hwPpb").classList.contains("hide")   ? {} : {ppb: parseInt($("ppb").value || "0", 10)}),
    releaseWhenIdle: $("releaseWhenIdle").checked,
    // ★ ALWAYS, not just when locked. A one-listener radio is precisely the one someone can sit
    //   on all evening, so it is the radio that most needs a limit.
    sessionLimitMin: parseInt($("sessionLimit").value || "0", 10),
    // ★ Never claim the spectrogram for a radio that cannot honestly draw one — the checkbox is
    //   hidden in that case, and a hidden control must not still be sending a value.
    spectrogram: !$("hwSpectro").classList.contains("hide") && $("spectrogram").checked
  };
}

/** The machine: stated once, the same on every tab. */
function collect() {
  stashRadio();
  const locked = radio().mode === "locked";
  return {
    name: $("name").value.trim(),
    place: $("place").value.trim(),
    country: $("country").value.trim().toUpperCase(),
    locator: $("locator").value.trim(),
    lat: $("lat").value.trim(),
    lon: $("lon").value.trim(),
    mdnsAdvertise: $("mdns").checked,
    mdnsName: $("name").value.trim(),

    cpuGovernor: $("cpuGovernor").value,
    trustedProxies: $("trustedProxies").value.trim(),
    radios: Array.isArray(cfg.radios) ? cfg.radios : []
  };
}

// ★ The two settings interact, so the page must recheck when either moves.
document.addEventListener("change", (e) => {
  const t = e.target;
  if (t && (t.id === "releaseWhenIdle")) syncSpectroOffer();
});

async function signIn(fromTicket) {
  $("signinErr").textContent = "";
  if (!fromTicket) {
    PASS = $("pass").value;
    if (!PASS) { $("signinErr").textContent = "Enter the admin password."; return; }
  }
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
    // ★ Open the first radio that has NOT been set up yet — that is the one the owner came here
    //   for. Falling back to the first tab when they are all done.
    const list = radioList();
    const todo = list.findIndex(r => !r.configured);
    curRadio = todo >= 0 ? todo : 0;
    if (list.length > 1) $("saveRadioBtn").classList.remove("hide");
    renderTabs();
    fill();
  } catch (e) { $("signinErr").textContent = "Could not reach the server."; }
}

$("signinBtn").onclick = () => signIn(false);

// ★ Arrived from the landing page already signed in — go straight in rather than asking again.
if (TICKET) signIn(true);
$("pass").addEventListener("keydown", e => { if (e.key === "Enter") $("signinBtn").click(); });

$("modeSingle").onclick = () => setMode(false);
$("modeLocked").onclick = () => setMode(true);
$("name").addEventListener("input", addr);
$("mdns").addEventListener("change", addr);
for (const id of ["lockFreq","rate"]) $(id).addEventListener("input", coverage);
for (const id of ["users","uncompressed"]) $(id).addEventListener("input", bwNote);

// ★★★ SAVE THIS RADIO, AND ONLY THIS RADIO. No restart: the owner is working through three tabs,
//     and bouncing every listener on every radio after each one would make the page unusable. The
//     settings are stored; they take effect when the server is restarted from the footer below.
$("saveRadioBtn").onclick = async () => {
  $("saveErr").textContent = "";
  const list = radioList();
  if (!list.length) return;
  stashRadio();
  list[curRadio].configured = true;   // ★ THIS is what puts the radio on air after a restart
  $("saveRadioBtn").disabled = true;
  $("barMsg").textContent = "Saving " + (list[curRadio].label || "radio") + "…";
  try {
    const r = await fetch("/vibeserver/config?" + await authQuery(),
                          {method:"POST", body: JSON.stringify(collect())});
    const j = await r.json().catch(() => ({}));
    if (!r.ok) {
      list[curRadio].configured = false;
      $("saveErr").textContent = j.error || ("Save failed (" + r.status + ").");
      $("barMsg").textContent = "";
    } else {
      $("barMsg").textContent = "Saved. Restart the server below to put it on air.";
      renderTabs();
    }
  } catch (e) {
    list[curRadio].configured = false;
    $("saveErr").textContent = "Could not reach the server.";
    $("barMsg").textContent = "";
  }
  $("saveRadioBtn").disabled = false;
};

$("saveBtn").onclick = async () => {
  $("saveErr").textContent = "";
  $("saveBtn").disabled = true;
  $("barMsg").textContent = "Saving…";
  try {
    // ★ restart:true is what separates this from the per-radio save above — see the server's
    //   config handler, which only bounces the receiver when it is asked to.
    const body = collect();
    if (radioList().length) radioList()[curRadio].configured = true;
    body.radios = cfg.radios;
    body.restart = true;
    const r = await fetch("/vibeserver/config?" + await authQuery(),
                          {method:"POST", body: JSON.stringify(body)});
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
    // ★★★ WAIT FOR A DIFFERENT PROCESS, NOT MERELY A LIVE ONE. The old server keeps answering for
    //     the moment between our POST and its exit, so "is it up?" says yes immediately — the
    //     button appeared at once and then led to a dead page (Stuart, 2026-08-06). `instance`
    //     changes on every start, so a reply carrying a NEW one is proof the restart has happened
    //     and this server is the one holding the settings we just saved.
    const waitBack = async () => {
      for (let i = 0; i < 60; i++) {
        await new Promise(r => setTimeout(r, 1000));
        try {
          const s = await fetch("/vibeserver.json", {cache:"no-store"});
          if (!s.ok) continue;
          const j = await s.json();
          // ★ An older server sends no `instance`. Rather than never showing the button, fall back
          //   to the old rule but only after a few seconds, by which time the restart has begun.
          const isNew = j.instance ? (j.instance !== BOOT_ID) : (i >= 4);
          if (isNew && j.configured) { backUp(); return; }
        } catch (e) { /* still down — expected, and the point */ }
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

/** ★★★ THE SERVER IS BACK — SO OFFER THE DOOR. Saving restarts the receiver, and the page used to
 *  reload itself the moment it answered again: that lands you back on the SETTINGS page, so the
 *  one thing you actually wanted next — to go and listen — meant typing the address by hand
 *  (Stuart, 2026-08-06). The restart is also the only moment we can be SURE the new settings are
 *  live, which makes it exactly the right time to offer the link.
 *  ★ A button, not an automatic redirect: an owner who has just changed one setting may well want
 *    to change another, and being thrown out of a settings page is worse than one more click.
 *  ★ Enabling Save again matters too — without it the page is left in a state where nothing can
 *    be done at all. */
function backUp() {
  $("saveBtn").disabled = false;
  $("barMsg").innerHTML =
    '<span class="ok">Receiver is back up with your settings.</span>' +
    '<a id="gotoRx" href="/" class="gotoBtn" style="margin-left:14px">Open the receiver &rarr;</a>';
}
</script>
)HTML";
