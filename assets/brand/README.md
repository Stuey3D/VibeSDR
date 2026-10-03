# VibeSDR brand — the 11.0 icon (2026-10-03)

The vector masters for every app icon. Designed with Claude on the web (`briefs/BRIEF-app-icon-2026-10.md`), then
each rim was redrawn on the curve of the mask it sits under (Stuart: *"make sure the rim is solidly lit evenly all
around"* — the old icon's glow was cut off at the corners).

| Master | Used for | Rim drawn on |
|---|---|---|
| `icon-ios.svg` | iOS / iPadOS / Mac / tvOS app icon, About page, website, favicon | Apple's continuous corner (superellipse n = 5), inset 12 |
| `icon-play.svg` | Google Play listing (512 px, uploaded by hand in Play Console) | Play's 20 % rounded square |
| `adaptive-background.svg` + `adaptive-foreground.svg` | Android 8+ launcher (the launcher picks the shape) | a circle at the 72 dp viewport's edge — whole under every launcher mask |
| `adaptive-monochrome.svg` | Android 13+ themed icon | none (the system tints it) |
| `legacy-square.svg`, `legacy-round.svg` | Android 7 and older launcher icons | the icon's own square / circle |
| `artwork.svg` | Now Playing album art (`artwork_base.png`, iOS + Android) | 10 % corners — the OS rounds album art only ~8–12 % |

- ★ App Store icons must have **no alpha** — `apply_icons.py` flattens onto the background colour.
- ★ The sub-brand family (Jr, Buddy, VibeServer, Lite, VibeDSP) is built separately — see `spike/tools/make_family_icons.py`.
- `build_rims.py` and `apply_icons.py` are the scripts that produced the shipped files from these masters (their
  paths point at the working copies used on the day; adjust `SRC` / `OUT` to re-run).
