**Two APKs this time — please read.**

VibeSDR for Android has moved to a new app ID, `net.vibesdr.app`, to match the upcoming Google Play listing. This
release ships two identical builds:

- **VibeSDR-11.0-RC4-530.apk** — the new app. **All future updates come here only.**
- **VibeSDR-legacy-11.0-RC4-530.apk** — the final update to the old app (`com.vibesdr.app`). It keeps your existing
  settings and bookmarks, and has the old icon so you can tell the two apart.

Both can be installed at the same time, so you can try the new one without losing anything. When you're happy, move
your bookmarks across (the legacy app's **VibeSDR has a new home** notice has an **EXPORT MY BOOKMARKS** button; in
the new app open the frequency card → BOOKMARKS → IMPORT) and uninstall "VibeSDR (legacy)". Other settings start
fresh in the new app.

While both are installed:
- Android will ask which app to use when you plug in an SDR or open a `vibesdr://` link.
- **Run VibeServer in only one of them.** The second refuses to start while the first is serving.
- Android Auto will show both.
