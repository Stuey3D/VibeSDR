// VibeTlsCore — per-server certificate pins (trust on first use), Apple side. GitHub #52.
//
// ★ PURE Foundation + Security, no React Native: scripts/tls/test-tls-pin-apple.sh compiles THIS file on the Mac
//   and runs it against real self-signed servers. The RN glue and the hooks live in VibeTlsTrust.mm.
// ★ The Android twin is android/app/src/main/java/com/vibesdr/app/VibePinnedTls.kt — the same rules, the same key
//   ("host:port", lower-case, no IPv6 brackets) and the same fingerprint (SHA-256 of the leaf's DER, lower-case hex).
#import <Foundation/Foundation.h>
#import <Security/Security.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, VibeTlsVerdict) {
  /// The platform trusts it, exactly as it would without us. Nothing of ours was consulted.
  VibeTlsVerdictSystem = 0,
  /// The platform refused it, and it is the very certificate the user trusted for THIS host:port.
  VibeTlsVerdictPinned = 1,
  /// Refused — as the platform would have.
  VibeTlsVerdictRejected = 2,
};

@interface VibeTlsCore : NSObject

+ (VibeTlsCore *)shared;
/// Tests pass their own defaults suite so the app's pins are never touched.
- (instancetype)initWithDefaults:(NSUserDefaults *)defaults;

/// "host:port" — host lower-cased, IPv6 brackets and a trailing dot removed. src/utils/tlsPin.ts pinKey() is the same.
+ (NSString *)keyForHost:(NSString *)host port:(NSInteger)port;
/// SHA-256 of the certificate's DER, lower-case hex, no separators.
+ (NSString *)sha256OfCertificate:(SecCertificateRef)cert;

- (nullable NSString *)pinForHost:(NSString *)host port:(NSInteger)port;
- (NSDictionary<NSString *, NSDictionary *> *)allPins;
- (void)pinHost:(NSString *)host port:(NSInteger)port sha256:(NSString *)sha256 subject:(nullable NSString *)subject;
- (BOOL)forgetHost:(NSString *)host port:(NSInteger)port;

/// ★★★ ENFORCEMENT. The platform's own evaluation first, with the host name — accepted exactly as before. Only a
/// chain the platform REFUSES is compared with the pin for this host:port, and only an exact leaf match whose dates
/// are valid is let through. No pin, or a different certificate → refused.
- (VibeTlsVerdict)evaluate:(SecTrustRef)trust host:(NSString *)host port:(NSInteger)port;

/// ★★ THE NSURLSession ANSWER (React Native's fetch, and any native URLSession of ours). A server-trust challenge the
/// platform passes → default handling, i.e. untouched; a pinned match → use the trust; anything else → default
/// handling, so the platform refuses it exactly as it always did. Every other kind of challenge → default handling.
- (NSURLSessionAuthChallengeDisposition)dispositionForChallenge:(NSURLAuthenticationChallenge *)challenge
                                                     credential:(NSURLCredential *_Nullable *_Nonnull)credential;

/// ★ INSPECTION, for the prompt only — never used to decide a live connection. Classifies what the platform thinks
/// of this trust: { systemTrusted: BOOL, reason: ok|untrusted|expired|hostname, sha256, der (base64 leaf),
/// pinned: the stored pin or NSNull, pinMatches: BOOL }.
- (NSDictionary *)inspect:(SecTrustRef)trust host:(NSString *)host port:(NSInteger)port;

/// ★ THE PROBE behind inspect(): a TLS handshake to https://host:port/ on a throwaway ephemeral session that is
/// CANCELLED at the certificate — no request is ever sent, whatever the certificate. Completes once, off the main
/// thread, with -inspect:'s dictionary, or { reason: "unreachable", message } when no certificate arrived.
- (void)probeHost:(NSString *)host port:(NSInteger)port timeout:(NSTimeInterval)timeout
       completion:(void (^)(NSDictionary *result))completion;

@end

/// ★ A URLSession delegate that answers server-trust challenges with -dispositionForChallenge: and nothing else — for
/// the native sockets of ours that use URLSession (VibePowerModule's legacy audio transport).
@interface VibeTlsSessionDelegate : NSObject <NSURLSessionDelegate>
@end

NS_ASSUME_NONNULL_END
