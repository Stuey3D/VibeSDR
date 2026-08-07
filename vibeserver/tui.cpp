// tui.cpp — the ONLY two things a terminal is still for: first-run, and getting back in.
//
// ★★★ WHAT THIS USED TO BE, AND WHY IT ISN'T. This was "the macOS menu bar over SSH": 26 fields in
// five sections, scrolling inside an 80x24 window. Stuart, 2026-08-04:
//     "TUI is literally just used to get the server running, I found it too unintuitive and not
//      very easy to use. Bare minimum in here to get the server displaying in a real browser, then
//      all the config is done in the browser with a proper visual page."
// So the "one config schema, THREE editors" rule is now TWO editors: the browser (everything) and
// the macOS menu bar (the Mac product). ★ A list of every option is not a design — that is exactly
// what made this screen unusable, and re-adding fields here is how it grows back.
//
// ★★ THE RULE FOR THIS FILE: if a setting can be reached from the browser, it does NOT belong
// here. This screen is credentials and recovery only.
//
// ★★★ THE TUI IS NOT THE SERVER. systemd owns the daemon's lifetime; closing the SSH session must
// never stop the radio. This only edits config and asks systemd to restart.

#include <ncurses.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <algorithm>

#include "vibeserver_config.h"
#include "vibeserver_api.h"

namespace {

const char* CONF = "/etc/vibeserver/config.json";

std::string run(const char* cmd) {
    std::string out; char buf[256];
    FILE* f = popen(cmd, "r");
    if (!f) return out;
    while (fgets(buf, sizeof buf, f)) out += buf;
    pclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

std::string svcState()  { return run("systemctl is-active vibeserver 2>/dev/null"); }
/** ★★★ ASK THE DRIVER, NOT `lsusb`.
 *
 *  This used to shell out to `lsusb` and grep for likely-looking vendor strings. Two ways that
 *  lied, and both were reported on the same evening:
 *
 *  ★★ `lsusb` LIVES IN usbutils, WHICH WE DO NOT DEPEND ON. On a minimal Debian or Ubuntu the
 *     command simply does not exist, `run()` returns empty, and the wizard says "No radio
 *     detected" — while `vibeserver --device 0` opens the very same dongle and runs perfectly,
 *     because THAT path asks librtlsdr. A setup screen that contradicts the working server sends
 *     the user hunting through udev rules and USB cables for a fault that is not there.
 *  ★ And grepping vendor strings can only ever guess. A device we cannot open still matches
 *    'realtek'; a device we CAN open under an unfamiliar name does not.
 *
 *  vs_device_count()/vs_device_name() are the same enumeration the server itself starts from, so
 *  this screen now agrees with it by construction — including RSPs and Airspy HF+s, which lsusb
 *  matching handled only by luck. */
std::string radioLine() {
    // ★ FIRST CHOICE: the driver, because it is the same enumeration the server starts from and
    //   it names the radio properly ("RTLSDRBlog Blog V4", not a USB ID).
    if (vs_device_count() > 0) {
        const char* n = vs_device_name(0);
        return (n && *n) ? std::string(n) : std::string("SDR");
    }
    // ★★★ SECOND CHOICE, AND NOT REDUNDANT — the two methods fail in OPPOSITE situations, which is
    //     exactly why neither can be trusted alone. Measured, both on the same evening:
    //
    //     · The DRIVER misses an SDRplay whose API is busy or still letting go. deviceCount()
    //       gives up after 2 s, and sdrplay_api's open waits on a SYSTEM-WIDE lock, so seconds
    //       after stopping the service it still reported nothing with an RSP1B plugged in.
    //     · `lsusb` misses everything on a machine without usbutils, which we do not depend on —
    //       that is the fault that had a user hunting through udev rules while
    //       `vibeserver --device 0` ran the same dongle perfectly.
    //
    //  ★ So: ask the driver, and if it says nothing, ask the USB bus before believing it. A false
    //    "no radio" is the single most expensive thing this screen can say — it sends people
    //    looking for a hardware fault that is not there.
    return run("lsusb 2>/dev/null | grep -iE 'airspy|sdrplay|rtl|realtek|1df7' "
               "| sed 's/.*ID [0-9a-f:]* //' | head -1");
}
std::string myIp() { return run("hostname -I 2>/dev/null | awk '{print $1}'"); }

/** ★★ THE PORT THE SERVICE IS ACTUALLY ON — and not somebody else's.
 *  A bare `ss` scan returns the FIRST listening socket on the box, which on a Pi you have just
 *  SSH'd into is port 22: the screen then cheerfully told you to point a browser at your own SSH
 *  daemon. Look only inside the range VibeServer picks from. Never report a port we cannot
 *  attribute to ourselves. (Kept verbatim from the old screen — the reasoning has not changed.) */
std::string listenPort() {
    std::string p = run("ss -tln 2>/dev/null | grep -oE ':(4800[0-9]|480[1-4][0-9])\\b' "
                        "| head -1 | tr -d ':'");
    return p.empty() ? std::string("48000") : p;
}

/** ★★★ READ IT THROUGH sudo, BECAUSE WE CANNOT READ IT DIRECTLY.
 *  config.json is 0600 and owned by the SERVICE user; the TUI runs as a human. A plain fopen()
 *  therefore fails exactly as it would on a missing file — and that is a DATA-LOSS bug, not a
 *  cosmetic one: the screen concluded "not set up yet", offered the first-run wizard, and
 *  finishing it would have overwritten a working server's entire configuration. Caught on the Pi
 *  demo, 2026-08-05, where it cheerfully offered to set up an already-configured receiver.
 *  ★★ Returns: 1 = read and parsed, 0 = genuinely absent, -1 = EXISTS BUT UNREADABLE.
 *     The caller must treat -1 as "do not touch anything", because the one thing worse than
 *     refusing to help is destroying the config we were asked to recover. */
int loadConfigViaSudo(vsconfig::Config& cfg, std::string& err) {
    if (run(("test -e " + std::string(CONF) + " && echo yes").c_str()) != "yes") return 0;
    const std::string j = run(("sudo cat " + std::string(CONF) + " 2>/dev/null").c_str());
    if (j.empty()) { err = "cannot read " + std::string(CONF) + " (need sudo)"; return -1; }
    if (!vsconfig::fromJson(j, cfg, err)) return -1;
    return 1;
}

/** Save via a temp file and `sudo install`, because the TUI runs as a human and the config belongs
 *  to the service user. ★ 0600: it holds the admin password and the PIN in clear. */
bool saveConfig(const vsconfig::Config& c, std::string& err) {
    char tmp[] = "/tmp/vibeserver-config.XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) { err = "could not create a temporary file"; return false; }
    const std::string j = vsconfig::toJson(c);
    if (write(fd, j.data(), j.size()) != (ssize_t)j.size()) {
        close(fd); unlink(tmp); err = "short write"; return false;
    }
    close(fd);
    std::string cmd = std::string("sudo install -o vibeserver -g vibeserver -m600 ")
                    + tmp + " " + CONF + " 2>&1";
    std::string out = run(cmd.c_str());
    unlink(tmp);
    if (!out.empty()) { err = out; return false; }
    return true;
}


/** ★★★ RE-READ, MUTATE, WRITE — NEVER write back the copy loaded when the TUI opened.
 *  The TUI used to be the only thing that wrote config.json, so holding a copy for the life of the
 *  screen was safe. It is not any more: the SERVER now writes it too, whenever an admin changes
 *  the gain from the client (see the persist handler in main.cpp). So a TUI left open while
 *  someone sets the gain, followed by a password reset here, would write back the stale copy and
 *  silently revert the gain — a read-modify-write race with a second writer that did not exist
 *  when this was written.
 *  ★★ The same lesson the live-persist path learned from the other side: a change to ONE field
 *     must not carry an opinion about every other field. Patch, do not overwrite.
 *  ★ have < 0 (exists but unreadable) refuses, exactly as at startup — the one thing worse than
 *    failing to help is destroying the config we were asked to recover. */
bool updateConfig(vsconfig::Config& cfg, const std::function<void(vsconfig::Config&)>& mutate,
                  std::string& err) {
    vsconfig::Config fresh;
    const int have = loadConfigViaSudo(fresh, err);
    if (have < 0) return false;
    if (have == 0) fresh = cfg;          // nothing on disk yet: our copy is the only truth there is
    mutate(fresh);
    if (!saveConfig(fresh, err)) return false;
    cfg = fresh;                          // keep the screen showing what is actually stored
    return true;
}

// ── Small input helpers ──────────────────────────────────────────────────────
void centreLine(int row, const char* s) { mvprintw(row, 2, "%s", s); }

/** One prompt, echoed or masked. Returns false if the user pressed Esc. */
bool prompt(int row, const char* label, std::string& value, bool mask, const char* help) {
    while (true) {
        move(row, 0); clrtoeol();
        move(row + 1, 0); clrtoeol();
        attron(A_BOLD); mvprintw(row, 2, "%s", label); attroff(A_BOLD);
        std::string shown = mask ? std::string(value.size(), '*') : value;
        mvprintw(row, 24, "%s_", shown.c_str());
        if (help) { attron(COLOR_PAIR(4)); mvprintw(row + 1, 4, "%s", help); attroff(COLOR_PAIR(4)); }
        refresh();
        int c = getch();
        if (c == 27) return false;
        if (c == '\n' || c == KEY_ENTER) return true;
        if (c == KEY_BACKSPACE || c == 127 || c == 8) { if (!value.empty()) value.pop_back(); continue; }
        if (c >= 32 && c < 127) value += (char)c;
    }
}

void header(const char* subtitle) {
    erase();
    attron(A_BOLD); mvprintw(0, 2, "VibeSDR  ·  VibeServer"); attroff(A_BOLD);
    attron(COLOR_PAIR(4)); mvprintw(1, 2, "%s", subtitle); attroff(COLOR_PAIR(4));
    mvprintw(2, 2, "----------------------------------------------------------------");
}

void message(int row, int pair, const char* s) {
    attron(COLOR_PAIR(pair)); mvprintw(row, 2, "%s", s); attroff(COLOR_PAIR(pair));
}

// ── 1. THE FIRST-RUN WIZARD ──────────────────────────────────────────────────
// ★★★ THREE QUESTIONS, ONE SCREEN EACH, AND NOTHING ELSE. Everything the old screen asked for —
// name, place, locator, frequency, rate, gain, limits, notches — moved to the browser. What is
// left is what a browser cannot be reached without.
//
// ★★ THE NAME IS DELIBERATELY NOT ASKED HERE. It was, in the first draft. But mDNS stays off until
// setup completes, so nothing needs a name until the browser page — and the URL this prints is an
// IP ADDRESS, which works with no name at all. Asking here and editing there is the two-editors
// drift this whole change exists to kill. The browser owns the name.
bool runWizard(vsconfig::Config& cfg) {
    // ── Step 1: the radio ────────────────────────────────────────────────────
    while (true) {
        header("First-time setup  ·  step 1 of 3");
        std::string radio = radioLine();
        if (radio.empty()) {
            message(4, 2, "No radio detected.");
            mvprintw(6, 2, "Plug an SDR into this machine, then press  r  to look again.");
            mvprintw(7, 2, "Supported: RTL-SDR, Airspy HF+, SDRplay RSP.");
            attron(A_BOLD); mvprintw(9, 2, "r = look again    q = quit"); attroff(A_BOLD);
            refresh();
            int c = getch();
            if (c == 'q') return false;
            continue;                      // ★ Never proceed to a server that cannot receive.
        }
        message(4, 1, "Radio found:");
        mvprintw(4, 20, "%s", radio.c_str());
        mvprintw(6, 2, "This is the receiver VibeServer will serve.");
        attron(A_BOLD); mvprintw(8, 2, "Enter = continue    r = look again    q = quit"); attroff(A_BOLD);
        refresh();
        int c = getch();
        if (c == 'q') return false;
        if (c == '\n' || c == KEY_ENTER) break;
    }

    // ── Step 2: the admin password — MANDATORY ───────────────────────────────
    // ★★★ There is no way to skip this, and that is a CHANGE. It used to be optional, and blank
    // meant nothing was protected. But this password is now what unlocks the browser setup page,
    // so a blank one would leave that page — and the whole radio — open to anyone who can reach
    // the machine.
    while (true) {
        header("First-time setup  ·  step 2 of 3");
        mvprintw(4, 2, "Choose an admin password.");
        attron(COLOR_PAIR(4));
        mvprintw(5, 2, "You will need it to finish setup in a browser, and to change");
        mvprintw(6, 2, "settings later. It also protects things that can DAMAGE the radio:");
        mvprintw(7, 2, "bias-T, direct sampling and calibration.");
        attroff(COLOR_PAIR(4));
        std::string a, b;
        if (!prompt(9,  "Password", a, true, "At least 6 characters.")) continue;
        if (a.size() < 6) { message(13, 2, "Too short — at least 6 characters."); getch(); continue; }
        if (!prompt(11, "Type it again", b, true, nullptr)) continue;
        if (a != b) { message(13, 2, "Those did not match. Try again."); getch(); continue; }
        cfg.adminPass = a;
        break;
    }

    // ── Step 3: the PIN — optional ───────────────────────────────────────────
    {
        header("First-time setup  ·  step 3 of 3");
        mvprintw(4, 2, "Set a PIN? (optional)");
        attron(COLOR_PAIR(4));
        mvprintw(5, 2, "A PIN decides who may CONNECT at all. Leave it blank and anyone");
        mvprintw(6, 2, "who can reach this machine may listen — which is usually what you");
        mvprintw(7, 2, "want on your own network.");
        attroff(COLOR_PAIR(4));
        std::string pin;
        prompt(9, "PIN (blank = none)", pin, false, "Press Enter to skip.");
        cfg.pin = pin;
    }
    return true;
}

// ── 2. STATUS + RECOVERY ─────────────────────────────────────────────────────
// ★★ THE LOCK-OUT ESCAPE HATCH, and the one thing the browser cannot be. Stuart, 2026-08-04:
//     "the TUI needs to remain a backup to reset the admin password etc if required as that can
//      only be accessed on the server itself or via SSH anyway."
// ★★★ The security argument is sound, not a compromise: reaching this screen already requires a
// shell on the machine. Anyone with that can read the config, stop the service or reinstall the
// package anyway — so SSH access is a STRICTLY STRONGER credential than the admin password.
// Demanding the forgotten password here would protect nothing and lock out only the owner.
// ★ And realistically it is a LAST RESORT after first setup, so it is plain on purpose. Four
// actions, confirm each, say what happened. The design effort goes into the browser page.
void statusScreen(vsconfig::Config& cfg) {
    std::string msg;
    while (true) {
        header("Status and recovery");
        const std::string st = svcState();
        const bool up = (st == "active");
        mvprintw(4, 2, "Service :");
        attron(up ? COLOR_PAIR(1) : COLOR_PAIR(2));
        mvprintw(4, 14, "%s", up ? "running" : (st.empty() ? "not installed" : st.c_str()));
        attroff(up ? COLOR_PAIR(1) : COLOR_PAIR(2));
        // ★★★ A RUNNING SERVER HOLDS THE DEVICE, so enumerating it here finds NOTHING — and
        //     printing "none detected" next to "Service: running" is the same contradiction that
        //     sent a user hunting through udev rules, only inverted: the screen would be calling a
        //     working receiver broken. If the service is up, the radio is by definition fine;
        //     what we cannot do is name it, so say that instead of guessing.
        // ★ Stop serving (x) and this line fills in — which is also exactly when you need it, in
        //   the middle of swapping one radio for another.
        std::string radio = radioLine();
        const char* radioTxt = !radio.empty() ? radio.c_str()
                             : up             ? "in use by the running server"
                                              : "none detected";
        mvprintw(5, 2, "Radio   : %s", radioTxt);
        mvprintw(6, 2, "Settings: %s", cfg.configured ? "configured" : "NOT set up yet");

        const std::string ip = myIp(), port = listenPort();
        attron(A_BOLD);
        mvprintw(8, 2, "Open this in a browser:");
        attron(COLOR_PAIR(3));
        mvprintw(8, 27, "http://%s:%s/", ip.c_str(), port.c_str());
        attroff(COLOR_PAIR(3));
        attroff(A_BOLD);
        attron(COLOR_PAIR(4));
        mvprintw(9, 2, "All settings live there. Setup from the VibeSDR app is coming soon.");
        attroff(COLOR_PAIR(4));

        // ★★ CHANGING THE RADIO IS ITS OWN SECTION, not a footnote under "locked out". Swapping
        //    an RSP for an Airspy is a normal thing to do with a receiver, and the only reason it
        //    needs help at all is that the SERVICE HOLDS THE DEVICE: unplugging underneath a
        //    running server is how you get "could not open (is another program using it?)" on the
        //    way back in. Stop, swap, start — the machine can say that better than a wiki can.
        attron(A_BOLD); mvprintw(11, 2, "Changing the radio"); attroff(A_BOLD);
        if (up) {
            mvprintw(12, 4, "x   stop serving and release the radio  (then unplug it)");
        } else {
            attron(COLOR_PAIR(3));
            mvprintw(12, 4, "s   start serving with the radio that is plugged in now");
            attroff(COLOR_PAIR(3));
        }

        attron(A_BOLD); mvprintw(14, 2, "If you are locked out"); attroff(A_BOLD);
        mvprintw(15, 4, "p   reset the admin password");
        mvprintw(16, 4, "n   reset or clear the PIN");
        mvprintw(17, 4, "z   reset to not-set-up  (the browser will ask you to set it up again)");
        mvprintw(18, 4, "r   restart the server");
        mvprintw(19, 4, "q   quit");

        if (!msg.empty()) message(21, 3, msg.c_str());
        refresh();

        int c = getch();
        msg.clear();
        if (c == 'q') return;
        if (c == 'x' && up) {
            // ★ STOP, not restart. The whole point is to let go of the USB device so the user can
            //   physically unplug it; a restart would grab it straight back.
            mvprintw(21, 2, "Stopping…"); refresh();
            run("sudo systemctl stop vibeserver 2>&1");
            msg = "Stopped. The radio is released — swap it now, then press s.";
        } else if (c == 's' && !up) {
            // ★★ LOOK BEFORE STARTING. Starting with nothing plugged in leaves a service that is
            //    "active" and cannot receive, which reads as a broken install rather than an
            //    empty USB port. Say what was found, and refuse politely when nothing was.
            std::string found = radioLine();
            if (found.empty()) {
                msg = "No radio found. Plug one in, then press s again.";
            } else {
                mvprintw(21, 2, "Starting with %s…", found.c_str()); refresh();
                run("sudo systemctl start vibeserver 2>&1");
                msg = "Serving " + found + ".";
            }
        } else if (c == 'r') {
            mvprintw(18, 2, "Restarting… (listeners will reconnect)"); refresh();
            run("sudo systemctl restart vibeserver 2>&1");
            msg = "Restarted.";
        } else if (c == 'p') {
            header("Reset the admin password");
            attron(COLOR_PAIR(4));
            mvprintw(4, 2, "You do not need the old one — having a shell on this machine");
            mvprintw(5, 2, "is already stronger proof than the password is.");
            attroff(COLOR_PAIR(4));
            std::string a, b;
            if (!prompt(7, "New password", a, true, "At least 6 characters. Esc to cancel.")) continue;
            if (a.size() < 6) { msg = "Too short — nothing changed."; continue; }
            if (!prompt(9, "Type it again", b, true, nullptr)) continue;
            if (a != b) { msg = "Those did not match — nothing changed."; continue; }
            std::string err;
            if (!updateConfig(cfg, [&](vsconfig::Config& c){ c.adminPass = a; }, err))
                { msg = "Save failed: " + err; continue; }
            run("sudo systemctl restart vibeserver 2>&1");
            msg = "Admin password changed, server restarted.";
        } else if (c == 'n') {
            header("Reset the PIN");
            attron(COLOR_PAIR(4));
            mvprintw(4, 2, "The PIN decides who may connect at all. Leave blank to remove it.");
            attroff(COLOR_PAIR(4));
            std::string pin;
            if (!prompt(6, "PIN (blank = none)", pin, false, "Esc to cancel.")) continue;
            std::string err;
            if (!updateConfig(cfg, [&](vsconfig::Config& c){ c.pin = pin; }, err))
                { msg = "Save failed: " + err; continue; }
            run("sudo systemctl restart vibeserver 2>&1");
            msg = pin.empty() ? "PIN removed, server restarted." : "PIN changed, server restarted.";
        } else if (c == 'z') {
            header("Reset to not-set-up");
            mvprintw(4, 2, "The next visit in a browser will ask you to set this server up again.");
            attron(COLOR_PAIR(4));
            mvprintw(5, 2, "Your admin password and PIN are KEPT — you will need the password");
            mvprintw(6, 2, "to sign in. Nothing else is lost until you save new settings.");
            attroff(COLOR_PAIR(4));
            attron(A_BOLD); mvprintw(8, 2, "Press  y  to confirm, anything else to cancel."); attroff(A_BOLD);
            refresh();
            if (getch() != 'y') { msg = "Cancelled."; continue; }
            std::string err;
            if (!updateConfig(cfg, [&](vsconfig::Config& c){ c.configured = false; }, err))
                { msg = "Save failed: " + err; continue; }
            run("sudo systemctl restart vibeserver 2>&1");
            msg = "Reset. Open the address above in a browser to set it up again.";
        }
    }
}

} // namespace

int vibeserverTui() {
    vsconfig::Config cfg;
    std::string err;
    const int have = loadConfigViaSudo(cfg, err);
    if (have < 0) {
        // ★★★ REFUSE RATHER THAN RISK IT. A config that exists but cannot be read must never be
        //     treated as absent — that path leads straight to the wizard overwriting it.
        std::fprintf(stderr,
            "VibeServer: %s\n"
            "  The configuration exists but this account cannot read it, so I will not\n"
            "  offer to replace it. Run:  sudo vibeserver\n", err.c_str());
        return 1;
    }

    initscr(); noecho(); cbreak(); keypad(stdscr, TRUE); curs_set(0);
    start_color(); use_default_colors();
    init_pair(1, COLOR_GREEN,  -1);
    init_pair(2, COLOR_RED,    -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_CYAN,   -1);

    // ★ Running `vibeserver` again after setup must NOT re-run the wizard. Anything already
    //   configured goes straight to status and recovery.
    // ★★★ AND THE HALF-WAY STATE IS ITS OWN SCREEN. The wizard saves `configured = false` because
    //     the BROWSER finishes setup — so between the two, `!cfg.configured` sent the owner
    //     straight back into the wizard. That is the wrong screen for the one question they are
    //     most likely to have come back with: "what was that address again?". They already gave
    //     us a name and a password; asking for them again treats a half-finished setup as no
    //     setup at all. Show them the address, and let them re-run the wizard only if they say so.
    bool wizardWanted = (have == 0);
    if (have == 1 && !cfg.configured) {
        header("Setup started");
        const std::string ip0 = myIp(), port0 = listenPort();
        message(4, 3, "This server has been started but setup is not finished.");
        mvprintw(6, 2, "Finish it in a browser on any machine on this network:");
        attron(A_BOLD | COLOR_PAIR(3));
        mvprintw(8, 4, "http://%s:%s/", ip0.c_str(), port0.c_str());
        attroff(A_BOLD | COLOR_PAIR(3));
        attron(COLOR_PAIR(4));
        mvprintw(10, 2, "Sign in with the admin password you chose here.");
        attroff(COLOR_PAIR(4));
        mvprintw(12, 2, "Press  W  to start the setup questions again, or any other key to exit.");
        refresh();
        const int k = getch();
        if (k != 'w' && k != 'W') { endwin();
            std::printf("\nFinish setup at  http://%s:%s/\n\n", ip0.c_str(), port0.c_str());
            return 0; }
        wizardWanted = true;
    }
    if (wizardWanted) {
        if (!runWizard(cfg)) { endwin(); return 0; }
        cfg.configured = false;       // ★★ The BROWSER finishes setup; this only makes it reachable.
        std::string serr;
        if (!saveConfig(cfg, serr)) {
            endwin();
            std::fprintf(stderr, "VibeServer: could not save the configuration — %s\n", serr.c_str());
            return 1;
        }
        run("sudo systemctl enable vibeserver 2>&1");
        run("sudo systemctl restart vibeserver 2>&1");

        // ── Hand over to the browser, and be specific about it ────────────────
        // ★★★ THE FIRST CONNECTION IS BY IP ADDRESS, ALWAYS. No .local, no discovery, no name:
        // mDNS is not running yet and the name does not exist yet. Setup is what creates the
        // friendly name and turns discovery on.
        header("Ready");
        const std::string ip = myIp(), port = listenPort();
        message(4, 1, "VibeServer is running.");
        mvprintw(6, 2, "Finish setting it up in a browser on any machine on this network:");
        attron(A_BOLD | COLOR_PAIR(3));
        mvprintw(8, 4, "http://%s:%s/", ip.c_str(), port.c_str());
        attroff(A_BOLD | COLOR_PAIR(3));
        attron(COLOR_PAIR(4));
        mvprintw(10, 2, "Sign in with the admin password you just chose.");
        mvprintw(11, 2, "Setup from the VibeSDR app is coming soon.");
        attroff(COLOR_PAIR(4));
        mvprintw(13, 2, "Run  vibeserver  again at any time for status, or to reset the password.");
        attron(A_BOLD); mvprintw(15, 2, "Press any key to finish."); attroff(A_BOLD);
        refresh();
        getch();
        endwin();
        // ★ Print it to the terminal as well: the curses screen vanishes on exit, and the address
        //   is the ONE thing they need to keep.
        std::printf("\nVibeServer is running.\n  Open  http://%s:%s/  to finish setup.\n\n",
                    ip.c_str(), port.c_str());
        return 0;
    }

    statusScreen(cfg);
    endwin();
    return 0;
}
