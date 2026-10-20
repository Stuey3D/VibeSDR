// VibePinnedSRPolicy — see VibePinnedSRPolicy.h and VibeTlsTrust.mm. GitHub #52.
#import "VibePinnedSRPolicy.h"
#import "VibeTlsCore.h"

@implementation VibePinnedSRPolicy {
  NSString *_host;
  NSInteger _port;
  VibeTlsCore *_core;
}
- (instancetype)initWithHost:(NSString *)host port:(NSInteger)port {
  return [self initWithHost:host port:port core:[VibeTlsCore shared]];
}
- (instancetype)initWithHost:(NSString *)host port:(NSInteger)port core:(VibeTlsCore *)core {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  // ★ "Disabling certificate chain validation is unsafe" — at the STREAM; it moves to evaluateServerTrust below,
  //   which is stricter than "chain only": system trust with the host name, else this host:port's exact pin.
  if ((self = [super initWithCertificateChainValidationEnabled:NO])) { _host = [host copy]; _port = port; _core = core; }
#pragma clang diagnostic pop
  return self;
}
- (BOOL)evaluateServerTrust:(SecTrustRef)serverTrust forDomain:(NSString *)domain {
  // ★ The host the socket was opened for, not `domain` alone — the pin is keyed on host:port.
  VibeTlsVerdict v = [_core evaluate:serverTrust host:_host port:_port];
  if (v == VibeTlsVerdictRejected) NSLog(@"[VibeTls] wss %@:%ld refused (not system-trusted, no matching pin)", _host, (long)_port);
  return v != VibeTlsVerdictRejected;
}
@end
