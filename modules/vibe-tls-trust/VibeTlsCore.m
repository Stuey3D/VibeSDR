// VibeTlsCore — see VibeTlsCore.h. GitHub #52 (casm): an OpenWebRX+ with the self-signed pair it is commonly set up
// with could not be reached at all, because the platform (rightly) does not trust a certificate no authority signed.
//
// ★★★ NEVER WEAKER THAN THE PLATFORM (the rule VibeTls.kt set for Android). Every chain the system trusts is accepted
//     exactly as before, and that decision is the system's own evaluation with the host name. Our pin is consulted
//     ONLY after the system has refused, ONLY for the host:port it was made for, and ONLY for the exact certificate
//     the user was shown. Nothing here trusts all, skips the host-name check for anyone else, or touches the system
//     trust store.
#import "VibeTlsCore.h"
#import <CommonCrypto/CommonDigest.h>

static NSString *const kPinsKey = @"VibeTlsPins";

/// One probe's delegate: captures the server trust, classifies it, cancels. Owned by its session until invalidated.
@interface VibeTlsProbe : NSObject <NSURLSessionDataDelegate>
@property (nonatomic, strong) VibeTlsCore *core;
@property (nonatomic, copy) NSString *host;
@property (nonatomic) NSInteger port;
@property (nonatomic, copy, nullable) void (^done)(NSDictionary *);
@end

@implementation VibeTlsProbe
- (void)finish:(NSDictionary *)r {
  void (^d)(NSDictionary *);
  @synchronized (self) { d = self.done; self.done = nil; }
  if (d) d(r);
}
- (void)URLSession:(NSURLSession *)session didReceiveChallenge:(NSURLAuthenticationChallenge *)challenge
 completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *))completionHandler {
  NSURLProtectionSpace *ps = challenge.protectionSpace;
  if ([ps.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust] && ps.serverTrust) {
    [self finish:[self.core inspect:ps.serverTrust host:self.host port:self.port]];
  }
  // ★ Cancelled whatever it was: this session exists only to see the certificate, never to talk to the server.
  completionHandler(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
  [self finish:@{ @"systemTrusted": @NO, @"reason": @"unreachable", @"sha256": @"", @"der": @"",
                  @"message": error.localizedDescription ?: @"no certificate received" }];
  [session finishTasksAndInvalidate];
}
@end

@implementation VibeTlsCore {
  NSUserDefaults *_defaults;
  NSMutableDictionary<NSString *, NSDictionary *> *_pins;
}

+ (VibeTlsCore *)shared {
  static VibeTlsCore *s;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ s = [[VibeTlsCore alloc] initWithDefaults:[NSUserDefaults standardUserDefaults]]; });
  return s;
}

- (instancetype)initWithDefaults:(NSUserDefaults *)defaults {
  if ((self = [super init])) {
    _defaults = defaults;
    NSDictionary *stored = [defaults dictionaryForKey:kPinsKey];
    _pins = [NSMutableDictionary new];
    // ★ Only well-formed entries survive a load — a pin is 64 hex characters or it is nothing.
    [stored enumerateKeysAndObjectsUsingBlock:^(id k, id v, BOOL *stop) {
      if ([k isKindOfClass:NSString.class] && [v isKindOfClass:NSDictionary.class]) {
        NSString *fp = ((NSDictionary *)v)[@"sha256"];
        if ([fp isKindOfClass:NSString.class] && fp.length == 64) self->_pins[k] = v;
      }
    }];
  }
  return self;
}

#pragma mark - keys and fingerprints

+ (NSString *)keyForHost:(NSString *)host port:(NSInteger)port {
  NSString *h = [host stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet].lowercaseString;
  if ([h hasPrefix:@"["] && [h hasSuffix:@"]"]) h = [h substringWithRange:NSMakeRange(1, h.length - 2)];
  while ([h hasSuffix:@"."]) h = [h substringToIndex:h.length - 1];
  return [NSString stringWithFormat:@"%@:%ld", h, (long)port];
}

+ (NSString *)sha256OfCertificate:(SecCertificateRef)cert {
  NSData *der = CFBridgingRelease(SecCertificateCopyData(cert));
  unsigned char d[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(der.bytes, (CC_LONG)der.length, d);
  NSMutableString *hex = [NSMutableString stringWithCapacity:64];
  for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [hex appendFormat:@"%02x", d[i]];
  return hex;
}

/// The chain as the server sent it (leaf first). Kept as an NSArray so ARC owns it.
static NSArray *chainOf(SecTrustRef trust) {
  if (!trust) return @[];
  return CFBridgingRelease(SecTrustCopyCertificateChain(trust)) ?: @[];
}

#pragma mark - the checks

/// ★ THE PLATFORM'S DECISION — the system roots, the dates, and the host name, as every connection already got.
///   Evaluated on the trust the connection handed us (not a copy), so stapled OCSP and TLS-delivered SCTs count.
static BOOL systemTrusts(SecTrustRef trust, NSString *host) {
  if (!trust || host.length == 0) return NO;
  SecPolicyRef pol = SecPolicyCreateSSL(true, (__bridge CFStringRef)host);
  SecTrustSetPolicies(trust, pol);
  CFRelease(pol);
  return SecTrustEvaluateWithError(trust, NULL);
}

/// The chain is publicly trusted, but not for this name. Asked only to EXPLAIN a refusal, never to accept one.
static BOOL trustedIgnoringName(SecTrustRef trust) {
  SecPolicyRef pol = SecPolicyCreateSSL(true, NULL);
  SecTrustSetPolicies(trust, pol);
  CFRelease(pol);
  return SecTrustEvaluateWithError(trust, NULL);
}

/// ★★ IS THE LEAF ITSELF IN DATE? A pinned certificate is still refused once it has expired (or before it starts):
///    trust on first use replaces "who signed it", never "is it current". Evaluated with the leaf as its own anchor
///    and the BASIC X.509 policy — measured 2026-10-20 on macOS 27: the SSL policy refuses a typical self-signed
///    server certificate for reasons unrelated to trust ("not standards compliant" for a 10-year validity,
///    "not permitted for this usage" for CA:TRUE, which `openssl req -x509` sets by default), while the basic policy
///    accepts it and still reports -67818 errSecCertificateExpired for one out of date.
static BOOL leafInDate(SecCertificateRef leaf) {
  SecPolicyRef pol = SecPolicyCreateBasicX509();
  SecTrustRef t = NULL;
  OSStatus st = SecTrustCreateWithCertificates(leaf, pol, &t);
  CFRelease(pol);
  if (st != errSecSuccess || !t) return NO;
  SecTrustSetAnchorCertificates(t, (__bridge CFArrayRef)@[ (__bridge id)leaf ]);
  SecTrustSetAnchorCertificatesOnly(t, true);
  BOOL ok = SecTrustEvaluateWithError(t, NULL);
  CFRelease(t);
  return ok;
}

#pragma mark - pins

- (nullable NSString *)pinForHost:(NSString *)host port:(NSInteger)port {
  NSString *k = [VibeTlsCore keyForHost:host port:port];
  @synchronized (self) { return _pins[k][@"sha256"]; }
}

- (NSDictionary<NSString *, NSDictionary *> *)allPins {
  @synchronized (self) { return [_pins copy]; }
}

- (void)pinHost:(NSString *)host port:(NSInteger)port sha256:(NSString *)sha256 subject:(nullable NSString *)subject {
  NSString *fp = sha256.lowercaseString;
  if (fp.length != 64) return;
  NSString *k = [VibeTlsCore keyForHost:host port:port];
  @synchronized (self) {
    _pins[k] = @{ @"sha256": fp, @"subject": subject ?: @"", @"at": @((long long)([NSDate date].timeIntervalSince1970 * 1000)) };
    [_defaults setObject:[_pins copy] forKey:kPinsKey];
  }
}

- (BOOL)forgetHost:(NSString *)host port:(NSInteger)port {
  NSString *k = [VibeTlsCore keyForHost:host port:port];
  @synchronized (self) {
    if (!_pins[k]) return NO;
    [_pins removeObjectForKey:k];
    [_defaults setObject:[_pins copy] forKey:kPinsKey];
    return YES;
  }
}

#pragma mark - decisions

- (VibeTlsVerdict)evaluate:(SecTrustRef)trust host:(NSString *)host port:(NSInteger)port {
  if (!trust) return VibeTlsVerdictRejected;
  if (systemTrusts(trust, host)) return VibeTlsVerdictSystem;
  NSString *pin = [self pinForHost:host port:port];
  if (!pin) return VibeTlsVerdictRejected;                       // ★ no pin: the platform's refusal stands
  NSArray *chain = chainOf(trust);
  if (chain.count == 0) return VibeTlsVerdictRejected;
  SecCertificateRef leaf = (__bridge SecCertificateRef)chain[0];
  if (![[VibeTlsCore sha256OfCertificate:leaf] isEqualToString:pin]) return VibeTlsVerdictRejected;   // changed
  if (!leafInDate(leaf)) return VibeTlsVerdictRejected;          // the trusted one, but out of date
  return VibeTlsVerdictPinned;
}

- (NSURLSessionAuthChallengeDisposition)dispositionForChallenge:(NSURLAuthenticationChallenge *)challenge
                                                     credential:(NSURLCredential *_Nullable *_Nonnull)credential {
  *credential = nil;
  NSURLProtectionSpace *ps = challenge.protectionSpace;
  if (![ps.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust] || !ps.serverTrust)
    return NSURLSessionAuthChallengePerformDefaultHandling;
  if ([self evaluate:ps.serverTrust host:ps.host port:ps.port] != VibeTlsVerdictPinned)
    return NSURLSessionAuthChallengePerformDefaultHandling;      // ★ trusted or refused — the system's answer either way
  *credential = [NSURLCredential credentialForTrust:ps.serverTrust];
  return NSURLSessionAuthChallengeUseCredential;
}

- (void)probeHost:(NSString *)host port:(NSInteger)port timeout:(NSTimeInterval)timeout
       completion:(void (^)(NSDictionary *result))completion {
  NSString *h = [host containsString:@":"] && ![host hasPrefix:@"["] ? [NSString stringWithFormat:@"[%@]", host] : host;
  NSURL *url = [NSURL URLWithString:[NSString stringWithFormat:@"https://%@:%ld/", h, (long)port]];
  if (!url) { completion(@{ @"systemTrusted": @NO, @"reason": @"error", @"sha256": @"", @"der": @"", @"message": @"bad address" }); return; }
  VibeTlsProbe *p = [VibeTlsProbe new];
  p.core = self; p.host = host; p.port = port; p.done = completion;
  NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
  cfg.timeoutIntervalForRequest = timeout;
  cfg.timeoutIntervalForResource = timeout;
  cfg.URLCache = nil;
  cfg.HTTPCookieStorage = nil;                                   // ★ nothing of ours rides a probe
  NSURLSession *s = [NSURLSession sessionWithConfiguration:cfg delegate:p delegateQueue:nil];
  [[s dataTaskWithURL:url] resume];
}

- (NSDictionary *)inspect:(SecTrustRef)trust host:(NSString *)host port:(NSInteger)port {
  NSArray *chain = chainOf(trust);
  if (chain.count == 0) return @{ @"systemTrusted": @NO, @"reason": @"error", @"sha256": @"", @"der": @"" };
  SecCertificateRef leaf = (__bridge SecCertificateRef)chain[0];
  NSString *fp = [VibeTlsCore sha256OfCertificate:leaf];
  NSData *der = CFBridgingRelease(SecCertificateCopyData(leaf));
  BOOL sys = systemTrusts(trust, host);
  NSString *reason = sys ? @"ok"
                   : !leafInDate(leaf) ? @"expired"
                   : trustedIgnoringName(trust) ? @"hostname"
                   : @"untrusted";
  NSString *pin = [self pinForHost:host port:port];
  return @{
    @"systemTrusted": @(sys),
    @"reason": reason,
    @"sha256": fp,
    @"der": [der base64EncodedStringWithOptions:0],
    @"pinned": pin ?: (id)[NSNull null],
    @"pinMatches": @(pin != nil && [pin isEqualToString:fp]),
  };
}

@end

@implementation VibeTlsSessionDelegate
- (void)URLSession:(NSURLSession *)session didReceiveChallenge:(NSURLAuthenticationChallenge *)challenge
 completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *))completionHandler {
  NSURLCredential *cred = nil;
  NSURLSessionAuthChallengeDisposition d = [[VibeTlsCore shared] dispositionForChallenge:challenge credential:&cred];
  completionHandler(d, cred);
}
@end
