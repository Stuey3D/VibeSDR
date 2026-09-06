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
  // ★★★ NO V4L NOTE HERE, DELIBERATELY. A release note is a promise about the app the reader is
  //     holding, and this app cannot drive a dongle: iOS has no RTL-SDR driver (declined, on
  //     purpose) and Jr is a remote. V4L support is real, but it lives in VibeServer and in
  //     Android's local-hardware mode, so it belongs in THEIR notes. Stuart, 2026-08-15: "for
  //     VibeSDR and VibeSDR Jr the V4L stuff is not for them as they cannot interact directly with
  //     the hardware, that is for VibeServer and for Android (when it is ready)."
  // ★★ Announcing hardware support to someone who cannot use it is the same fault as leaving a
  //    control on screen that does nothing on their radio: they conclude the FEATURE is broken,
  //    not that it was never theirs. Same rule as AGENTS.md's "a control that only works on one
  //    radio should not be there".
  { v: 'V10.5', detail: 'The automatic gain control now knows what band it is on \u2014 and it stops the receiver inventing stations. \u2014 A VERY STRONG LOCAL TRANSMITTER MAKES A RECEIVER LIE. Two miles from a 104.2 MHz transmitter, an RTL\u2011SDR does not simply hear it loudly: it starts MANUFACTURING copies of it on empty frequencies, complete with that station\u2019s own RDS name and a stereo lock. They look exactly like real stations \u2014 they stand well clear of the noise, they decode \u2014 and the gain control used to settle happily into one and stay there, while the real, weaker stations either side were buried. \u2014 THE GIVEAWAY IS THAT THEY GROW FASTER THAN THE GAIN THAT MAKES THEM. A real signal gets one decibel louder for each decibel of gain you add; a manufactured one grows about three times as fast. VibeAGC now watches that ratio on every adjustment it makes, recognises what it is looking at, and comes down out of it. Measured on air, a frequency carrying nothing but a copy of that transmitter went from being received at full strength with the wrong station\u2019s name attached, to being correctly reported as empty \u2014 and two stations that were previously destroyed by it became listenable. \u2014 ON FM THE DAMAGE IS NOT WHERE YOU ARE LISTENING, IT IS ALL AROUND IT. Tuned to the strong station itself, everything looks healthy \u2014 it is a real signal and it behaves like one \u2014 while the receiver quietly sprays over the rest of the band. The gain control now watches the neighbourhood either side of whatever you are listening to, not just the station itself. \u2014 AND IT IS TUNED PER BAND, INVISIBLY. FM broadcast, wide digital signals and the narrow modes (AM, medium wave, shortwave, single sideband) want genuinely different behaviour: on medium wave and shortwave a station fading away for a few seconds is ordinary weather, not a fault, so reacting quickly there would mean fidgeting with the gain every time the band breathed. Each gets its own settings and its own reaction speed, chosen automatically. There is still one switch. \u2014 THE GAIN NOW COMES DOWN AS FAST AS IT GOES UP. It could always jump upwards, because it can work out in advance how far it can safely go; coming down it could only creep one step at a time, which is what made it feel slow. It now sizes a downward jump from the evidence in the same way. \u2014 The overload readout tells you what is actually happening: it now reports how much of the signal is being clipped rather than how close the peak is, which are very different things \u2014 a receiver can sit a whisker below its limit and be perfectly clean. \u2014 Tuning could stop working on a SHARED receiver \u2014 fixed \u2014 and the automatic bandwidth has stopped fidgeting. \u2014 ON A SHARED RECEIVER THE DIAL COULD GO DEAD. A receiver where everyone shares one tuner deliberately stays quiet until you actually ask for something, so that joining a room does not yank the dial to wherever your app happened to be pointing. That silence could fail to lift: the frequency moved on screen, the audio kept playing, and the radio never moved \u2014 and reconnecting did not clear it, because the silence was re\u2011armed every time the connection reopened. Changing mode and back was the accidental cure. It now lifts the moment anything you asked for differs from what the connection opened with, whichever way you tuned. Receivers with a single listener were never affected. \u2014 AUTOMATIC BANDWIDTH NO LONGER HUNTS. On a healthy station it could sit and flip between two widths every couple of seconds, because the signal\u2011quality reading it steers by wobbles a few decibels and the control magnified that into fifteen kilohertz of filter. The reading is smoothed now, so it settles \u2014 and the narrowing that makes a WEAK station listenable is unchanged, which was the point of leaving the measurement alone and filtering only the noise on it. \u2014 THE RECEIVER NARROWS AGAIN WHEN A NEIGHBOUR IS THE PROBLEM. Narrowing hard is worth about ten decibels against a strong station on the next channel, and measurably COSTS you against plain noise \u2014 so it is steered by a measurement of whether narrowing is actually helping, taken continuously, rather than by how strong the station is. It engages only when it is winning, and it is honest about the price: at that width RDS and full stereo cannot fit. \u2014 IMS now suppresses MULTIPATH by blending the stereo difference signal, which is what the name means on the tuners it is borrowed from; it no longer narrows the receiver, and automatic bandwidth owns that filter on its own. — A SHARED SERVER NOW COMES BACK ON ITS OWN AFTER THE INTERNET DROPS. If you publish your receiver to the public directory, a broken connection used to take it off the list permanently: the tunnel that carries it never restarted, and the server went on announcing an address with nothing behind it — so it looked available and answered nobody. It now notices within a second or so, rebuilds the tunnel and tells the directory where it has moved to, retrying patiently for as long as the connection is out. Your receiver keeps the same friendly address throughout: only the anonymous tunnel name underneath it changes, and nobody sees that. This matters most on the connections most likely to blink — mobile broadband and satellite — where a receiver on a hilltop should not need somebody to go and restart it. — A brief outage could also cost a server its public name outright, because a connection that could not be reached at all was mistaken for the directory disowning it; those are now told apart, and only a genuine refusal releases the name. — REPORTED FROM A REAL RECEIVER, AND FIXED. Michael, DL8LDN, tested the app against OpenWebRX and wrote up four faults on GitHub; all four are answered here. — THERE WAS NO WAY BACK FROM THE OWRX MAP on a notched iPhone: the back arrows sat underneath the status bar, behind the clock and the battery, so the map became a room with no door. The inset that keeps controls clear of the notch was being passed at one call site and not the other. — AN OWRX BOOKMARK FOR A DIGITAL MODE was silently ignored — tapping it moved the frequency and left the demodulator where it was, so you arrived at a DMR or DRM channel still listening in FM. Analogue bookmarks always worked, which is what made it look like the bookmark rather than the mode. — THE DECODER PANEL WAS THROWING AWAY ITS OWN HISTORY about a hundred lines in, so a busy band appeared to decode far less in the app than the same receiver showed in a browser. Nothing was ever missed on the way in — the app is only a window on the server’s decoder and receives exactly what the browser does — it was discarded immediately afterwards, and the trim cut through the middle of a line, so the oldest entry on screen was usually half a spot and read as a decode that had gone wrong. The panel now keeps around ten times as much and never shows you half a line. — AND DIGITAL MODES THAT DECODE IN BLOCKS NOW GET A DEEPER CUSHION. DRM, DAB, HD Radio and the digital voice modes do not deliver audio in a steady trickle the way AM and FM do: the decoder thinks for a few hundred milliseconds and then hands over a whole chunk at once. Both players were built for the trickle — one discarded the tail of every chunk as if it were a backlog, the other ran dry waiting between them — and the result was audio that broke up in the app while a browser played the very same stream cleanly. Each of those modes now tells the player how much to hold. Ordinary AM, FM and single sideband are untouched and keep their existing responsiveness, because a fix for the modes fewest people can test must not cost the ones everybody uses.' },
  { v: 'V10.4', detail: 'Shared receivers \u2014 one dial, several listeners, like an FM\u2011DX tuner. A VibeServer can now be set to let everyone who connects share ONE tuner rather than each getting their own, and this release is the app half of that. \u2014 WHEN SOMEBODY ELSE TUNES, THE APP FOLLOWS: the frequency, the demodulator and the waterfall all move with the dial, and a line tells you who moved it and where to. Before this the audio followed and nothing else did, so the app sat showing one station while playing another \u2014 which also stopped station logos appearing, because they are looked up by the frequency the app thought it was on. \u2014 A CANNED CHAT comes with it, because a dial anyone can turn needs a way to ask. There is no typing and there are no names: twelve fixed phrases (\u201cCan I tune?\u201d, \u201cPlease hold \u2014 chasing DX\u201d, \u201cDecoding \u2014 about 10 minutes\u201d) and everyone is User 1, User 2. That is deliberate \u2014 it means there is nothing to moderate, nothing to translate and nothing anyone can be abused with, on a receiver whose owner is asleep. It works the same in the app, in a browser and on a watch. \u2014 ON APPLE WATCH THE DIAL IS LOCKED until you deliberately arm it, and it locks itself again after a couple of minutes. A crown turns in a coat sleeve, and on a shared receiver that would change the station under everyone else listening. \u2014 RECEIVERS NOW INTRODUCE THEMSELVES. A server can describe its AERIAL and leave a standing message, and the app shows both before you connect \u2014 an unexplained limit reads as a fault, and \u201cwideband dongle, wire aerial\u201d sets expectations that no amount of tuning will. A receiver restricted to particular bands now names them (\u201cMW Broadcast, FM Broadcast\u201d) instead of reciting frequency ranges, and says whether its listeners share one dial or each get their own. \u2014 The maintenance notice can be dismissed, and a multi\u2011radio server has a way back to the server list. \u2014 APPLE WATCH: a saved RTL\u2011TCP or SpyServer receiver could not be started from the watch \u2014 tapping it did nothing at all, with no error. The watch was refusing protocols the PHONE has always handled, since the phone is what actually connects. Reported on GitHub, and fixed. \u2014 VibeServer itself now installs on ORDINARY INTEL AND AMD LINUX MACHINES as well as the Raspberry Pi, with the same one\u2011line install.' },
  { v: 'V10.3.1', detail: 'Audio could go silent \u2014 fixed, in three places. A change in 10.3 made the start of playback a race with itself, so on some connections the app set the audio path up and then immediately tore it down again: a perfect signal, a moving data meter and no sound, on any server, and often fine on the next attempt. \u2014 Connecting to an UberSDR receiver by its address on your OWN NETWORK had no sound at all. The connection the app uses for audio cannot reach a plain (unencrypted) address on a local network on this version of iOS, while the very same address answers everything else normally \u2014 which is why a receiver reached through a tunnel or a public address was always fine. The app now notices and switches to a connection that works. \u2014 Audio could also start before the server had finished registering the session, which some servers refuse silently; the app now waits, and if audio stalls it re-introduces itself instead of knocking on a door that will not open \u2014 and puts you back on the frequency you were listening to. \u2014 The app also keeps its own record of what the audio path did, so a report from Diagnostics says what actually happened rather than what was assumed.' },
  { v: 'V10.3', detail: 'The weak\u2011signal release for broadcast FM, and the controls to go with it. \u2014 A VibeServer running 3.1 now treats FOUR DIFFERENT FAULTS separately, because they want opposite cures: NR takes the hiss off a weak station by rolling the top off the stereo difference signal (FM noise is worst exactly where stereo lives) while keeping the bass and mid image, and it treats mono hiss too; IMS narrowed the receiver only when a strong NEIGHBOUR is interfering, and deliberately NOT for noise, where narrowing measurably costs you (what IMS does changed in 10.5 \u2014 see above); CEQ undoes a REFLECTION \u2014 multipath is distortion, not weakness, and a strong station can suffer it; NB removes impulse noise from ignition, thermostats and switch\u2011mode supplies. All four are on by default and each declines to act unless its own measurements say it will help, so a strong station is left exactly as it was. Each has its own switch in the audio menu for A/B\u2011ing what it is doing. \u2014 ADVANCED RDS gained the readings behind them: MPX signal\u2011to\u2011noise with the filter corners it produced, multipath depth (and \u201ctoo noisy to judge\u201d when it honestly cannot tell), what the equaliser achieved before and after, how much the blanker is removing, and whether narrowing the receiver would help or cost. Alternative frequencies are now listed with a TICK against the ones confirmed \u2014 on a noisy station the unconfirmed ones are usually phantoms invented by block errors. \u2014 FM stereo holds on far better: stations that under\u2011inject their pilot (below the 6\u20137.5 kHz specification) now lock and stay locked, and two long\u2011standing faults that dropped stereo for a second at a time \u2014 one on every RDS refresh, one every time the Advanced RDS panel was opened \u2014 are fixed. Measured from on\u2011air recordings: seventeen dropouts became none.' },
  { v: 'V10.1', detail: 'A VibeServer can now run SEVERAL RADIOS BEHIND ONE ADDRESS, and the app understands one. \u2014 Connect to a multi\u2011radio server and it asks which receiver you want, listing every one with what it is, where it is pointed and whether arriving means sharing it \u2014 so you can choose without taking a seat on a radio to find out what it is. A server with a single radio behaves exactly as before: a list of one is not a choice. \u2014 Before this, connecting to such a server failed in the least helpful way possible: the app asked for a radio the address does not have and got a bare disconnection back, which looks identical to a server being down. \u2014 STATION LOGOS NOW COME FROM THE BROADCASTER. They are looked up by the station\u2019s PI code \u2014 the identity a transmitter repeats and error\u2011protects \u2014 through RadioDNS, so you get the broadcaster\u2019s own artwork rather than a guess made from an eight\u2011character name. One mis\u2011read name used to stick for good: a weak BBC Radio 3 could wear Radio 1\u2019s logo and never correct itself. Stations that publish no artwork fall back to the name search as before.' },
  { v: 'V10.0.2', detail: 'Fixes, all of them things that made a working radio look broken. \u2014 SSTV pictures came out SLANTED, which is the one fault that makes a decoded picture worthless. A transmission that stopped early was left uncorrected, and the correction that should have straightened it was being refused by its own safety check. Pictures are straight. \u2014 RadioText could be WIPED on exactly the stations where you most want it: a weak signal, a repaired block, and the buffer cleared itself. \u2014 The app could abort outright while a connection was being torn down. \u2014 Audio could die on a noisy or half\u2011received station and stay dead until you retuned, while the spectrum, RDS and the signal meter carried on perfectly. One bad packet was stopping the audio decoder and nothing ever restarted it. It now picks itself back up. \u2014 A radio that stopped delivering \u2014 an Airspy nudged on its USB plug, an SDRplay that stalled \u2014 was reported as dead until the server was restarted, even though it was still plugged in and lit. The server now keeps trying to recover it, and keeps telling you the truth while it does. \u2014 Changing the Airspy HF+ sample rate left the radio and the rest of the app disagreeing about what rate it was running at: audio came out too fast or too slow, some rates landed off frequency, and once it wedged the radio hard enough to need the host rebooting. THE PICKER IS GONE. That radio is not built to be re\u2011rated while it is streaming \u2014 no other SDR client allows it either \u2014 so the rate is now chosen once, in the server\u2019s config, and applied when the radio opens. \u2014 The Airspy HF+ no longer shows you the dead edges of its own capture. The outer stretch of an HF+\u2019s span is the skirt of its anti\u2011alias filter: nothing can be received there, and it was dragging the auto\u2011range down and inviting you to tune into it. The display now stops where the radio does, with the roll\u2011off still visible so it reads as a receiver rather than a cliff. \u2014 FM could go completely silent and STAY silent \u2014 off\u2011tune, or on a weak signal \u2014 while the spectrum and the meter carried on as if nothing was wrong. Two separate faults with the same symptom: nothing was removing the DC the FM detector produces off\u2011tune, and a single bad sample could latch the audio to silence for good. Both fixed and measured: sustained silence became one 0.3% blip in 25 minutes of listening. \u2014 The spectrum asked for 20 frames a second and delivered 14. It delivers 20. \u2014 On a Mac, and on an iPad pushed onto a big external display, the controls stretched the full width of the screen \u2014 a layout meant for iPad thumbs, spread across a metre of glass. They now stop at a sensible width and sit centred. A hand\u2011held iPad is unchanged. \u2014Plugging a radio into an Android phone announced \u201cRTL\u2011SDR\u201d whatever you had plugged in. It names the radio you actually connected. \u2014 A VibeServer on a Raspberry Pi now announces itself on the network, so it appears in the server list instead of having to be typed in by IP address.' },
  { v: 'V10.0.1', detail: 'Fixes. \u2014 Noise reduction, squelch and the auto\u2011notch did nothing from the app when connected to a VibeServer: the DSP runs on the server there, and the app was still driving its own idle copy. They work now, as de\u2011emphasis and stereo were fixed last release. \u2014 THE DECODER BOX CAN BE MADE BIG. Every decoder gets a BIG button: a whole SSTV or WEFAX picture at once instead of a third of it, more rows of RTTY, NAVTEX and Morse, more FT8 spots, more aircraft. The picture now shrinks to fit the box rather than being cropped by it, which mattered most on an iPad \u2014 the bigger the screen, the less of the image you used to see. \u2014 Saving a decoded picture now shares a real image file, so Save to Photos and Save to Files appear on iPhone and the share sheet is no longer blank on a Mac. \u2014 The Airspy HF+ sample\u2011rate picker did nothing on Android: every option was being forced to at least 1 MHz, an RTL\u2011SDR rule, while the HF+ tops out at 912 kHz. It now offers the rates the radio itself reports. \u2014 A receiver is no longer handed back while a decoder is still producing: half an hour of SSTV with nobody touching the screen used to look exactly like a phone left in a pocket. \u2014 Server directories now time out instead of loading for ever, and the Apple Watch shows its own \u201ccouldn\u2019t load \u2014 tap to retry\u201d instead of spinning indefinitely when the phone cannot reach them. \u2014 The built\u2011in web client falls back to uncompressed audio by itself if the browser accepts Opus and then fails to play it, instead of going silent. \u2014 Under the bonnet: messages a server sends that we do not understand are now recorded rather than ignored, which is how this class of problem gets found at all.' },
  { v: 'V10.0.0', detail: 'VibeSDR Jr \u2014 a whole SDR on your wrist with no phone at all \u2014 plus VibeServer on the Mac, Airspy HF+ support, Advanced RDS calibrated against a professional analyser, a GPU waterfall with unsharp sharpening, automatic link management, iCloud sync, full keyboard control, and the Apple Watch companion renamed VibeSDR Buddy. See the What\u2019s New list above for the full account. \u2014 FIXED in this release: FM de\u2011emphasis and FM stereo never reached a networked VibeServer; tuning steps stopped at 1 kHz above 30 MHz, putting some airband channels out of reach entirely; the demodulator popup\u2019s RF gain slider only spoke the RTL\u2011SDR\u2019s gain model and did nothing on an Airspy or SDRplay, so it has been removed; and the guided tour pointed at the wrong menus for noise reduction and bookmarks.' },
  { v: 'V9.0.0', detail: 'The Apple Watch companion \u2014 the waterfall itself, live on your wrist, drawn from the same data and the same palette as the phone. Turn the Digital Crown to tune, tap the frequency to type one, press and hold for the menu (demodulator, tuning step, zoom, servers). It works with the iPhone LOCKED IN YOUR POCKET, which is the whole point \u2014 and it will start the phone for you: open the watch app with VibeSDR closed and the phone wakes in the background, connects to your default receiver, and the waterfall arrives on your wrist without the phone screen ever coming on. Four screens, chosen by what the receiver actually is: spectrum waterfall, FM-DX tuner (station, distance, RDS), DAB service list, and ADS-B aircraft. Switch receivers from your favourites without touching the phone. Control the iPhone\u2019s system volume and mute from the wrist \u2014 it reads the phone\u2019s REAL volume, including changes you make on the phone, so the two can never disagree. It shows the band you\u2019re in, in words, from the ITU plan for wherever the RECEIVER is, with marks on the ticker showing where that band ends \u2014 and your watch\u2019s own battery, because this is an app you might leave running on a hilltop. When the link is rough it tells you WHICH link: there are two radio hops in the chain (phone-to-server, watch-to-phone) and they fail independently, so a small diagram shows which one is struggling, over a waterfall that keeps drawing. \u2014 FIXED: the waterfall could freeze for good on a locked phone on mobile data and never come back, while audio and tuning carried on working perfectly. A mobile network can silently invalidate a connection without ever closing it, and nothing was watching for that: the spectrum socket sat there, open and dead, forever. VibeSDR now actively probes the link, rebuilds it the moment it stops answering, and reacts instantly when the phone changes network. \u2014 VibeServer: the web client now shows the server\u2019s NAME as well as its address, so there is no IP to remember. FIXED: on first use its sample-rate box read 3.2 MS/s while the receiver was actually running at 2.4 \u2014 it now defaults to 2.4 (the fastest an RTL-SDR can reliably sustain), tells the receiver so the two agree, and marks the higher rates as liable to drop samples. FIXED: on a 1366\u00d7768 laptop the control bar\u2019s buttons overlapped and the signal readout ran off the edge of the window.' },
  { v: 'V8.0.1', detail: 'Fix: saved favourites could start connecting as a VibeServer instead of the UberSDR receiver they actually are \u2014 and the wrong answer was then saved back onto the favourite. VibeSDR now identifies a VibeServer by a marker only a real one carries, and repairs any favourites that were mislabelled.' },
  { v: 'V8.0.0', detail: 'VibeServer \u2014 turn an Android phone with an RTL-SDR into a receiver anyone on your network can use, from a browser or from VibeSDR itself. The serving phone does all the DSP and sends compressed audio and a ready-made waterfall, so it is roughly 25\u00d7 lighter on the network than raw RTL-TCP and works comfortably over Wi-Fi or a hotspot. Point any browser at the phone\u2019s address and you get the full VibeSDR client \u2014 waterfall and spectrum with the same palettes, click-to-tune, panning and cursor zoom, audio with recording, the decoders (RTTY, NAVTEX, WEFAX, SSTV, FT8 with its map), station search, bookmarks you can export, and OS media controls with artwork. Access is protected by a PIN using challenge-response, so the PIN itself never crosses the network, and you can switch the web client off entirely so only the VibeSDR app can connect. You can leave clients free to choose their own bandwidth, or pin it \u2014 pinned, their picker disappears and says the server set it. The receiver can publish its own location (opt-in, never assumed) by device position or by naming a town or Maidenhead locator, and clients then show the receiver\u2019s name and place, and measure spot distances and band edges from the ANTENNA rather than from wherever the listener happens to be. If the app is killed while serving, the server rebuilds itself and carries on. \u2014 The RTL-TCP box becomes CUSTOM SERVER: type any address and VibeSDR works out what is listening (VibeServer, OpenWebRX, KiwiSDR, UberSDR, FM-DX, rtl_tcp or SpyServer), so one box reaches every backend. Local hardware is now RTL-SDR, with Listen and Use as server side by side. \u2014 Fixes: entering a frequency in a different band now switches to the right demodulator and span (jumping from a medium-wave station to FM used to leave you in AM with a 5 kHz filter); the waterfall no longer shows half a minute of stale history after a big jump; the lower sample rates (0.96 and 1.2 MHz) no longer break up; rtl_tcp no longer plays chipmunks on some rates; dragging the gain slider no longer breaks the audio; panning past the tuned station no longer drops audio or crawls; and auto-contrast now defaults to 5 (10 was too dark). \u2014 The receiver also NAMES THE STATIONS IT CAN HEAR: when a station announces itself over RDS, VibeSDR remembers it against the frequency, so the search bar fills itself in with what this aerial actually receives. It keeps itself honest \u2014 the PI code spots a different broadcaster on a frequency immediately, and a station that goes unheard for 30 days expires rather than sitting on top of static. The name is reconstructed by majority vote across repetitions, so it can recover a name no single transmission delivered cleanly. Save stations to the receiver (shared with everyone) or to your own browser, and import an existing list to either. \u2014 Point a browser at vibesdr.local: no IP address to remember, and a second phone serving on the same network renames itself automatically. \u2014 Station logos and country flags now actually appear, on every backend and on AM and shortwave too, not just FM; where the country genuinely cannot be known (a station arriving on sporadic-E, say) VibeSDR declines to show a flag rather than showing your own country\u2019s.' },
  { v: 'V7.1.0', detail: 'SpyServer compatibility, a reorganised audio menu, and today’s fixes. VibeSDR now connects to SpyServer receivers via sdr:// links (tap one anywhere, or paste sdr://host:port into the Custom URL box) and can save them as favourites — low-bandwidth, so good over mobile data. All audio controls (noise reduction, noise blanker, squelch, auto-notch, recording + playback) moved into a new Audio button to declutter the main menu; the demodulator popup gained the bandwidth sliders and Share moved next to the frequency keypad. You can now favourite the receiver you’re listening to straight from the menu. Every menu section gained a small icon for easier scanning. On FM-DX, recording/library moved into the same Audio button. Fixes: sharing a recording no longer freezes the controls; the waterfall no longer blanks on USB/RTL-TCP at full zoom-out; and iOS cold-start deep links open the linked instance.' },
  { v: 'V7.0.1', detail: 'Networked-radio stability and two iOS fixes. Sharing an RTL-SDR over a phone hotspot or busy Wi-Fi is far more reliable: the sharing phone now holds a Wi-Fi lock so its radio can’t drop into power-save mid-stream, the receiving side keeps a short buffer so a brief Wi-Fi stall no longer breaks the audio, and the sharing screen shows a live link-health indicator. iOS: FM stereo now actually plays in stereo on local hardware and RTL-TCP (it was quietly downmixed to mono), and scanning a QR code or opening a vibesdr:// link with the app closed now goes to the correct receiver instead of your default one.' },
  { v: 'V7.0.0', detail: 'FM-DX Webserver support — a whole new kind of receiver. VibeSDR now connects to the worldwide network of FM-DX Webserver tuners (from servers.fmdx.org): real, remote FM broadcast tuners you share with other listeners. New vintage-radio tuning dial that learns and pins every station name as you tune across the band, full RDS (station name, RadioText, PI code, PTY, TP/TA, stereo), a dBf signal meter, transmitter details (site, power, distance and bearing from the receiver), tap-to-tune alternative frequencies, station logos and country flags. Because the tuner is shared, there’s built-in chat, a listener counter, and the lock-screen skip buttons are disabled so you can’t accidentally retune it for everyone. The demodulator button opens mono/stereo, cEQ, iMS and an antenna switch (when the server offers one). Station logos and country flags now also appear on local RTL-SDR and networked WFM using the RDS PI code, with an on-device logo cache so they persist even offline. Pausing from the lock screen disconnects to save battery and reconnects on play.' },
  { v: 'V6.1.0', detail: 'Networked RTL-SDR. VibeSDR now auto-discovers rtl_tcp servers on your Wi-Fi (via Bonjour/mDNS) and lists them under a new “Discovered” section — no IP typing needed (iOS and Android). And on Android you can now share a plugged-in RTL-SDR over the network as an RTL-TCP server, so an iPhone or any rtl_tcp client can use the dongle remotely — handy for a good antenna location or an always-on phone. Includes an optional bandwidth cap, an editable name shown to other devices, and a live status notification. Plugging in an RTL-SDR on Android now asks whether to listen on the device or share it. The location and local-network permission prompts also now explain exactly what they’re for.' },
  { v: 'V6.0.0', detail: 'A major under-the-hood upgrade for iOS 27. VibeSDR now builds on React Native’s New Architecture (required for iOS 27 / Xcode 27 support, since Apple no longer accepts the older toolchain). Alongside that: RTL-SDR local hardware and RTL-TCP tuning is fixed — typing a frequency now retunes cleanly first time (it previously needed a nudge of the tuning drum), a race in the on-device tuner has been eliminated. Local Hardware and each RTL-TCP source now remember their own last frequency, mode and hardware settings independently (including VHF/UHF stations, which used to reset). Plugging an RTL-SDR into an Android phone and choosing “Open in VibeSDR” now goes straight to Local Hardware instead of your default instance. On Android, background audio now correctly holds up on devices that aggressively restrict apps — if your phone is throttling VibeSDR in the background, the app now detects it and shows you how to allow background usage. Plus the first-launch tutorial no longer appears on top of the welcome screen.' },
  { v: 'V5.2.2', detail: 'iPad and tablet polish. The signal meter now frames the frequency correctly on tablets (the coloured level showed above and below the readout on phones but not on larger screens), and the on-screen decoders (RTTY, NAVTEX, WEFAX, SSTV, Morse) now work in landscape on tablets, which have the room a phone doesn’t. The HAPTICS toggle is now hidden on devices with no haptic motor (all iPads, and any Android tablet without one) so it’s no longer a dead button.' },
  { v: 'V5.2.1', detail: 'Privacy: the optional location used to sort the instance list by distance is now taken and shared at approximate (coarse, ~1 km) accuracy only, instead of a precise fix. Location stays entirely optional and every other feature works without it.' },
  { v: 'V5.2.0', detail: 'Deep linking (early feature, still being rolled out). A vibesdr:// link — and a QR code from an UberSDR instance — can open VibeSDR straight onto that instance, optionally at a set frequency and mode. The link/QR side is still being built on the UberSDR end, so not every instance offers a link yet. The share button now also includes an “Open in VibeSDR” app link alongside the web link. Opening a link no longer bounces back to your default instance.' },
  { v: 'V5.1.5', detail: 'Android layout fix: on phones using the classic three-button navigation bar, the menu’s CLOSE button could sit underneath the system buttons and be hard to tap. The menu now respects the navigation-bar inset so CLOSE always clears it. No effect on gesture-navigation devices. Android-only.' },
  { v: 'V5.1.4', detail: 'KiwiSDR servers don’t support chat, so the Chat button is now greyed out and disabled while you’re connected to a KiwiSDR — the Share button stays available. No other changes.' },
  { v: 'V5.1.3', detail: 'Polish + reliability. New first-launch info screen explaining the power-saving behaviour (the waterfall fully freezes in the background by design and takes a moment to resume). Returning from the background now shows a calm “Reinitialising” notice while the waterfall comes back, instead of a misleading “Connection lost” — and if the spectrum genuinely fails to resume while audio keeps playing, you get a clear reconnect / instance-list prompt. Fixed a swipe-up-from-the-home-bar gesture that could nudge the tuning, and fixed the menu’s MIN / MAX zoom buttons (full-out / full-in) which previously did nothing.' },
  { v: 'V5.1.2', detail: 'iOS 26/27 audio fix. On iOS 27 the audio could go silent after a while even though the connection looked fine — the underlying audio socket was stalling (reporting alive while frames quietly stopped). The native audio path was moved onto Apple’s Network framework, with a session safety-net that keeps the stream rendering, fixing the dropouts. iOS-only.' },
  { v: 'V5.1.1', detail: 'OpenWebRX squelch & noise reduction now follow the server’s presets. If a server owner has set a default squelch level on a profile (e.g. a 2 m NFM profile at −65 dB) — or an initial noise-reduction level — selecting that profile now applies it automatically, with the menu sliders updated to match, instead of staying off or stuck on your previous setting. Matches the OpenWebRX web client’s behaviour.' },
  { v: 'V5.1', detail: 'Unlocked VFO + waterfall panning, and a saved-recordings player. A new VFO Lock toggle (menu) lets you free the waterfall: locked (default) is exactly as before, unlocked lets you drag to pan the band while staying tuned, with a floating “Centre on VFO” button. Gestures are now tap-to-tune, drag-to-pan, pinch-to-zoom, with panning moved onto the UI thread for smoothness on poor connections. On Local Hardware / RTL-TCP the dongle (RF) centre becomes a true second VFO — with the VFO unlocked you can pan the view across the full captured bandwidth at native resolution while a station stays tuned, the dongle locking at the capture edge with an RF-CENTRE marker and hard walls. New Recordings screen lists, plays (with scrub), shares and deletes your recordings in-app — no more recordings stranded in storage. See What\'s New above.' },
  { v: 'V5.0.1', detail: 'CW fix for Local Hardware (USB RTL-SDR): tuning straight onto a CW signal used to go silent — the beat-note offset and the actual filter width had drifted apart, so the morse was only audible when tuned well off the signal. They\'re now kept in sync, so a signal tuned dead-on gives a clear, audible ~600 Hz tone with readable morse. The mode pill also reads “CW” to match the button.' },
  { v: 'V5', detail: 'New on-device DSP engine — SDR++ Brown (and FFTW and VOLK) have been REMOVED and replaced with VibeDSP, VibeSDR\'s own clean-room, GPL-free signal-processing engine for Local Hardware and RTL-TCP. It is hand-optimised with ARM NEON SIMD throughout, so it runs noticeably cooler and lighter on the battery — especially on low-end phones and tablets — while matching the old engine. It also brings real improvements: true single-sideband SSB (proper image rejection, not double-sideband), genuine FM stereo with a 19 kHz pilot PLL + RDS, a per-channel audio AGC for SSB/CW, working de-emphasis (50/75 µs), a reliable stereo indicator and a force-mono switch. See What\'s New above.' },
  { v: 'V4', detail: 'Local SDR hardware — VibeSDR now runs a radio on-device. Plug an RTL-SDR into an Android phone over USB (“Local Hardware”), or connect to a networked rtl_tcp server from either platform, and the app demodulates AM/SSB/CW/NFM/WFM itself with a bundled on-device DSP core — full waterfall, drum, audio and decoders, with a hardware-control submenu. Adds an MMSE noise-reduction engine and an adaptive Auto Notch (on every backend), plus a client-side dBFS squelch for KiwiSDR. (V5 later replaced that DSP core with VibeSDR’s own clean-room engine.) See What\'s New above.' },
  { v: 'V3', detail: 'Multi-backend release — VibeSDR now speaks three SDR server protocols (UberSDR, OpenWebRX/OpenWebRX+ and KiwiSDR) behind the same interface, with a new directory chooser in the server picker. (KiwiSDRs have very few slots, so owners choose who connects — some refuse apps or block broadcast bands. A refusal or sudden drop is the owner\'s restriction, not a fault in VibeSDR.)' },
  { v: 'V2.2.2', detail: 'Store-readiness pass: clearer location prompt (it’s only for sorting/filtering instances by distance, and denying it changes nothing else); removed two unused Android permissions (microphone and draw-over-other-apps); added a privacy policy and an App Store distribution exception to the GPL licence. No functional changes.' },
  { v: 'V2.2.1', detail: 'In-car fix: a Siri voice command interrupts the car audio session, which paused and disconnected VibeSDR — it then sat dead until you pressed Play. It now auto-resumes (reconnects on the new frequency) the moment Siri finishes, with no manual Play. A genuine takeover by another app (e.g. a Mac grabbing your AirPods) still waits for Play, as before.' },
  { v: 'V2.2', detail: 'Siri voice control (iOS). Say "Hey Siri, tune VibeSDR" — Siri asks what — then a frequency (7.150 MHz / 7150 kHz / 7151.5), a station ("Radio Caroline"), or a band ("40m ham", "CB"). It tunes with the right demodulator + step, honouring any spoken mode. When a name matches several bookmarks (e.g. "Radio 5") Siri reads the frequencies and you pick by voice; "China Radio at 11 MHz" narrows the list. Also "change VibeSDR mode" → AM / SAM / synchronous AM / LSB / lower side band / …, and "set VibeSDR step rate" → 100 Hz, 9 kHz, … It runs in the background while VibeSDR is playing, so it works over headphones / CarPlay / the lock screen without unlocking. (Tuning is a two-step ask-and-answer — Apple only allows a value inside a one-shot Siri phrase for fixed lists. Android’s in-car answer stays the Android Auto Bookmarks/Band-Plan browse — Google Assistant needs Play Store publishing.)' },
  { v: 'V2.1.12', detail: 'SNR meter now reads radiod’s channel SNR (baseband power − noise density) straight from the audio stream — the demodulator’s own measurement of the tuned channel — so it’s accurate against the local noise floor and completely independent of the waterfall zoom. (Corrected for radiod’s +30 dB audio-floor offset, so it stays honest 0–50 dB rather than the inflated 30–80 dB.)' },
  { v: 'V2.1.11', detail: 'Simpler pause/play: pause now disconnects and play reconnects (the server lets the session go on suspend anyway, and reconnecting is near-instant) — no more mute timeout or countdown. The media card shows a clear Disconnected state, and a “Failed to reconnect — open VibeSDR” state with an ⚠️ if the server is full or busy. Also fixes the SNR meter drifting with zoom level (the noise floor is now measured zoom-independently; dBFS and S-meter were already fine).' },
  { v: 'V2.1.10', detail: 'Data Saver polish: while paused the media controls show a static “auto-disconnect at HH:MM to save data & power”, and once it disconnects the controls are released entirely (no half-working Play button). Reopening the app fully reconnects and unmutes.' },
  { v: 'V2.1.9', detail: 'Resuming after a Data Saver disconnect now does a full from-scratch reconnect (new session) instead of reopening the old one — fixes the frozen waterfall / dead-audio state that previously needed a trip back to the instance list.' },
  { v: 'V2.1.8', detail: 'The album art now reflects state at a glance: the server-logo corner becomes a muted-speaker with the minutes-to-disconnect while paused, and a disconnected icon once the Data Saver drops the stream.' },
  { v: 'V2.1.7', detail: 'When another app takes over audio (e.g. a Mac grabbing your shared AirPods, or another media app on the phone), VibeSDR now registers it as a mute — the muted banner shows and the Data Saver countdown starts — instead of silently sitting connected. Works on both iOS and Android now. Press Play to come back.' },
  { v: 'V2.1.6', detail: 'Data Saver moved to the main menu (under Controls). Pause now genuinely pauses on iOS — the lock-screen button no longer springs back to play, and the app stops grabbing shared AirPods from a Mac while paused. The Admin section is now labelled “Instance Admin”.' },
  { v: 'V2.1.4', detail: 'Fixes a frozen waterfall when returning to the app after another audio app suspended it (it now reconnects on its own). Adds a Data Saver: after a chosen spell muted — lock screen, AirPods out, or pause — the SDR stream disconnects to stop wasting data and battery. Pick the timeout under Power Saving (Off / Instant / 5–30 min); the media controls show a countdown, then “Open App to Resume”, and Play reconnects.' },
  { v: 'V2.1.3', detail: 'Media-control skips (lock screen, Apple Watch, Android Auto, headphones) now snap to the step grid like the VFO drum — so skipping from an off-grid frequency lands on a clean multiple of the step rate.' },
  { v: 'V2.1.2', detail: 'Band-aware tuning tweaks: utility and beacon (NDB) ranges now default to USB / 500 Hz, and the 11m CB band follows the receiver’s ITU region — NFM in Europe/Asia-Pacific, AM in the Americas.' },
  { v: 'V2.1.1', detail: 'Band-aware mode/step now also applies to remote tuning — lock screen, Apple Watch and connected headphones — not just the car. It still stays out of your way while you’re hands-on tuning with the VFO drum on the waterfall.' },
  { v: 'V2.1', detail: 'In-car upgrades: Android Auto now shows browsable Bookmarks and Band Plan lists (tap to tune), not just skip buttons. Band-aware tuning sets the right demodulator and step for each band — applied when you pick a band from the search list, and automatically as you cross band edges while connected to a car (handheld tuning is never changed automatically). CarPlay browsing is ready for when the entitlement is in place.' },
  { v: 'V2.0.1', detail: 'Bug fixes: bandwidth sliders now match the server’s 6 kHz limit (were going to 8 kHz), and bookmark/band-plan search shows the full result list in a scrollable dropdown (previously capped at 25 results, so higher bands like 20m were cut off).' },
  { v: 'V2', detail: "Fully native rewrite with a custom GPU waterfall/spectrum stack (V1's headline future plan, delivered), native audio with background playback and media controls, on-device decoders, chat, bookmarks and much more — see What's New above." },
  { v: 'V1', detail: 'Initial version — UberSDR support only, using the server-provided waterfall and spectrum.' },
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
  'VibeServer: it knows what radio it has. An SDRplay RSP (on the Mac) is offered its LNA state and IF gain reduction with a live total\u2011system\u2011gain readout so you can WATCH the AGC work; an Airspy HF+ gets its attenuator, preamp and AGC (its sample rate is set once in the server\u2019s config, because that radio does not survive being re\u2011rated on a live stream); an RTL\u2011SDR gets its gain table. Controls that a given radio does not have are no longer shown \u2014 a slider that cannot move the receiver is worse than no slider.',
  'ADVANCED RDS \u2014 a broadcast analyser, on a phone. Not just the station name: the PI code and what it means, PTY, TP/TA/MS flags, RadioText and RadioText+ (what is playing right now), Long PS, PTYN, the clock, the language, alternative frequencies and the other stations in the network. Then the measurements a DXer would normally buy hardware for: pilot deviation, RDS deviation, the block error rate, a live constellation with a scatter figure, the MPX spectrum, and the symbol trace that explains the error rate at a glance.',
  'Advanced RDS: the RDS\u2011to\u2011pilot PHASE \u2014 the field this panel exists for. Correct is near 0\u00b0, or near 90\u00b0 for quadrature encoding; the middle ground indicates a transmitter fault, and reading it normally takes equipment most people do not have. It will also tell you when a station\u2019s encoder is not locked to its own pilot at all, which draws as a rotating ring rather than two lobes \u2014 a real fault, found on air, confirmed on two independent receivers.',
  'Advanced RDS was calibrated against a PIRA broadcast analyser by Hans van Eijsden \u2014 without whom these numbers would be guesses. Hans measured our readings against professional equipment station by station, found that our phase was reading as its own reflection and that the deviation figures were scaled wrong, and kept testing until they agreed. He is also the only route we have to testing Long PS, RadioText+ and PTYN, because UK broadcasters simply do not transmit them. Thank you, Hans.',
  'Airspy HF+ Discovery support, on Android and on the Mac \u2014 an outstanding receiver for FM DX, and now a first\u2011class one here: attenuator, preamp and AGC, with RDS decoding to match. Measured on air rather than assumed: on a strong local signal the preamp costs more than it gives, and the app now says what each control really does instead of offering three that look alike.',
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
    detail: 'The small, permissively-licensed FFT kernel inside VibeDSP — the on-device waterfall and spectrum, and the MMSE noise reduction, Auto Notch and decoders.' },
  { name: 'Zstandard — Yann Collet / Meta',
    detail: 'Compression used inside the bundled DSP core.' },
  { name: 'libairspyhf — Airspy (BSD-3)',
    detail: 'The Airspy HF+ Discovery driver. BSD-licensed and shipping as a static library, so the radio works the moment it is plugged in with nothing to install.' },
  { name: 'libhackrf — Great Scott Gadgets (GPL-2.0-or-later)',
    detail: 'The HackRF One driver, for the experimental HackRF support in VibeServer. Bundled as a patched copy: upstream has no way to open a radio from a USB file descriptor, which is the only way an Android app is allowed to open one at all. VibeSDR is GPL-3, which those terms permit.' },
  { name: 'SDRplay — SDRplay Ltd',
    detail: 'The RSP API used by VibeServer on macOS. Its headers only — the closed-source library is never bundled, and is loaded at runtime if you have installed it. No SDRplay software, no SDRplay support, and nothing breaks.' },
  { name: 'libusb',
    detail: 'USB device access for VibeServer on macOS.' },
  { name: 'Hans van Eijsden',
    detail: 'Calibrated Advanced RDS against a PIRA broadcast analyser, station by station — he found that our phase reading was its own reflection and that the deviation figures were scaled wrong, and kept testing until they agreed. Without him those numbers would be guesses rather than measurements. He is also the only route we have to testing Long PS, RadioText+ and PTYN, which UK broadcasters simply do not transmit.' },
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
  { name: 'opus-decoder (wasm-audio-decoders) — Ethan Halsall',
    detail: 'libopus compiled to WebAssembly. It is what lets VibeServer’s web client play Opus in any browser, on a plain http:// address — where the browser’s own WebCodecs decoder is unavailable. MIT-licensed. Found by way of UberSDR, which uses it for the same reason.' },
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
              {/* ★ Show the build ONLY when we actually know it. Constants.nativeBuildVersion is null in
                    some contexts (an iOS app running on Apple Silicon among them), and "build ?" is worse
                    than no build number at all — it reads as the app being unsure what it is. */}
              <Text style={styles.appVer}>
                Version {APP_VERSION}{Constants.nativeBuildVersion ? ` · build ${Constants.nativeBuildVersion}` : ''}
              </Text>
              {/* ★ Keep this in step with the backends we actually speak. It sat at three receivers and
                    "your own RTL-SDR" long after FM-DX, SpyServer, VibeServer, the Airspy HF+ and the
                    SDRplay RSP arrived — a one-line description of the app that had stopped describing it. */}
              <Text style={styles.appSub}>A native mobile client for UberSDR, OpenWebRX, KiwiSDR, Web-888, FM-DX and SpyServer receivers — and your own RTL-SDR, Airspy HF+ or SDRplay, shared by VibeServer</Text>
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

          <Text style={styles.section}>WHAT'S NEW IN V10</Text>
          {V10_CHANGES.map((c) => (
            <View key={c} style={styles.bulletRow}>
              <Text style={styles.bulletDot}>•</Text>
              <Text style={styles.bulletText}>{c}</Text>
            </View>
          ))}

          {/* ★ ONLY THE CURRENT RELEASE GETS EXPANDED HIGHLIGHTS. V9 down to V2 each had a full
              bulleted list here AS WELL AS an entry in VERSION HISTORY below — the same ground
              twice, and the page grew by a screenful every release. Highlights answer "what
              changed in the version you just installed"; everything older is the changelog's job.
              When V11 ships: retire V10_CHANGES and put V11's here. */}

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
