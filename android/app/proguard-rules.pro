# VibeSDR — R8 rules for the release build. ★ Read docs/ANDROID-R8.md before changing anything here.
#
# R8 is ON for release (gradle.properties: android.enableMinifyInReleaseBuilds). It shrinks and RENAMES everything
# no rule keeps, so anything reached BY NAME — from C++, from a manifest, from reflection, from JavaScript — must be
# kept here or by a library's own consumer rules.
#
# What keeps what:
#   ★ JNI (the native server)  → GENERATED from the C++: build/generated/r8/vibe-jni-keep.pro (scripts/r8-jni.py),
#                                checked against R8's output on every release build. Do not hand-write JNI rules here.
#   ★ Manifest components      → AGP's aapt rules (activities, services, receivers, Application) — automatic.
#   ★ React Native modules     → react-android's consumer rules keep every NativeModule implementation whole,
#                                @ReactProp setters, @DoNotStrip and every `native` method.
#   ★ Expo modules, OkHttp, ML Kit, AndroidX, Media, Car App → their own consumer rules.

# ── Crash traces stay readable ───────────────────────────────────────────────────────────────────────────────
# ★ Line numbers survive, so a trace (Play's vitals, Diagnostics → VibeJavaCrash/VibeExitInfo) retraces to the line
#   with that build's mapping.txt. Class and method names in a trace are R8's short names until retraced.
-keepattributes SourceFile,LineNumberTable

# ── Found by name in our own Kotlin ──────────────────────────────────────────────────────────────────────────
# RtlTcpServerService opens the app with Class.forName("com.vibesdr.app.VibeActivity"). The manifest already keeps
# it; this says so where somebody will look.
-keep class com.vibesdr.app.VibeActivity

# ── Libraries that ship without consumer rules ───────────────────────────────────────────────────────────────
# react-native-reanimated
-keep class com.swmansion.reanimated.** { *; }
-keep class com.facebook.react.turbomodule.** { *; }
