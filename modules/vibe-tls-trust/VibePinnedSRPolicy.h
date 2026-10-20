// VibePinnedSRPolicy — the WebSocket half of per-server certificate pins (GitHub #52). See VibeTlsTrust.mm.
// ★ Its own file (SocketRocket + VibeTlsCore only, no React) so scripts/test-tls-pin-apple.sh can run the REAL class
//   against a self-signed server on the Mac, through the macOS slice of the same SocketRocket the app links.
#import <SocketRocket/SRSecurityPolicy.h>
@class VibeTlsCore;

NS_ASSUME_NONNULL_BEGIN

/// ★ SocketRocket's documented extension point (SRSecurityPolicy.h: "Use a subclass … for more fine grained
///   customization", the pattern of its own SRPinningSecurityPolicy). The stream's own chain check is switched off
///   so THIS evaluation decides — and it starts with the system's verdict for the host name, so a publicly trusted
///   certificate is accepted exactly as the default policy would. SocketRocket asks before it writes the handshake
///   and fails the socket ("Invalid server certificate") on NO.
@interface VibePinnedSRPolicy : SRSecurityPolicy
- (instancetype)initWithHost:(NSString *)host port:(NSInteger)port;
/// Tests hand in their own pin store; the app uses +[VibeTlsCore shared].
- (instancetype)initWithHost:(NSString *)host port:(NSInteger)port core:(VibeTlsCore *)core;
@end

NS_ASSUME_NONNULL_END
