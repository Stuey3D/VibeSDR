// VibeLocalSDR — iOS native module (ObjC++) bridging the shared C++ local-SDR
// shim to React Native as `NativeModules.VibeLocalSDR`, mirroring the Android
// Kotlin module so the existing JS works unchanged. iOS supports the RTL-TCP path
// only (no USB host SDR); USB methods (startSpectrum/listDevices/openAndProbe)
// reject. The DSP/shim lives in libvibelocalsdr_ios.a (+ volk/fftw3f/zstd).
#import <React/RCTBridgeModule.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <cmath>
#include <string>
#include <vector>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include "local_sdr_shim.h"

@interface VibeLocalSDR : NSObject <RCTBridgeModule>
@end

@implementation VibeLocalSDR

RCT_EXPORT_MODULE();

+ (BOOL)requiresMainQueueSetup { return NO; }

static double numOr(NSDictionary *o, NSString *k, double dflt) {
  id v = o[k]; return [v isKindOfClass:[NSNumber class]] ? [v doubleValue] : dflt;
}

// ── RTL-TCP ─────────────────────────────────────────────────────────────────
RCT_EXPORT_METHOD(startTcp:(NSDictionary *)opts
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject) {
  NSString *host = opts[@"host"];
  if (![host isKindOfClass:[NSString class]] || host.length == 0) {
    reject(@"no_host", @"host required", nil); return;
  }
  int port = (int)numOr(opts, @"port", 1234);
  double centerFreq = numOr(opts, @"centerFreq", 14100000.0);
  double sampleRate = numOr(opts, @"sampleRate", 2400000.0);
  int gain          = opts[@"gainTenthDb"] ? (int)numOr(opts, @"gainTenthDb", -1) : -1;
  int fftSize       = (int)numOr(opts, @"fftSize", 1024);
  double fftRate    = numOr(opts, @"fftRate", 20.0);
  NSString *mode    = [opts[@"mode"] isKindOfClass:[NSString class]] ? opts[@"mode"] : @"nfm";

  std::string err;
  int bound = vibe::LocalSdrShim::instance().startTcp(
      std::string(host.UTF8String), port, centerFreq, sampleRate, gain,
      fftSize, fftRate, std::string(mode.UTF8String), err);
  if (bound <= 0) {
    reject(@"start_failed",
           [NSString stringWithFormat:@"rtl_tcp %@:%d failed: %s", host, port, err.c_str()], nil);
    return;
  }
  resolve(@{ @"port": @(bound),
             @"wsBaseUrl": [NSString stringWithFormat:@"http://127.0.0.1:%d", bound] });
}

// ── SpyServer ───────────────────────────────────────────────────────────────
RCT_EXPORT_METHOD(startSpyServer:(NSDictionary *)opts
                        resolver:(RCTPromiseResolveBlock)resolve
                        rejecter:(RCTPromiseRejectBlock)reject) {
  NSString *host = opts[@"host"];
  if (![host isKindOfClass:[NSString class]] || host.length == 0) {
    reject(@"no_host", @"host required", nil); return;
  }
  int port = (int)numOr(opts, @"port", 5555);
  double centerFreq = numOr(opts, @"centerFreq", 100000000.0);
  double sampleRate = numOr(opts, @"sampleRate", 2400000.0);
  int gain          = opts[@"gainTenthDb"] ? (int)numOr(opts, @"gainTenthDb", -1) : -1;
  int fftSize       = (int)numOr(opts, @"fftSize", 1024);
  double fftRate    = numOr(opts, @"fftRate", 20.0);
  NSString *mode    = [opts[@"mode"] isKindOfClass:[NSString class]] ? opts[@"mode"] : @"nfm";

  std::string err;
  int bound = vibe::LocalSdrShim::instance().startSpyServer(
      std::string(host.UTF8String), port, centerFreq, sampleRate, gain,
      fftSize, fftRate, std::string(mode.UTF8String), err);
  if (bound <= 0) {
    reject(@"start_failed",
           [NSString stringWithFormat:@"SpyServer %@:%d failed: %s", host, port, err.c_str()], nil);
    return;
  }
  resolve(@{ @"port": @(bound),
             @"wsBaseUrl": [NSString stringWithFormat:@"http://127.0.0.1:%d", bound] });
}

RCT_EXPORT_METHOD(stopSpectrum:(RCTPromiseResolveBlock)resolve
                      rejecter:(RCTPromiseRejectBlock)reject) {
  vibe::LocalSdrShim::instance().stop();
  resolve(nil);
}

// ── Decoder sidecar (Kiwi/OWRX FT8 etc.) ────────────────────────────────────
RCT_EXPORT_METHOD(startDecoderService:(RCTPromiseResolveBlock)resolve
                             rejecter:(RCTPromiseRejectBlock)reject) {
  std::string err;
  int port = vibe::LocalSdrShim::instance().startDecoderService(err);
  if (port <= 0) { reject(@"start_failed", [NSString stringWithUTF8String:err.c_str()], nil); return; }
  resolve(@(port));
}
RCT_EXPORT_METHOD(stopDecoderService) { vibe::LocalSdrShim::instance().stop(); }
RCT_EXPORT_METHOD(feedDecoderPcm:(NSString *)b64 rate:(nonnull NSNumber *)rate) {
  NSData *data = [[NSData alloc] initWithBase64EncodedString:b64 options:0];
  if (!data) return;
  int n = (int)(data.length / 2);
  if (n < 2) return;
  vibe::LocalSdrShim::instance().feedDecoderPcm((const int16_t *)data.bytes, n, rate.intValue);
}
RCT_EXPORT_METHOD(setDecoderFreq:(double)hz) { vibe::LocalSdrShim::instance().setDecoderFreq(hz); }

// ── Hardware controls ───────────────────────────────────────────────────────
RCT_EXPORT_METHOD(setGain:(double)g)            { vibe::LocalSdrShim::instance().setGain((int)g); }
RCT_EXPORT_METHOD(setPpm:(double)p)             { vibe::LocalSdrShim::instance().setPpm((int)p); }
RCT_EXPORT_METHOD(setBiasTee:(BOOL)on)          { vibe::LocalSdrShim::instance().setBiasTee(on); }
RCT_EXPORT_METHOD(setAgc:(BOOL)on)              { vibe::LocalSdrShim::instance().setAgc(on); }
RCT_EXPORT_METHOD(setDirectSampling:(double)m)  { vibe::LocalSdrShim::instance().setDirectSampling((int)m); }
RCT_EXPORT_METHOD(setSampleRate:(double)r)      { vibe::LocalSdrShim::instance().setSampleRate(r); }
RCT_EXPORT_METHOD(setDeemphasis:(double)tau)    { vibe::LocalSdrShim::instance().setDeemphasis(tau); }
RCT_EXPORT_METHOD(setSquelch:(BOOL)on db:(double)db) { vibe::LocalSdrShim::instance().setSquelch(on, (float)db); }
RCT_EXPORT_METHOD(setNR:(BOOL)on)               { vibe::LocalSdrShim::instance().setNR(on); }
RCT_EXPORT_METHOD(setNotch:(BOOL)on)            { vibe::LocalSdrShim::instance().setNotch(on); }
RCT_EXPORT_METHOD(setStereoEnabled:(BOOL)on)    { vibe::LocalSdrShim::instance().setStereoEnabled(on); }
RCT_EXPORT_METHOD(setNrStrength:(double)s)      { vibe::LocalSdrShim::instance().setNrStrength((float)s); }
RCT_EXPORT_METHOD(getNrCpu:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject) {
  resolve(@(vibe::LocalSdrShim::instance().getNrCpu()));
}
// rtl_tcp CLIENT link health (jitter buffer). Matches the Android module's shape.
RCT_EXPORT_METHOD(getNetStatus:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject) {
  auto s = vibe::LocalSdrShim::instance().getNetStatus();
  resolve(@{
    @"tcp":            @(s.tcp),
    @"stalls":         @((double)s.stalls),
    @"droppedSamples": @((double)s.droppedSamples),
    @"bufferedMs":     @((double)s.bufferedMs),
    @"spy":            @(s.spy),
    @"canControl":     @(s.canControl),
    @"closed":         @(s.closed),
  });
}

RCT_EXPORT_METHOD(getTunerGains:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject) {
  std::vector<int> g = vibe::LocalSdrShim::instance().getTunerGains();
  NSMutableArray *out = [NSMutableArray arrayWithCapacity:g.size()];
  for (int v : g) [out addObject:@(v)];
  resolve(out);
}

// VibeServer serving needs the USB dongle, which iOS does not have. The setter
// exists so JS need not branch, but there is nothing here to serve.
RCT_EXPORT_METHOD(setServeOnLan:(BOOL)on) { vibe::LocalSdrShim::setServeOnLan(on); }

// ── Faceplate character folding (docs/BRIEF-faceplates.md §7) ───────────────
// A non-Latin station name (Cyrillic, Greek, Arabic, CJK…) → plain ASCII with ICU's
// `Any-Latin; Latin-ASCII`, so the dot-matrix and 14-segment displays can draw it. JS
// (src/constants/displayText.ts) folds the result further and falls back to the frequency
// when nothing usable comes back. Mirrors VibeLocalSdrModule.transliterate on Android.
// ★ SYNCHRONOUS (the interop layer maps a non-void return to a sync JSI call): it runs while a
//   display prepares its text, and JS memoises every answer. Any failure → the input unchanged.
// ★ Display only: the result is never stored or searched.
RCT_EXPORT_BLOCKING_SYNCHRONOUS_METHOD(transliterate:(NSString *)text) {
  if (![text isKindOfClass:[NSString class]] || text.length == 0) return @"";
  NSString *out = [text stringByApplyingTransform:@"Any-Latin; Latin-ASCII" reverse:NO];
  return out ?: text;
}

// ── DEVICE CLASS — Transparency effects' low-end default (src/constants/transparency.ts) ─────
// { totalMemoryBytes, model, isMac }. Mirrors VibeLocalSdrModule.deviceClass on Android; read once
// by src/services/deviceClass.ts. SYNCHRONOUS so the first frame already has the default.
// ★ model = utsname.machine, the identifier VibeCrashLog.hardwareModel() reports ("iPhone11,8").
//   That lives in the app target's Swift, out of this pod's reach, so it is mirrored here.
// ★★ On a Mac (the iPad app on Apple silicon, or Catalyst) machine is not an iPhone identifier: say
//   so with isMac, and report the Mac's own hw.model ("Mac14,2") when sysctl will give it. The JS
//   rule never downgrades a Mac on memory, model or version.
static NSString *sysctlString(const char *name) {
  size_t size = 0;
  if (sysctlbyname(name, NULL, &size, NULL, 0) != 0 || size == 0) return nil;
  std::vector<char> buf(size + 1, 0);
  if (sysctlbyname(name, buf.data(), &size, NULL, 0) != 0) return nil;
  NSString *s = [NSString stringWithUTF8String:buf.data()];
  return s.length ? s : nil;
}

RCT_EXPORT_BLOCKING_SYNCHRONOUS_METHOD(deviceClass) {
  NSProcessInfo *pi = NSProcessInfo.processInfo;
  BOOL isMac = NO;
  if (@available(iOS 14.0, *)) isMac = pi.isiOSAppOnMac;
  if (@available(iOS 13.0, *)) isMac = isMac || pi.isMacCatalystApp;
  NSString *model = nil;
  if (isMac) model = sysctlString("hw.model");
  if (!model) {
    struct utsname u; uname(&u);
    model = [NSString stringWithUTF8String:u.machine] ?: @"";
  }
  return @{
    @"totalMemoryBytes": @((double)pi.physicalMemory),
    @"model": model,
    @"isMac": @(isMac),
  };
}

// ── FRAME RATE CAP — CONTROL CUSTOMISATION → FACEPLATE → FRAME RATE (src/services/frameRate.ts) ─
// ★★★ Power audit, 2026-10-01: on a ProMotion iPhone the app held the panel at 120 Hz the whole time
//   the radio streamed. react-native-worklets' AnimationFrameQueue asks for 120 (and RN's own
//   display links ask for the default, which is the panel's maximum), Info.plist sets
//   CADisableMinimumFrameDurationOnPhone, and Skia canvases / Reanimated frame callbacks follow.
// ★★ ONE OWNER FOR EVERY DISPLAY LINK: rather than patching each library that makes one (worklets,
//   Reanimated's nodes manager and keyboard observer, RN's RCTDisplayLink / timers / native
//   animated, Skia's video) — and missing the next one an upgrade adds — the three CADisplayLink
//   entry points are wrapped ONCE, here, before main():
//     +displayLinkWithTarget:selector:  every new link is tracked (weakly) and, while capped, born capped;
//     -setPreferredFrameRateRange:      what the library ASKED for is remembered; the cap is applied;
//     -setPreferredFramesPerSecond:     the same, for the deprecated property worklets still sets.
//   Lifting the cap puts back exactly what each library asked for, so 'full' is today's behaviour.
// ★ LIVE: setFrameRateCap re-applies to every tracked link at once — no restart. The value is also
//   kept in NSUserDefaults, so the next launch is capped from its first link, before JS has run.
// ★ Thread-safe: worklets creates and sets its link on the JS queue, others on main. One recursive
//   lock (@synchronized) around the registry; a thread-local flag passes our OWN calls to the
//   original setters straight through, in case one setter is implemented with the other.
static NSString *const kVibeFrameRateCapKey = @"VibeFrameRateCapHz";

typedef struct { int kind; CAFrameRateRange range; NSInteger fps; } VibeFpsRequest;  // kind 0 none · 1 range · 2 fps
static const char kVibeFpsRequestKey = 0;

static NSObject *gFpsSync;
static NSHashTable *gFpsLinks;                // weak — a link's lifetime is its owner's business
static NSInteger gFpsCap = 0;                 // 0 = no cap (the panel's own maximum)
static __thread int gFpsInOriginal = 0;

static id   (*gOrigMake)(id, SEL, id, SEL);
static void (*gOrigSetRange)(id, SEL, CAFrameRateRange);
static void (*gOrigSetFps)(id, SEL, NSInteger);

static VibeFpsRequest vibeFpsRequestOf(CADisplayLink *link) {
  VibeFpsRequest q = {0, CAFrameRateRangeDefault, 0};
  NSValue *v = objc_getAssociatedObject(link, &kVibeFpsRequestKey);
  if (v) [v getValue:&q size:sizeof q];
  return q;
}

static CAFrameRateRange vibeFpsClamp(CAFrameRateRange r, float cap) {
  if (cap <= 0) return r;
  // The default range means "as fast as the panel goes": under a cap, that is the cap.
  // ★ The cap is a CEILING, never a fixed rate (Stuart: "lock it to 60 … UP TO 60; any drops lower than
  //   60 the refresh rate drops as it would normally"): the floor stays low so the system can still
  //   step a quiet screen down the way ProMotion does without the cap.
  if (r.maximum <= 0 || CAFrameRateRangeIsEqualToRange(r, CAFrameRateRangeDefault))
    return CAFrameRateRangeMake(MIN(10.0f, cap), cap, cap);
  float mx = MIN(r.maximum, cap);
  return CAFrameRateRangeMake(MIN(r.minimum, mx), mx, r.preferred > 0 ? MIN(r.preferred, mx) : r.preferred);
}

/** Give `link` what its owner asked for (`q`), under `cap`. Caller holds gFpsSync. */
static void vibeFpsApply(CADisplayLink *link, VibeFpsRequest q, NSInteger cap) {
  gFpsInOriginal++;
  if (q.kind == 2) {
    NSInteger f = q.fps;
    if (cap > 0 && (f <= 0 || f > cap)) f = cap;
    gOrigSetFps(link, @selector(setPreferredFramesPerSecond:), f);
  } else {
    gOrigSetRange(link, @selector(setPreferredFrameRateRange:),
                  vibeFpsClamp(q.kind == 1 ? q.range : CAFrameRateRangeDefault, (float)cap));
  }
  gFpsInOriginal--;
}

static id vibeFpsMake(id cls, SEL cmd, id target, SEL sel) {
  CADisplayLink *link = gOrigMake(cls, cmd, target, sel);
  if (!link) return link;
  @synchronized (gFpsSync) {
    [gFpsLinks addObject:link];
    if (gFpsCap > 0) vibeFpsApply(link, vibeFpsRequestOf(link), gFpsCap);
  }
  return link;
}

static void vibeFpsRecordAndApply(CADisplayLink *link, VibeFpsRequest q) {
  @synchronized (gFpsSync) {
    objc_setAssociatedObject(link, &kVibeFpsRequestKey, [NSValue valueWithBytes:&q objCType:@encode(VibeFpsRequest)],
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    vibeFpsApply(link, q, gFpsCap);
  }
}

static void vibeFpsSetRange(CADisplayLink *link, SEL cmd, CAFrameRateRange r) {
  if (gFpsInOriginal) { gOrigSetRange(link, cmd, r); return; }
  vibeFpsRecordAndApply(link, (VibeFpsRequest){1, r, 0});
}

static void vibeFpsSetFps(CADisplayLink *link, SEL cmd, NSInteger fps) {
  if (gFpsInOriginal) { gOrigSetFps(link, cmd, fps); return; }
  vibeFpsRecordAndApply(link, (VibeFpsRequest){2, CAFrameRateRangeDefault, fps});
}

// ★ Before main(): every display link a library makes later is made through the wrappers.
__attribute__((constructor)) static void vibeFpsInstall(void) {
  @autoreleasepool {
    Class c = [CADisplayLink class];
    Method mk = class_getClassMethod(c, @selector(displayLinkWithTarget:selector:));
    Method sr = class_getInstanceMethod(c, @selector(setPreferredFrameRateRange:));
    Method sf = class_getInstanceMethod(c, @selector(setPreferredFramesPerSecond:));
    if (!mk || !sr || !sf) return;   // an SDK without them: no cap, nothing broken
    gFpsSync  = [NSObject new];
    gFpsLinks = [NSHashTable weakObjectsHashTable];
    NSInteger stored = [[NSUserDefaults standardUserDefaults] integerForKey:kVibeFrameRateCapKey];
    gFpsCap = stored > 0 ? stored : 0;
    gOrigSetRange = (void (*)(id, SEL, CAFrameRateRange))method_setImplementation(sr, (IMP)vibeFpsSetRange);
    gOrigSetFps   = (void (*)(id, SEL, NSInteger))method_setImplementation(sf, (IMP)vibeFpsSetFps);
    gOrigMake     = (id (*)(id, SEL, id, SEL))method_setImplementation(mk, (IMP)vibeFpsMake);
  }
}

static void vibeFpsSetCap(NSInteger cap) {
  if (!gFpsSync) return;
  @synchronized (gFpsSync) {
    if (cap == gFpsCap) return;
    gFpsCap = cap;
    for (CADisplayLink *link in gFpsLinks.allObjects) vibeFpsApply(link, vibeFpsRequestOf(link), cap);
  }
}

// 0 = no cap; otherwise the cap in Hz (the app only ever sends 60). Live, and remembered for the
// next launch.
RCT_EXPORT_METHOD(setFrameRateCap:(double)hz) {
  NSInteger cap = (std::isfinite(hz) && hz > 0) ? (NSInteger)lround(hz) : 0;
  [[NSUserDefaults standardUserDefaults] setInteger:cap forKey:kVibeFrameRateCapKey];
  dispatch_async(dispatch_get_main_queue(), ^{ vibeFpsSetCap(cap); });
}

// The panel's top rate: 120 on ProMotion, 60 elsewhere. ★ UIKit — read on the main thread.
RCT_EXPORT_METHOD(maxRefreshRate:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject) {
  dispatch_async(dispatch_get_main_queue(), ^{
    UIScreen *screen = nil;
    for (UIScene *sc in UIApplication.sharedApplication.connectedScenes) {
      if ([sc isKindOfClass:[UIWindowScene class]]) { screen = ((UIWindowScene *)sc).screen; break; }
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (!screen) screen = UIScreen.mainScreen;
#pragma clang diagnostic pop
    resolve(@((double)screen.maximumFramesPerSecond));
  });
}

// ── USB (Android-only) — reject on iOS ──────────────────────────────────────
RCT_EXPORT_METHOD(startSpectrum:(NSDictionary *)opts
                       resolver:(RCTPromiseResolveBlock)resolve
                       rejecter:(RCTPromiseRejectBlock)reject) {
  reject(@"unsupported", @"Local USB hardware is not available on iOS (use RTL-TCP)", nil);
}
RCT_EXPORT_METHOD(listDevices:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject) {
  resolve(@[]);
}

@end
