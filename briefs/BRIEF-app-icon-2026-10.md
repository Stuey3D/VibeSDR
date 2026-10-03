# BRIEF: New VibeSDR app icon (tuning drum, needle removed)

## Why
The control redesign removed the red index needle from the drum. The app icon was built around that needle, so it has been refined to match: same trapezoid window, radio glyph, green glow and rim, with the red needle replaced by a ribbed tuning drum.

## Files in this package
| File | Size | Use |
|---|---|---|
| `assets/icon.png` | 1024×1024, RGB, no alpha | Expo `icon` + iOS App Store icon (square corners; iOS applies the mask) |
| `assets/adaptive-icon-foreground.png` | 1024×1024, transparent | Android adaptive foreground (radio glyph only) |
| `assets/adaptive-icon-background.png` | 1024×1024, RGB | Android adaptive background (window, drum, glow — full bleed) |
| `assets/adaptive-icon-monochrome.png` | 1024×1024, transparent white | Android 13+ themed icon |
| `assets/play-store-icon.png` | 512×512, RGB | Play Console listing (upload manually) |
| `assets/favicon.png` | 48×48 | Expo web favicon |
| `source/*.svg` | — | Vector masters for each of the above |
| `preview/preview-sheet.png` | — | iOS sizes 360→40 px, Android circle/squircle masks |

## Tasks
1. Find where the current icons live (`grep -rn "icon" app.json app.config.* 2>/dev/null` and look in `assets/`). If existing filenames differ, **overwrite the existing files in place** with these images rather than introducing new paths, unless the existing paths are messy.
2. Ensure `app.json` / `app.config.*` has the equivalent of:
   ```json
   {
     "expo": {
       "icon": "./assets/icon.png",
       "ios": { "icon": "./assets/icon.png" },
       "android": {
         "adaptiveIcon": {
           "foregroundImage": "./assets/adaptive-icon-foreground.png",
           "backgroundImage": "./assets/adaptive-icon-background.png",
           "monochromeImage": "./assets/adaptive-icon-monochrome.png",
           "backgroundColor": "#030403"
         }
       },
       "web": { "favicon": "./assets/favicon.png" }
     }
   }
   ```
3. If `ios/` and `android/` native folders are committed, regenerate them so the icons are baked in: `npx expo prebuild --clean`, then re-apply any native customisations that prebuild overwrites. If the project uses CNG (native folders git-ignored), no prebuild step is needed in the repo.
4. Search for any other copies of the old needle icon (e.g. `grep -rli "icon" --include=*.json`, image files named `icon*`, `logo*`, `appicon*`) — splash screen, README badge, VibeServer web client favicon — and list them for Stuart rather than replacing them silently.
5. Do **not** change the watch app (VibeSDR Jr / Companion) icons in this task.
6. Commit as: `chore(icon): refined app icon — tuning drum replaces red needle`.

## TRAPs
- **TRAP:** `icon.png` must stay alpha-free. App Store Connect rejects icons with an alpha channel, and some image tools re-add one on export. Don't re-save it through a tool that adds transparency.
- **TRAP:** in step 3, a stale native project is the usual reason an icon "didn't change". Editing `app.json` alone does nothing if `ios/`/`android/` are committed and not regenerated.
- **TRAP:** iOS and Android launchers cache icons. Delete the app and reinstall to verify; an over-the-top install often still shows the old icon.
- **TRAP:** the Android layers deliberately have **no rim**. Launcher masks vary (circle, squircle, teardrop), so a drawn rim would be cut off unevenly. Don't add one back.
- **TRAP:** the Play Store listing icon is not taken from the build. Upload `play-store-icon.png` in Play Console › Store presence › Main store listing.
- **TRAP:** if the project later adopts iOS 26 Liquid Glass layered icons (an Icon Composer `.icon` file), that replaces `ios.icon`. Use `source/icon-ios.svg` layers as the starting point; it's out of scope here.
