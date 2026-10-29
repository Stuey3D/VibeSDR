# VibeServer Lite — R8 rules for the release build. ★ Read docs/ANDROID-R8.md first.
#
#   ★ JNI (the native server)  → GENERATED from the main app's C++: build/generated/r8/vibe-jni-keep.pro
#                                (scripts/r8-jni.py), checked against R8's output on every release build.
#   ★ Manifest components      → AGP's aapt rules — automatic.
#   ★ React Native modules     → react-android's consumer rules keep every NativeModule implementation whole.
#   ★ async-storage, slider, safe-area-context, OkHttp, AndroidX → their own consumer rules.

# Line numbers survive, so a trace in Diagnostics retraces to the line with that build's mapping.txt.
-keepattributes SourceFile,LineNumberTable
