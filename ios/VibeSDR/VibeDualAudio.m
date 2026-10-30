#import <React/RCTBridgeModule.h>

// ★ V12 two servers at once — the second voice (VibeDualAudio.swift). Must match its @objc signatures EXACTLY: a
//   mismatched selector only fails at runtime.
@interface RCT_EXTERN_MODULE (VibeDualAudio, NSObject)

RCT_EXTERN_METHOD(setNativeMix : (nonnull NSNumber *)pos gain : (nonnull NSNumber *)gain active : (BOOL)active)

RCT_EXTERN_METHOD(setAuxMix : (nonnull NSNumber *)pos gain : (nonnull NSNumber *)gain)
RCT_EXTERN_METHOD(setNativeWidth : (nonnull NSNumber *)w)
RCT_EXTERN_METHOD(setAuxWidth : (nonnull NSNumber *)w)

RCT_EXTERN_METHOD(startAux)

RCT_EXTERN_METHOD(stopAux)

RCT_EXTERN_METHOD(pushAuxOpus : (NSString *)b64 rate : (nonnull NSNumber *)rate channels : (nonnull NSNumber *)channels)

RCT_EXTERN_METHOD(pushAuxPcm : (NSString *)b64 rate : (nonnull NSNumber *)rate channels : (nonnull NSNumber *)channels)

RCT_EXTERN_METHOD(getLevels
                  : (RCTPromiseResolveBlock)resolve rejecter
                  : (RCTPromiseRejectBlock)reject)

@end
