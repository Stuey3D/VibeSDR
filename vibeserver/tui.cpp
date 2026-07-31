// tui.cpp — the settings screen for a headless VibeServer.
//
// ★★★ WHY THIS EXISTS. Stuart, 2026-07-31: "I want to be able to SSH in, type vibeserver, and the
// TUI pops up." A Pi in a cupboard has no screen, and the person running it is not necessarily
// someone who edits systemd units — his own words: "I know basic command line like sudo apt
// install; it's more for someone like me installing it and using it." So the bar is APT AND
// NOTHING ELSE: installing may use the terminal, CONFIGURING must not.
//
// ★★ IT IS THE macOS MENU-BAR APP, OVER SSH — the third editor in the "one config schema, three
// editors" rule (memory/vibeserver_pi_two_products.md). Same settings, same order, same words. When
// a control moves on the Mac it moves here too.
//
// ★★★ THE TUI IS NOT THE SERVER. s-tui is a monitor: you close it and nothing dies. If this were
// the daemon, closing an SSH session would kill the receiver — exactly wrong for a box meant to sit
// in a loft. systemd owns the lifetime; this only edits the config and asks systemd to reload.
//
// ★ How the same binary is both: NO ARGUMENTS AND A TERMINAL means a human typed `vibeserver`, so
// show the TUI. systemd has no TTY, so the service is unaffected even with empty VIBESERVER_ARGS.

#include <ncurses.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <algorithm>

namespace {

const char* CONF = "/etc/vibeserver/vibeserver.conf";

std::string run(const char* cmd) {
    std::string out; char buf[256];
    FILE* f = popen(cmd, "r");
    if (!f) return out;
    while (fgets(buf, sizeof buf, f)) out += buf;
    pclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

struct Field {
    const char* label;
    const char* flag;      // the CLI flag it maps to; nullptr = a bare switch
    std::string value;
    const char* help;
    bool  isToggle = false;
};

// ★ ORDER MATCHES THE MAC. Radio first (what am I), then network (how do people reach me), then
// the ceilings an owner sets for other people. Someone who has seen the menu-bar app should not
// have to re-learn anything here.
// ★★★ SAME SETTINGS, SAME ORDER, SAME WORDS AS THE MAC PANEL. The first version of this screen
// carried nine CLI flags and called itself "the macOS menu bar over SSH" — which was the rule
// stated and then not followed. Worse, the omissions were the SAFETY ones: no admin password meant
// a public Pi let any listener touch bias-T, direct sampling and calibration
// (Stuart, 2026-07-31: "the Pi isn't safe to use to make a server public").
// ★★ Sections mirror the Mac: Receiver → Access → Limits → Radio → Advanced. Someone who has seen
// the menu-bar app should not have to re-learn anything here, and when a control moves there it
// moves here too.
struct Section { const char* title; };

std::vector<Field> fields = {
    { "~Receiver",         nullptr,       "",  "" },
    { "Name",              "--name",      "",  "Shown to listeners over the spectrum." },
    { "Place",             "--place",     "",  "Town or area shown beneath the name." },
    { "Country",           "--country",   "",  "Two-letter code — sets the flag and the ITU band plan." },
    { "Locator",           "--locator",   "",  "Maidenhead square. Deliberately coarse (~4 km) — a receiver's position is published." },
    { "Latitude",          "--lat",       "",  "Exact coordinates win over a locator. Leave blank to use the locator." },
    { "Longitude",         "--lon",       "",  "Exact coordinates win over a locator." },

    { "~Access",           nullptr,       "",  "" },
    { "PIN",               "--pin",       "",  "Who may CONNECT at all. Blank = anyone who can reach this machine." },
    { "Admin password",    "--admin-pass","",  "Who may change settings that can DAMAGE the radio: bias-T, direct sampling, calibration. SET THIS on a public receiver." },
    { "Time limit (min)",  "--session-limit","","Per listener. Blank or 0 = unlimited. For a receiver on the internet." },

    { "~Limits",           nullptr,       "",  "" },
    { "Max bandwidth",     "--max-bw",    "",  "Ceiling on what a listener may ask for. Blank = no cap." },
    { "Max frame rate",    "--max-fps",   "",  "Spectrum frames/sec ceiling. Blank = no cap." },
    { "Lock capture rate", "--lock-rate", "",  "Pin the capture rate so listeners cannot change it." },
    { "Force idle saver",  "--force-idle-saver","off","Listeners may NOT switch idle power-saving off. For solar or metered hosts.", true },
    { "Uncompressed audio","--uncompressed","off","off | choice | compat. Raw audio is ~187 KB/s per listener, about 20x compressed." },

    { "~Radio",            nullptr,       "",  "" },
    { "Frequency (Hz)",    "--freq",      "",  "Where the receiver starts. Listeners can retune." },
    { "Mode",              "--mode",      "",  "am | lsb | usb | nfm | wfm | cw" },
    { "Sample rate (Hz)",  "--rate",      "",  "Capture width. The Airspy HF+ tops out at 912000." },
    { "Gain (tenths dB)",  "--gain",      "",  "Blank or negative = automatic." },

    { "~Advanced",         nullptr,       "",  "" },
    { "Port",              "--port",      "",  "Blank = the first free port from 48000." },
    { "Web client",        "--no-web",    "on","Serve the browser client at /. Off = the app only.", true },
};

/** Section headings are rows too, so the list stays one array — but they are never selectable. */
bool isHeading(const Field& f) { return f.label[0] == '~'; }

/** Read VIBESERVER_ARGS out of the config and spread it across the fields. */
void loadConf() {
    FILE* f = fopen(CONF, "r");
    if (!f) return;
    char line[1024];
    std::string args;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VIBESERVER_ARGS=", 16) != 0) continue;
        args = line + 16;
        break;
    }
    fclose(f);
    // strip quotes/newline
    std::string a;
    for (char c : args) if (c != '"' && c != '\n' && c != '\r') a += c;
    // split
    std::vector<std::string> tok; std::string cur;
    for (char c : a) { if (c == ' ') { if (!cur.empty()) tok.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) tok.push_back(cur);
    for (size_t i = 0; i < tok.size(); i++) {
        for (auto& fl : fields) {
            if (!fl.flag || tok[i] != fl.flag) continue;
            if (fl.isToggle) { fl.value = "off"; }          // --no-web present ⇒ web off
            else if (i + 1 < tok.size()) { fl.value = tok[++i]; }
        }
    }
}

std::string buildArgs() {
    std::string s;
    for (auto& f : fields) {
        if (f.isToggle) { if (f.value == "off") { s += (s.empty()?"":" "); s += f.flag; } continue; }
        if (f.value.empty()) continue;
        s += (s.empty()?"":" "); s += f.flag; s += " "; s += f.value;
    }
    return s;
}

/** ★ Write via a temp file and `sudo install`, so a half-written config can never be left behind
 *  if the save is interrupted — the service reads this on every start. */
bool saveConf(std::string& err) {
    char tmp[] = "/tmp/vibeserver.conf.XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) { err = "could not create a temporary file"; return false; }
    FILE* f = fdopen(fd, "w");
    fprintf(f, "# VibeServer options — written by the VibeServer settings screen.\n");
    fprintf(f, "# Edit by hand if you prefer; run `vibeserver` to edit them here.\n\n");
    fprintf(f, "VIBESERVER_ARGS=\"%s\"\n", buildArgs().c_str());
    fclose(f);
    std::string cmd = std::string("sudo install -m644 ") + tmp + " " + CONF + " 2>&1";
    std::string out = run(cmd.c_str());
    unlink(tmp);
    if (!out.empty()) { err = out; return false; }
    return true;
}

std::string svcState()  { return run("systemctl is-active vibeserver 2>/dev/null"); }
std::string svcSince()  { return run("systemctl show vibeserver -p ActiveEnterTimestamp --value 2>/dev/null"); }
std::string radioLine() { return run("lsusb 2>/dev/null | grep -iE 'airspy|rtl|realtek' | sed 's/.*ID [0-9a-f:]* //' | head -1"); }
/** ★★ THE PORT THE SERVICE IS ACTUALLY ON — and not somebody else's.
 *  A bare `ss` scan returned the FIRST listening socket on the box, which on a Pi you just SSH'd
 *  into is port 22: the settings screen cheerfully told you to point a browser at your own SSH
 *  daemon. Take the configured port if there is one, otherwise look only inside the range
 *  VibeServer actually auto-picks from. Never report a port we cannot attribute to ourselves.
 *  ★ `ss -p` cannot name another user's process unless we are root, so filtering by process is not
 *  available here — the port range is. */
std::string listenPort() {
    for (auto& f : fields) if (f.flag && strcmp(f.flag, "--port") == 0 && !f.value.empty()) return f.value;
    std::string p = run("ss -tln 2>/dev/null | grep -oE ':(4800[0-9]|480[1-4][0-9])\\b' | head -1 | tr -d ':'");
    return p;
}
std::string myIp()      { return run("hostname -I 2>/dev/null | awk '{print $1}'"); }

void draw(int sel, const std::string& msg, bool editing) {
    erase();
    int row = 0;
    attron(A_BOLD); mvprintw(row++, 2, "VibeSDR  ·  VibeServer settings"); attroff(A_BOLD);
    mvprintw(row++, 2, "----------------------------------------------------------------");

    // ── Status. What a person actually wants to know first: is it running, and where do I point a
    //    browser? Both are read from the SYSTEM, never assumed.
    std::string st = svcState();
    bool up = (st == "active");
    mvprintw(row, 2, "Service :");
    attron(up ? COLOR_PAIR(1) : COLOR_PAIR(2));
    mvprintw(row++, 12, "%s", up ? "running" : (st.empty() ? "not installed" : st.c_str()));
    attroff(up ? COLOR_PAIR(1) : COLOR_PAIR(2));
    std::string radio = radioLine();
    mvprintw(row++, 2, "Radio   : %s", radio.empty() ? "none detected" : radio.c_str());
    std::string ip = myIp(), port = listenPort();
    if (up && !port.empty())
        mvprintw(row++, 2, "Listen  : http://%s:%s/", ip.c_str(), port.c_str());
    else row++;
    mvprintw(row++, 2, "Since   : %s", svcSince().c_str());
    row++;

    attron(A_BOLD); mvprintw(row++, 2, "Settings"); attroff(A_BOLD);

    // ★★★ SCROLL, OR THE BOTTOM HALF IS UNREACHABLE. The full list is longer than a default 80x24
    // terminal, and an SSH window is exactly where you get 24 lines — so without this the Radio and
    // Advanced sections simply do not exist for anyone who has not resized their window, which is
    // the same class of bug as a control drawn behind another one.
    // ★ Keep the SELECTED row visible and let the rest move around it, rather than paging: the
    // cursor is where attention is, and a list that jumps a page at a time loses your place.
    const int listTop  = row;
    const int reserve  = 5;                                   // help + hint + message lines below
    const int visible  = std::max(4, LINES - listTop - reserve);
    static int scroll  = 0;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + visible) scroll = sel - visible + 1;
    if (scroll > (int)fields.size() - visible) scroll = std::max(0, (int)fields.size() - visible);

    for (size_t i = (size_t)scroll; i < fields.size() && (int)(i - scroll) < visible; i++) {
        if (isHeading(fields[i])) {
            attron(A_BOLD | COLOR_PAIR(4));
            mvprintw(row++, 2, "%s", fields[i].label + 1);
            attroff(A_BOLD | COLOR_PAIR(4));
            continue;
        }
        bool cur = ((int)i == sel);
        if (cur) attron(A_REVERSE);
        mvprintw(row, 2, " %-18s ", fields[i].label);
        attroff(A_REVERSE);
        std::string v = fields[i].value.empty() ? "(default)" : fields[i].value;
        if (cur && editing) { attron(A_UNDERLINE); mvprintw(row, 23, "%s_", fields[i].value.c_str()); attroff(A_UNDERLINE); }
        else mvprintw(row, 23, "%s", v.c_str());
        row++;
    }
    if (scroll > 0 || scroll + visible < (int)fields.size())
        mvprintw(row++, 2, "   … %d of %d", sel + 1, (int)fields.size());
    row++;
    mvprintw(row++, 2, "%s", fields[sel].help);
    row++;
    attron(A_BOLD);
    mvprintw(row++, 2, editing ? "Enter=accept  Esc=cancel"
                               : "↑↓ move   Enter/Space=edit   s=save & restart   r=restart   q=quit");
    attroff(A_BOLD);
    if (!msg.empty()) { row++; attron(COLOR_PAIR(3)); mvprintw(row++, 2, "%s", msg.c_str()); attroff(COLOR_PAIR(3)); }
    refresh();
}

} // namespace

int vibeserverTui() {
    loadConf();
    initscr(); noecho(); cbreak(); keypad(stdscr, TRUE); curs_set(0);
    start_color(); use_default_colors();
    init_pair(1, COLOR_GREEN,  -1);
    init_pair(2, COLOR_RED,    -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_CYAN,   -1);

    int sel = 1; std::string msg; bool editing = false; std::string buf;   // 0 is a heading
    for (;;) {
        draw(sel, msg, editing);
        int c = getch();
        if (editing) {
            if (c == 27) { editing = false; fields[sel].value = buf; msg.clear(); }
            else if (c == '\n' || c == KEY_ENTER) { editing = false; msg = "changed — press s to save and restart"; }
            else if (c == KEY_BACKSPACE || c == 127 || c == 8) { if (!fields[sel].value.empty()) fields[sel].value.pop_back(); }
            else if (c >= 32 && c < 127) fields[sel].value += (char)c;
            continue;
        }
        switch (c) {
            case KEY_UP:
                do { sel = (sel + (int)fields.size() - 1) % (int)fields.size(); } while (isHeading(fields[sel]));
                msg.clear(); break;
            case KEY_DOWN:
                do { sel = (sel + 1) % (int)fields.size(); } while (isHeading(fields[sel]));
                msg.clear(); break;
            case '\n': case KEY_ENTER: case ' ':
                if (fields[sel].isToggle) { fields[sel].value = (fields[sel].value == "off") ? "on" : "off";
                                            msg = "changed — press s to save and restart"; }
                else { editing = true; buf = fields[sel].value; }
                break;
            case 's': {
                // ★ SAY WHAT IS ABOUT TO HAPPEN. A restart disconnects whoever is listening, and a
                // person running a public receiver deserves to know that before it happens rather
                // than after.
                msg = "saving…"; draw(sel, msg, false);
                std::string err;
                if (!saveConf(err)) { msg = "save failed: " + err; break; }
                msg = "saved — restarting (listeners will reconnect)…"; draw(sel, msg, false);
                run("sudo systemctl restart vibeserver 2>&1");
                msg = "applied.";
                break;
            }
            case 'r':
                msg = "restarting…"; draw(sel, msg, false);
                run("sudo systemctl restart vibeserver 2>&1");
                msg = "restarted.";
                break;
            case 'q': case 3: endwin(); return 0;
            default: break;
        }
    }
}
