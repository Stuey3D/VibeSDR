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
  /* ★★★ THE hidden ATTRIBUTE MUST WIN. [hidden] is only display:none in the USER-AGENT sheet, so
     ANY author rule that sets display beats it — .modalWrap{display:flex} did, and the serial
     dialog appeared over the SERVER tab the moment anyone signed in, on a radio they had not even
     opened (Stuart, 2026-08-08). This is the THIRD element to hit this trap in this project, after
     #hostRow and the client's ADMIN MODE banner, so it is fixed once here for every element rather
     than patched per selector — which is the pattern that guarantees a fourth. */
  [hidden]{display:none !important}
  .hide{display:none}
  .modalWrap{position:fixed;inset:0;z-index:200;background:rgba(0,0,0,.72);
             display:flex;align-items:center;justify-content:center;padding:20px}
  .modalBox{background:var(--panel);border:1px solid var(--line);border-radius:12px;
            padding:22px 24px;max-width:520px;width:100%}
  .bandList{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}
  .bandChip{display:inline-flex;align-items:center;gap:6px;border:1px solid var(--line);border-radius:6px;padding:3px 8px;font-size:12.5px;background:#0a0704}
  .bandChip button{background:none;border:0;color:var(--dim);cursor:pointer;font-size:14px;padding:0 2px}
  .bandChip button:hover{color:var(--bad)}
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
    <!-- ★★★ THE MACHINE, NOT A RADIO. These were interleaved with the per-radio cards, so an owner
         working through three tabs met the same server-wide questions on each one and could not
         tell which answers belonged to what. One tab, asked once (Stuart, 2026-08-08: "A Server tab
         which does all things server related ... anything not specifically radio hardware related").
         ★ The shortwave schedule lives here too: it is ONE download shared by every radio. -->
    <div id="serverPane">
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
        <h2>Audio</h2>
        <p class="why">How listeners receive sound, on every radio.</p>
      <label><span class="lbl">Uncompressed audio</span>
        <select id="uncompressed">
          <option value="0">Off — everyone gets Opus</option>
          <option value="1">Listener's choice</option>
          <option value="2">Only as a fallback for old browsers</option>
        </select>
        <div class="hint">Raw audio is about twenty times the bandwidth of Opus, out of your
          upload.</div></label>
      </div>

      <!-- ★★★ ONE CHOICE FOR THE MACHINE. Opus or uncompressed is about what this server's UPLINK
           can carry, and there is one uplink however many radios are plugged in — asking per radio
           invited three answers to a question that has one, and three ways to get it wrong
           (Stuart, 2026-08-08: "only one selection Opus applies to all radios"). -->
<div class="card">
      <h2>Power saving</h2>
        <p class="why">Applies to every radio on this machine.</p>
      <p class="why">Applies in both modes.</p>
      <label style="display:flex;align-items:center;gap:10px;margin-top:16px">
        <input type="checkbox" id="forceIdle" style="width:16px;height:16px;accent-color:var(--amber)">
        <span>Listeners may not switch off idle power saving</span></label>
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

      <!-- ★★★ ONE DOWNLOAD FOR THE WHOLE MACHINE. The schedule is a single file every radio
           process reads, so it belongs to the server and not to whichever radio tab happened to be
           open — where it used to sit, inside a per-radio Range card, implying one copy per radio
           (Stuart, 2026-08-08: "We only need one lot of eibi downloads for the entire server they
           can be shared to all radios"). -->
      <div class="card">
        <h2>Shortwave schedule</h2>
        <p class="why">Station names for the search box, shared by every radio on this machine.</p>
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
      </div>
    </div>

    <!-- ★★★ MODE FIRST, BECAUSE IT DECIDES THE REST. "How will it be used?" changes which of the
         cards below even apply — a shared radio has a locked range and a listener count, a
         single-user one has neither and gets the allow/block lists instead. Asking it last, as
         this page used to, means answering questions that the next answer makes irrelevant. -->
    <div id="radioPane">
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
      <!-- ★★★ WHERE LISTENERS MAY TUNE THIS RADIO. Single-user only: in shared mode the locked
           range IS the limit, so offering these there would be two answers to one question
           (Stuart, 2026-08-08: "blocked bands wont be needed in shared mode as the locked nature
           of it is the block/allow").
           ★★ ALLOW says what is reachable; BLOCK carves holes out of it. Leaving ALLOW empty means
           the whole radio, so somebody who only wants to keep listeners off the airband does not
           have to enumerate everything else first. -->
      <div class="card hide" id="bandCard">
        <h2>Where listeners may tune</h2>
        <p class="why">Leave both empty and this radio tunes anywhere it can hear.</p>

        <label><span class="lbl">Allowed — only these, if you list any</span>
          <div class="row" style="gap:8px">
            <select id="allowPick" style="flex:1 1 200px"></select>
            <input type="text" id="allowType" placeholder="or 87.5MHz - 108MHz" style="flex:1 1 180px">
            <button type="button" class="ghost" id="allowAdd" style="flex:0 0 auto">Add</button>
          </div>
          <div id="allowList" class="bandList"></div></label>

        <label style="margin-top:14px"><span class="lbl">Blocked — never these</span>
          <div class="row" style="gap:8px">
            <select id="blockPick" style="flex:1 1 200px"></select>
            <input type="text" id="blockType" placeholder="or 108MHz - 137MHz" style="flex:1 1 180px">
            <button type="button" class="ghost" id="blockAdd" style="flex:0 0 auto">Add</button>
          </div>
          <div id="blockList" class="bandList"></div></label>

        <div class="note" id="bandNote"></div>
        <div class="row" id="bandCopyRow" style="gap:8px;margin-top:6px">
          <select id="bandCopyFrom" style="flex:1 1 200px"></select>
          <button type="button" class="ghost" id="bandCopy" style="flex:0 0 auto">Copy these lists</button>
        </div>
        <div class="hint">A listener who tunes to the edge of an allowed band stops there and is
          told why; tuning the same way again jumps to the next allowed band. That is exactly how
          this receiver already handles a gap the hardware cannot cover, so it is one behaviour to
          learn rather than two.
          <br><b>The server enforces this as well as the page</b>, so it holds for any client, not
          just this one.</div>
      </div>

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
      <!-- ★★★ THE DONGLE'S NAME, CHANGEABLE HERE BECAUSE THE ALTERNATIVE IS A SHELL COMMAND.
           Four identical RTL dongles all arrive calling themselves 00000001, and until they have
           different names the server cannot tell which is which — so this is the FIRST thing an
           owner of several needs, and the person who needs it most is the least likely to want to
           type a command (Stuart, 2026-08-08: "I would be nervous to use CLI to do it ... which is
           why I used SDR Console on windows").
           ★★ It writes to the dongle's EEPROM. Everything protective lives on the server — refuse
           a chip it cannot fully parse, take a backup first, verify by reading back — and the one
           thing a browser can add is making the operator say the name out loud before it happens.
           ★ RTL only: an Airspy's serial is factory-burned and an SDRplay has none at all. -->
      <div id="hwSerial" class="hide" style="margin-top:14px;border-top:1px solid var(--line);padding-top:12px">
        <label><span class="lbl">This dongle's serial</span>
          <!-- ★★ SHOW WHAT IT IS, do not ask for it. An empty box invited someone to type a long
               string of zeros from memory, and getting the COUNT wrong is easy and silent — the
               dongle ends up with a name nobody meant (Stuart, 2026-08-08). The change happens in
               a dialog that starts from the CURRENT value, so the common case (bump the last
               digit) is an edit rather than a re-entry. -->
          <div class="row" style="gap:8px">
            <input type="text" id="rtlSerialNow" readonly
                   style="flex:2 1 220px;opacity:.85;cursor:default">
            <button type="button" id="rtlSerialGo" class="ghost" style="flex:0 0 auto">Change serial</button>
          </div>
          <div class="hint" id="rtlSerialState"></div>
          <div class="hint">Gives this dongle a name of its own, so the server can tell it apart
            from an identical one. <b>It writes to the dongle's memory.</b> A backup is taken first
            and the result is read back and checked, but do not do this on a machine that might
            lose power mid-write.
            <br><b>The change only takes effect when the dongle loses power</b> — the chip reads its
            memory at power-on, so a reboot is needed. Nothing else does it: restarting the service
            or re-plugging in software leaves the old name in place.</div></label>
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
           band conditions table — both read the same window, so one radio provides both.
           <br><strong>This radio then never idles.</strong> A picture of the last 24 hours cannot
           be drawn by a receiver that sleeps through them, so it keeps capturing with nobody
           listening: it will not park to save power, and it cannot be released to another program.
           Expect it to use a core and a few watts around the clock. Every other radio on this
           machine still parks when idle — so on a multi-radio server this is one radio's power,
           not the whole machine's.</p>
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
        <!-- ★★ HOW LONG TO WAIT, because the right answer depends on what else wants the radio.
             It was fixed at five minutes with nothing to change it — long enough that the other
             program is still waiting when somebody gives up (Stuart, 2026-08-08: "5 minutes is a
             bit long anyway"). -->
        <label><span class="lbl">Let go after</span>
          <select id="idleGrace">
            <option value="0">Immediately</option>
            <option value="30">30 seconds</option>
            <option value="60">1 minute</option>
            <option value="300">5 minutes</option>
            <option value="900">15 minutes</option>
          </select>
          <div class="hint">Counted from the moment the last listener leaves. <b>Immediately</b>
            hands the radio over the instant nobody is listening, which suits a machine where the
            other program matters as much as this one — but a listener arriving seconds later has
            to wait for it to come back, and cannot if the other program has taken it. A short
            wait absorbs somebody reloading their browser.</div></label>
      </div>
      <div id="hwPpb" class="hide">
        <label><span class="lbl">Calibration (ppb)</span>
          <input type="number" id="ppb" step="1" placeholder="0"></label>
        <p class="why">The Airspy HF+ is calibrated in parts per BILLION. 0 unless you have measured it.</p>
      </div>
    </div>
    </div>


    <!-- ★★★ THE ONE OPERATION HERE THAT WRITES TO HARDWARE gets a dialog of its own rather than a
         browser prompt(): it can show the current name, pre-fill the new one so the usual change is
         a single keystroke, and refuse to enable Change until both entries agree exactly. -->
    <div id="serialModal" class="modalWrap" hidden>
      <div class="modalBox">
        <h2 style="margin-top:0">Change this dongle's serial</h2>
        <p class="why">Now: <b id="serialModalNow" class="addr"></b></p>
        <label><span class="lbl">New serial</span>
          <input type="text" id="serialA" maxlength="32" autocomplete="off" spellcheck="false"></label>
        <label><span class="lbl">Type it again</span>
          <input type="text" id="serialB" maxlength="32" autocomplete="off" spellcheck="false"></label>
        <div class="hint" id="serialMatch"></div>
        <div class="hint"><b>This writes to the dongle's memory.</b> A backup is taken first and the
          result is read back and checked. Do not do this on a machine that might lose power
          mid-write. The new name only appears after the dongle loses power, so a reboot is needed.</div>
        <p style="margin-top:16px;display:flex;gap:10px">
          <button type="button" id="serialDo" disabled>Change serial</button>
          <button type="button" id="serialCancel" class="ghost">Cancel</button>
        </p>
      </div>
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
  if (typeof renderBands === "function") renderBands();
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


// ── The dongle's serial ─────────────────────────────────────────────────────────────────────
//
// ★★★ A CHANGE HERE IS NOT FINISHED UNTIL THE MACHINE REBOOTS, so the page carries that fact
//     forward instead of leaving the owner to remember it. The RTL2832U reads its EEPROM at
//     power-on, and on a Pi only a reboot drops USB port power — restarting the service or
//     re-plugging in software leaves the old name in place, which reads as "the write failed".
//     ★★ The unit name contains the serial too (vibeserver@<serial>.service), and the boot-time
//        reconciliation renames it from the config. So the reboot is the correct action, not a
//        workaround for a stubborn chip.
let SERIAL_PENDING = null;   // {old,new} while a change is written but not yet powered off

/** ★ The serial is operator-supplied text that lands in innerHTML — escape it. Short, but this is
 *  the one string on this page that a person types and the page then renders as markup. */
function esc(t) {
  return String(t == null ? "" : t).replace(/[&<>"']/g,
    c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
}


// ── Where listeners may tune ────────────────────────────────────────────────────────────────
//
// ★★ THE LISTS ARE EDITED AS CHIPS, not as a comma-separated string in a text box. An owner who
//    mistypes one entry in a string loses the whole line to a parse they cannot see; a chip they
//    can add and remove one at a time is a thing they can check at a glance, which matters for a
//    setting whose whole job is to be exactly right.
let BANDS = [];              // [{id,label}] — from the server, never a copy in this page

function bandLabel(entry) {
  const b = BANDS.find(x => x.id === entry);
  return b ? b.label : entry;
}

function bandChips(which) {
  const list = (radio()[which === "allow" ? "allowRanges" : "blockRanges"] || "")
    .split(",").map(t => t.trim()).filter(Boolean);
  const host = $(which + "List");
  host.innerHTML = list.map((e, i) =>
    `<span class="bandChip">${esc(bandLabel(e))}<button type="button" data-w="${which}" data-i="${i}"
       title="Remove">&times;</button></span>`).join("")
    || `<span class="hint">${which === "allow"
          ? "Empty — this radio can tune anywhere it hears."
          : "Empty — nothing is blocked."}</span>`;
  Array.from(host.querySelectorAll("button")).forEach(b => {
    b.onclick = () => {
      const arr = list.slice(); arr.splice(parseInt(b.getAttribute("data-i"), 10), 1);
      radio()[which === "allow" ? "allowRanges" : "blockRanges"] = arr.join(",");
      bandChips(which); bandSummary();
    };
  });
}

function bandAdd(which) {
  const typed = ($(which + "Type").value || "").trim();
  const picked = $(which + "Pick").value;
  const entry = typed || picked;
  if (!entry) return;
  const key = which === "allow" ? "allowRanges" : "blockRanges";
  const cur = (radio()[key] || "").split(",").map(t => t.trim()).filter(Boolean);
  // ★ No duplicates: the server would merge them anyway, and a list showing the same band twice
  //   makes an owner doubt which one is in force.
  if (!cur.includes(entry)) cur.push(entry);
  radio()[key] = cur.join(",");
  $(which + "Type").value = "";
  bandChips(which); bandSummary();
}

/** ★ Say what the two lists ADD UP TO, because that is the question an owner actually has and it
 *  is not obvious from two lists — especially "you have blocked everything you allowed". */
function bandSummary() {
  const el = $("bandNote");
  const a = (radio().allowRanges || "").split(",").filter(t => t.trim());
  const b = (radio().blockRanges || "").split(",").filter(t => t.trim());
  if (!a.length && !b.length) { el.textContent = "Listeners can tune anywhere this radio hears."; return; }
  el.textContent = (a.length ? `Only ${a.map(bandLabel).join(", ")}` : "Everything this radio hears")
    + (b.length ? `, except ${b.map(bandLabel).join(", ")}.` : ".");
}

function renderBands() {
  // ★ Mode is chosen by two CARDS here, not a <select> — read it from the radio itself rather than
  //   from a control that does not exist. (Caught by scripts/check-setup-page.mjs, which is the
  //   whole reason that check exists: $("mode") parsed perfectly and would have been null at run
  //   time, taking the rest of the function with it.)
  const single = radio().mode !== "locked";
  // ★ Shared mode has a locked range, which IS the limit — two answers to one question.
  $("bandCard").classList.toggle("hide", !single);
  if (!single) return;
  for (const w of ["allow", "block"]) {
    const sel = $(w + "Pick");
    sel.innerHTML = '<option value="">— pick a band —</option>'
      + BANDS.map(b => `<option value="${esc(b.id)}">${esc(b.label)}</option>`).join("");
    bandChips(w);
  }
  // Copy from another radio — the lists only, which is all that was asked for and sidesteps
  // carrying a per-device calibration (or a bias-T) onto hardware it does not suit.
  const list = radioList();
  const others = list.map((r, i) => ({r, i})).filter(x => x.i !== curRadio);
  $("bandCopyRow").classList.toggle("hide", others.length === 0);
  $("bandCopyFrom").innerHTML = others.map(x =>
    `<option value="${x.i}">${esc(x.r.label || x.r.driver || ("Radio " + (x.i + 1)))}</option>`).join("");
  bandSummary();
}


/** ★★★ THE REQUEST MUST REACH THE PROCESS THAT HOLDS THE DONGLE. This page is served by the FRONT
 *  DOOR, which owns no radio — so a relative /vibeserver/rtl-serial went there, released nothing
 *  (it had nothing to release), and then failed to open a dongle the RADIO's own process was still
 *  holding: "something else is using it", where the something else was VibeServer (Stuart,
 *  2026-08-08, with the radio not in use at all).
 *  ★ Only the owning process can let go of its own device, which is exactly what its handler does. */
function radioPath() {
  const r = radio();
  return (r && r.serial) ? "/r/" + encodeURIComponent(r.serial) : "";
}

async function serialStatus() {
  const el = $("rtlSerialState");
  try {
    const r = await fetch(radioPath() + "/vibeserver/rtl-serial?" + await authQuery(), {cache:"no-store"});
    if (!r.ok) { if (el) el.textContent = ""; return; }
    const j = await r.json();
    SERIAL_PENDING = j.pending ? {old: j.old, new: j.new, took: j.took} : null;
    if (!el) return;
    if (j.pending && j.took) {
      // ★ Proven, not assumed: the server clears the marker only when it SEES the new serial on
      //   the bus and the old one gone.
      el.innerHTML = '<span class="ok">Confirmed — this dongle now reports ' + esc(j.new) + '.</span>';
    } else if (j.pending) {
      el.innerHTML = '<span style="color:var(--amber)">Written, but not in effect yet: still '
        + 'reporting ' + esc(j.old || "its old serial") + '. Reboot to finish.</span>';
    } else {
      el.textContent = j.bus && j.bus.length
        ? "Currently: " + j.bus.map(esc).join(", ")
        : "";
      el.style.color = "var(--dim)";
    }
    paintSaveButton();
  } catch (e) { /* an older server has no such endpoint; the card simply stays quiet */ }
}

/** ★★ THE DIALOG STARTS FROM THE CURRENT NAME. Bumping the last digit is the usual change, and an
 *  empty box asked people to retype a run of zeros from memory — miscount them and the dongle
 *  quietly ends up called something nobody intended. Editing what is already there cannot go wrong
 *  that way.
 *  ★ Both entries must still agree exactly, and the button stays disabled until they do: the
 *    confirmation is what catches a slip in the EDIT, which is the mistake this shape can still
 *    make. The count of characters is shown because that is the thing being got wrong. */
function serialModalOpen() {
  const now = (radio().serial || "").trim();
  $("serialModalNow").textContent = now || "(none)";
  $("serialA").value = now;
  $("serialB").value = "";
  $("serialMatch").textContent = "";
  $("serialDo").disabled = true;
  $("serialModal").hidden = false;
  const a = $("serialA");
  a.focus();
  // ★ Caret at the END, nothing selected: the point is to edit the last character, and a selected
  //   value is one keystroke from being wiped entirely.
  a.setSelectionRange(a.value.length, a.value.length);
}

function serialModalCheck() {
  const a = $("serialA").value.trim(), b = $("serialB").value.trim();
  const el = $("serialMatch");
  const now = (radio().serial || "").trim();
  if (!a) { el.textContent = ""; $("serialDo").disabled = true; return; }
  if (a === now) {
    el.textContent = "That is the name it already has."; el.style.color = "var(--dim)";
    $("serialDo").disabled = true; return;
  }
  if (!b) { el.textContent = `${a.length} characters — type it again to confirm.`;
            el.style.color = "var(--dim)"; $("serialDo").disabled = true; return; }
  if (a !== b) { el.textContent = "The two do not match."; el.style.color = "var(--bad)";
                 $("serialDo").disabled = true; return; }
  el.textContent = `Will be set to ${a} (${a.length} characters).`;
  el.style.color = "var(--good)";
  $("serialDo").disabled = false;
}

async function serialChange() {
  const want = $("serialA").value.trim();
  if (!want || want !== $("serialB").value.trim()) return;
  const el = $("rtlSerialState"), b = $("serialDo");
  b.disabled = true;
  el.textContent = "writing…"; el.style.color = "var(--dim)";
  $("serialModal").hidden = true;
  try {
    const r = await fetch(radioPath() + "/vibeserver/rtl-serial?" + await authQuery(), {
      method: "POST", headers: {"Content-Type": "application/json"},
      body: JSON.stringify({serial: want}),
    });
    const j = await r.json();
    el.textContent = j.message || (j.ok ? "Done." : "It did not work.");
    el.style.color = j.ok ? "var(--good)" : "var(--bad)";
    if (j.ok) await serialStatus();
  } catch (e) {
    el.textContent = "Could not reach the server."; el.style.color = "var(--bad)";
  }
}

/** ★ The master button changes JOB when a serial is waiting on a power cycle: a restart would not
 *  finish it, and offering one would look like it had. */
function paintSaveButton() {
  const b = $("saveBtn");
  if (!b) return;
  if (SERIAL_PENDING && !SERIAL_PENDING.took) {
    b.textContent = "Save and reboot";
    b.title = "A dongle's serial has been changed. Only a reboot puts it into effect.";
  } else if (b.textContent === "Save and reboot") {
    b.textContent = cfg && cfg.configured ? "Save changes" : "Save and start";
    b.title = "";
  }
}

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
  // ★★★ ONE RATE, AND IT IS THE RADIO'S OWN DEFAULT — the highest it offers. This listed
  //     768/456/228 and NOT 912, which is precisely backwards: 912 is the only rate the HF+ is
  //     supported at here, and every rate that WAS offered is one the source says breaks. See
  //     nearestRate() in airspyhf_source.cpp: the dead-lobe crop is a per-rate table whose numbers
  //     are MEASURED AT 912, and 228 kHz tunes about 7.8 kHz off frequency on this firmware.
  //     Stuart, 2026-08-02: "I think we just offer the default on the server too otherwise that
  //     breaks all the dead space fix we added." The server does exactly that; this page did not.
  // ★★ WHAT IT COST: changing the landing frequency re-applied a rate from this list, the crop
  //    was then computed for the wrong span, and Radio Caroline read 665 kHz instead of 648 — a
  //    receiver that is simply wrong about where it is listening (Stuart, 2026-08-09). Offering a
  //    choice whose every option is broken is worse than offering none: it reads as a setting the
  //    owner got wrong.
  airspyhf: { rates: [912000],
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
    // ★★★ THE FRONT DOOR OWNS NO RADIO, so NOTHING in the directory is "mine" — and this used to
    //     leave `mine` at its optimistic default and ask the door for hardware it does not have.
    //     The answer was empty, and because the mine-branch has no driver fallback the Span list
    //     was left blank on a page reached from the landing page's SETUP button (Stuart,
    //     2026-08-08: "sample rate has gone for the rsp1b").
    // ★ With radios listed, "mine" is true only when one of them says so AND it is this one.
    const me = (dir.radios || []).find(x => x.mine);
    if (r.serial && (dir.radios || []).length) mine = !!me && me.serial === r.serial;
  } catch (e) { /* single-radio server: it is always ours */ }

  if (mine) {
    try { hw = await (await fetch("/vibeserver/hardware", {cache:"no-store"})).json(); } catch (e) {}
    // ★ Same fallback as the other branch. A control with no options is not "unknown", it is
    //   BROKEN-looking, and the driver's own list is always better than an empty box.
    if (!hw || !hw.rates || !hw.rates.length) {
      const d = DRIVER_HW[r.driver] || DRIVER_HW.rtl;
      hw = { driver: r.driver, present: hw ? !!hw.present : false, rates: d.rates,
             gains: (hw && hw.gains) || [], biasT: d.biasT, rfNotch: d.rfNotch, offline: !hw };
    }
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
  // ★ The band names ride along with the hardware description — one fetch, and they are the
  //   server's own list rather than a copy that could drift from it.
  if (hw && Array.isArray(hw.bands) && hw.bands.length) BANDS = hw.bands;
  renderBands();

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
    show("hwSerial", drv === "rtl" || drv === "rtlsdr");
    // ★ FROM WHAT WE ALREADY KNOW, not from the status request. It was filled inside that fetch,
    //   so any failure or 401 left the box blank while the dialog — which reads the radio directly
    //   — showed the name perfectly well: the same value looking absent in one place and present
    //   in another (Stuart, 2026-08-08). The request only ever refreshes what is beneath it.
    if ($("rtlSerialNow")) $("rtlSerialNow").value = (radio().serial || "").trim();
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

  // ★★★ `present` MEANS "THIS PROCESS HAS THE RADIO OPEN", NOT "THE RADIO EXISTS" — and bailing on
  //     it here threw away the whole point of the driver tables above. During FIRST-TIME SETUP
  //     nothing has opened the radio yet, so `present` is false and this returned "No radio
  //     detected" for a radio sitting plugged in six inches away: the RSP's notches simply were not
  //     on the page. Start the server once, come back, and there they were (Stuart, 2026-08-08).
  //
  // ★★ The comment on DRIVER_HW says it exactly — "that is exactly when the owner is setting it up
  //    … properties of the MODEL, not of a live handle" — and then this line defeated it. A
  //    fallback that an early return skips is not a fallback.
  //
  // ★ So the only thing that makes this page useless is not knowing WHICH RADIO it is. Everything
  //   below branches on the driver, and the driver is in the config the moment one is detected.
  const drv = (hw && hw.driver) || r.driver || "";
  if (!drv) {
    el.innerHTML = `<p class="hint">No radio detected, so there is nothing to set here.
      Plug one in and reload this page.</p>`;
    return;
  }
  if (drv === "sdrplay") {
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
  } else if (drv === "airspyhf") {
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
  const startState = drv === "sdrplay"
    ? `<b>Signals will look weak until you do.</b> This receiver starts with the tuner at
       <b>minimum gain and maximum attenuation</b>, deliberately: we know nothing about your
       aerial yet, and coming UP to a working gain is the only safe direction — the alternative
       risks overloading the front end on the way down. A near-empty waterfall on a brand-new
       server is this protection working, not a fault in the hardware or the software.`
    : drv === "airspyhf"
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
  // ★★★ FROM THE RADIO, NOT FROM THE MACHINE. These two read `cfg` — the machine-wide config —
  //     while the line below them correctly reads radio(). On a multi-radio server cfg has no
  //     notches, so every render reset them to off and the next save wrote that back: tick it,
  //     save, and it was gone (Stuart, 2026-08-08: "SDRPlay notches are not saving either").
  //     The notches belong to the radio that HAS them, and the inconsistency sat one line apart.
  if ($("rfNotch")) $("rfNotch").checked = !!radio().rfNotch;
  if ($("dabNotch")) $("dabNotch").checked = !!radio().dabNotch;
  if ($("gain")) $("gain").value = String(radio().gain != null ? radio().gain : -1);

  // ★ And the dongle's own serial, refreshed for THIS tab. It was read once at sign-in, so
  //   switching to another radio left the line underneath describing the previous one — or
  //   nothing at all (Stuart: "RTL not showing its current serial number below the box any more").
  serialStatus();
}

// ── Several radios ───────────────────────────────────────────────────────────────────────────
// ★★★ WHICH TAB IS OPEN. Everything below reads and writes cfg.radios[curRadio]; the shared
//     settings live on cfg itself and are the same whichever tab you are on.
let curRadio = 0;

// ★★★ WHICH RADIO THE FORM ON SCREEN ACTUALLY BELONGS TO — which is NOT always `curRadio`.
//
//     fill() deliberately does not set the sample rate ("the options do not exist until
//     renderHw() has heard back from the radio"), so renderHw() applies it ASYNCHRONOUSLY. In the
//     window between clicking a tab and that fetch returning, the form still holds the PREVIOUS
//     radio's rate — and stashRadio() copies whatever is on screen into cfg.radios[curRadio].
//     Switch tabs and save quickly and one radio's sample rate lands in another's config.
//
// ★★★ THAT IS NOT THEORETICAL. Stuart switched tabs to change the landing station on two radios
//     and his Airspy came back misaligned at EVERY sample rate — a constant offset a rate change
//     could not clear, because the stored rate was never the one he picked (2026-08-09). It is the
//     one-radio-assumption family again, this time reaching sideways between radios.
//
// ★ -2 means "the form belongs to nobody yet". stashRadio() refuses to copy in that state, so a
//   half-populated form can never be written to a radio — the worst it can do is decline to save
//   an edit, and refreshHw() below is awaited on the save path so even that cannot happen.
let formRadio = -2;
let hwPending  = Promise.resolve();

/** Re-render the hardware pane for the CURRENT tab, and only then let the form be read back.
 *  ★ The index is captured so a slow render for a tab the owner has already left cannot mark the
 *    form as belonging to a radio that is no longer on screen. */
function refreshHw() {
  const want = curRadio;
  formRadio = -2;
  hwPending = renderHw().then(() => { if (curRadio === want) formRadio = want; })
                        .catch(() => { if (curRadio === want) formRadio = want; });
  return hwPending;
}

/** The radios the owner ticked in the setup screen. A radio that was NOT ticked has no tab: it is
 *  not going to be served, and offering somewhere to configure it would say otherwise. */
function radioList() { return Array.isArray(cfg.radios) ? cfg.radios.filter(r => r.enabled !== false) : []; }
function radio()     { return radioList()[curRadio] || {}; }

function renderTabs() {
  const list = radioList();
  const tabs = $("radioTabs"), hint = $("radioTabHint"), wrap = $("radioTabsWrap");
  // ★★ THE CHOOSER IS ALWAYS SHOWN NOW, even with one radio: there is a SERVER tab to reach, and
  //    a machine with a single receiver still has a name, a location and a schedule.
  wrap.classList.remove("hide");
  paintPanes();
  // ★★★ THREE STATES, THREE COLOURS — and they answer different questions.
  //     GREEN: set up, and will be served. RED: not set up, so it will NOT be served no matter
  //     how much you tick it elsewhere. AMBER: the tab you are editing right now.
  //     ★ The amber "you are here" has to win, or the tab you are working on becomes the one you
  //       cannot pick out — so the current tab is amber whatever its state, and its readiness is
  //       carried by the dot instead.
  const serverOn = curRadio < 0;
  const serverTab = `<button type="button" data-i="-1" title="Settings for the whole machine"`
    + ` style="padding:6px 12px;border-radius:6px;border:1px solid;cursor:pointer;margin-right:6px;`
    + (serverOn ? `background:var(--amber);color:#000;border-color:var(--amber)`
                : `background:rgba(255,184,51,.10);color:var(--amber);border-color:var(--line)`)
    + `">SERVER</button>`;
  tabs.innerHTML = serverTab + list.map((r, i) => {
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
      renderTabs(); paintPanes();
      if (curRadio >= 0) { fill(); refreshHw(); }
    };
  });
  const unsaved = list.filter(r => !r.configured).length;
  if (curRadio < 0) { hint.textContent = "Settings that belong to this machine, not to any one radio."; return; }
  hint.textContent = unsaved
    ? `Pick a radio to set it up. ${unsaved} still to do — a radio marked • is not on air yet.`
    : "Pick a radio to change how it is set up.";
}

/** ★ One tab is showing at a time, and which one is the ONLY thing that decides it. */
function paintPanes() {
  const sp = $("serverPane"), rp = $("radioPane");
  if (sp) sp.classList.toggle("hide", curRadio >= 0);
  if (rp) rp.classList.toggle("hide", curRadio < 0);
  const sr = $("saveRadioBtn");
  // ★ "Save radio settings" is meaningless on the server tab — hidden rather than left to fail.
  if (sr) sr.classList.toggle("hide", curRadio < 0 || radioList().length < 2);
}

/** Copy what is on screen into the radio this tab belongs to, without saving to the server. */
function stashRadio() {
  const list = radioList();
  if (!list.length || curRadio < 0) return;
  // ★★★ THE GUARD. Without it this copies a half-populated form — carrying the PREVIOUS radio's
  //     sample rate — into this radio's config. See the note on formRadio above.
  if (formRadio !== curRadio) return;
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
  $("uncompressed").value = String(cfg.uncompressed || 0);
  $("forceIdle").checked = !!cfg.forceIdleSaver;
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

  if ($("biasT")) $("biasT").checked = !!r.biasT;
  if ($("ppm"))   $("ppm").value = r.ppm != null ? r.ppm : 0;
  if ($("ppb"))   $("ppb").value = r.ppb != null ? r.ppb : 0;
  if ($("releaseWhenIdle")) $("releaseWhenIdle").checked = !!r.releaseWhenIdle;
  // ★ 300 s is the historical default, so a radio saved before this control existed
  //   keeps the behaviour it already had rather than silently changing.
  if ($("idleGrace")) $("idleGrace").value = String(r.idleGrace != null ? Math.round(r.idleGrace) : 300);
  $("sessionLimit").value = r.sessionLimitMin || 0;
  if ($("spectrogram"))     $("spectrogram").checked = !!r.spectrogram;
  setMode((r.mode || "single") === "locked");
  addr(); coverage(); bwNote(); refreshHw(); eibiStatus();
  $("eibiGet").addEventListener("click", eibiFetch);
  $("rtlSerialGo").addEventListener("click", serialModalOpen);
  $("serialCancel").addEventListener("click", () => { $("serialModal").hidden = true; });
  $("serialDo").addEventListener("click", serialChange);
  for (const id of ["serialA", "serialB"]) $(id).addEventListener("input", serialModalCheck);
  $("allowAdd").addEventListener("click", () => bandAdd("allow"));
  $("blockAdd").addEventListener("click", () => bandAdd("block"));
  $("bandCopy").addEventListener("click", () => {
    const from = radioList()[parseInt($("bandCopyFrom").value, 10)];
    if (!from) return;
    radio().allowRanges = from.allowRanges || "";
    radio().blockRanges = from.blockRanges || "";
    renderBands();
  });

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
  const drv = radio().driver || "";
  return {
    mode: radio().mode,
    // ★ The band lists are edited as chips straight onto the radio object, so they are carried
    //   through here rather than read back off a form field that does not exist. Blank in shared
    //   mode, because a locked range is already the limit and a stale list left behind would be
    //   enforced invisibly if the owner switched back.
    allowRanges: locked ? "" : (radio().allowRanges || ""),
    blockRanges: locked ? "" : (radio().blockRanges || ""),
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


    // ★ Only send what this radio actually has a control for. Posting rfNotch for an Airspy would
    //   be storing a setting that can never apply — the config would describe a radio we are not.
    ...($("rfNotch")  ? {rfNotch:  $("rfNotch").checked}  : {}),
    ...($("dabNotch") ? {dabNotch: $("dabNotch").checked} : {}),
    ...($("gain")     ? {gain: parseInt($("gain").value, 10)} : {}),
    // ★ Only what this radio HAS. Sending ppb for a dongle would store a setting that can never
    //   apply — the config would then describe a radio we are not.
    // ★★★ ASK THE RADIO, NOT THE SCREEN. These read `classList.contains("hide")` — the visibility
    //     that renderHw() sets ASYNCHRONOUSLY — so during a tab switch they described the radio you
    //     had just left: a dongle could be sent a ppb it can never use, and an Airspy a ppm. The
    //     driver is in the config and is true the instant the tab changes, which is the whole
    //     point of deriving a thing from data rather than from the DOM's current mood.
    ...(DRIVER_HW[drv] && DRIVER_HW[drv].biasT ? {biasT: $("biasT").checked} : {}),
    ...(drv === "rtl" || drv === "rtlsdr" ? {ppm: parseInt($("ppm").value || "0", 10)} : {}),
    ...(drv === "airspyhf" ? {ppb: parseInt($("ppb").value || "0", 10)} : {}),
    releaseWhenIdle: $("releaseWhenIdle").checked,
    idleGrace: parseFloat($("idleGrace").value),
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
    uncompressed: parseInt($("uncompressed").value, 10),
    forceIdleSaver: $("forceIdle").checked,
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
    // ★★ THE SERVER TAB FIRST, ALWAYS. It used to open whichever radio was not set up yet, which
    //    reads as "start here" — but the machine's own name and location come before any radio,
    //    and on a server that is already running it dropped the owner into a radio they had not
    //    asked about (Stuart, 2026-08-08: "the server tab needs to be the first tab you see").
    // ★ The radios still show their own state on the tabs, so a half-finished one is not hidden by
    //   this — a red tab with a dot is a better prompt than being teleported to it.
    const list = radioList();
    curRadio = -1;
    if (list.length > 1) $("saveRadioBtn").classList.remove("hide");
    renderTabs();
    fill();
    // ★ Also picks up a change written BEFORE a reboot that has since happened — the page can
    //   then confirm it took, which is the whole point of keeping the marker on disk.
    serialStatus();
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
  // ★ The same wait as the master save: this button is the ONE most likely to be pressed straight
  //   after opening a tab, which is precisely the window where the form is still the last radio's.
  await hwPending;
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
    // ★ Same reasoning as the master save: name the fault, do not guess at the network.
    list[curRadio].configured = false;
    $("saveErr").textContent = "Save failed — " + ((e && e.message) ? e.message : String(e));
    $("barMsg").textContent = "";
  }
  $("saveRadioBtn").disabled = false;
};

$("saveBtn").onclick = async () => {
  $("saveErr").textContent = "";
  $("saveBtn").disabled = true;
  $("barMsg").textContent = "Saving…";
  // ★★★ WAIT FOR THE FORM TO BELONG TO THIS RADIO. stashRadio() refuses to read a form that is
  //     still showing the radio you just left — which protects the config, but on its own would
  //     silently discard an edit made in that window. Saving a tab you have only just opened is
  //     exactly what an owner setting up two radios does, so the save waits instead.
  await hwPending;
  try {
    // ★ restart:true is what separates this from the per-radio save above — see the server's
    //   config handler, which only bounces the receiver when it is asked to.
    const body = collect();
    // ★★★ THE SERVER TAB HAS NO RADIO, AND curRadio IS -1 THERE. This tested only that the list was
    //     non-empty, so on the Server tab it evaluated radioList()[-1].configured = true —
    //     TypeError: Cannot set properties of undefined — thrown BEFORE the request was built. The
    //     catch below then reported "Could not reach the server", so the devtools network tab
    //     showed ZERO requests while the page blamed the network.
    // ★★★ IT IS THE FIRST THING SABER REPORTED and it took all day to find, because the message
    //     named the wrong subsystem: I chased the front door, a 503, a restart race and a flush
    //     race before "0 requests in the network tab" made it obvious the fetch never happened
    //     (2026-08-09). The Server tab is ALSO the tab the page opens on, so this is the first
    //     thing a new owner does.
    // ★ Marking a radio configured belongs to a radio's own tab; from the machine's tab there is
    //   nothing to mark.
    if (curRadio >= 0 && radioList()[curRadio]) radioList()[curRadio].configured = true;
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
    // ★★★ A PENDING SERIAL NEEDS A REBOOT, NOT A RESTART. Restarting the service cannot power-
    //     cycle a USB port, so the dongle would still answer to its old name and the change would
    //     look like it had silently failed. The reboot also lets the boot-time reconciliation
    //     rename vibeserver@<serial>.service, which a restart cannot do either.
    const needReboot = SERIAL_PENDING && !SERIAL_PENDING.took;
    if (needReboot) {
      try {
        await fetch("/vibeserver/admin/reboot?" + await authQuery(), {method: "POST"});
      } catch (e) { /* the machine going down mid-request is the expected outcome */ }
    }
    $("barMsg").innerHTML = needReboot
      ? '<span class="ok">Saved. Rebooting so the new serial takes effect…</span>'
      : '<span class="ok">Saved. Restarting the receiver…</span>';
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
          if (isNew && j.configured) {
            // ★ Verify rather than celebrate: a reboot that happened is not the same as a serial
            //   that changed, and the owner should be told which of those they got.
            await serialStatus();
            backUp();
            return;
          }
        } catch (e) { /* still down — expected, and the point */ }
      }
      $("barMsg").textContent = "Saved, but the server has not come back. Check it on the machine.";
      $("saveBtn").disabled = false;
    };
    waitBack();
  } catch (e) {
    // ★★★ SAY WHAT ACTUALLY WENT WRONG. This reported "Could not reach the server" for ANY
    //     exception in the block above — including a plain TypeError thrown before the request was
    //     ever made. Saber spent a day being told the network had failed while his devtools showed
    //     ZERO requests leaving the page, which sent me chasing the server, the front door, a
    //     restart race and a flush race in turn (2026-08-09). A message that names the wrong
    //     subsystem is worse than no message: it is a false lead with authority.
    // ★ The network case still reads naturally — a fetch that genuinely cannot connect throws
    //   "Failed to fetch", which is exactly what an owner should see and pass on.
    $("saveErr").textContent = "Save failed — " + ((e && e.message) ? e.message : String(e));
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
