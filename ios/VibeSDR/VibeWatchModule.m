#import <React/RCTBridgeModule.h>
#import <React/RCTEventEmitter.h>

@interface RCT_EXTERN_MODULE (VibeWatchModule, RCTEventEmitter)

RCT_EXTERN_METHOD(isReachable
                  : (RCTPromiseResolveBlock)resolve rejecter
                  : (RCTPromiseRejectBlock)reject)

RCT_EXTERN_METHOD(sendRow
                  : (NSString *)rowB64 freq
                  : (nonnull NSNumber *)freq span
                  : (nonnull NSNumber *)span snr
                  : (nonnull NSNumber *)snr level
                  : (nonnull NSNumber *)level lo
                  : (nonnull NSNumber *)lo hi
                  : (nonnull NSNumber *)hi meter
                  : (NSString *)meter sql
                  : (nonnull NSNumber *)sql)

RCT_EXTERN_METHOD(startWatchSpectrum
                  : (NSString *)url binBandwidth
                  : (nonnull NSNumber *)binBandwidth tuneHz
                  : (nonnull NSNumber *)tuneHz filterLow
                  : (nonnull NSNumber *)filterLow filterHigh
                  : (nonnull NSNumber *)filterHigh brightness
                  : (nonnull NSNumber *)brightness contrast
                  : (nonnull NSNumber *)contrast convOffsetHz
                  : (nonnull NSNumber *)convOffsetHz)

RCT_EXTERN_METHOD(retuneWatchSpectrum : (nonnull NSNumber *)tuneHz)

RCT_EXTERN_METHOD(stopWatchSpectrum)

RCT_EXTERN_METHOD(sendFmdx : (NSString *)json)

RCT_EXTERN_METHOD(sendDial : (NSString *)json)

RCT_EXTERN_METHOD(sendLogo : (NSString *)b64)

RCT_EXTERN_METHOD(sendStations : (NSString *)json)

RCT_EXTERN_METHOD(sendDab : (NSString *)json)

RCT_EXTERN_METHOD(sendFavourites : (NSString *)json)

RCT_EXTERN_METHOD(sendDirectory : (NSString *)json)
// ★★★ AND DECLARED HERE TOO. A Swift @objc method the bridge header does not list is not a
//     missing method — it is a SILENT one: the JS side calls it, gets undefined, and nothing
//     anywhere says so. This file has cost us that twice already.
RCT_EXTERN_METHOD(sendRadios : (NSString *)json)

/* ★★★ THE THIRD TIME A SWIFT @objc METHOD WAS NEVER LISTED HERE, AND THE FIRST TIME IT CRASHED
 *   THE APP. VibeWatchModule.swift has declared sendReceiver(_:lon:) all along; without this line
 *   it simply IS NOT THERE as far as JS is concerned, so `Native!.sendReceiver(lat, lon)` in
 *   watchProvider is a call on `undefined`.
 * ★★ It threw inside a useEffect on SDRScreen, so React unmounted the whole screen and crashGuard
 *   reset to the picker — which looked exactly like "the VibeServer connect silently failed", and
 *   left the phone showing a 13-view tree where a healthy one is 565. That is the black screen,
 *   the two-attempt connect and the "VibeServer will not connect" report: ONE missing line.
 * ★ Only VibeServers hit it because only they arrive with a receiver location to send. */
RCT_EXTERN_METHOD(sendReceiver : (nonnull NSNumber *)lat lon : (nonnull NSNumber *)lon)
RCT_EXTERN_METHOD(sendPhone : (NSString *)status)
RCT_EXTERN_METHOD(sendSession : (nonnull NSNumber *)secsLeft limitMin : (nonnull NSNumber *)limitMin)
RCT_EXTERN_METHOD(sendPinRequest : (NSString *)name)

RCT_EXTERN_METHOD(isClosedByUser
                  : (RCTPromiseResolveBlock)resolve rejecter
                  : (RCTPromiseRejectBlock)reject)

RCT_EXTERN_METHOD(clearClosedByUser)

RCT_EXTERN_METHOD(sendVolume : (nonnull NSNumber *)vol muted : (BOOL)muted)

RCT_EXTERN_METHOD(sendAircraft : (NSString *)json)

RCT_EXTERN_METHOD(sendState
                  : (nonnull NSNumber *)freq mode
                  : (NSString *)mode step
                  : (nonnull NSNumber *)step meter
                  : (NSString *)meter level
                  : (nonnull NSNumber *)level why
                  : (NSString *)why link
                  : (nonnull NSNumber *)link band
                  : (NSString *)band bandCol
                  : (NSString *)bandCol bandLo
                  : (nonnull NSNumber *)bandLo bandHi
                  : (nonnull NSNumber *)bandHi)

RCT_EXTERN_METHOD(sendSettings
                  : (NSString *)lutB64 smoothing
                  : (nonnull NSNumber *)smoothing needle
                  : (NSString *)needle needleIntensity
                  : (nonnull NSNumber *)needleIntensity sharpness
                  : (nonnull NSNumber *)sharpness peakHold
                  : (BOOL)peakHold)

@end
