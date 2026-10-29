# R8 and edge-to-edge on Android (V12)

Two Play Console notices on 11.0.13, both "for your next release":

1. **DEX code optimization is below our threshold — Obfuscation (1 %)**, fix by Feb 2027. Play reads the
   figures from the `r8.json` R8 writes into every AAB (`BUNDLE-METADATA/com.android.tools/r8.json`, `stats`):
   obfuscation, optimisation and shrinking, each the share of code R8 was *allowed* to touch. Under 25 % in
   any one of them may affect visibility. With R8 off, all three were close to zero.
2. **Your app uses deprecated APIs or parameters for edge-to-edge** (Android 15).

## 1. R8 is on for release builds

| App | Where it is switched on | Rules |
|---|---|---|
| Play app + GitHub APK (`net.vibesdr.app`, `:app:bundleCurrentRelease` / `:app:assembleCurrentRelease`) and the legacy APK | `android/gradle.properties`: `android.enableMinifyInReleaseBuilds=true`, `android.enableShrinkResourcesInReleaseBuilds=true` | `android/app/proguard-rules.pro` + generated JNI rules |
| VibeServer Lite (`com.vibesdr.serverlite`, `android/gradlew -p lite/android :app:assembleRelease`) | `lite/android/app/build.gradle`, `buildTypes.release` | `lite/android/app/proguard-rules.pro` + generated JNI rules |

Both use `proguard-android-optimize.txt` — the plain `proguard-android.txt` carries `-dontoptimize`, which
leaves Play's *optimisation* figure at 0 %. Debug builds are untouched.

### What is kept, and by what

| Reached by name from | Kept by |
|---|---|
| **C++ (JNI)** — the `Java_com_vibesdr_app_*` exports, and C++ calling back into Kotlin with `FindClass` + `GetStaticMethodID` (`VibeHttp.get`, `VibeMapGL.download`, `VibeMapGL.fetchIf`) | **Generated from the C++ on every release build**: `scripts/r8-jni.py keep` → `build/generated/r8/vibe-jni-keep.pro` (task `generateJniKeepRules`, run from `preBuild`). Nobody writes JNI rules by hand. |
| The manifest (Application, activities, services, receivers) | AGP's own aapt rules |
| JavaScript (React Native modules, `@ReactMethod`, `@ReactProp`, `@DoNotStrip`, every `native` method) | `react-android`'s consumer rules (`-keep class * implements NativeModule { *; }` and friends) |
| Expo modules, OkHttp/Okio, ML Kit, AndroidX, Media, Car App, the three Lite libraries | their own consumer rules |
| `Class.forName("com.vibesdr.app.VibeActivity")` in `RtlTcpServerService` | the manifest, and stated again in `proguard-rules.pro` |
| Drawables found with `getIdentifier()` (lock-screen art: `artwork_*`, `logo_*`) | `android/app/src/main/res/raw/vibe_keep.xml` (resource shrinking) |
| Images the JS bundle uses | the `raw/keep.xml` React Native's bundle task generates |

`-keepattributes SourceFile,LineNumberTable` stays on, so a stack trace retraces to the line.

### The JNI check — a native change cannot silently break R8

After R8 runs (`minify*ReleaseWithR8`), the build runs `scripts/r8-jni.py check` against R8's **output dex**
and **fails before anything is packaged** if a class or method the C++ reaches by name is missing, renamed, or has
a different signature from the one the C++ asks `GetStaticMethodID` for. A passing build prints:

    r8-jni: 151 native/by-name member(s) present in 7 class(es)
    r8-jni: OK

(Lite prints the classes it does not compile, e.g. `VibeSpectrum`, `VibeBinaural` — listed and skipped.)

Run it by hand on any APK or AAB:

    python3 -I scripts/r8-jni.py list  --cpp android/app/src/main/cpp            # what the C++ references
    python3 -I scripts/r8-jni.py check --cpp android/app/src/main/cpp \
        --src android/app/src/main/java  path/to/app.apk                        # or .aab, .dex, a dex dir
    scripts/test-r8-jni.sh [app.apk]                                             # self-test: must pass AND must fail

★ How it finds the class for a `GetMethodID(cls, …)`: the nearest preceding `FindClass("…")` or enclosing
`Java_…` function. If C++ ever looks a class up another way, the check reports a *missing* method — a loud false
alarm, never a silent pass. Teach `scan_cpp()` the new shape.

★ OpenRSP's own `Java_org_openrsp…` exports (vendor/openrsp) have no Java side in our apps; they are not ours to keep.

### mapping.txt — keep it, Play needs it

* **AAB:** R8's mapping travels *inside* the bundle (`BUNDLE-METADATA/com.android.tools.build.obfuscation/proguard.map`),
  so Play deobfuscates crashes for that upload with nothing extra to do.
* **Every build:** `android/app/build/outputs/mapping/<variant>/mapping.txt`, Lite's
  `lite/android/app/build/outputs/mapping/release/mapping.txt`. Archive it beside each GitHub APK / Lite APK: a
  Diagnostics crash note from a user (VibeJavaCrash, VibeExitInfo) names R8's short names until it is retraced with
  `retrace` from the SDK's cmdline-tools and **that build's** mapping.

### Lite's API check still works

`scripts/check-lite-api.py` (runs in `signLiteRelease`) looks for classes extending React's bridge module bases;
those names are kept by `react-android`'s rules, so it still sees the modules — and it fails if it sees none.

### Lite's JS dependencies (fixed on this branch)

`lite/app/package.json` on v12-experimental said `react-native 0.87.1` — a Dependabot cherry-pick ("Bump sprintf-js")
had rewritten it, while Lite's Gradle consumes React Native **0.73.11** AARs (its API-21 floor). The bundle step
cannot work like that. This branch puts `package.json` / `package-lock.json` back to main's (0.73.11 / metro-config
0.73.5). Lite needs its own `npm install` in `lite/app` (a symlinked `node_modules` does not work: Metro resolves the
real path and then cannot find `@babel/runtime`).

### A log line that is not a fault

On Android 9–12 an R8 build logs, at INFO, `Rejecting re-init on previously-failed class … h0.a …
NoClassDefFoundError: android.animation.ValueAnimator$DurationScaleChangeListener`. `h0.a` is AndroidX
DynamicAnimation's API-33-only listener lambda; the class cannot load below API 33 and is never used there. The app
runs normally (tested on Android 9: connect, audio, background/foreground). The unminified build does not print it.

## 2. Edge-to-edge

**Our code:** the only deprecated use was `android:statusBarColor` / `android:navigationBarColor` in `AppTheme`
(`res/values/styles.xml`), and the `backgroundColor` / `translucent` props on React Native's `<StatusBar>` in
`SDRScreen.tsx` — both ignored once the activity is edge-to-edge (`edgeToEdgeEnabled=true`; React Native logs
"Ignored status bar change, current activity is edge-to-edge"). Both removed; the app looks the same because React
Native's activity already makes both bars transparent and the screens pad with the safe-area insets.

**Libraries** — call sites of `Window.setStatusBarColor` / `setNavigationBarColor` (and the getters) in the
**release dex after R8** (before R8 there were also AndroidX `EdgeToEdge*`/`WindowCompat.enableEdgeToEdge` and
Material `BottomSheetDialog`/`SheetDialog`/`EdgeToEdgeUtils`; R8 removed them as unused):

| Library | Call site that survives R8 | Fixed by an update? |
|---|---|---|
| React Native 0.86 (`react-android`) | `WindowUtilKt.enableEdgeToEdge` — sets both bars transparent, plus `LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES` below API 30. This is React Native's OWN edge-to-edge setup and runs on every launch. | No — React Native's `main` branch still does the same (checked 2026-10-29). Nothing to change on our side. |
| React Native 0.86 | `StatusBarModule.setColor` / `getTypedExportedConstants` (`<StatusBar backgroundColor>`); R8 shares one lambda class between this and a react-native-screens animator, so a scan may name `rnscreens…DimmingViewManager` — the code is React Native's. | No. Our code no longer calls it. |
| Material Components 1.13.0 (transitive) | `MaterialDatePicker` (inlined `EdgeToEdgeUtils`) | 1.13.0 is current; not used by our screens. |

## 3. Numbers (V12 beta37, Play app `:app:bundleCurrentRelease`, 2026-10-29)

| | R8 off (before) | R8 on (after) |
|---|---|---|
| Play `r8.json` — obfuscation / optimisation / shrinking | ~0 % (R8 not run) | **79.8 % / 79.4 % / 79.7 %** |
| dex in the APK (uncompressed) | 31.2 MB (4 files) | 8.2 MB (1 file) |
| GitHub APK | 148.6 MB | 140.5 MB |
| AAB | 182.8 MB | 180.8 MB (carries the 72 MB mapping, compressed) |
| Lite dex | 9.5 MB (V11 Lite 89) | 2.3 MB |

Most of the APK is native libraries (cloudflared 29 MB, ML Kit translate, Skia, the server), which R8 does not touch.
R8 itself takes about a minute and uses as many threads as the Gradle JVM sees processors — `--max-workers` does
not limit it. To keep the Mac cool, start the daemon with
`"-Dorg.gradle.jvmargs=-Xmx6144m -XX:MaxMetaspaceSize=2048m -XX:ActiveProcessorCount=2"` (R8 then reports 2 threads in
`r8.json`).
