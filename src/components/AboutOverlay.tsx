/**
 * AboutOverlay — full-screen "About VibeSDR" page (opened from the menu
 * footer). What's new in V4, full credits for everything borrowed or built
 * on, and the GPL-3.0 licence statement. Pure native scroll view styled to
 * match BrowserOverlay's bar + the a11y menu skin.
 */

import React from 'react';
import {
  Alert, Image, Linking, Modal, ScrollView, Share, StyleSheet, Text, TouchableOpacity, View,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import Constants from 'expo-constants';   // nativeBuildVersion = the actual installed CFBundleVersion
import { APP_VERSION } from '../constants/version';
import { buildDiagnostics } from '../services/diagnostics';

/** Support address, same as the one in PRIVACY.md. */
const SUPPORT_EMAIL = 'stuey3dttb@icloud.com';

export interface AboutOverlayProps {
  visible: boolean;
  onClose: () => void;
}

// Stuart's personal message — source: reference/VibeSDR About.rtf (proofread,
// voice preserved; V1-era references updated for V2).
const STUEY_URL = 'https://stuey3d.tunnel.ubersdr.org/';
const STUART_MESSAGE: string[] = [
  'Hi, my name is Stuart (Stuey3D) and I am the UI designer and tester for this project.',
  "VibeSDR came about from the frustration of trying to use extremely powerful WebSDR servers on mobile devices, only to be served a broken website that wasn't optimised for mobile and tried to fit the entire desktop site onto a phone screen. I'd had an idea for a mobile-optimised SDR interface for a while, but I am not a developer and I don't know how to code, so I had no way of realising my UI ideas.",
  "After some back and forth with M9PSY, the creator of the amazing UberSDR web SDR software, he made a basic mobile web UI for UberSDR — some desktop assets reused and repositioned for mobile — but most of UberSDR's cool features, such as the decoders and maps, were still not available to users. I had helped M9PSY test various features over the previous few weeks and suggested new ones, and he encouraged me to make my own add-ons for UberSDR, since it already had sections where a user could add their own code for info banners or badges. I told him I was no coder, but he said just get AI to do it — I was good at testing and providing feedback, and that's all the AI needed.",
  "A bit sceptical, I started by getting ChatGPT to create a simple PSKReporter tracking badge, which grew to be quite sophisticated with all sorts of stats tracking and graphs. I was amazed at how quickly I got it to such an advanced level — even though with ChatGPT you'd fix one thing and break ten others, so many hours were spent testing everything. During this time, at my suggestion, M9PSY added a widget gallery to UberSDR that lets a user add up to ten widgets to their page — so my PSKR badge was suddenly a widget any UberSDR instance owner could install at the click of a button.",
  'I was so excited about my progress with the PSKR badge that I chatted to M9PSY about it, and he said: "I\'ll let you into a little secret — Claude is better."',
  "So with that knowledge I embarked on my next widget: a search bar for the bookmarks, as at the time UberSDR lacked a bookmark search feature. Guessing M9PSY was probably fed up with me badgering him for new features, I decided to build my own bookmark search (which is part of this app too). It turned out very well, so I showed it off to M9PSY — who said he had already built a bookmark search but hadn't enabled it yet. That being said, I asked whether his refreshed dynamically during a session, as the EiBi bookmarks change with station schedules. His loaded once on page load and that was it — so my search bar had more functionality, having discovered the dynamic bookmarks early.",
  'M9PSY kept encouraging me and explained that a widget is HTML code that loads last and can be very powerful — even removing the entire UI and replacing it. Interesting, I thought: maybe Claude and I could build a UI skin that would make controlling UberSDR better.',
  'Over 500 builds of the UI — which I dubbed Pocket UberSDR at the time — Claude and I managed to get around 90–95% of the full UberSDR desktop functionality running in a mobile-optimised interface. We got most of the radio decoders working on-device, plus viewers for the on-server add-ons such as Digital Spots, CW Spots and HFDL aircraft tracking. I even beat M9PSY to getting server-side noise reduction working on mobile.',
  'That UI was a labour of love, with many late nights and early mornings spent testing on multiple devices and reporting back, and it became what I truly believe to be the ultimate mobile SDR interface. M9PSY openly admits he is old skool and doesn\'t really use mobile devices, so mobile was much lower down his priority list for UberSDR. Armed with my new confidence, and with Claude by my side, I like to think I successfully saved him a job.',
  "One big issue remained: my UI was an optional skin/widget for UberSDR, and for everybody to be able to use it, individual server owners would need to add it to their servers. I felt a little deflated — most users set up their servers, forget them, and leave them running without even adding the cool new features M9PSY has been building (such as 24/7 server-side WEFAX/SSTV/NAVTEX decoding), so what chance did my UI have if server owners wouldn't add it? And then it hit me: I'll make an app I can share directly with users. Like Thanos, I thought \"Fine, I'll do it myself\" — and the app you are using is the result of that thought.",
  'When you load the app you are taken to the server list, where you can set a default that loads straight away (ideal for server owners, or if you simply have a favourite). And regardless of whether the server owner has added my UI to their server, you get the full mobile-optimised interface — tested all the way down to a 4-inch screen (iPhone SE in Display Zoom mode).',
  "I am extremely proud of this app. It has been a labour of love and a lot of testing — but I am simply one man with no coding experience, just a vision and a tool (Claude) to realise that vision. I also believe in openness and honesty, and I will never hide the fact that this app was coded with AI — which is why it is called VibeSDR, to really lean into the vibe-coding aspect of it. I know some people do not like AI, due to its resource usage and the amount of slop posted daily on social media. For me, I would not feel happy sharing this app if I hadn't tested it as much as one man can, and the name VibeSDR was chosen so that people who are anti-AI can choose not to use this app if they feel that's right for them.",
  "Is this app perfect? Probably not. Will there be bugs? Yeah, probably. I have genuinely spent hours upon hours testing, and a lot of time fixing tiny things like alignment issues that would have been \"good enough\" — but I wanted them perfect. I really hope that when you use this app you can see the thought and love that have gone into it, and that everything is laid out logically.",
  "Well, that's it — my app-creation history laid out bare. I hope you enjoy this app as much as I enjoyed making it.",
];

const VERSION_HISTORY: { v: string; detail: string }[] = [
  { v: 'V1', detail: 'Initial version — UberSDR support only, using the server-provided waterfall and spectrum.' },
  { v: 'V2', detail: "Fully native rewrite with a custom GPU waterfall/spectrum stack (V1's headline future plan, delivered), native audio with background playback and media controls, on-device decoders, chat, bookmarks and much more — see What's New above." },
  { v: 'V2.0.1', detail: 'Bug fixes: bandwidth sliders now match the server’s 6 kHz limit (were going to 8 kHz), and bookmark/band-plan search shows the full result list in a scrollable dropdown (previously capped at 25 results, so higher bands like 20m were cut off).' },
  { v: 'V2.1', detail: 'In-car upgrades: Android Auto now shows browsable Bookmarks and Band Plan lists (tap to tune), not just skip buttons. Band-aware tuning sets the right demodulator and step for each band — applied when you pick a band from the search list, and automatically as you cross band edges while connected to a car (handheld tuning is never changed automatically). CarPlay browsing is ready for when the entitlement is in place.' },
  { v: 'V2.1.1', detail: 'Band-aware mode/step now also applies to remote tuning — lock screen, Apple Watch and connected headphones — not just the car. It still stays out of your way while you’re hands-on tuning with the VFO drum on the waterfall.' },
  { v: 'V2.1.2', detail: 'Band-aware tuning tweaks: utility and beacon (NDB) ranges now default to USB / 500 Hz, and the 11m CB band follows the receiver’s ITU region — NFM in Europe/Asia-Pacific, AM in the Americas.' },
  { v: 'V2.1.3', detail: 'Media-control skips (lock screen, Apple Watch, Android Auto, headphones) now snap to the step grid like the VFO drum — so skipping from an off-grid frequency lands on a clean multiple of the step rate.' },
  { v: 'V2.1.4', detail: 'Fixes a frozen waterfall when returning to the app after another audio app suspended it (it now reconnects on its own). Adds a Data Saver: after a chosen spell muted — lock screen, AirPods out, or pause — the SDR stream disconnects to stop wasting data and battery. Pick the timeout under Power Saving (Off / Instant / 5–30 min); the media controls show a countdown, then “Open App to Resume”, and Play reconnects.' },
  { v: 'V2.1.6', detail: 'Data Saver moved to the main menu (under Controls). Pause now genuinely pauses on iOS — the lock-screen button no longer springs back to play, and the app stops grabbing shared AirPods from a Mac while paused. The Admin section is now labelled “Instance Admin”.' },
  { v: 'V2.1.7', detail: 'When another app takes over audio (e.g. a Mac grabbing your shared AirPods, or another media app on the phone), VibeSDR now registers it as a mute — the muted banner shows and the Data Saver countdown starts — instead of silently sitting connected. Works on both iOS and Android now. Press Play to come back.' },
  { v: 'V2.1.8', detail: 'The album art now reflects state at a glance: the server-logo corner becomes a muted-speaker with the minutes-to-disconnect while paused, and a disconnected icon once the Data Saver drops the stream.' },
  { v: 'V2.1.9', detail: 'Resuming after a Data Saver disconnect now does a full from-scratch reconnect (new session) instead of reopening the old one — fixes the frozen waterfall / dead-audio state that previously needed a trip back to the instance list.' },
  { v: 'V2.1.10', detail: 'Data Saver polish: while paused the media controls show a static “auto-disconnect at HH:MM to save data & power”, and once it disconnects the controls are released entirely (no half-working Play button). Reopening the app fully reconnects and unmutes.' },
  { v: 'V2.1.11', detail: 'Simpler pause/play: pause now disconnects and play reconnects (the server lets the session go on suspend anyway, and reconnecting is near-instant) — no more mute timeout or countdown. The media card shows a clear Disconnected state, and a “Failed to reconnect — open VibeSDR” state with an ⚠️ if the server is full or busy. Also fixes the SNR meter drifting with zoom level (the noise floor is now measured zoom-independently; dBFS and S-meter were already fine).' },
  { v: 'V2.1.12', detail: 'SNR meter now reads radiod’s channel SNR (baseband power − noise density) straight from the audio stream — the demodulator’s own measurement of the tuned channel — so it’s accurate against the local noise floor and completely independent of the waterfall zoom. (Corrected for radiod’s +30 dB audio-floor offset, so it stays honest 0–50 dB rather than the inflated 30–80 dB.)' },
  { v: 'V2.2', detail: 'Siri voice control (iOS). Say "Hey Siri, tune VibeSDR" — Siri asks what — then a frequency (7.150 MHz / 7150 kHz / 7151.5), a station ("Radio Caroline"), or a band ("40m ham", "CB"). It tunes with the right demodulator + step, honouring any spoken mode. When a name matches several bookmarks (e.g. "Radio 5") Siri reads the frequencies and you pick by voice; "China Radio at 11 MHz" narrows the list. Also "change VibeSDR mode" → AM / SAM / synchronous AM / LSB / lower side band / …, and "set VibeSDR step rate" → 100 Hz, 9 kHz, … It runs in the background while VibeSDR is playing, so it works over headphones / CarPlay / the lock screen without unlocking. (Tuning is a two-step ask-and-answer — Apple only allows a value inside a one-shot Siri phrase for fixed lists. Android’s in-car answer stays the Android Auto Bookmarks/Band-Plan browse — Google Assistant needs Play Store publishing.)' },
  { v: 'V2.2.1', detail: 'In-car fix: a Siri voice command interrupts the car audio session, which paused and disconnected VibeSDR — it then sat dead until you pressed Play. It now auto-resumes (reconnects on the new frequency) the moment Siri finishes, with no manual Play. A genuine takeover by another app (e.g. a Mac grabbing your AirPods) still waits for Play, as before.' },
  { v: 'V2.2.2', detail: 'Store-readiness pass: clearer location prompt (it’s only for sorting/filtering instances by distance, and denying it changes nothing else); removed two unused Android permissions (microphone and draw-over-other-apps); added a privacy policy and an App Store distribution exception to the GPL licence. No functional changes.' },
  { v: 'V3', detail: 'Multi-backend release — VibeSDR now speaks three SDR server protocols (UberSDR, OpenWebRX/OpenWebRX+ and KiwiSDR) behind the same interface, with a new directory chooser in the server picker. (KiwiSDRs have very few slots, so owners choose who connects — some refuse apps or block broadcast bands. A refusal or sudden drop is the owner\'s restriction, not a fault in VibeSDR.)' },
  { v: 'V4', detail: 'Local SDR hardware — VibeSDR now runs a radio on-device. Plug an RTL-SDR into an Android phone over USB (“Local Hardware”), or connect to a networked rtl_tcp server from either platform, and the app demodulates AM/SSB/CW/NFM/WFM itself with a bundled on-device DSP core — full waterfall, drum, audio and decoders, with a hardware-control submenu. Adds an MMSE noise-reduction engine and an adaptive Auto Notch (on every backend), plus a client-side dBFS squelch for KiwiSDR. (V5 later replaced that DSP core with VibeSDR’s own clean-room engine.) See What\'s New above.' },
  { v: 'V5', detail: 'New on-device DSP engine — SDR++ Brown (and FFTW and VOLK) have been REMOVED and replaced with VibeDSP, VibeSDR\'s own clean-room, GPL-free signal-processing engine for Local Hardware and RTL-TCP. It is hand-optimised with ARM NEON SIMD throughout, so it runs noticeably cooler and lighter on the battery — especially on low-end phones and tablets — while matching the old engine. It also brings real improvements: true single-sideband SSB (proper image rejection, not double-sideband), genuine FM stereo with a 19 kHz pilot PLL + RDS, a per-channel audio AGC for SSB/CW, working de-emphasis (50/75 µs), a reliable stereo indicator and a force-mono switch. See What\'s New above.' },
  { v: 'V5.0.1', detail: 'CW fix for Local Hardware (USB RTL-SDR): tuning straight onto a CW signal used to go silent — the beat-note offset and the actual filter width had drifted apart, so the morse was only audible when tuned well off the signal. They\'re now kept in sync, so a signal tuned dead-on gives a clear, audible ~600 Hz tone with readable morse. The mode pill also reads “CW” to match the button.' },
  { v: 'V5.1', detail: 'Unlocked VFO + waterfall panning, and a saved-recordings player. A new VFO Lock toggle (menu) lets you free the waterfall: locked (default) is exactly as before, unlocked lets you drag to pan the band while staying tuned, with a floating “Centre on VFO” button. Gestures are now tap-to-tune, drag-to-pan, pinch-to-zoom, with panning moved onto the UI thread for smoothness on poor connections. On Local Hardware / RTL-TCP the dongle (RF) centre becomes a true second VFO — with the VFO unlocked you can pan the view across the full captured bandwidth at native resolution while a station stays tuned, the dongle locking at the capture edge with an RF-CENTRE marker and hard walls. New Recordings screen lists, plays (with scrub), shares and deletes your recordings in-app — no more recordings stranded in storage. See What\'s New above.' },
  { v: 'V5.1.1', detail: 'OpenWebRX squelch & noise reduction now follow the server’s presets. If a server owner has set a default squelch level on a profile (e.g. a 2 m NFM profile at −65 dB) — or an initial noise-reduction level — selecting that profile now applies it automatically, with the menu sliders updated to match, instead of staying off or stuck on your previous setting. Matches the OpenWebRX web client’s behaviour.' },
  { v: 'V5.1.2', detail: 'iOS 26/27 audio fix. On iOS 27 the audio could go silent after a while even though the connection looked fine — the underlying audio socket was stalling (reporting alive while frames quietly stopped). The native audio path was moved onto Apple’s Network framework, with a session safety-net that keeps the stream rendering, fixing the dropouts. iOS-only.' },
  { v: 'V5.1.3', detail: 'Polish + reliability. New first-launch info screen explaining the power-saving behaviour (the waterfall fully freezes in the background by design and takes a moment to resume). Returning from the background now shows a calm “Reinitialising” notice while the waterfall comes back, instead of a misleading “Connection lost” — and if the spectrum genuinely fails to resume while audio keeps playing, you get a clear reconnect / instance-list prompt. Fixed a swipe-up-from-the-home-bar gesture that could nudge the tuning, and fixed the menu’s MIN / MAX zoom buttons (full-out / full-in) which previously did nothing.' },
  { v: 'V5.1.4', detail: 'KiwiSDR servers don’t support chat, so the Chat button is now greyed out and disabled while you’re connected to a KiwiSDR — the Share button stays available. No other changes.' },
  { v: 'V5.1.5', detail: 'Android layout fix: on phones using the classic three-button navigation bar, the menu’s CLOSE button could sit underneath the system buttons and be hard to tap. The menu now respects the navigation-bar inset so CLOSE always clears it. No effect on gesture-navigation devices. Android-only.' },
  { v: 'V5.2.0', detail: 'Deep linking (early feature, still being rolled out). A vibesdr:// link — and a QR code from an UberSDR instance — can open VibeSDR straight onto that instance, optionally at a set frequency and mode. The link/QR side is still being built on the UberSDR end, so not every instance offers a link yet. The share button now also includes an “Open in VibeSDR” app link alongside the web link. Opening a link no longer bounces back to your default instance.' },
  { v: 'V5.2.1', detail: 'Privacy: the optional location used to sort the instance list by distance is now taken and shared at approximate (coarse, ~1 km) accuracy only, instead of a precise fix. Location stays entirely optional and every other feature works without it.' },
  { v: 'V5.2.2', detail: 'iPad and tablet polish. The signal meter now frames the frequency correctly on tablets (the coloured level showed above and below the readout on phones but not on larger screens), and the on-screen decoders (RTTY, NAVTEX, WEFAX, SSTV, Morse) now work in landscape on tablets, which have the room a phone doesn’t. The HAPTICS toggle is now hidden on devices with no haptic motor (all iPads, and any Android tablet without one) so it’s no longer a dead button.' },
  { v: 'V6.1.0', detail: 'Networked RTL-SDR. VibeSDR now auto-discovers rtl_tcp servers on your Wi-Fi (via Bonjour/mDNS) and lists them under a new “Discovered” section — no IP typing needed (iOS and Android). And on Android you can now share a plugged-in RTL-SDR over the network as an RTL-TCP server, so an iPhone or any rtl_tcp client can use the dongle remotely — handy for a good antenna location or an always-on phone. Includes an optional bandwidth cap, an editable name shown to other devices, and a live status notification. Plugging in an RTL-SDR on Android now asks whether to listen on the device or share it. The location and local-network permission prompts also now explain exactly what they’re for.' },
  { v: 'V10.0.0', detail: 'VibeSDR Jr \u2014 a whole SDR on your wrist with no phone at all \u2014 plus VibeServer on the Mac, Airspy HF+ support, Advanced RDS calibrated against a professional analyser, a GPU waterfall with unsharp sharpening, automatic link management, iCloud sync, full keyboard control, and the Apple Watch companion renamed VibeSDR Buddy. See the What\u2019s New list above for the full account. \u2014 FIXED in this release: FM de\u2011emphasis and FM stereo never reached a networked VibeServer; tuning steps stopped at 1 kHz above 30 MHz, putting some airband channels out of reach entirely; the demodulator popup\u2019s RF gain slider only spoke the RTL\u2011SDR\u2019s gain model and did nothing on an Airspy or SDRplay, so it has been removed; and the guided tour pointed at the wrong menus for noise reduction and bookmarks.' },
  { v: 'V9.0.0', detail: 'The Apple Watch companion \u2014 the waterfall itself, live on your wrist, drawn from the same data and the same palette as the phone. Turn the Digital Crown to tune, tap the frequency to type one, press and hold for the menu (demodulator, tuning step, zoom, servers). It works with the iPhone LOCKED IN YOUR POCKET, which is the whole point \u2014 and it will start the phone for you: open the watch app with VibeSDR closed and the phone wakes in the background, connects to your default receiver, and the waterfall arrives on your wrist without the phone screen ever coming on. Four screens, chosen by what the receiver actually is: spectrum waterfall, FM-DX tuner (station, distance, RDS), DAB service list, and ADS-B aircraft. Switch receivers from your favourites without touching the phone. Control the iPhone\u2019s system volume and mute from the wrist \u2014 it reads the phone\u2019s REAL volume, including changes you make on the phone, so the two can never disagree. It shows the band you\u2019re in, in words, from the ITU plan for wherever the RECEIVER is, with marks on the ticker showing where that band ends \u2014 and your watch\u2019s own battery, because this is an app you might leave running on a hilltop. When the link is rough it tells you WHICH link: there are two radio hops in the chain (phone-to-server, watch-to-phone) and they fail independently, so a small diagram shows which one is struggling, over a waterfall that keeps drawing. \u2014 FIXED: the waterfall could freeze for good on a locked phone on mobile data and never come back, while audio and tuning carried on working perfectly. A mobile network can silently invalidate a connection without ever closing it, and nothing was watching for that: the spectrum socket sat there, open and dead, forever. VibeSDR now actively probes the link, rebuilds it the moment it stops answering, and reacts instantly when the phone changes network. \u2014 VibeServer: the web client now shows the server\u2019s NAME as well as its address, so there is no IP to remember. FIXED: on first use its sample-rate box read 3.2 MS/s while the receiver was actually running at 2.4 \u2014 it now defaults to 2.4 (the fastest an RTL-SDR can reliably sustain), tells the receiver so the two agree, and marks the higher rates as liable to drop samples. FIXED: on a 1366\u00d7768 laptop the control bar\u2019s buttons overlapped and the signal readout ran off the edge of the window.' },
  { v: 'V8.0.1', detail: 'Fix: saved favourites could start connecting as a VibeServer instead of the UberSDR receiver they actually are \u2014 and the wrong answer was then saved back onto the favourite. VibeSDR now identifies a VibeServer by a marker only a real one carries, and repairs any favourites that were mislabelled.' },
  { v: 'V8.0.0', detail: 'VibeServer \u2014 turn an Android phone with an RTL-SDR into a receiver anyone on your network can use, from a browser or from VibeSDR itself. The serving phone does all the DSP and sends compressed audio and a ready-made waterfall, so it is roughly 25\u00d7 lighter on the network than raw RTL-TCP and works comfortably over Wi-Fi or a hotspot. Point any browser at the phone\u2019s address and you get the full VibeSDR client \u2014 waterfall and spectrum with the same palettes, click-to-tune, panning and cursor zoom, audio with recording, the decoders (RTTY, NAVTEX, WEFAX, SSTV, FT8 with its map), station search, bookmarks you can export, and OS media controls with artwork. Access is protected by a PIN using challenge-response, so the PIN itself never crosses the network, and you can switch the web client off entirely so only the VibeSDR app can connect. You can leave clients free to choose their own bandwidth, or pin it \u2014 pinned, their picker disappears and says the server set it. The receiver can publish its own location (opt-in, never assumed) by device position or by naming a town or Maidenhead locator, and clients then show the receiver\u2019s name and place, and measure spot distances and band edges from the ANTENNA rather than from wherever the listener happens to be. If the app is killed while serving, the server rebuilds itself and carries on. \u2014 The RTL-TCP box becomes CUSTOM SERVER: type any address and VibeSDR works out what is listening (VibeServer, OpenWebRX, KiwiSDR, UberSDR, FM-DX, rtl_tcp or SpyServer), so one box reaches every backend. Local hardware is now RTL-SDR, with Listen and Use as server side by side. \u2014 Fixes: entering a frequency in a different band now switches to the right demodulator and span (jumping from a medium-wave station to FM used to leave you in AM with a 5 kHz filter); the waterfall no longer shows half a minute of stale history after a big jump; the lower sample rates (0.96 and 1.2 MHz) no longer break up; rtl_tcp no longer plays chipmunks on some rates; dragging the gain slider no longer breaks the audio; panning past the tuned station no longer drops audio or crawls; and auto-contrast now defaults to 5 (10 was too dark). \u2014 The receiver also NAMES THE STATIONS IT CAN HEAR: when a station announces itself over RDS, VibeSDR remembers it against the frequency, so the search bar fills itself in with what this aerial actually receives. It keeps itself honest \u2014 the PI code spots a different broadcaster on a frequency immediately, and a station that goes unheard for 30 days expires rather than sitting on top of static. The name is reconstructed by majority vote across repetitions, so it can recover a name no single transmission delivered cleanly. Save stations to the receiver (shared with everyone) or to your own browser, and import an existing list to either. \u2014 Point a browser at vibesdr.local: no IP address to remember, and a second phone serving on the same network renames itself automatically. \u2014 Station logos and country flags now actually appear, on every backend and on AM and shortwave too, not just FM; where the country genuinely cannot be known (a station arriving on sporadic-E, say) VibeSDR declines to show a flag rather than showing your own country\u2019s.' },
  { v: 'V7.1.0', detail: 'SpyServer compatibility, a reorganised audio menu, and today’s fixes. VibeSDR now connects to SpyServer receivers via sdr:// links (tap one anywhere, or paste sdr://host:port into the Custom URL box) and can save them as favourites — low-bandwidth, so good over mobile data. All audio controls (noise reduction, noise blanker, squelch, auto-notch, recording + playback) moved into a new Audio button to declutter the main menu; the demodulator popup gained the bandwidth sliders and Share moved next to the frequency keypad. You can now favourite the receiver you’re listening to straight from the menu. Every menu section gained a small icon for easier scanning. On FM-DX, recording/library moved into the same Audio button. Fixes: sharing a recording no longer freezes the controls; the waterfall no longer blanks on USB/RTL-TCP at full zoom-out; and iOS cold-start deep links open the linked instance.' },
  { v: 'V7.0.1', detail: 'Networked-radio stability and two iOS fixes. Sharing an RTL-SDR over a phone hotspot or busy Wi-Fi is far more reliable: the sharing phone now holds a Wi-Fi lock so its radio can’t drop into power-save mid-stream, the receiving side keeps a short buffer so a brief Wi-Fi stall no longer breaks the audio, and the sharing screen shows a live link-health indicator. iOS: FM stereo now actually plays in stereo on local hardware and RTL-TCP (it was quietly downmixed to mono), and scanning a QR code or opening a vibesdr:// link with the app closed now goes to the correct receiver instead of your default one.' },
  { v: 'V7.0.0', detail: 'FM-DX Webserver support — a whole new kind of receiver. VibeSDR now connects to the worldwide network of FM-DX Webserver tuners (from servers.fmdx.org): real, remote FM broadcast tuners you share with other listeners. New vintage-radio tuning dial that learns and pins every station name as you tune across the band, full RDS (station name, RadioText, PI code, PTY, TP/TA, stereo), a dBf signal meter, transmitter details (site, power, distance and bearing from the receiver), tap-to-tune alternative frequencies, station logos and country flags. Because the tuner is shared, there’s built-in chat, a listener counter, and the lock-screen skip buttons are disabled so you can’t accidentally retune it for everyone. The demodulator button opens mono/stereo, cEQ, iMS and an antenna switch (when the server offers one). Station logos and country flags now also appear on local RTL-SDR and networked WFM using the RDS PI code, with an on-device logo cache so they persist even offline. Pausing from the lock screen disconnects to save battery and reconnects on play.' },
  { v: 'V6.0.0', detail: 'A major under-the-hood upgrade for iOS 27. VibeSDR now builds on React Native’s New Architecture (required for iOS 27 / Xcode 27 support, since Apple no longer accepts the older toolchain). Alongside that: RTL-SDR local hardware and RTL-TCP tuning is fixed — typing a frequency now retunes cleanly first time (it previously needed a nudge of the tuning drum), a race in the on-device tuner has been eliminated. Local Hardware and each RTL-TCP source now remember their own last frequency, mode and hardware settings independently (including VHF/UHF stations, which used to reset). Plugging an RTL-SDR into an Android phone and choosing “Open in VibeSDR” now goes straight to Local Hardware instead of your default instance. On Android, background audio now correctly holds up on devices that aggressively restrict apps — if your phone is throttling VibeSDR in the background, the app now detects it and shows you how to allow background usage. Plus the first-launch tutorial no longer appears on top of the welcome screen.' },
];

const FUTURE_PLANS: string[] = [
  'There’s no fixed roadmap from here — V4 delivered the big one (local SDR hardware) and V5 replaced its engine with VibeSDR’s own GPL-free DSP. Ongoing work is polish, more decoders and more backends as they come. If general USB SDR access ever lands on iOS, the on-device engine is already cross-platform (it powers RTL-TCP on iPhone today), so Local Hardware would follow.',
];

/** WHAT VIBESDR WILL NOT DO, and WHY — said plainly, up front, and without apology.
 *
 *  Each of these is a question people actually ask, and every one of them has an answer
 *  that makes VibeSDR look BETTER, not worse: they are principles and legal facts, not
 *  gaps. A limitation you explain is a design decision; the same limitation left in
 *  silence is read as a broken app.
 *
 *  The WebSDR note deliberately hands off into the CREDITS list that follows it — the
 *  point being that every backend we DO speak is either open source or has its author's
 *  blessing, and the credits are the evidence.
 */
const LIMITATIONS: { q: string; a: string[] }[] = [
  {
    q: 'Why no WebSDR support?',
    a: ['Intentional. WebSDR (websdr.org) is closed-source software, and its author has not sanctioned third-party clients. VibeSDR only implements platforms that welcome independent clients — every backend it speaks to is either open source or supported with its creator’s blessing (see Credits below). Out of respect for that principle, WebSDR support will not be added.'],
  },
  {
    q: 'Why no native DAB+, DRM, HD Radio, or DMR decoding?',
    a: [
      'Patents and codec licensing — not technical difficulty. The VibeDSP engine could implement these demodulators, but the audio codecs behind them are legally encumbered for a shipped app: HD Radio sits on Xperi’s patent portfolio, DAB+ and DRM on HE-AAC/xHE-AAC codec licensing, and DMR, D-STAR, Fusion and NXDN on the AMBE/IMBE vocoder patents. Shipping unlicensed implementations in App Store or Play Store builds is a risk VibeSDR will not take. Genuinely open digital voice modes — Codec2-based FreeDV and M17 — are unencumbered and remain candidates for native support.',
      'The supported route: many OpenWebRX / OpenWebRX+ servers decode digital modes server-side. When you select such a mode on one of those servers, VibeSDR simply plays the already-decoded PCM audio the server sends — no demodulator or codec ships in, or runs inside, the app. That’s why DAB+ works in VibeSDR on some servers despite none of these decoders existing in the app itself.',
    ],
  },
  {
    q: 'Why do the skip buttons vanish on FM-DX?',
    a: ['An FM-DX Webserver is one physical tuner shared by every connected listener — tuning it retunes it for everyone at once. Lock-screen and in-car skip buttons would let you change the station for people you can’t see, so they’re disabled out of courtesy while connected to FM-DX. You’ll see a reminder on the lock-screen artwork too. It’s the same principle behind the connection warning when you join an FM-DX server: one tuner, many listeners.'],
  },
];

const V10_CHANGES: string[] = [
  'VibeSDR Jr \u2014 a WHOLE SDR, on your wrist, with no phone at all. Not a remote control and not a companion: Jr connects straight to a receiver over Wi\u2011Fi or your watch\u2019s own mobile data, decodes the audio itself and draws its own waterfall. Leave the phone at home. Turn the Digital Crown to tune, tap the frequency to type one, press and hold the waterfall for the menu. It speaks UberSDR, VibeServer, OpenWebRX and KiwiSDR, with four screens chosen by what the receiver actually is \u2014 spectrum waterfall, FM\u2011DX tuner, DAB service list and ADS\u2011B aircraft. Audio keeps playing when you lower your wrist or press the crown, so it behaves like a radio rather than an app.',
  'Jr: the waterfall is genuinely yours to read. Auto contrast with a manual Floor and Ceiling when it guesses wrong on a strong signal, brightness and contrast, the full palette list, peak hold, and a band label in words from the ITU plan for wherever the RECEIVER is. Bookmarks, favourites, a numeric keypad, Water Lock support, and your watch\u2019s own battery next to the clock \u2014 because this is an app you might leave running on a hilltop.',
  'VibeSDR Buddy \u2014 the Apple Watch companion, now clearly its own thing alongside Jr. Buddy rides on the phone: it shows the same waterfall from the same data and the same palette, works with the iPhone LOCKED IN YOUR POCKET, and will even start the phone for you \u2014 open Buddy with VibeSDR closed and the phone wakes in the background, connects to your default receiver, and the waterfall arrives on your wrist without the phone screen ever coming on. When the link is rough it tells you WHICH link: there are two radio hops in the chain and they fail independently, so a small diagram marks the one that is struggling over a waterfall that keeps drawing.',
  'VibeServer \u2014 share your radio with the world. Plug an RTL\u2011SDR or an Airspy HF+ into your Android phone \u2014 or run VibeServer on a Mac, where it also drives SDRplay RSP receivers \u2014 and it turns that radio into a receiver anyone can listen to \u2014 in this app, on a watch, or in any browser, because a full web client is built in and needs no software installed at the other end. On macOS it lives quietly in the menu bar: set it to start with the Mac and it sits there waiting for a radio to be plugged in.',
  'VibeServer: the controls a public receiver actually needs. A PIN to keep it private, or leave it open. A per\u2011listener time limit with a visible countdown and a cool\u2011off before the same person can come back \u2014 without which a limit is decoration. An IN USE badge so nobody taps a busy receiver. And an admin password that lets the owner take their own radio back, unlock the protected settings (bias\u2011T, calibration, direct sampling) and listen without a time limit \u2014 sent as a one\u2011time cryptographic proof, so the password itself never crosses the network.',
  'VibeServer: it knows what radio it has. An SDRplay RSP (on the Mac) is offered its LNA state and IF gain reduction with a live total\u2011system\u2011gain readout so you can WATCH the AGC work; an Airspy HF+ gets its attenuator, preamp and AGC and the sample rates the radio itself reports; an RTL\u2011SDR gets its gain table. Controls that a given radio does not have are no longer shown \u2014 a slider that cannot move the receiver is worse than no slider.',
  'ADVANCED RDS \u2014 a broadcast analyser, on a phone. Not just the station name: the PI code and what it means, PTY, TP/TA/MS flags, RadioText and RadioText+ (what is playing right now), Long PS, PTYN, the clock, the language, alternative frequencies and the other stations in the network. Then the measurements a DXer would normally buy hardware for: pilot deviation, RDS deviation, the block error rate, a live constellation with a scatter figure, the MPX spectrum, and the symbol trace that explains the error rate at a glance.',
  'Advanced RDS: the RDS\u2011to\u2011pilot PHASE \u2014 the field this panel exists for. Correct is near 0\u00b0, or near 90\u00b0 for quadrature encoding; the middle ground indicates a transmitter fault, and reading it normally takes equipment most people do not have. It will also tell you when a station\u2019s encoder is not locked to its own pilot at all, which draws as a rotating ring rather than two lobes \u2014 a real fault, found on air, confirmed on two independent receivers.',
  'Advanced RDS was calibrated against a PIRA broadcast analyser by Hans van Eijsden \u2014 without whom these numbers would be guesses. Hans measured our readings against professional equipment station by station, found that our phase was reading as its own reflection and that the deviation figures were scaled wrong, and kept testing until they agreed. He is also the only route we have to testing Long PS, RadioText+ and PTYN, because UK broadcasters simply do not transmit them. Thank you, Hans.',
  'Airspy HF+ Discovery support, on Android and on the Mac \u2014 an outstanding receiver for FM DX, and now a first\u2011class one here: its own sample\u2011rate list, attenuator, preamp and AGC, with RDS decoding to match. Measured on air rather than assumed: on a strong local signal the preamp costs more than it gives, and the app now says what each control really does instead of offering three that look alike.',
  'FM\u2011DX: the OIRT band. Tuners set below 87.5 MHz \u2014 common across eastern Europe and Japan \u2014 were simply unreachable, because the dial refused to go there. The dial still opens at 87.5 so it is not a mile of empty scale, but it now GROWS the moment a server proves it can tune lower, and a tap puts it back.',
  'A RAZOR\u2011SHARP WATERFALL. The spectrum display was rebuilt to run on the GPU, with an unsharp\u2011mask sharpening pass and a sharpness control you can set to taste \u2014 weak signals that used to smear into the noise now stand up as distinct traces, and carriers resolve as lines rather than smudges. The same processing now runs in the built\u2011in web client, so a browser looks like the app. Smoother zooming and panning throughout, and an auto\u2011contrast that no longer lets one strong side lobe squash everything else flat.',
  'AUTOMATIC LINK MANAGEMENT. VibeSDR now works out how much your connection can actually carry and adapts to it, continuously \u2014 frame rate, resolution and audio all step up and down together rather than the picture simply breaking up. It opens deliberately slowly, because connection setup is when a marginal link is most likely to drop, and the link glyph cycles while it decides. Move from Wi\u2011Fi to mobile, walk to the edge of Bluetooth range, or share a receiver over a phone hotspot, and it settles to whatever that link will sustain instead of demanding what it cannot.',
  'Your favourites, bookmarks and settings now sync through iCloud, so a phone, an iPad and an Apple Watch all show the same list \u2014 and a deletion on one is a deletion everywhere, rather than something that quietly comes back.',
  'FULL KEYBOARD CONTROL on iPad and with any Bluetooth keyboard \u2014 the whole app, without touching the screen. Arrow keys tune and zoom, single keys pick the demodulator, Tab walks the panels and every control can be reached and operated from the keys. Press ? at any time for the complete shortcut list. It coexists properly with iOS Full Keyboard Access, which claims the plain arrows, Tab and Space for itself \u2014 so VibeSDR hands those back when a text field has focus and takes them when it does not.',
  'New controls throughout: a tuning drum you can flick, or switch it for tuner\u2011style buttons \u2014 your choice, on both the main screen and the FM\u2011DX dial. Long\u2011press to sweep, a step\u2011rate menu instead of cycling through a list, a VFO lock, and a zoom drum that pans as well as zooms. Threshold controls are now a LIVE meter with a ball you drag along it, so you set them against what the receiver is actually hearing rather than against a number.',
  'A REDESIGNED MENU. Everything moved to where you actually reach for it: the audio controls (noise reduction, noise blanker, squelch, auto\u2011notch, recording and playback) gathered into one Audio button, the demodulator popup carrying its own bandwidth sliders, the decoders together, and the radio\u2019s own hardware controls in a panel that changes shape depending on which receiver you are connected to. Sections carry icons so you can find things at a glance, and the settings that belong to the RECEIVER are kept apart from the ones that belong to YOU.',
  'The built\u2011in web client grew up: a sharper waterfall with the same unsharp\u2011mask processing as the app, a proper step\u2011rate menu, live station logos, and every control now shows its true state \u2014 noise reduction and the notch filter could read OFF while they were actually ON, which made a perfectly good receiver sound broken.',
  'FIXED \u2014 Advanced RDS could half\u2011die after a server had been left idle: groups, alternative frequencies and PTY kept arriving while the station name, PI and RadioText never appeared again, even on a 61 dB signal. FIXED \u2014 audio could keep playing after a session time limit had ended. FIXED \u2014 a shared radio could stall the audio and RDS while the spectrum carried on, needing a retune to recover. FIXED \u2014 VibeServer could abort when the first listener connected after a long idle period.',
  'FIXED \u2014 FM de\u2011emphasis did nothing on a networked VibeServer. The control was there and the server understood it; the app was the only client that never sent it, so 50\u00b5s and 75\u00b5s made no audible difference. FM stereo had the same gap. Both now reach the server.',
  'FIXED \u2014 tuning steps below 1 kHz above 30 MHz. Some airband channels sit on 100 Hz boundaries, so a VOLMET at 128.5928 MHz could not be reached with the controls at all \u2014 only by typing it, and on the watch not at all. 100 Hz and 500 Hz steps added.',
  'The RF gain slider has been removed from the demodulator popup. It only ever spoke the RTL\u2011SDR\u2019s gain model, so on an Airspy HF+ (which has no variable gain \u2014 an attenuator and a preamp) or an SDRplay RSP (IF gain reduction) it was a live\u2011looking control that did nothing. The correct per\u2011radio controls are in the hardware panel, where they always were.',
  'FIXED \u2014 the tour sent people to the settings cog for noise reduction and bookmarks. Audio moved to the speaker button and bookmarks to the frequency card some releases ago; the tour had never been told.',
]

const V9_CHANGES: string[] = [
  'VibeSDR on your wrist. A real Apple Watch app — not a remote control with a few buttons, but the waterfall itself, live on the watch, drawn from the same data and the same palette as the phone. Turn the Digital Crown to tune, tap the frequency to type one, press and hold for the menu (demodulator, tuning step, zoom, servers).',
  'It works with the iPhone LOCKED IN YOUR POCKET, which is the whole point. It will even start the phone for you: open the watch app with VibeSDR closed and the phone wakes up in the background, connects to your default receiver, and the waterfall arrives on your wrist without the phone screen ever coming on.',
  'Four screens, chosen by what the receiver actually is: the spectrum waterfall, the FM-DX tuner (with the station, its distance and its RDS), the DAB service list, and the ADS-B aircraft table. Switch receivers from your favourites without touching the phone.',
  'Control the iPhone’s volume from the wrist, and mute it. It reads the phone’s REAL system volume — including changes you make on the phone itself — so the two can never disagree.',
  'The band you are in, in words (“20m Ham Band”, “41m Broadcast Band”), from the ITU band plan for wherever the RECEIVER is — with marks on the frequency ticker showing where that band ends. And your watch’s own battery, next to the clock, because this is an app you might leave running on a hilltop.',
  'When the link is rough, the watch now tells you WHICH link. There are two radio hops in the chain — phone-to-server, and watch-to-phone — and they fail independently. A small diagram shows which one is struggling, over a waterfall that keeps drawing; tuning goes on working throughout, and the app says so rather than throwing up a blank screen.',
  'FIXED: the waterfall could freeze for good on a locked phone, on mobile data, and never come back — while audio and tuning carried on working perfectly. A mobile network can silently invalidate a connection without ever closing it, and nothing was watching for that: the spectrum socket sat there, open and dead, forever. VibeSDR now actively probes the link, rebuilds it the moment it stops answering, and reacts instantly when the phone changes network (Wi-Fi to mobile and back).',
  'VibeServer: the web client now shows the server’s NAME as well as its address (vibesdr-yourphone.local), so there is no IP to remember, and it survives your router handing the phone a different address tomorrow.',
  'VibeServer FIXED: on first use the web client’s sample-rate box showed 3.2 MS/s while the receiver was actually running at 2.4 — it was reporting a rate the radio was not using. It now defaults to 2.4 MS/s (the fastest an RTL-SDR can reliably sustain), tells the receiver so the two agree, and marks the higher rates as liable to drop samples.',
  'VibeServer FIXED: on a smaller laptop screen (1366×768 and below) the control bar’s buttons overlapped each other and the signal readout ran off the edge of the window. The whole bar now scales properly down to 1280×720.',
];

const V8_0_1_CHANGES: string[] = [
  'Fixed: your saved favourites could start connecting as a VibeServer instead of the UberSDR receiver they actually are. VibeSDR works out what kind of server it is talking to by looking at its web page — and it was treating the word “VibeSDR” as evidence of a VibeServer. But UberSDR receivers carry a “open in VibeSDR” link of their own, so a perfectly ordinary UberSDR looked like one of ours. Worse, the wrong answer was then saved back onto the favourite. VibeSDR now looks for a marker only a real VibeServer carries, and repairs any favourites that were mislabelled.',
];

const V8_CHANGES: string[] = [
  'VibeServer — turn an Android phone with an RTL-SDR into a receiver anyone on your network can use, from a web browser or from VibeSDR on another phone. Point a browser at the phone’s address and the full client is there: no install, no app.',
  'The serving phone does all the DSP and sends compressed audio plus a ready-made waterfall, so it is roughly 25× lighter on the network than raw RTL-TCP — it works comfortably over Wi-Fi, and even over a phone hotspot.',
  'The web client is the real thing, not a cut-down view: the same waterfall palettes and colouring as the app, click-to-tune, panning and cursor zoom, audio with recording, the decoders (RTTY, NAVTEX, WEFAX, SSTV, and FT8 with its map), station search, bookmarks you can export, the band plan, and OS media controls with artwork on the lock screen. The decoders run on the server, so a browser never has to do any DSP.',
  'PIN protected by challenge-response, so the PIN itself never crosses the network — or run it open on a LAN you trust. You can also switch the web client off entirely, so only the VibeSDR app can connect and nobody stumbles into your receiver from a URL.',
  'Reach it by name: point a browser at vibesdr.local — no IP address to remember. A second phone serving on the same network renames itself automatically.',
  'The receiver names the stations it can hear. When a station announces itself over RDS, VibeSDR remembers it against the frequency, so the search bar fills itself in with what this aerial actually receives rather than a schedule of what merely exists. The station’s PI code keeps it honest — a different broadcaster on the same frequency is spotted immediately — and the name is reconstructed by majority vote across repetitions, so it can recover a name that no single transmission delivered cleanly.',
  'Receiver location, opt-in. Granting location to sort a server list is not consent to broadcast your position, so publishing stays off until you choose it: use the device’s coarse position, name a town, or give a Maidenhead locator (which needs no internet — the shed case). Clients then measure spot distances, map centring and the regional band plan from the ANTENNA, not from wherever the listener happens to be sitting.',
  'The RTL-TCP box becomes CUSTOM SERVER: type any address and VibeSDR works out what is listening — VibeServer, OpenWebRX, KiwiSDR, UberSDR, FM-DX, rtl_tcp or SpyServer — so one box reaches every backend. Local hardware is now RTL-SDR, with Listen and Use as server side by side.',
  'Station logos and country flags now actually appear — on every backend, and on AM and shortwave too, not just FM. Where the country genuinely cannot be known (a station arriving on sporadic-E, say) VibeSDR declines to show a flag rather than showing your own country’s.',
  'Fixes: entering a frequency in a different band now switches to the right demodulator and span (jumping from a medium-wave station to FM used to leave you in AM with a 5 kHz filter); the waterfall no longer shows half a minute of stale history after a big jump; the lower sample rates (0.96 and 1.2 MHz) no longer break up; rtl_tcp no longer plays chipmunks on some rates; dragging the gain slider no longer breaks the audio; and panning past the tuned station no longer drops audio or crawls.',
];

const V7_1_CHANGES: string[] = [
  'SpyServer receivers: connect to a SpyServer by tapping or pasting an sdr://host:port link — the kind shared on the Airspy directory, forums and Discord. Paste it into the Custom URL box (or tap a link anywhere), confirm, and you’re listening; tap the heart to save it as a favourite. Low-bandwidth, so it works well over mobile data and hotspots.',
  'Reorganised audio controls: a new Audio button (the speaker) gathers everything audio in one place — noise reduction, noise blanker, squelch, auto-notch, recording and playback. The demodulator popup now holds the bandwidth sliders, and Share has moved next to the frequency keypad. The main menu is much less cluttered as a result.',
  'Favourite a server from the menu: found a good receiver while listening? Open the menu and tap Favourite — it’s saved to your list so you don’t have to remember which one it was.',
  'Clearer menu: every menu section now carries a small icon, so the settings are easier to scan at a glance whatever language you read. The Display Settings button now shows a monitor.',
  'FM-DX tidy-up: recording and the recordings library now live behind the same Audio button as every other backend (they used to sit in the FM-DX header), and the button pulses while recording.',
  'Fixes: sharing a finished recording no longer freezes the controls (an iOS pop-over could get stuck over the sheet); the waterfall no longer blanks on USB and RTL-TCP at full zoom-out; and a QR/deep link opened from a cold start on iOS now lands on the linked instance instead of your default.',
];

const V7_CHANGES: string[] = [
  '7.0.1 — Sharing an RTL-SDR over a hotspot or busy Wi-Fi is much more stable: the sharing phone keeps a Wi-Fi lock so its radio doesn’t power-save mid-stream, the receiving side holds a short buffer so a brief stall no longer breaks up the audio, and the sharing screen shows a live link-health indicator.',
  '7.0.1 (iOS) — FM stereo now plays in real stereo on local hardware and RTL-TCP; it was being downmixed to mono.',
  '7.0.1 (iOS) — Scanning a QR code or opening a vibesdr:// link with the app fully closed now opens the correct receiver instead of your default instance.',
  'FM-DX Webserver support: VibeSDR now connects to the worldwide network of FM-DX Webserver tuners (from servers.fmdx.org) — real remote FM broadcast tuners, shared live with other listeners. Pick one from the new FM-DX directory (or save it as a favourite).',
  'Vintage tuning dial: a green-on-black analogue-style band scale with a red needle that learns and pins every station name as you tune across the band. Pinch or use the zoom drum to zoom in on a crowded part of the band; tap the dial to tune.',
  'Full RDS + station info: station name (PS), scrolling RadioText, PI code, programme type, TP/TA and stereo, plus a dBf signal meter, the transmitter’s site, power, distance and bearing from the receiver, and tap-to-tune alternative frequencies.',
  'Station logos and country flags: shown for the tuned station — and now also on local RTL-SDR and networked WFM, matched from the RDS PI code. Discovered logos are cached on your device so they still show when you’re offline.',
  'Shared-tuner etiquette: because everyone shares one tuner, there’s built-in chat and a listener counter, a heads-up when you connect, and the lock-screen skip buttons are disabled so you can’t accidentally retune it for everyone.',
  'Demodulator options: tap the demodulator button for mono/stereo, cEQ, iMS, and an antenna switch when the server offers one (probed automatically on connect).',
  'Power-saving pause: pausing from the lock screen or pulling your headphones disconnects the shared tuner to save battery and network, and playback reconnects the moment you press play.',
];

const V6_1_CHANGES: string[] = [
  'Auto-discovery of RTL-TCP servers on your network: VibeSDR now finds rtl_tcp servers on your Wi-Fi automatically (via Bonjour/mDNS) and lists them under a new “Discovered” section — no need to type an IP address. Tap to connect, or tap the star to save it. Works on both iOS and Android.',
  'Share your RTL-SDR over the network (Android): plug an RTL-SDR into an Android phone and you can now run it as an RTL-TCP server — other devices (an iPhone running VibeSDR, SDR#, etc.) connect to it over Wi-Fi and use the dongle remotely. Great for putting the dongle somewhere with a good antenna, or on a phone that’s always on. Includes an optional bandwidth cap if the connection struggles, an editable name shown to other devices, and a live status notification.',
  'Plug-in choice (Android): plugging in an RTL-SDR now asks whether you want to listen on this device or share it over the network.',
  'Clearer permission prompts: the location and local-network permission requests now explain exactly what they’re for (sorting the instance list by distance / aligning the map, and discovering SDR servers on your network).',
];

const V6_CHANGES: string[] = [
  'iOS 27 support: VibeSDR is rebuilt on React Native’s New Architecture, required for the iOS 27 / Xcode 27 toolchain (Apple no longer accepts the older one). This is a big under-the-hood modernisation; everything you already use works the same.',
  'Type-to-tune fixed on RTL-SDR (USB) and RTL-TCP: typing in a frequency now retunes cleanly the first time. It previously needed a nudge of the tuning drum to “take” — a timing race in the on-device tuner has been eliminated (also fixes iPhone RTL-TCP).',
  'Per-source memory: Local Hardware and each RTL-TCP server now remember their own last frequency, mode and hardware settings (gain, sample rate, bias-T, etc.) independently — including VHF/UHF stations like FM, which used to reset to a default on reconnect.',
  'Plug-and-go on Android: plugging an RTL-SDR into the phone and choosing “Open in VibeSDR” now takes you straight to Local Hardware instead of your default instance.',
  'Background audio on aggressive phones (Android): some phones (e.g. certain Motorola models) restrict apps in the background by default, which broke up local audio when the screen was off. VibeSDR now detects this and shows you exactly how to allow background usage so playback holds up.',
  'Swiping VibeSDR out of the recent-apps list now fully shuts it down on Android — it no longer lingers as a media notification in the shade (GitHub #6). The Stop button in the media notification also closes it.',
  'The first-launch tutorial no longer appears on top of the welcome / power-saving screen.',
];

const V5_1_CHANGES: string[] = [
  'iPad/tablet polish: the signal meter now frames the frequency correctly on tablets, on-screen decoders work in landscape on tablets, and the HAPTICS toggle is hidden on devices with no haptic motor (all iPads) (new in 5.2.2)',
  'Privacy: the optional location used to sort the instance list by distance is now taken and shared at approximate (coarse, ~1 km) accuracy only, never a precise fix. It stays entirely optional (new in 5.2.1)',
  'Deep linking (early feature, still rolling out): a vibesdr:// link or QR code from an UberSDR instance opens VibeSDR straight onto that receiver, optionally at a set frequency and mode. The link/QR side is still being built on the UberSDR end, so not every instance offers a link yet. Sharing a tuned station now also includes an “Open in VibeSDR” app link (new in 5.2.0)',
  'Android fix: on phones with the three-button navigation bar, the menu’s CLOSE button could hide behind the system buttons — the menu now clears the navigation-bar inset so CLOSE is always tappable (new in 5.1.5)',
  'KiwiSDR servers don’t support chat, so the Chat button is now greyed out and disabled while you’re connected to a KiwiSDR instance (the Share button stays available) (new in 5.1.4)',
  'First-launch info screen explaining the power-saving behaviour: switching away fully freezes the waterfall/spectrum to save battery (it resumes in a second or two), and after 30 s on screen they slow down — which you can turn off in the menu. The full background freeze is by design and can’t be disabled (new in 5.1.3)',
  'Returning from the background now shows a calm “Reinitialising” notice while the waterfall comes back, instead of a misleading “Connection lost”. If the spectrum genuinely fails to resume while audio keeps playing, you get a clear prompt to reconnect or pick another server (new in 5.1.3)',
  'Fixed a swipe-up from the home bar that could nudge the tuning, and fixed the menu’s MIN / MAX zoom buttons (zoom fully out / fully in) which previously did nothing (new in 5.1.3)',
  'iOS 26/27 audio fix: audio could go silent after a while on iOS 27 even with a healthy connection — the native audio path was moved onto Apple’s Network framework with a render safety-net to stop the dropouts (new in 5.1.2)',
  'OpenWebRX squelch & noise reduction now honour the server owner’s per-profile presets: pick a profile that ships with a default squelch (e.g. a 2 m NFM profile at −65 dB) or an initial NR level and it’s applied automatically, with the menu sliders updated to match — instead of staying off or stuck on your last setting (new in 5.1.1)',
  'Waterfall panning is back, opt-in: a new VFO Lock toggle in the menu. Locked (the default) keeps the VFO centred exactly as before; unlock it and you can drag the waterfall to look around the band while staying tuned, with a floating “Centre on VFO” button to snap back',
  'Full waterfall gestures: tap to tune, drag left/right to pan, pinch to zoom — panning now runs on the UI thread so it stays smooth even on a busy/laggy connection',
  'Local Hardware / RTL-TCP get a true second VFO for the dongle (RF) centre: with the VFO unlocked you can pan the view right across the captured bandwidth at full resolution while a station stays tuned — the dongle centre locks at the edge and the RF-CENTRE marker slides off as you scroll, with solid walls at the capture limits',
  'Saved Recordings player: a new Recordings screen (from the menu) lists every recording you’ve made — play them back in-app with a scrub bar, share them, or delete them. No more recordings stranded in storage with no way to reach them. (Recordings are now kept in the app on both platforms.)',
];

const V5_CHANGES: string[] = [
  'SDR++ Brown, FFTW and VOLK are gone — the on-device radio (Local Hardware + RTL-TCP) now runs on VibeDSP, VibeSDR’s own clean-room DSP engine, written from scratch and free of any GPL third-party DSP',
  'Hand-optimised with ARM NEON SIMD across every hot path (FFT, filters, demodulators, resampler, IQ conversion) — runs cooler and uses less battery, especially on low-end phones/tablets',
  'True single-sideband SSB with proper image rejection (the Weaver method) — the wrong sideband is now silent, not bleeding through',
  'Genuine FM stereo: a 19 kHz pilot PLL with smooth stereo blend, a reliable stereo indicator, RDS station name/RadioText, and a force-mono switch for weak signals',
  'Audio AGC for AM/SSB/CW (steady level, no fading or crackle) and working FM de-emphasis (50 µs / 75 µs / off)',
];

const V4_CHANGES: string[] = [
  'Local SDR hardware (Android): plug an RTL-SDR into your phone over USB and pick “Local Hardware” — VibeSDR runs the radio on-device with a bundled DSP core, with the same waterfall, drum, audio and decoders as any remote server, plus a hardware-control submenu (gain, PPM, bias-T, AGC, sample rate, direct sampling)',
  'RTL-TCP (iOS + Android): connect to a networked rtl_tcp server by host:port, with saveable named favourites — the same on-device demodulation, so it works on iPhone too',
  'On-device demodulation of AM / SSB / CW / NFM / WFM with offset tuning (no zero-IF DC-spike break-up on AM)',
  'MMSE noise reduction for local sources: a much stronger spectral denoiser than before (strength 0–20)',
  'Auto Notch on every backend: an adaptive filter that removes steady carriers / heterodynes while leaving voice intact',
  'KiwiSDR: a client-side dBFS squelch, the SNR meter retired (no Kiwi feed), and clearer messaging when an owner restricts access',
];

const V3_CHANGES: string[] = [
  'OpenWebRX / OpenWebRX+ support: waterfall, audio (incl. WFM HD), profile auto-switching, server-gated modes (WFM/DMR/DStar/DAB…), squelch + noise-reduction sliders, basic text chat, and server-side decoders (RTTY, SSTV, FAX, Packet, POCSAG/FLEX, ADS-B, ISM, Meshtastic/Meshcore/LoRa…)',
  'KiwiSDR support: waterfall, audio, tuning, real dBm S-meter, server-side zoom and a live connection meter — same custom interface as every other backend. (Owners choose who connects: some refuse apps or block broadcast bands, so a server may refuse or drop you — that\'s the owner\'s restriction, not VibeSDR.)',
  'New directory chooser in the server picker: Favourites/Default pinned on top, then UberSDR, Receiverbook (OpenWebRX + KiwiSDR) and the KiwiSDR network — with type logos, user counts and full receivers greyed out',
  'Custom server URLs auto-detect the backend (UberSDR / OpenWebRX / KiwiSDR)',
  'Crash hardening: a flaky server can no longer take the app down — you are returned to the server list with a clear, server-attributed message',
  'Swipe-up-to-minimise no longer knocks the tuning off',
  'Menu footer shows the connected backend’s logo and software version',
];

const V2_CHANGES: string[] = [
  'GPU waterfall — Skia runtime shader with in-shader temporal line synthesis',
  'Full UberSDR skin parity: server maps (HFDL/digi/CW), spots tables, stats and legends',
  'Native audio engines on iOS and Android (Opus over WebSocket, background playback)',
  'Client noise reduction: NR, NR2 and noise blanker, processed on-device',
  'Server-side NR with dynamic filter list and live parameter control',
  'AAC audio recorder with share sheet',
  'Voice Tuning System (VTS): station/band announcements, bookmark skipping and search',
  'User bookmarks — per-instance or global, UberSDR-compatible import/export',
  'Live chat with user list, mute, zoom-sync and tune-sync',
  'Media session: lock-screen/watch/car controls with tune-step or bookmark skip',
  'Admin pages in-app (Admin / Noise Floor / Band Conditions / Listeners)',
  'Spectrum backdrop image with opacity control and station ID overlay',
  'VFO glow and frost controls, signal-meter modes, S-meter calibration',
  'Instance picker: location-aware sorting, country flags, favourites, defaults',
  'Share tuned station as a tappable deep link',
  'Haptic tuning feedback and ProMotion 120 Hz rendering',
];

const CREDITS: { name: string; detail: string }[] = [
  { name: 'M9PSY (madpsy) — UberSDR',
    detail: 'The biggest thank-you of all. M9PSY got me into AI-assisted coding and encouraged this whole project into existence — without him there is no VibeSDR. UberSDR is the server this client is built for, and the on-device decoders (RTTY / NAVTEX, WEFAX, SSTV and more) for Local Hardware and KiwiSDR are based on his UberSDR decoders. Also: the protocol, web-UI design reference, NR2 / noise-blanker / WebSDR-NR DSP algorithms, colour palettes, band plans, bookmark format and the waterfall smoothing pipeline. Cheers, mate.' },
  { name: 'ka9q-radio — Phil Karn, KA9Q',
    detail: 'The SDR engine (radiod) underneath UberSDR.' },
  { name: 'SDR++ & SDR++ Brown — Alexandre Rouma & contributors',
    detail: 'VibeSDR’s original on-device radio (V4) was built on the SDR++ Brown DSP core to get Local Hardware and RTL-TCP up and running quickly. In V5 that was replaced with VibeDSP — VibeSDR’s own clean-room, GPL-free engine — and all SDR++ Brown code was removed; none is bundled now. Some waterfall colour palettes also originate here. Thank you for making on-device SDR possible. Licensed under the GNU GPL v3.' },
  { name: 'librtlsdr & rtl_tcp — Osmocom / Steve Markgraf, and the RTL-SDR Blog fork',
    detail: 'The RTL-SDR USB driver and the rtl_tcp protocol behind the Local Hardware and RTL-TCP backends.' },
  { name: 'KissFFT — Mark Borgerding (BSD-3)',
    detail: 'The small, permissively-licensed FFT kernel inside VibeDSP, used for the on-device waterfall/spectrum.' },
  { name: 'Zstandard — Yann Collet / Meta',
    detail: 'Compression used inside the bundled DSP core.' },
  { name: 'KISS FFT — Mark Borgerding',
    detail: 'The compact FFT behind the MMSE noise reduction, Auto Notch and on-device decoders.' },
  { name: 'ft8_lib — Karlis Goba, YL3JG',
    detail: 'FT8 / FT4 decoding for the on-device digital-mode decoders.' },
  { name: 'OpenWebRX — Jakob Ketterl (DD5JFK) & OpenWebRX+ (Marat Fayzullin)',
    detail: 'The OpenWebRX server and its OpenWebRX+ fork — protocol reference for the OpenWebRX backend (waterfall, audio, modes, decoders and chat).' },
  { name: 'KiwiSDR — John Seamons (ZL/KF6VO)',
    detail: 'The KiwiSDR receiver and its open web client — protocol reference for the KiwiSDR backend.' },
  { name: 'FM-DX Webserver — NoobishSVK & contributors',
    detail: 'The FM-DX Webserver project and the servers.fmdx.org receiver map — protocol reference for VibeSDR’s FM-DX backend (tuning, RDS, signal, transmitter data, chat) and its 3LAS MP3 audio stream. Licensed under the GNU GPL v3.' },
  { name: 'radio-browser.info',
    detail: 'The community radio-station directory used to look up and match station logos, by name and country — on every backend, and on AM and shortwave stations as well as FM. Community data, freely licensed.' },
  { name: 'Nominatim & OpenStreetMap',
    detail: 'Geocoding for the receiver’s location: turning a town name into a position, and a position back into a town and country, so a VibeServer can tell its clients where it actually is. Data © OpenStreetMap contributors, ODbL. Used sparingly — once per receiver, then cached — in line with the Nominatim usage policy.' },
  { name: 'librdsparser — Konrad Kosmatka',
    detail: 'Reference for the RDS PI-code + ECC → country mapping (IEC 62106) that shows country flags from live RDS. MIT-licensed.' },
  { name: 'Opus — Xiph.Org Foundation',
    detail: 'Audio codec used for all streaming and decoding.' },
  { name: 'EiBi',
    detail: 'Shortwave broadcast schedules used for live station bookmarks.' },
  { name: 'GQRX, KiwiSDR, CuteSDR, SdrDx, OpenWebRX, matplotlib',
    detail: 'Origins of the waterfall colour palettes.' },
  { name: 'Leaflet, OpenStreetMap & CARTO',
    detail: 'Map rendering and tiles for the HFDL / digital / CW maps.' },
  { name: 'Atkinson Hyperlegible — Braille Institute',
    detail: 'Primary UI typeface. Nixie One and VT323 are used for the frequency displays.' },
  { name: 'React Native, Expo, Hermes, Skia, Reanimated, Gesture Handler, OkHttp',
    detail: 'The frameworks and libraries that make the app run.' },
];

export default function AboutOverlay({ visible, onClose }: AboutOverlayProps) {
  if (!visible) return null;
  return (
    <Modal
      visible
      animationType="slide"
      supportedOrientations={['portrait', 'landscape']}
      onRequestClose={onClose}
    >
      <SafeAreaView style={styles.root} edges={['top']}>
        <View style={styles.bar}>
          <TouchableOpacity onPress={onClose} hitSlop={12} activeOpacity={0.7}>
            <Text style={styles.back}>← SDR</Text>
          </TouchableOpacity>
          <Text style={styles.title}>About VibeSDR</Text>
          <View style={{ width: 50 }} />
        </View>

        <ScrollView style={styles.scroll} contentContainerStyle={styles.content}>
          <View style={styles.heroRow}>
            <Image source={require('../../assets/icon.png')} style={styles.icon} />
            <View style={{ flex: 1 }}>
              <Text style={styles.appName}>VibeSDR</Text>
              <Text style={styles.appVer}>Version {APP_VERSION} · build {Constants.nativeBuildVersion ?? '?'}</Text>
              <Text style={styles.appSub}>A native mobile client for UberSDR, OpenWebRX & KiwiSDR receivers — and your own RTL-SDR hardware</Text>
            </View>
          </View>

          {/* ★ DIAGNOSTICS. Local only: assembled on demand, SHOWN to the user, then handed
              to the system share sheet so they choose where it goes. The app never sends
              anything itself — which is what keeps this out of the App Privacy declaration
              and keeps "everything stays on your device" true. See services/diagnostics.ts. */}
          <Text style={styles.section}>DIAGNOSTICS</Text>
          <Text style={[styles.body, { marginBottom: 10 }]}>
            If VibeSDR has crashed or misbehaved, this gathers what it recorded — the app
            version, your device and iOS version, and the last error with its stack. It
            contains no PINs, no passwords and no location, you can read it before it goes
            anywhere, and nothing is sent unless you send it.
          </Text>
          <TouchableOpacity
            onPress={async () => {
              const report = await buildDiagnostics();
              Alert.alert(
                'Send diagnostics?',
                report.length > 900 ? report.slice(0, 900) + '\n…' : report,
                [
                  { text: 'Cancel', style: 'cancel' },
                  {
                    text: 'Share',
                    onPress: () => {
                      Share.share({
                        message: report,
                        title: 'VibeSDR diagnostics',
                      }).catch(() => {});
                    },
                  },
                ],
              );
            }}>
            <Text style={styles.link}>Send diagnostics…</Text>
          </TouchableOpacity>
          <Text style={[styles.body, { marginTop: 6, opacity: 0.75 }]}>
            Send it to {SUPPORT_EMAIL} — or anywhere you like; it is your file.
          </Text>

          <Text style={styles.section}>A MESSAGE FROM STUART</Text>
          {STUART_MESSAGE.map((p, i) => (
            <Text key={i} style={[styles.body, { marginBottom: 10 }]}>{p}</Text>
          ))}
          <TouchableOpacity onPress={() => Linking.openURL(STUEY_URL)}>
            <Text style={styles.link}>Visit my UberSDR instance: stuey3d.tunnel.ubersdr.org</Text>
          </TouchableOpacity>

          <Text style={styles.section}>WHAT'S NEW IN V10 BETA 1</Text>
          {V10_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          <Text style={styles.section}>PREVIOUSLY, IN V9.0.0</Text>
          {V9_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          <Text style={styles.section}>V8.0.1 CHANGES</Text>
          {V8_0_1_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          <Text style={styles.section}>V8.0.0 CHANGES</Text>
          {V8_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          <Text style={styles.section}>V7.1.0 CHANGES</Text>
          {V7_1_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          <Text style={styles.section}>V7.0.0 CHANGES</Text>
          {V7_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          <Text style={styles.section}>V6.1.0 CHANGES</Text>
          {V6_1_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          <Text style={styles.section}>V6.0.0 CHANGES</Text>
          {V6_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          <Text style={styles.section}>V5.1 – 5.2 CHANGES</Text>
          {V5_1_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          <Text style={styles.section}>V5 HIGHLIGHTS</Text>
          {V5_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          <Text style={styles.section}>V4 HIGHLIGHTS</Text>
          {V4_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          <Text style={styles.section}>V3 HIGHLIGHTS</Text>
          {V3_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          <Text style={styles.section}>V2 HIGHLIGHTS</Text>
          {V2_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          <Text style={styles.section}>VERSION HISTORY</Text>
          {VERSION_HISTORY.map((v) => (
            <View key={v.v} style={styles.creditBlock}>
              <Text style={styles.creditName}>{v.v}</Text>
              <Text style={styles.creditDetail}>{v.detail}</Text>
            </View>
          ))}

          <Text style={styles.section}>FUTURE PLANS</Text>
          {FUTURE_PLANS.map((p, i) => (
            <Text key={i} style={styles.body}>{p}</Text>
          ))}

          {/* Deliberately placed immediately BEFORE the credits: the WebSDR answer says
              every backend we do speak is either open source or has its author's
              blessing, and the list that follows is the evidence for that claim. */}
          <Text style={styles.section}>LIMITATIONS — AND WHY THEY&rsquo;RE DELIBERATE</Text>
          {LIMITATIONS.map((l) => (
            <View key={l.q}>
              <Text style={styles.limQ}>{l.q}</Text>
              {l.a.map((p, i) => (
                <Text key={i} style={styles.body}>{p}</Text>
              ))}
            </View>
          ))}

          <Text style={styles.section}>CREDITS</Text>
          <Text style={styles.body}>
            VibeSDR stands on the work of other open projects. Thank you to all of them.
          </Text>
          {CREDITS.map((c) => (
            <View key={c.name} style={styles.creditBlock}>
              <Text style={styles.creditName}>{c.name}</Text>
              <Text style={styles.creditDetail}>{c.detail}</Text>
            </View>
          ))}

          <Text style={styles.section}>LICENCE</Text>
          <Text style={styles.body}>
            VibeSDR is free software, released under the GNU General Public License
            version 3 (GPL-3.0). Here “free” means freedom, not price: you’re free to
            use, study, share and modify it, and the complete source code is public.
          </Text>
          <Text style={styles.body}>
            If you bought VibeSDR from an app store, that nominal price simply covers the
            store’s distribution and developer-account fees — the GPL expressly allows
            charging for distribution, and paying changes none of your freedoms. You can
            always get the source and build it yourself for nothing. Distributed in the
            hope that it’s useful, but WITHOUT ANY WARRANTY — without even the implied
            warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
          </Text>
          <TouchableOpacity onPress={() => Linking.openURL('https://github.com/Stuey3D/VibeSDR')}>
            <Text style={styles.link}>Source code: github.com/Stuey3D/VibeSDR</Text>
          </TouchableOpacity>
          <TouchableOpacity onPress={() => Linking.openURL('https://www.gnu.org/licenses/gpl-3.0.html')}>
            <Text style={styles.link}>www.gnu.org/licenses/gpl-3.0</Text>
          </TouchableOpacity>

          <Text style={styles.section}>PRIVACY</Text>
          <Text style={styles.body}>
            VibeSDR collects no personal data — no analytics, ads, or tracking.
            Location is optional and used only to sort instances by distance; deny it
            and everything still works. Your bookmarks and settings stay on your device.
          </Text>
          <TouchableOpacity onPress={() => Linking.openURL('https://github.com/Stuey3D/VibeSDR/blob/main/PRIVACY.md')}>
            <Text style={styles.link}>Full privacy policy</Text>
          </TouchableOpacity>

          <View style={{ height: 40 }} />
        </ScrollView>
      </SafeAreaView>
    </Modal>
  );
}

const F = 'Atkinson Hyperlegible';

const styles = StyleSheet.create({
  root:  { flex: 1, backgroundColor: '#000' },
  bar:   {
    flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
    paddingHorizontal: 14, paddingTop: 6, paddingBottom: 8, backgroundColor: '#0a0a0a',
    borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: 'rgba(255,255,255,0.18)',
  },
  back:  { color: '#ffe566', fontFamily: F, fontSize: 16 },
  title: { color: 'rgba(255,255,255,0.85)', fontFamily: F, fontSize: 15 },

  scroll:  { flex: 1 },
  content: { paddingHorizontal: 18, paddingTop: 16 },

  heroRow: { flexDirection: 'row', alignItems: 'center', gap: 14, marginBottom: 6 },
  icon:    { width: 64, height: 64, borderRadius: 14 },
  appName: { color: '#fff', fontFamily: F, fontSize: 22, fontWeight: 'bold', letterSpacing: 1 },
  appVer:  { color: '#ffe566', fontFamily: F, fontSize: 13, marginTop: 2 },
  appSub:  { color: 'rgba(255,255,255,0.70)', fontFamily: F, fontSize: 12, marginTop: 2 },

  section: {
    color: 'rgba(180,190,210,0.80)', fontFamily: F, fontSize: 12, fontWeight: 'bold',
    letterSpacing: 2, marginTop: 22, marginBottom: 8,
    borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: 'rgba(255,255,255,0.12)',
    paddingTop: 14,
  },
  body: { color: 'rgba(255,255,255,0.85)', fontFamily: F, fontSize: 13, lineHeight: 19, marginBottom: 8 },
  /** The question, in the Limitations section. Brighter than the answer, so the section
   *  can be SKIMMED — people arrive here with one specific question, not to read an essay. */
  limQ: {
    color: '#ffe566', fontFamily: F, fontSize: 13, lineHeight: 19,
    marginTop: 4, marginBottom: 4,
  },

  bulletRow:  { flexDirection: 'row', gap: 8, marginBottom: 5, paddingRight: 4 },
  bulletDot:  { color: '#ffe566', fontFamily: F, fontSize: 13, lineHeight: 19 },
  bulletText: { flex: 1, color: 'rgba(255,255,255,0.85)', fontFamily: F, fontSize: 13, lineHeight: 19 },

  creditBlock:  { marginBottom: 12 },
  creditName:   { color: '#fff', fontFamily: F, fontSize: 13, fontWeight: 'bold' },
  creditDetail: { color: 'rgba(255,255,255,0.70)', fontFamily: F, fontSize: 12, lineHeight: 17, marginTop: 2 },

  link: { color: '#6ec8ff', fontFamily: F, fontSize: 13, marginTop: 2 },
});
