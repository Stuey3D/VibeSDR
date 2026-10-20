// VibeTlsTrust — per-server certificate pins (trust on first use) wired into React Native's own sockets. GitHub #52.
//
// ★★★ THE PROBLEM. casm's OpenWebRX+ answered https on 8073 with a self-signed certificate, and the app could not
//     connect: the platform refuses a certificate no recognised authority signed, and it is right to. Browsers and SSH
//     let a person look at THAT certificate and trust it for THAT server; this does the same. The decision and the
//     pins live in VibeTlsCore (pure, tested on the Mac by scripts/test-tls-pin-apple.sh); this file is the glue.
//
// ★★ THE HOOKS, AND WHY THESE. React Native's prebuilt core (RCT_USE_PREBUILT_RNCORE) cannot be patched with
//    patch-package, so each hook is one RN itself offers, or the smallest runtime addition that does the same job:
//    • WebSocket (OWRX, Kiwi, FM-DX, UberSDR/VibeServer spectrum) — RCTSetCustomSRWebSocketProvider, RN's own seam.
//      It is used ONLY for a host:port that has a pin; every other socket is built by RN exactly as before (nil).
//      The provider is not given the sub-protocols, so a pinned socket is opened with none — no socket in this app
//      asks for one (every `new WebSocket(url, null|undefined, …)`); if one ever does, pass them here first.
//    • fetch / XHR (the https-first probe, /status.json, UberSDR's /connection …) — RCTHTTPRequestHandler is the
//      NSURLSession delegate and does not implement the session-level challenge method, so the system answers every
//      challenge. We ADD that one method at load (class_addMethod, never a swizzle — nothing of RN's is replaced),
//      and it hands every challenge back to the system except a refused server trust that matches its pin. If a
//      future RN implements the method itself we leave it alone and say so in the log.
//    ✗ Not a global "allow self-signed" switch, not a trust-all policy, and nothing touches the system trust store.
//    ★ ATS: Info.plist has NSAllowsArbitraryLoads (plain-http receivers), which is what lets a delegate accept a
//      pinned trust. Without it NSURLSession would refuse the pin and only the WebSocket path would work.
//
// JS: src/services/tlsTrust.ts (NativeModules.VibeTlsTrust) — inspect / listPins / trust / forget.
#import <React/RCTBridgeModule.h>
#import <React/RCTWebSocketModule.h>
#import <React/RCTHTTPRequestHandler.h>
#import <SocketRocket/SRWebSocket.h>
#import <SocketRocket/SRSecurityPolicy.h>
#import <objc/runtime.h>
#import "VibeTlsCore.h"
#import "VibePinnedSRPolicy.h"

static NSInteger portOf(NSURL *url) {
  if (url.port) return url.port.integerValue;
  NSString *s = url.scheme.lowercaseString;
  return ([s isEqualToString:@"wss"] || [s isEqualToString:@"https"]) ? 443 : 80;
}

#pragma mark - fetch: the challenge method RN does not implement

static void installFetchHook(void) {
  Class cls = NSClassFromString(@"RCTHTTPRequestHandler");
  SEL sel = @selector(URLSession:didReceiveChallenge:completionHandler:);
  if (!cls) { NSLog(@"[VibeTls] RCTHTTPRequestHandler not found — fetch pins inactive"); return; }
  if ([cls instancesRespondToSelector:sel]) {
    NSLog(@"[VibeTls] RCTHTTPRequestHandler already answers challenges — fetch pins NOT installed (RN changed; revisit)");
    return;
  }
  IMP imp = imp_implementationWithBlock(^(id __unused me, NSURLSession *__unused session,
                                          NSURLAuthenticationChallenge *challenge,
                                          void (^completion)(NSURLSessionAuthChallengeDisposition, NSURLCredential *)) {
    NSURLCredential *cred = nil;
    NSURLSessionAuthChallengeDisposition d = [[VibeTlsCore shared] dispositionForChallenge:challenge credential:&cred];
    completion(d, cred);
  });
  // v@:@@@? — void, self, _cmd, session, challenge, block
  class_addMethod(cls, sel, imp, "v@:@@@?");
}

#pragma mark - installing the hooks

/// ★ Its own class because RCT_EXPORT_MODULE() already defines the module's +load.
@interface VibeTlsHooks : NSObject
@end

@implementation VibeTlsHooks
/// ★ At load, before React Native creates its NSURLSession (lazily, on the first request) or opens any socket.
+ (void)load {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    installFetchHook();
    RCTSetCustomSRWebSocketProvider(^SRWebSocket *(NSURLRequest *request) {
      NSURL *url = request.URL;
      NSString *scheme = url.scheme.lowercaseString;
      if (!url.host || !([scheme isEqualToString:@"wss"] || [scheme isEqualToString:@"https"])) return nil;
      NSInteger port = portOf(url);
      // ★★ No pin → nil → React Native builds the socket itself, with its default policy, exactly as before.
      if (![[VibeTlsCore shared] pinForHost:url.host port:port]) return nil;
      VibePinnedSRPolicy *policy = [[VibePinnedSRPolicy alloc] initWithHost:url.host port:port];
      return [[SRWebSocket alloc] initWithURLRequest:request protocols:nil securityPolicy:policy];
    });
  });
}
@end

#pragma mark - the module

@interface VibeTlsTrust : NSObject <RCTBridgeModule>
@end

@implementation VibeTlsTrust

RCT_EXPORT_MODULE();

+ (BOOL)requiresMainQueueSetup { return NO; }

/// What the platform makes of host:port's certificate, for the prompt. A handshake only — see probeHost.
RCT_EXPORT_METHOD(inspect:(NSString *)host port:(double)port
                  resolver:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject) {
  [[VibeTlsCore shared] probeHost:host port:(NSInteger)port timeout:5 completion:^(NSDictionary *r) { resolve(r); }];
}

RCT_EXPORT_METHOD(listPins:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject) {
  resolve([[VibeTlsCore shared] allPins]);
}

RCT_EXPORT_METHOD(trust:(NSString *)host port:(double)port sha256:(NSString *)sha256 subject:(NSString *)subject
                  resolver:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject) {
  if (sha256.length != 64) { reject(@"bad_fingerprint", @"a SHA-256 fingerprint is 64 hex characters", nil); return; }
  [[VibeTlsCore shared] pinHost:host port:(NSInteger)port sha256:sha256 subject:subject];
  NSLog(@"[VibeTls] trusted %@ = %@", [VibeTlsCore keyForHost:host port:(NSInteger)port], sha256);
  resolve(@YES);
}

RCT_EXPORT_METHOD(forget:(NSString *)host port:(double)port
                  resolver:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject) {
  BOOL had = [[VibeTlsCore shared] forgetHost:host port:(NSInteger)port];
  if (had) NSLog(@"[VibeTls] forgot %@", [VibeTlsCore keyForHost:host port:(NSInteger)port]);
  resolve(@(had));
}

@end
