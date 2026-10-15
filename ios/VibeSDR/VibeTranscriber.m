#import <React/RCTBridgeModule.h>
#import <React/RCTEventEmitter.h>

@interface RCT_EXTERN_MODULE (VibeTranscriber, RCTEventEmitter)

RCT_EXTERN_METHOD(capabilities
                  : (RCTPromiseResolveBlock)resolve rejecter
                  : (RCTPromiseRejectBlock)reject)

RCT_EXTERN_METHOD(start : (NSString *)language translateTo : (NSString *)translateTo)

RCT_EXTERN_METHOD(stop)

RCT_EXTERN_METHOD(download
                  : (NSString *)language translateTo
                  : (NSString *)translateTo resolver
                  : (RCTPromiseResolveBlock)resolve rejecter
                  : (RCTPromiseRejectBlock)reject)

RCT_EXTERN_METHOD(remove
                  : (NSString *)language resolver
                  : (RCTPromiseResolveBlock)resolve rejecter
                  : (RCTPromiseRejectBlock)reject)

@end
