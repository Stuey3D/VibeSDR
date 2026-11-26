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
//
// ★★★ AND THE WINDOW'S RESERVED REGIONS (iPhone Duo, Stuart 2026-11-25). On the Duo's folded front screen the camera
//  and the system's clock / Wi-Fi sit in a cluster at the top right that safe-area insets do NOT report (insets.top is
//  0: the app is meant to draw under it), and open, the hinge runs down the middle. iOS 27.1 says where both are:
//  UIView -reservedRegionsOfKind: — `occlusionRegionKind` (something drawn over the app: the camera + status icons,
//  frame INCLUDING the margins interactive content should keep) and `divisionRegionKind` (where an element should
//  divide in two: the crease). Read on the key window, so the frames are in React Native's window coordinates, and
//  sent with every geometry change (the same KVO as the orientation: a fold or unfold changes the scene's geometry).
//  ★ Compiled only against an SDK that has the API (__IPHONE_27_1), and called only on iOS 27.1+, so an older Xcode
//    still builds this file and an older iOS reports no regions — JS then keeps the plain safe-area behaviour.
#import <React/RCTBridgeModule.h>
#import <React/RCTEventEmitter.h>
#import <UIKit/UIKit.h>
#import <math.h>

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
  NSDictionary *_lastRegions;
  id _hingeInteraction;          // UIHingeInteraction (iOS 27.1+), on the key window
  __weak UIView *_hingeView;
  NSDictionary *_hinge;          // { status, angleDeg } or nil = no hinge in this window's hierarchy
}

RCT_EXPORT_MODULE();

+ (BOOL)requiresMainQueueSetup { return NO; }
- (dispatch_queue_t)methodQueue { return dispatch_get_main_queue(); }   // UIKit reads + KVO on the main thread

- (NSArray<NSString *> *)supportedEvents { return @[@"interfaceOrientation", @"reservedRegions"]; }

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

- (UIWindow *)keyWindowOf:(UIWindowScene *)scene {
  if (!scene) return nil;
  for (UIWindow *w in scene.windows) if (w.isKeyWindow) return w;
  return scene.windows.firstObject;
}

/** { occlusion: [{x,y,w,h,mt,mr,mb,ml}], division: [...] } in the key window's coordinates; both empty when unknown. */
- (NSDictionary *)regions {
  NSMutableArray *occ = [NSMutableArray array], *div = [NSMutableArray array];
#if defined(__IPHONE_27_1) && __IPHONE_OS_VERSION_MAX_ALLOWED >= __IPHONE_27_1
  if (@available(iOS 27.1, *)) {
    UIWindow *win = [self keyWindowOf:[self activeScene]];
    if (win) {
      NSArray *kinds = @[UIViewReservedRegionKind.occlusionRegionKind, UIViewReservedRegionKind.divisionRegionKind];
      NSArray *outs = @[occ, div];
      for (NSUInteger i = 0; i < kinds.count; i++) {
        for (UIViewReservedRegion *r in [win reservedRegionsOfKind:kinds[i]]) {
          if (!r.isActive) continue;
          CGRect f = r.frame;
          if (CGRectIsEmpty(f) || CGRectIsNull(f) || CGRectIsInfinite(f)) continue;
          UIEdgeInsets m = r.margins;
          [outs[i] addObject:@{ @"x": @(f.origin.x), @"y": @(f.origin.y), @"w": @(f.size.width), @"h": @(f.size.height),
                                @"mt": @(m.top), @"mr": @(m.right), @"mb": @(m.bottom), @"ml": @(m.left) }];
        }
      }
    }
  }
#endif
  /* ★ And the status bar's own frame (UIStatusBarManager): on the Duo it is the clock + Wi-Fi pill, which moves with the
   *  orientation and is not always a reserved region (open, it is the system's — Stuart: "the main thing in fully open
   *  mode is avoiding the status pill in all orientations"). Zero-sized when hidden (iPhone landscape). */
  NSMutableArray *sb = [NSMutableArray array];
  UIWindowScene *scene = [self activeScene];
  UIWindow *win = [self keyWindowOf:scene];
  CGRect f = scene.statusBarManager.statusBarHidden ? CGRectZero : scene.statusBarManager.statusBarFrame;
  if (win && !CGRectIsEmpty(f)) {
    f = [win convertRect:f fromCoordinateSpace:scene.coordinateSpace];
    [sb addObject:@{ @"x": @(f.origin.x), @"y": @(f.origin.y), @"w": @(f.size.width), @"h": @(f.size.height) }];
  }
  return @{ @"occlusion": occ, @"division": div, @"statusBar": sb, @"hinge": _hinge ?: [NSNull null] };
}

/* ★★ THE HINGE (UIHingeInteraction, iOS 27.1). Apple's posture API: present = this window is on a folding display;
 *  status closed / partiallyOpen / fullyOpen; angle in radians (sent rounded to 5°, so the stream is a few events per
 *  fold, not one per frame). Installed on the key window, and moved if the key window changes. */
- (void)attachHinge {
#if defined(__IPHONE_27_1) && __IPHONE_OS_VERSION_MAX_ALLOWED >= __IPHONE_27_1
  if (@available(iOS 27.1, *)) {
    UIWindow *win = [self keyWindowOf:[self activeScene]];
    if (!win || win == _hingeView) return;
    if (_hingeInteraction && _hingeView) [_hingeView removeInteraction:_hingeInteraction];
    __weak __typeof(self) weakSelf = self;
    UIHingeInteraction *hi = [[UIHingeInteraction alloc] initWithUpdateHandler:^(UIHingeInteraction *i, UIHingeInteractionUpdate *u) {
      __strong __typeof(weakSelf) me = weakSelf;
      if (!me) return;
      UIHinge *h = u.hinge;
      NSDictionary *next = nil;
      if (h) {
        NSString *st = @"unknown";
        switch (h.status) {
          case UIHingeStatusClosed:        st = @"closed"; break;
          case UIHingeStatusPartiallyOpen: st = @"partiallyOpen"; break;
          case UIHingeStatusFullyOpen:     st = @"fullyOpen"; break;
          default: break;
        }
        double deg = round(h.angle * 180.0 / M_PI / 5.0) * 5.0;
        next = @{ @"status": st, @"angleDeg": @(deg) };
      }
      me->_hinge = next;
      [me emitIfChanged];
    }];
    [win addInteraction:hi];
    _hingeInteraction = hi;
    _hingeView = win;
  }
#endif
}

- (void)detachHinge {
  if (_hingeInteraction && _hingeView) [_hingeView removeInteraction:_hingeInteraction];
  _hingeInteraction = nil;
  _hingeView = nil;
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
  if (![now isEqualToString:_last]) {
    _last = now;
    [self sendEventWithName:@"interfaceOrientation" body:@{ @"orientation": now }];
  }
  NSDictionary *regions = [self regions];
  if (![regions isEqualToDictionary:_lastRegions]) {
    _lastRegions = regions;
    [self sendEventWithName:@"reservedRegions" body:regions];
  }
}

/** A geometry change lands before the window has laid out at its new size: ask again once it has. */
- (void)emitSoonAndSettled {
  [self emitIfChanged];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.35 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
    [self emitIfChanged];
  });
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change
                       context:(void *)context {
  if (context != kVibeGeometryCtx) { [super observeValueForKeyPath:keyPath ofObject:object change:change context:context]; return; }
  dispatch_async(dispatch_get_main_queue(), ^{ [self emitSoonAndSettled]; });
}

- (void)sceneActivated:(NSNotification *)note {
  [self attachScenes];
  [self attachHinge];
  [self emitSoonAndSettled];
}

- (void)startObserving {
  _hasListeners = YES;
  _last = nil;
  _lastRegions = nil;
  dispatch_async(dispatch_get_main_queue(), ^{
    [self attachScenes];
    [self attachHinge];
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
    [self detachHinge];
  });
}

- (void)invalidate {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [self detachScenes];
  dispatch_async(dispatch_get_main_queue(), ^{ [self detachHinge]; });
  [super invalidate];
}

RCT_EXPORT_METHOD(get:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject) {
  resolve([self current]);
}

RCT_EXPORT_METHOD(getReservedRegions:(RCTPromiseResolveBlock)resolve rejecter:(RCTPromiseRejectBlock)reject) {
  [self attachHinge];
  resolve([self regions]);
}

@end
