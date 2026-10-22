// icom_audio.cpp — see icom_audio.h. Four sources: ALSA (Linux), CoreAudio AUHAL (macOS), and the
// clock-paced "tone" and "wav:" test sources. ★ Every one delivers 48 kHz int16 in 10 ms blocks from
// a buffer allocated ONCE at start — the capture thread never touches the heap.
#include "icom_audio.h"
#include "vibe_thread.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#if defined(__linux__)
#  include <dirent.h>
#  include <fcntl.h>
#  include <limits.h>
#  include <stdlib.h>
#  include <sys/ioctl.h>
#  include <unistd.h>
#  include <sound/asound.h>
#  define VIBE_ICOM_ALSA 1
#endif
#if defined(__APPLE__)
#  include <TargetConditionals.h>
#  if TARGET_OS_OSX
#    include <AudioToolbox/AudioToolbox.h>
#    include <CoreAudio/CoreAudio.h>
#    define VIBE_ICOM_COREAUDIO 1
#  endif
#endif

namespace icom {

namespace {

constexpr int kRate = 48000;
constexpr int kBlock = 480;   // 10 ms

// ── A clock-paced test source: the base for "tone" and "wav:" ──────────────────────────────
class PacedSource : public AudioCapture {
public:
    bool start(PcmFn fn, std::string& err) override {
        if (!prepare(err)) return false;
        fn_ = std::move(fn);
        run_.store(true);
        th_ = std::thread([this] {
            vibeAudioThread("vibe-icom-au");
            std::vector<int16_t> buf((size_t)kBlock * 2);
            auto next = std::chrono::steady_clock::now();
            while (run_.load()) {
                next += std::chrono::microseconds(10000);
                fill(buf.data(), kBlock);
                fn_(buf.data(), kBlock, 2, kRate);
                std::this_thread::sleep_until(next);
            }
        });
        return true;
    }
    void stop() override { run_.store(false); if (th_.joinable()) th_.join(); }
    ~PacedSource() override { stop(); }
protected:
    virtual bool prepare(std::string&) { return true; }
    virtual void fill(int16_t* lr, int frames) = 0;
private:
    PcmFn fn_;
    std::atomic<bool> run_{false};
    std::thread th_;
};

/** A 1 kHz tone at -20 dBFS over a little noise — "is audio arriving at all?" */
class ToneSource final : public PacedSource {
public:
    std::string describe() const override { return "test tone (1 kHz)"; }
protected:
    void fill(int16_t* lr, int frames) override {
        for (int i = 0; i < frames; i++) {
            ph_ += 2.0 * M_PI * 1000.0 / kRate; if (ph_ > 2.0 * M_PI) ph_ -= 2.0 * M_PI;
            seed_ = seed_ * 1664525u + 1013904223u;
            const double n = ((double)(seed_ >> 8) / 16777216.0 - 0.5) * 0.01;
            const int16_t v = (int16_t)std::lround((0.1 * std::sin(ph_) + n) * 32767.0);
            lr[2 * i] = v; lr[2 * i + 1] = v;
        }
    }
private:
    double ph_ = 0; uint32_t seed_ = 1;
};

/** A 16-bit PCM WAV at 48 kHz, looped. ★ Read whole at start (test material, minutes at most). */
class WavSource final : public PacedSource {
public:
    explicit WavSource(std::string p) : path_(std::move(p)) {}
    std::string describe() const override { return "WAV file " + path_; }
protected:
    bool prepare(std::string& err) override {
        FILE* f = std::fopen(path_.c_str(), "rb");
        if (!f) { err = "cannot open " + path_; return false; }
        std::vector<uint8_t> all;
        uint8_t tmp[65536]; size_t n;
        while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0) all.insert(all.end(), tmp, tmp + n);
        std::fclose(f);
        if (all.size() < 44 || std::memcmp(all.data(), "RIFF", 4) || std::memcmp(all.data() + 8, "WAVE", 4)) { err = path_ + " is not a WAV file"; return false; }
        size_t at = 12; int ch = 0, rate = 0, bits = 0;
        while (at + 8 <= all.size()) {
            const uint32_t len = all[at + 4] | all[at + 5] << 8 | all[at + 6] << 16 | (uint32_t)all[at + 7] << 24;
            if (!std::memcmp(all.data() + at, "fmt ", 4) && at + 24 <= all.size()) {
                ch = all[at + 10] | all[at + 11] << 8;
                rate = (int)(all[at + 12] | all[at + 13] << 8 | all[at + 14] << 16 | (uint32_t)all[at + 15] << 24);
                bits = all[at + 22] | all[at + 23] << 8;
            } else if (!std::memcmp(all.data() + at, "data", 4)) {
                if (ch < 1 || ch > 2 || bits != 16 || rate != kRate) { err = path_ + " must be 16-bit, mono or stereo, 48 kHz"; return false; }
                const size_t bytes = std::min<size_t>(len, all.size() - at - 8);
                const size_t frames = bytes / (2 * (size_t)ch);
                pcm_.resize(frames * 2);
                const int16_t* s = (const int16_t*)(all.data() + at + 8);
                for (size_t i = 0; i < frames; i++) { pcm_[2 * i] = s[i * ch]; pcm_[2 * i + 1] = s[i * ch + (ch - 1)]; }
                return !pcm_.empty() || (err = path_ + " has no audio", false);
            }
            at += 8 + len + (len & 1);
        }
        err = path_ + " has no data chunk"; return false;
    }
    void fill(int16_t* lr, int frames) override {
        const size_t total = pcm_.size() / 2;
        for (int i = 0; i < frames; i++) { lr[2 * i] = pcm_[2 * pos_]; lr[2 * i + 1] = pcm_[2 * pos_ + 1]; if (++pos_ >= total) pos_ = 0; }
    }
private:
    std::string path_;
    std::vector<int16_t> pcm_;
    size_t pos_ = 0;
};

// ── ALSA, by its kernel interface ──────────────────────────────────────────────────────────
#if VIBE_ICOM_ALSA
/** ★ What tinyalsa's param helpers do: every mask full and every interval open, then narrow. */
void hwInit(snd_pcm_hw_params& p) {
    std::memset(&p, 0, sizeof p);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++) {
        snd_mask& m = p.masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        std::memset(m.bits, 0xFF, sizeof m.bits);
    }
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
        snd_interval& i = p.intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        i.min = 0; i.max = ~0u;
    }
    p.rmask = ~0u; p.cmask = 0; p.info = ~0u;
}
void hwMask(snd_pcm_hw_params& p, int n, unsigned bit) {
    snd_mask& m = p.masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    std::memset(m.bits, 0, sizeof m.bits);
    m.bits[bit >> 5] |= 1u << (bit & 31);
}
void hwInt(snd_pcm_hw_params& p, int n, unsigned v) {
    snd_interval& i = p.intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i.min = v; i.max = v; i.integer = 1;
}

/** ★★ THE CODEC NEXT TO THE RADIO. On an IC-7300 the CP2102 and the audio codec hang off the
 *  radio's own internal hub, so the codec is the sound card whose USB path shares the CP2102's
 *  parent ("1-1.2.3" → hub "1-1.2"). Read from sysfs — no device is opened to find it. */
int findSiblingCard(const std::string& usbPath) {
    const size_t dot = usbPath.rfind('.');
    if (usbPath.empty() || dot == std::string::npos) return -1;
    const std::string hub = "/" + usbPath.substr(0, dot) + "/";
    DIR* d = opendir("/sys/class/sound");
    if (!d) return -1;
    int found = -1;
    while (dirent* e = readdir(d)) {
        int card = -1;
        if (std::sscanf(e->d_name, "card%d", &card) != 1) continue;
        char real[PATH_MAX];
        const std::string link = std::string("/sys/class/sound/") + e->d_name + "/device";
        if (!realpath(link.c_str(), real)) continue;
        if (std::strstr(real, hub.c_str())) {
            char cap[64]; std::snprintf(cap, sizeof cap, "/dev/snd/pcmC%dD0c", card);
            if (access(cap, F_OK) == 0) { found = card; break; }
        }
    }
    closedir(d);
    return found;
}

class AlsaSource final : public AudioCapture {
public:
    AlsaSource(int card, int dev) : card_(card), dev_(dev) {}
    ~AlsaSource() override { stop(); }
    std::string describe() const override { return "ALSA hw:" + std::to_string(card_) + "," + std::to_string(dev_) + name_; }
    bool lost() const override { return lost_.load(); }
    bool start(PcmFn fn, std::string& err) override {
        char path[64]; std::snprintf(path, sizeof path, "/dev/snd/pcmC%dD%dc", card_, dev_);
        fd_ = ::open(path, O_RDWR);
        if (fd_ < 0) {
            err = std::string("could not open the radio's audio (") + path + "): " + std::strerror(errno)
                + (errno == EBUSY ? " — a sound server (PulseAudio/PipeWire) or another program has it"
                   : errno == EACCES ? " — the vibeserver user must be in the 'audio' group" : "");
            return false;
        }
        { char n[96] = {0}; char p2[64]; std::snprintf(p2, sizeof p2, "/proc/asound/card%d/id", card_);
          if (FILE* f = std::fopen(p2, "r")) { if (std::fgets(n, sizeof n, f)) { std::string s(n); while (!s.empty() && s.back() == '\n') s.pop_back(); name_ = " (" + s + ")"; } std::fclose(f); } }
        // ★ Stereo first (the 7300's codec is stereo, the same audio on both), mono if refused.
        bool ok = false;
        for (unsigned ch : {2u, 1u}) {
            snd_pcm_hw_params hp; hwInit(hp);
            hwMask(hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
            hwMask(hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
            hwMask(hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
            hwInt(hp, SNDRV_PCM_HW_PARAM_CHANNELS, ch);
            hwInt(hp, SNDRV_PCM_HW_PARAM_RATE, kRate);
            hwInt(hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, kBlock);
            hwInt(hp, SNDRV_PCM_HW_PARAM_PERIODS, 8);       // 80 ms of slack for a busy Pi 2
            if (ioctl(fd_, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) == 0) { ch_ = (int)ch; ok = true; break; }
        }
        if (!ok) { err = "the radio's audio would not run at 48 kHz 16-bit: " + std::string(std::strerror(errno)); ::close(fd_); fd_ = -1; return false; }
        snd_pcm_sw_params sp; std::memset(&sp, 0, sizeof sp);
        sp.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
        sp.period_step = 1;
        sp.avail_min = kBlock;
        sp.start_threshold = 1;
        sp.stop_threshold = kBlock * 8;
        sp.boundary = (snd_pcm_uframes_t)kBlock * 8;
        while (sp.boundary * 2 <= (snd_pcm_uframes_t)LONG_MAX - kBlock * 8) sp.boundary *= 2;
        if (ioctl(fd_, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) != 0) { err = "the radio's audio refused its settings"; ::close(fd_); fd_ = -1; return false; }
        if (ioctl(fd_, SNDRV_PCM_IOCTL_PREPARE) != 0) { err = "the radio's audio would not start"; ::close(fd_); fd_ = -1; return false; }
        fn_ = std::move(fn);
        run_.store(true);
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() override {
        run_.store(false);
        if (fd_ >= 0) ioctl(fd_, SNDRV_PCM_IOCTL_DROP);   // wakes a blocked read
        if (th_.joinable()) th_.join();
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }
private:
    int card_, dev_, fd_ = -1, ch_ = 2;
    std::string name_;
    PcmFn fn_;
    std::atomic<bool> run_{false}, lost_{false};
    std::thread th_;
    void loop() {
        vibeAudioThread("vibe-icom-au");
        std::vector<int16_t> buf((size_t)kBlock * 2);
        while (run_.load()) {
            snd_xferi x{}; x.buf = buf.data(); x.frames = kBlock;
            if (ioctl(fd_, SNDRV_PCM_IOCTL_READI_FRAMES, &x) == 0) {
                if (x.result > 0) fn_(buf.data(), (int)x.result, ch_, kRate);
                continue;
            }
            if (!run_.load()) break;
            if (errno == EPIPE) { ioctl(fd_, SNDRV_PCM_IOCTL_PREPARE); continue; }   // overrun: carry on
            if (errno == EINTR || errno == EAGAIN) continue;
            lost_.store(true);       // ENODEV and friends: the radio went away
            break;
        }
    }
};
#endif

// ── CoreAudio AUHAL input (macOS) ──────────────────────────────────────────────────────────
#if VIBE_ICOM_COREAUDIO
AudioDeviceID findInputDevice(const std::string& want) {
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz) != noErr) return 0;
    std::vector<AudioDeviceID> ids(sz / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, ids.data()) != noErr) return 0;
    for (AudioDeviceID id : ids) {
        // input channels?
        AudioObjectPropertyAddress sa{kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMain};
        UInt32 bsz = 0;
        if (AudioObjectGetPropertyDataSize(id, &sa, 0, nullptr, &bsz) != noErr || bsz == 0) continue;
        std::vector<uint8_t> bl(bsz);
        if (AudioObjectGetPropertyData(id, &sa, 0, nullptr, &bsz, bl.data()) != noErr) continue;
        const AudioBufferList* l = (const AudioBufferList*)bl.data();
        UInt32 chans = 0; for (UInt32 i = 0; i < l->mNumberBuffers; i++) chans += l->mBuffers[i].mNumberChannels;
        if (!chans) continue;
        CFStringRef nm = nullptr; UInt32 nsz = sizeof nm;
        AudioObjectPropertyAddress na{kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(id, &na, 0, nullptr, &nsz, &nm) != noErr || !nm) continue;
        char buf[256] = {0};
        CFStringGetCString(nm, buf, sizeof buf, kCFStringEncodingUTF8);
        CFRelease(nm);
        if (std::strstr(buf, want.c_str())) return id;
    }
    return 0;
}

class CoreAudioSource final : public AudioCapture {
public:
    explicit CoreAudioSource(std::string name) : want_(std::move(name)) {}
    ~CoreAudioSource() override { stop(); }
    std::string describe() const override { return "CoreAudio \"" + want_ + "\""; }
    bool start(PcmFn fn, std::string& err) override {
        dev_ = findInputDevice(want_);
        if (!dev_) { err = "no audio input called \"" + want_ + "\" — is the radio's USB audio connected?"; return false; }
        // ★ AUHAL does not resample: ask the DEVICE for 48 kHz (the codec's native rate) first.
        Float64 rate = kRate;
        AudioObjectPropertyAddress ra{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioObjectSetPropertyData(dev_, &ra, 0, nullptr, sizeof rate, &rate);
        AudioComponentDescription cd{kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent comp = AudioComponentFindNext(nullptr, &cd);
        if (!comp || AudioComponentInstanceNew(comp, &au_) != noErr) { err = "CoreAudio input unit unavailable"; return false; }
        UInt32 on = 1, off = 0;
        AudioUnitSetProperty(au_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &on, sizeof on);
        // ★★ OUTPUT DISABLED — this unit can never play into the radio's MOD input.
        AudioUnitSetProperty(au_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &off, sizeof off);
        AudioUnitSetProperty(au_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev_, sizeof dev_);
        AudioStreamBasicDescription f{};
        f.mSampleRate = kRate; f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        f.mChannelsPerFrame = 2; f.mBitsPerChannel = 16; f.mFramesPerPacket = 1;
        f.mBytesPerFrame = 4; f.mBytesPerPacket = 4;
        if (AudioUnitSetProperty(au_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f) != noErr) {
            err = "the radio's audio would not run at 48 kHz 16-bit"; stop(); return false;
        }
        AURenderCallbackStruct cb{&CoreAudioSource::onInput, this};
        AudioUnitSetProperty(au_, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof cb);
        buf_.assign(8192 * 2, 0);   // ★ allocated once; the render thread never allocates
        fn_ = std::move(fn);
        if (AudioUnitInitialize(au_) != noErr || AudioOutputUnitStart(au_) != noErr) {
            // ★ The usual cause: microphone access refused (System Settings > Privacy > Microphone).
            err = "macOS refused the audio input — allow VibeServer under System Settings > Privacy & Security > Microphone";
            stop(); return false;
        }
        return true;
    }
    void stop() override {
        if (au_) { AudioOutputUnitStop(au_); AudioUnitUninitialize(au_); AudioComponentInstanceDispose(au_); au_ = nullptr; }
    }
private:
    std::string want_;
    AudioDeviceID dev_ = 0;
    AudioComponentInstance au_ = nullptr;
    std::vector<int16_t> buf_;
    PcmFn fn_;
    static OSStatus onInput(void* ctx, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts,
                            UInt32 bus, UInt32 frames, AudioBufferList*) {
        auto* self = (CoreAudioSource*)ctx;
        if (frames * 2 > self->buf_.size()) return noErr;
        AudioBufferList bl; bl.mNumberBuffers = 1;
        bl.mBuffers[0].mNumberChannels = 2; bl.mBuffers[0].mDataByteSize = frames * 4; bl.mBuffers[0].mData = self->buf_.data();
        if (AudioUnitRender(self->au_, flags, ts, bus, frames, &bl) == noErr)
            self->fn_(self->buf_.data(), (int)frames, 2, kRate);
        return noErr;
    }
};
#endif

}  // namespace

std::unique_ptr<AudioCapture> makeAudio(const std::string& spec, const std::string& usbPath, std::string& err) {
    if (spec == "none") return nullptr;
    if (spec == "tone") return std::unique_ptr<AudioCapture>(new ToneSource());
    if (spec.rfind("wav:", 0) == 0) return std::unique_ptr<AudioCapture>(new WavSource(spec.substr(4)));
#if VIBE_ICOM_ALSA
    if (spec.rfind("alsa:hw:", 0) == 0) {
        int c = -1, d = 0;
        if (std::sscanf(spec.c_str() + 8, "%d,%d", &c, &d) < 1 || c < 0) { err = "icomAudio alsa:hw:<card>,<device>"; return nullptr; }
        return std::unique_ptr<AudioCapture>(new AlsaSource(c, d));
    }
    if (spec.empty() || spec == "auto") {
        const int c = findSiblingCard(usbPath);
        if (c < 0) { err = "the radio's USB audio was not found next to its CI-V port — set icomAudio=alsa:hw:<card>,0"; return nullptr; }
        return std::unique_ptr<AudioCapture>(new AlsaSource(c, 0));
    }
#endif
#if VIBE_ICOM_COREAUDIO
    if (spec.rfind("coreaudio:", 0) == 0) return std::unique_ptr<AudioCapture>(new CoreAudioSource(spec.substr(10)));
    // ★ The 7300's codec names itself "USB Audio CODEC" (TI PCM2900 family) [unverified on macOS].
    if (spec.empty() || spec == "auto") return std::unique_ptr<AudioCapture>(new CoreAudioSource("USB Audio CODEC"));
#endif
    (void)usbPath;
    err = "icomAudio \"" + spec + "\" is not available on this platform";
    return nullptr;
}

}  // namespace icom
