//
// Use this file to import your target's public headers that you would like to expose to Swift.
//
#import "opus/include/opus/opus.h"
#import <Accelerate/Accelerate.h>
#import <React/RCTBridgeModule.h>
#import <React/RCTEventEmitter.h>
#import "VibeSsbTune.h"   // V12 TRANSCRIBE: the shared recogniser feed (vibedsp/ssb_offset), C API
#import "VibeBinaural.h"  // V12 BINAURAL TUNING: the shared spatialiser + offset sources (vibedsp/binaural), C API
#import <VibeTlsTrust/VibeTlsCore.h>   // per-server certificate pins (GitHub #52)
