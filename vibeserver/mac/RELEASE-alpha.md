## VibeServer for macOS — Alpha (build in progress)

VibeServer turns a Mac and an RTL-SDR dongle into a small, private SDR server. Plug in a dongle,
click **Open in Browser**, and you have a full waterfall, tuning and audio in the browser — for
yourself, or for your phone, watch and friends on your network. Same C++ DSP core as the VibeSDR
app; no dependencies to install.

### ⚠️ This is an early alpha — build in progress

- **One SDR at a time.** It serves a single RTL-SDR (and its variants — Blog V4, Nooelec NESDR,
  etc.). Multi-radio pools are designed but not built yet.
- **The advanced admin features aren't here yet** — link-management ceilings and the PIN are in,
  but the full multi-user, roles and hardware-profile wizard are still to come.
- **One listener at a time.** A second connection is politely turned away ("in use, try again
  later") until proper multi-client lands.

### Recommended use

- **Best on your local network** — share this Mac's radio with your own devices at home.
- **If you port-forward it to the internet, SET A PIN.** In Settings ▸ Access. A listener can
  change the dongle's hardware settings (gain, sample rate, bias-T on some models), so an open
  server on the public internet is handing that control to strangers. The PIN keeps it yours.

### Install

1. Download and unzip **VibeServer.zip**.
2. Move **VibeServer.app** to your Applications folder and open it — it lives in the menu bar
   (no Dock icon).
3. Plug in your RTL-SDR, then **left-click the menu-bar icon** to open the web client, or
   **right-click** for settings.

Notarised by Apple, so it opens without a Gatekeeper warning. Requires macOS 14 or later.

*Feedback welcome — this is being built in the open.*
