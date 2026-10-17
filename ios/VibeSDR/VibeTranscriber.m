#import <React/RCTBridgeModule.h>
#import <React/RCTEventEmitter.h>

// ★ Must match VibeTranscriber.swift's @objc signatures EXACTLY — a mismatched selector only fails at runtime.
@interface RCT_EXTERN_MODULE (VibeTranscriber, RCTEventEmitter)

RCT_EXTERN_METHOD(capabilities
                  : (RCTPromiseResolveBlock)resolve rejecter
                  : (RCTPromiseRejectBlock)reject)

RCT_EXTERN_METHOD(start : (NSString *)language translateTo : (NSString *)translateTo)

RCT_EXTERN_METHOD(tuning : (NSString *)mode freqHz : (double)freqHz)

RCT_EXTERN_METHOD(stop)

RCT_EXTERN_METHOD(download
                  : (NSString *)language translateTo
                  : (NSString *)translateTo keep
                  : (BOOL)keep resolver
                  : (RCTPromiseResolveBlock)resolve rejecter
                  : (RCTPromiseRejectBlock)reject)

RCT_EXTERN_METHOD(keep
                  : (NSArray<NSString *> *)languages resolver
                  : (RCTPromiseResolveBlock)resolve rejecter
                  : (RCTPromiseRejectBlock)reject)

@end
