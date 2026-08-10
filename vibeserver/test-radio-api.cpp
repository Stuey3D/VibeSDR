// ★★★ THE RADIO LIST A HOST WRITES ITS CONFIG FROM.
//
// Full mode's GUI turns this into a `radios[]` config, and the front door forks one process per
// radio BY SERIAL. So a wrong answer here does not crash anything — it points a radio's saved
// settings at different hardware, which is far worse and completely silent.
//
// ★★ IT MUST PASS WITH NO RADIO ATTACHED. That is the state of every CI machine and most dev
//    Macs, and "0 radios" is exactly when a GUI is most likely to ask about radio 0 anyway.
#include "vibeserver_api.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
static void ok(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", what);
    if (!cond) failures++;
}

int main() {
    std::printf("radio list API — identity, not just a name\n");

    vs_radios_refresh();
    const int n = vs_radio_count();
    std::printf("  (%d radio(s) attached)\n", n);
    ok(n >= 0, "the count is not negative");

    for (int i = 0; i < n; i++) {
        const char* drv = vs_radio_driver(i);
        // ★ The driver is what decides which start path a radio takes, so an unknown one is a
        //   radio that cannot be served at all.
        const bool known = !std::strcmp(drv, "rtlsdr") || !std::strcmp(drv, "sdrplay")
                        || !std::strcmp(drv, "airspyhf");
        ok(known, "every radio reports a driver we can actually start");
        ok(std::strlen(vs_radio_name(i)) > 0, "every radio has a display name");
    }

    // ★★★ OUT OF RANGE MUST BE EMPTY, NOT STALE. These return a pointer into ONE shared buffer,
    //     so the dangerous failure is returning the PREVIOUS radio's serial for an index that
    //     does not exist — a GUI would then write a config naming hardware it never asked about.
    if (n > 0) (void)vs_radio_serial(n - 1);          // prime the buffer with a real value
    ok(std::strlen(vs_radio_serial(n))    == 0, "★ one past the end is empty, not the last one");
    ok(std::strlen(vs_radio_serial(999))  == 0, "far out of range is empty");
    ok(std::strlen(vs_radio_serial(-1))   == 0, "a negative index is empty");
    ok(std::strlen(vs_radio_driver(-1))   == 0, "negative index: driver too");
    ok(std::strlen(vs_radio_name(999))    == 0, "far out of range: name too");

    // Collision reporting must answer even with nothing attached (it is asked before serving).
    const int collide = vs_radio_serials_collide();
    ok(collide == 0 || collide == 1, "the serial-collision answer is a clean boolean");
    if (n < 2) ok(collide == 0, "fewer than two radios cannot collide");

    std::printf(failures ? "\n\033[31m%d failed\033[0m\n" : "\n\033[32mpassed\033[0m\n", failures);
    return failures ? 1 : 0;
}
