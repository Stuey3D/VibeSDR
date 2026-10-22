// icom_audio.h — the Icom's RECEIVE audio, captured from the radio's own USB audio codec.
//
// ★★★ CAPTURE ONLY. Nothing in VibeServer ever opens the radio's codec for PLAYBACK: its output
//     is the radio's USB MOD input, and audio into it with VOX on would transmit (plan §3.1, §3.3).
//     There is no playback code here to call by mistake.
//
// ★★ WHAT THE AUDIO IS. The radio has already demodulated it, through its own filter, AGC and NR
//    (plan §0). It goes into VibeServer AFTER the demodulator stage — the shim's onAudio() — so the
//    Opus fan-out, the decoders (FT8/RTTY/NAVTEX/WEFAX/SSTV), NR and the notch all work on it as
//    they do on our own demodulated audio, and our IQ chain is simply not running.
//
// ★★ WHY RAW ALSA IOCTLS ON LINUX, NOT tinyalsa OR libasound (decision recorded in the plan):
//    • libasound2 is a system library — "nothing from the system but glibc" for the .deb
//      (feedback_linux_deb_no_external_deps). Out.
//    • tinyalsa (BSD-3) would be fine, but it is ~2 000 lines for mixers, plugins and playback we
//      must NOT have. What capture needs is five ioctls on /dev/snd/pcmC?D?c — HW_PARAMS, SW_PARAMS,
//      PREPARE, READI_FRAMES, and DROP — which is exactly what tinyalsa itself does underneath, in
//      ~150 lines with no new file in vendor/. Lighter on a Pi 2 (no PulseAudio, nothing to load).
//    • A desktop running PulseAudio/PipeWire may hold the card; then the open fails with EBUSY and
//      the setup page says so (the same "one program holds the port" story as CI-V).
//  ★ macOS: CoreAudio AUHAL input (needs com.apple.security.device.audio-input and the
//    NSMicrophoneUsageDescription prompt — see vibeserver/mac).
//  ★ Tests: "tone" and "wav:<file>" — paced by the clock exactly like a real 48 kHz device, so the
//    end-to-end test runs the same fan-out and decoder path the radio would, with no hardware.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace icom {

/** Interleaved int16, `channels` 1 or 2, at `rate` (48 000 from every source here). Called on the
 *  capture thread, which runs at AUDIO priority. ★ Must not block. */
using PcmFn = std::function<void(const int16_t* pcm, int frames, int channels, int rate)>;

class AudioCapture {
public:
    virtual ~AudioCapture() = default;
    virtual bool start(PcmFn fn, std::string& err) = 0;
    virtual void stop() = 0;
    virtual std::string describe() const = 0;   ///< for the setup page: "ALSA hw:2,0 (USB Audio CODEC)"
    /** True once the device has gone away on its own (unplugged); the owner restarts it. */
    virtual bool lost() const { return false; }
    /** What is wrong with it NOW, in plain words, or "" — for a source whose state can change after start
     *  (the Android "push" source: permission refused later, the codec unplugged). */
    virtual std::string problem() const { return ""; }
    /** True once since the last call if describe()/problem() changed — IcomSource re-sends hwinfo on it. */
    virtual bool takeChanged() { return false; }
};

/** spec: "auto" (the codec next to the radio on the USB bus), "none", "tone", "wav:<path>",
 *  "alsa:hw:<card>,<dev>" (Linux), "coreaudio:<name substring>" (macOS), "push" (Android).
 *  @param usbPath the CI-V transport's sysfs name ("1-1.2.3") — the codec is its sibling on the
 *                 radio's internal hub. */
std::unique_ptr<AudioCapture> makeAudio(const std::string& spec, const std::string& usbPath, std::string& err);

/* ★★★ "push" — ANDROID'S RADIO AUDIO (plan Phase 4). The kernel's snd-usb-audio owns the radio's codec, so libusb
 *     cannot have it and there is no ALSA node an app may open. Java records it instead — AudioRecord with
 *     setPreferredDevice(the radio's TYPE_USB_DEVICE input), 48 kHz mono PCM16 — and pushes each block in here
 *     (VibeIcomAudio.kt → JNI). The push source is what IcomSource opened, so the TX mute, onAudio(), the Opus
 *     fan-out and the decoders are exactly the Linux path's.
 *  ★ CAPTURE ONLY, still: AudioRecord is an INPUT; nothing here or in Java ever opens the radio's codec for playback.
 *  ★ No source live (no Icom served, or between stop and start) = the block is dropped; never queued.
 *  ★ pushPcm runs on Java's AudioRecord thread and does not allocate; the lock it takes is held only by
 *    start()/stop() and itself, so stop() returning means no block is still on its way into the engine. */
void pushPcm(const int16_t* pcm, int frames, int channels, int rate);
/** What the app knows about the radio's audio — the device it recorded from ("USB Audio CODEC (USB)"), and what is
 *  wrong ("" = nothing): the microphone permission refused, no USB input found, Android routed elsewhere. */
void setPushState(const std::string& describe, const std::string& problem);
/** Is a "push" source live (an Icom is being served and wants audio)? The app starts AudioRecord only then. */
bool pushWanted();

}  // namespace icom
