// VibeInterfaceOrientation — the window scene's INTERFACE orientation, for JS as
// `NativeModules.VibeInterfaceOrientation` (src/hooks/useIslandSide.ts).
//
// ★★★ WHY (Stuart, 2026-10-06, RC16 iPhone landscape: "the health and timer pills are huge when collapsed, bigger
//  than when they are open"). iOS reports the landscape safe-area inset on BOTH sides, whichever side the Dynamic
//  Island / notch is really on, so the edge chips padded ~59 pt of empty frame on the side WITHOUT one. Nothing in
//  React Native says which way round the phone is; UIWindowScene's interface orientation does.
//
//  ★ Apple, UIInterfaceOrientation: landscapeRight = "the Home button on the right" ⇒ the top of the device (the
//    island / notch) is on the LEFT; landscapeLeft ⇒ it is on the RIGHT. (UIDeviceOrientation is the mirror image —
//    deliberately not used: it is the device, not the interface, and a rotation-locked phone disagrees with it.)
//  ★ Observed by KVO on UIWindowScene.effectiveGeometry (iOS 16+, documented KVO-compliant and "the recommended way
//    to receive notifications of changes to the window scene's geometry"). A 180° flip between the two landscapes
//    changes neither the window size nor the (symmetric) insets, so JS cannot see it any other way.
//  ★ Lives in the VibeICloudKVS pod only to avoid a new pod (no Podfile / Podfile.lock change). The pod globs
//    *.{mm,h}, so `pod install` (Xcode Cloud's ci_post_clone runs it) picks this file up. Android has no counterpart:
//    it reports asymmetric cutout insets already.
#import <React/RCTBridgeModule.h>
#import <React/RCTEventEmitter.h>
#import <UIKit/UIKit.h>

static NSString *VibeOrientationName(UIInterfaceOrientation o) {
  switch (o) {
    case UIInterfaceOrientationPortrait:           return @"portrait";
    case UIInterfaceOrientationPortraitUpsideDown: return @"portraitUpsideDown";
    case UIInterfaceOrientationLandscapeLeft:      return @"landscapeLeft";
    case UIInterfaceOrientationLandscapeRight:     return @"landscapeRight";
    default:                                       return @"unknown";
  }
}

static void *kVibeGeometryCtx = &kVibeGeometryCtx;

@interface VibeInterfaceOrientation : RCTEventEmitter <RCTBridgeModule>
@end

@implementation VibeInterfaceOrientation {
  BOOL _hasListeners;
  NSHashTable<UIWindowScene *> *_observed;   // weak: a scene that goes away takes its observation with it
  NSString *_last;
}

RCT_EXPORT_MODULE();

+ (BOOL)requiresMainQueueSetup { return NO; }
- (dispatch_queue_t)methodQueue { return dispatch_get_main_queue(); }   // UIKit reads + KVO on the main thread

- (NSArray<NSString *> *)supportedEvents { return @[@"interfaceOrientation"]; }

- (UIWindowScene *)activeScene {
  UIWindowScene *any = nil;
  for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
    if (![s isKindOfClass:UIWindowScene.class]) continue;
    if (s.activationState == UISceneActivationStateForegroundActive) return (UIWindowScene *)s;
    if (!any) any = (UIWindowScene *)s;
  }
  return any;
}

- (NSString *)current {
  UIWindowScene *scene = [self activeScene];
  return scene ? VibeOrientationName(scene.effectiveGeometry.interfaceOrientation) : @"unknown";
}

- (void)attachScenes {
  if (!_observed) _observed = [NSHashTable weakObjectsHashTable];
  for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
    if (![s isKindOfClass:UIWindowScene.class] || [_observed containsObject:(UIWindowScene *)s]) continue;
    [s addObserver:self forKeyPath:@"effectiveGeometry" options:NSKeyValueObservingOptionNew context:kVibeGeometryCtx];
    [_observed addObject:(UIWindowScene *)s];
  }
}

- (void)detachScenes {
  for (UIWindowScene *s in _observed.allObjects) {
    @try { [s removeObserver:self forKeyPath:@"effectiveGeometry" context:kVibeGeometryCtx]; } @catch (__unused id e) {}
  }
  [_observed removeAllObjects];
}

- (void)emitIfChanged {
  if (!_hasListeners) return;
  NSString *now = [self current];
  if ([now isEqualToString:_last]) return;
  _last = now;
  [self sendEventWithName:@"interfaceOrientation" body:@{ @"orientation": now }];
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change
                       context:(void *)context {
  if (context != kVibeGeometryCtx) { [super observeValueForKeyPath:keyPath ofObject:object change:change context:context]; return; }
  dispatch_async(dispatch_get_main_queue(), ^{ [self emitIfChanged]; });
}

- (void)sceneActivated:(NSNotification *)note {
  [self attachScenes];
  [self emitIfChanged];
}

- (void)startObserving {
  _hasListeners = YES;
  _last = nil;
  dispatch_async(dispatch_get_main_queue(), ^{
    [self attachScenes];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(sceneActivated:)
                                                 name:UISceneDidActivateNotification object:nil];
    [self emitIfChanged];   // the first value, so a listener never waits for a rotation to learn the side
  });
}

- (void)stopObserving {
  _hasListeners = NO;
  dispatch_async(dispatch_get_main_queue(), ^{
    [[NSNotificationCenter defaultCenter] removeObserver:self name:UISceneDidActivateNotification object:nil];
    [self detachScenes];
  });
}

- (void)invalidate {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [self detachScenes];
  [super invalidate];
}

RCT_EXPORT_METHOD(get:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject) {
  resolve([self current]);
}

@end
