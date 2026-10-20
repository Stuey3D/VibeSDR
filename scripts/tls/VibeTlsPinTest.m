// VibeTlsPinTest.m — the Apple pin core (modules/vibe-tls-trust/VibeTlsCore.m, the file the app compiles) against
// real self-signed servers on 127.0.0.1 (tls-test-server.mjs). Run by scripts/test-tls-pin-apple.sh. GitHub #52.
//
// What it proves, with NSURLSession wired exactly as the app's fetch hook is (dispositionForChallenge):
//   • no pin → refused, and the server never saw the request          • the probe classifies "untrusted" + fingerprint
//   • pinned → accepted, for THAT host:port only (not another port)   • a different certificate → refused ("changed")
//   • a pinned certificate past its date → refused ("expired")        • forget → refused again
//   • the pins survive a reload of the store (restart)
#import <Foundation/Foundation.h>
#import "VibeTlsCore.h"
#ifdef VIBE_WITH_SR
#import <SocketRocket/SRWebSocket.h>
#import "VibePinnedSRPolicy.h"

/// ★ React Native's WebSocket on iOS IS SocketRocket; this opens one exactly as VibeTlsTrust.mm's provider does for a
///   pinned host:port, and returns the server's first message (nil = refused / failed).
@interface WsProbe : NSObject <SRWebSocketDelegate>
@property (nullable) NSString *got;
@property dispatch_semaphore_t sem;
@end
@implementation WsProbe
- (void)webSocket:(SRWebSocket *)ws didReceiveMessageWithString:(NSString *)string { self.got = string; dispatch_semaphore_signal(self.sem); }
- (void)webSocket:(SRWebSocket *)ws didFailWithError:(NSError *)error { dispatch_semaphore_signal(self.sem); }
- (void)webSocket:(SRWebSocket *)ws didCloseWithCode:(NSInteger)c reason:(NSString *)r wasClean:(BOOL)w { dispatch_semaphore_signal(self.sem); }
@end

static NSString *wsOpen(VibeTlsCore *core, NSInteger port) {
  NSURLRequest *req = [NSURLRequest requestWithURL:[NSURL URLWithString:[NSString stringWithFormat:@"wss://localhost:%ld/ws/", (long)port]]];
  VibePinnedSRPolicy *pol = [[VibePinnedSRPolicy alloc] initWithHost:@"localhost" port:port core:core];
  SRWebSocket *ws = [[SRWebSocket alloc] initWithURLRequest:req protocols:nil securityPolicy:pol];
  WsProbe *p = [WsProbe new]; p.sem = dispatch_semaphore_create(0);
  ws.delegate = p;
  [ws setDelegateDispatchQueue:dispatch_queue_create("ws", DISPATCH_QUEUE_SERIAL)];
  [ws open];
  dispatch_semaphore_wait(p.sem, dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC));
  [ws close];
  return p.got;
}
#endif

static int bad = 0;
#define CHECK(cond, what) do { if (cond) printf("  ok   %s\n", what); else { bad++; printf("  FAIL %s\n", what); } } while (0)

@interface Hook : NSObject <NSURLSessionDelegate>
@property VibeTlsCore *core;
@end
@implementation Hook
- (void)URLSession:(NSURLSession *)s didReceiveChallenge:(NSURLAuthenticationChallenge *)c
 completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *))done {
  NSURLCredential *cred = nil;
  NSURLSessionAuthChallengeDisposition d = [self.core dispositionForChallenge:c credential:&cred];
  done(d, cred);
}
@end

/// GET https://localhost:<port>/ through a fresh session with the app's hook. Returns the body, or nil on failure.
static NSString *fetch(VibeTlsCore *core, NSInteger port) {
  Hook *h = [Hook new]; h.core = core;
  NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
  cfg.timeoutIntervalForRequest = 5;
  NSURLSession *s = [NSURLSession sessionWithConfiguration:cfg delegate:h delegateQueue:nil];
  __block NSString *body = nil;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  NSURL *u = [NSURL URLWithString:[NSString stringWithFormat:@"https://localhost:%ld/", (long)port]];
  [[s dataTaskWithURL:u completionHandler:^(NSData *d, NSURLResponse *r, NSError *e) {
    if (!e && d) body = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
    dispatch_semaphore_signal(sem);
  }] resume];
  dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC));
  [s invalidateAndCancel];
  return body;
}

static NSDictionary *probe(VibeTlsCore *core, NSString *host, NSInteger port) {
  __block NSDictionary *res = nil;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [core probeHost:host port:port timeout:5 completion:^(NSDictionary *r) { res = r; dispatch_semaphore_signal(sem); }];
  dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC));
  return res;
}

int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc < 8) { fprintf(stderr, "usage: test <portSelf> <fpSelf> <portLocalhost> <fpLocalhost> <portExpired> <fpExpired> <requestLog>\n"); return 2; }
    NSInteger pSelf = atoi(argv[1]), pLh = atoi(argv[3]), pEx = atoi(argv[5]);
    NSString *fpSelf = @(argv[2]), *fpLh = @(argv[4]), *fpEx = @(argv[6]);
    NSString *logPath = @(argv[7]);
    NSString *suite = @"net.vibesdr.tls-pin-test";
    [[NSUserDefaults new] removePersistentDomainForName:suite];
    NSUserDefaults *ud = [[NSUserDefaults alloc] initWithSuiteName:suite];
    VibeTlsCore *core = [[VibeTlsCore alloc] initWithDefaults:ud];

    printf("keys\n");
    CHECK([[VibeTlsCore keyForHost:@"SDR.Example.org." port:8073] isEqualToString:@"sdr.example.org:8073"], "host lower-cased, trailing dot dropped");
    CHECK([[VibeTlsCore keyForHost:@"[fe80::1]" port:443] isEqualToString:@"fe80::1:443"], "IPv6 brackets dropped");

    printf("self-signed CN=openwebrx (OWRX+-style, 10 years, CA:TRUE), no pin\n");
    NSDictionary *r = probe(core, @"localhost", pSelf);
    CHECK([r[@"reason"] isEqualToString:@"untrusted"], "probe says untrusted");
    CHECK(![r[@"systemTrusted"] boolValue], "probe: not system-trusted");
    CHECK([r[@"sha256"] isEqualToString:fpSelf], "probe fingerprint = openssl's");
    CHECK([r[@"pinned"] isKindOfClass:NSNull.class], "probe: no pin");
    CHECK(fetch(core, pSelf) == nil, "fetch refused without a pin");
    NSString *log = [NSString stringWithContentsOfFile:logPath encoding:NSUTF8StringEncoding error:nil] ?: @"";
    NSString *reqMark = [NSString stringWithFormat:@"REQ %ld", (long)pSelf];
    CHECK(![log containsString:reqMark], "the server never saw a request (probe or refused fetch)");

#ifdef VIBE_WITH_SR
    printf("WebSocket (SocketRocket, the pinned policy) without a pin\n");
    CHECK(wsOpen(core, pSelf) == nil, "wss refused without a pin");
    log = [NSString stringWithContentsOfFile:logPath encoding:NSUTF8StringEncoding error:nil] ?: @"";
    NSString *upMark = [NSString stringWithFormat:@"UPGRADE %ld", (long)pSelf];
    CHECK(![log containsString:upMark], "SocketRocket never sent the upgrade (cookies, User-Agent) to the refused server");
#else
    printf("  (SocketRocket not found — the WebSocket cases need `pod install`; not run)\n");
#endif

    printf("pin it\n");
    [core pinHost:@"localhost" port:pSelf sha256:fpSelf subject:@"openwebrx"];
    NSString *body = fetch(core, pSelf);
    CHECK(body && [body containsString:@"openwebrx"], "fetch accepted with the pin");
    r = probe(core, @"localhost", pSelf);
    CHECK([r[@"pinMatches"] boolValue], "probe: pin matches");
#ifdef VIBE_WITH_SR
    NSString *hello = wsOpen(core, pSelf);
    CHECK([hello hasPrefix:@"CLIENT DE SERVER"], "wss accepted with the pin, server hello received");
#endif

    printf("the same pin does not cover another port\n");
    CHECK(fetch(core, pLh) == nil, "other port refused (a pin is host:port, not host)");

    printf("certificate changed\n");
    [core pinHost:@"localhost" port:pLh sha256:fpSelf subject:@"openwebrx"];   // pinned the OLD cert for that port
    CHECK(fetch(core, pLh) == nil, "a different certificate is refused");
#ifdef VIBE_WITH_SR
    CHECK(wsOpen(core, pLh) == nil, "wss: a different certificate is refused");
#endif
    r = probe(core, @"localhost", pLh);
    CHECK(![r[@"pinMatches"] boolValue] && [r[@"sha256"] isEqualToString:fpLh], "probe: new fingerprint, pin does not match");

    printf("expired, even when pinned\n");
    [core pinHost:@"localhost" port:pEx sha256:fpEx subject:@"localhost"];
    CHECK(fetch(core, pEx) == nil, "an expired pinned certificate is refused");
    r = probe(core, @"localhost", pEx);
    CHECK([r[@"reason"] isEqualToString:@"expired"], "probe says expired");

    printf("restart\n");
    VibeTlsCore *again = [[VibeTlsCore alloc] initWithDefaults:[[NSUserDefaults alloc] initWithSuiteName:suite]];
    CHECK([[again pinForHost:@"LOCALHOST" port:pSelf] isEqualToString:fpSelf], "pin survives a reload");
    CHECK(fetch(again, pSelf) != nil, "and still connects");

    printf("forget\n");
    CHECK([core forgetHost:@"localhost" port:pSelf], "forget returns YES");
    CHECK(fetch(core, pSelf) == nil, "refused again after forget");

    printf("nothing listening\n");
    r = probe(core, @"localhost", 1);
    CHECK([r[@"reason"] isEqualToString:@"unreachable"], "probe: unreachable");

    // ★ Optional, needs the internet: a publicly trusted server is the system's, and a wrong name is "hostname".
    r = probe(core, @"vibesdr.net", 443);
    if ([r[@"reason"] isEqualToString:@"unreachable"]) printf("  (offline — public-CA cases not run)\n");
    else {
      printf("public CA (network)\n");
      CHECK([r[@"systemTrusted"] boolValue] && [r[@"reason"] isEqualToString:@"ok"], "vibesdr.net: system-trusted");
      r = probe(core, @"wrong.host.badssl.com", 443);
      if (![r[@"reason"] isEqualToString:@"unreachable"])
        CHECK([r[@"reason"] isEqualToString:@"hostname"], "wrong.host.badssl.com: hostname mismatch, not offered");
    }

    [[NSUserDefaults new] removePersistentDomainForName:suite];
    printf(bad ? "\n%d FAILED\n" : "\nall passed\n", bad);
    return bad ? 1 : 0;
  }
}
