// VibeICloudKVS — iOS native module exposing NSUbiquitousKeyValueStore to JS as
// `NativeModules.VibeICloudKVS`. This is deliberately a DUMB key/value pipe: it
// stores and returns strings, and knows nothing about favourites, bookmarks or
// dials. All merging lives in src/services/cloudSync.ts, so there is one place
// that knows how to union a list and one place that knows how to talk to iCloud.
//
// Android has no counterpart — the JS wrapper treats a missing module as
// "iCloud unavailable" and every sync call becomes a no-op.
#import <React/RCTBridgeModule.h>
#import <React/RCTEventEmitter.h>
#import <Foundation/Foundation.h>

// Apple's documented ceilings. We check against them BEFORE writing, because a
// KVS write that exceeds them fails SILENTLY — setObject: returns void and the
// value simply never leaves the device. The brief is explicit that a sync which
// quietly stops is worse than one that never started, so an over-size write is
// rejected loudly (promise rejects) instead.
static const NSUInteger kMaxValueBytes = 1000 * 1024;   // 1 MB per store, so also the per-value cap
static const NSUInteger kMaxKeys       = 1024;

@interface VibeICloudKVS : RCTEventEmitter <RCTBridgeModule>
@end

@implementation VibeICloudKVS {
  BOOL _hasListeners;
}

RCT_EXPORT_MODULE();

+ (BOOL)requiresMainQueueSetup { return NO; }

- (NSArray<NSString *> *)supportedEvents { return @[@"kvsChanged"]; }

- (void)startObserving {
  _hasListeners = YES;
  [[NSNotificationCenter defaultCenter]
      addObserver:self
         selector:@selector(storeChanged:)
             name:NSUbiquitousKeyValueStoreDidChangeExternallyNotification
           object:[NSUbiquitousKeyValueStore defaultStore]];
  // Pull whatever arrived while we were not listening.
  [[NSUbiquitousKeyValueStore defaultStore] synchronize];
}

- (void)stopObserving {
  _hasListeners = NO;
  [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
}

// Fires when ANOTHER device (or Jr on the same account) changed a key. The
// reason matters: a QuotaViolationChange means our own writes are being
// rejected, which JS must surface rather than swallow.
- (void)storeChanged:(NSNotification *)note {
  if (!_hasListeners) return;
  NSNumber *reason = note.userInfo[NSUbiquitousKeyValueStoreChangeReasonKey];
  NSArray  *keys   = note.userInfo[NSUbiquitousKeyValueStoreChangedKeysKey] ?: @[];
  [self sendEventWithName:@"kvsChanged" body:@{
    @"keys":   keys,
    @"reason": reason ?: @(-1),
    @"quotaExceeded": @(reason != nil && [reason integerValue] == NSUbiquitousKeyValueStoreQuotaViolationChange),
  }];
}

// Is iCloud actually usable? ubiquityIdentityToken is nil when the user is
// signed out of iCloud entirely — the store still accepts writes in that state
// but they never leave the device, which would look exactly like a working sync.
RCT_EXPORT_METHOD(isAvailable:(RCTPromiseResolveBlock)resolve
                     rejecter:(RCTPromiseRejectBlock)reject) {
  resolve(@([NSFileManager defaultManager].ubiquityIdentityToken != nil));
}

RCT_EXPORT_METHOD(getItem:(NSString *)key
                 resolver:(RCTPromiseResolveBlock)resolve
                 rejecter:(RCTPromiseRejectBlock)reject) {
  NSString *v = [[NSUbiquitousKeyValueStore defaultStore] stringForKey:key];
  resolve(v ?: (id)kCFNull);
}

RCT_EXPORT_METHOD(multiGet:(NSArray<NSString *> *)keys
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject) {
  NSUbiquitousKeyValueStore *store = [NSUbiquitousKeyValueStore defaultStore];
  NSMutableDictionary *out = [NSMutableDictionary dictionaryWithCapacity:keys.count];
  for (NSString *k in keys) {
    NSString *v = [store stringForKey:k];
    if (v) out[k] = v;
  }
  resolve(out);
}

RCT_EXPORT_METHOD(setItem:(NSString *)key
                    value:(NSString *)value
                 resolver:(RCTPromiseResolveBlock)resolve
                 rejecter:(RCTPromiseRejectBlock)reject) {
  NSUbiquitousKeyValueStore *store = [NSUbiquitousKeyValueStore defaultStore];
  NSUInteger bytes = [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
  if (bytes > kMaxValueBytes) {
    reject(@"kvs_too_large",
           [NSString stringWithFormat:@"value for %@ is %lu bytes (KVS limit %lu)",
                                      key, (unsigned long)bytes, (unsigned long)kMaxValueBytes],
           nil);
    return;
  }
  NSDictionary *all = store.dictionaryRepresentation;
  if (all[key] == nil && all.count >= kMaxKeys) {
    reject(@"kvs_key_limit",
           [NSString stringWithFormat:@"iCloud key-value store is full (%lu keys)",
                                      (unsigned long)all.count],
           nil);
    return;
  }
  [store setString:value forKey:key];
  resolve(@([store synchronize]));
}

RCT_EXPORT_METHOD(removeItem:(NSString *)key
                    resolver:(RCTPromiseResolveBlock)resolve
                    rejecter:(RCTPromiseRejectBlock)reject) {
  NSUbiquitousKeyValueStore *store = [NSUbiquitousKeyValueStore defaultStore];
  [store removeObjectForKey:key];
  resolve(@([store synchronize]));
}

// Per-server keys (last tune, dials, display prefs) are discovered rather than
// enumerated from a manifest — the set of servers differs on every device, and a
// manifest would be one more thing to merge.
RCT_EXPORT_METHOD(getAllKeys:(RCTPromiseResolveBlock)resolve
                    rejecter:(RCTPromiseRejectBlock)reject) {
  resolve([NSUbiquitousKeyValueStore defaultStore].dictionaryRepresentation.allKeys);
}

// Bytes currently used, so JS can warn before the store is full rather than
// after a write has already been refused.
RCT_EXPORT_METHOD(usage:(RCTPromiseResolveBlock)resolve
                rejecter:(RCTPromiseRejectBlock)reject) {
  NSDictionary *all = [NSUbiquitousKeyValueStore defaultStore].dictionaryRepresentation;
  NSUInteger bytes = 0;
  for (NSString *k in all) {
    bytes += [k lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    id v = all[k];
    if ([v isKindOfClass:[NSString class]]) {
      bytes += [(NSString *)v lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    }
  }
  resolve(@{ @"bytes": @(bytes), @"keys": @(all.count),
             @"maxBytes": @(kMaxValueBytes), @"maxKeys": @(kMaxKeys) });
}

RCT_EXPORT_METHOD(synchronize:(RCTPromiseResolveBlock)resolve
                     rejecter:(RCTPromiseRejectBlock)reject) {
  resolve(@([[NSUbiquitousKeyValueStore defaultStore] synchronize]));
}

@end
