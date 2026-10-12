/**
 * LOGO_DECODE — spread onto every REMOTE station-logo <Image> (DAB rows and head, VTS bar, FM-DX tuner, StationLogo).
 *
 * ★★★ ANDROID DECODES A REMOTE IMAGE AT ITS OWN SIZE unless told otherwise (ReactImageView.shouldResize: AUTO resizes
 *   only local files). radio-browser favicons are often hundreds of px for a 34 pt box: on the DAB screen 44 rows
 *   decoded to 47.5 MB (~1.1 MB each), over the XCover 4s's 42 MB GPU texture cache (Android 11, 720p), so EVERY frame
 *   evicted and re-uploaded all 44 (the marquees redraw every row) — prepareTree 33 ms a frame, ~16 fps. Perfetto,
 *   2026-10-12. iOS already decodes at the view's size and ignores both props.
 * ★★ resizeMultiplier 2: decoded at TWICE the box (Fresco rounds up further), because a size change does NOT re-decode
 *   a single-source Image (ReactImageView.onSizeChanged) — the logo is a little bigger in the advanced signal view
 *   (Stuart) and would otherwise be stretched from the small copy. The prop is real (ReactImageManager
 *   @ReactProp "resizeMultiplier", ImageViewNativeComponent) but missing from React Native's TypeScript types —
 *   hence the untyped half.
 */
export const LOGO_DECODE = { resizeMethod: 'resize' as const, ...({ resizeMultiplier: 2 } as {}) };
