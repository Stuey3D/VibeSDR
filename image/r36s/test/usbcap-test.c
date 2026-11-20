// usbcap-test.c — vibe-usbcap's ring, trigger and dump on synthetic samples (no I2C, no /dev/mem, no clock).
//   cc -O2 -Wall -Wextra -o /tmp/usbcap-test image/r36s/test/usbcap-test.c && /tmp/usbcap-test
// It includes the program itself with USBCAP_TEST, so what is tested is the code on the card, not a copy.
#define USBCAP_TEST
#include "../native/vibe-usbcap.c"

static char OUT[400000];
static size_t OUTN;
static int LINES;
static void cap_emit(void *ctx, const char *l) {
    (void)ctx;
    size_t n = strlen(l);
    if (OUTN + n + 2 < sizeof OUT) { memcpy(OUT + OUTN, l, n); OUTN += n; OUT[OUTN++] = '\n'; OUT[OUTN] = 0; }
    LINES++;
}
static int fake_regs(void *ctx, uint8_t *pe, uint8_t *bc) { (void)ctx; *pe = 0x46; *bc = 0x09; return 0; }

static int P, F;
static void ok(int c, const char *what) {
    if (c) P++; else F++;
    printf("   %s %s\n", c ? "ok  " : "FAIL", what);
}
static int count(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p++) n++;
    return n;
}

static struct cap C;
static int64_t T;
static struct smp S0;
static void reset(void) {
    cap_init(&C, 1000, 1000000, 1000000);
    C.emit = cap_emit; C.regs = fake_regs;
    C.nwatch = 1; strcpy(C.watch[0], "1-1");
    OUTN = 0; OUT[0] = 0; LINES = 0;
    T = 100000000;   // 100 s
    memset(&S0, 0, sizeof S0);
    S0.cout = 0xa0; S0.cin = 0xdd; S0.sts = 0xb3; S0.sys = 0x40;
    S0.bv = 3720; S0.bc = 1366; S0.uv = 3330; S0.hprt = 0x00001405; S0.present = 1;
}
static int run(int n, const struct smp *s) {   // n samples, 1 ms apart
    int d = 0;
    for (int i = 0; i < n; i++) { struct smp x = *s; x.t = T; d += cap_push(&C, &x); T += 1000; }
    return d;
}

int main(void) {
    // ── steady: nothing printed ──
    reset();
    ok(run(5000, &S0) == 0 && OUTN == 0, "steady samples: no output at all");
    ok(C.n == 5000, "the ring fills");
    run(5000, &S0);
    ok(C.n == RING, "★ the ring is bounded (RING samples)");

    // ── a drop: a 3 ms port sag + the clamp bit, then the OTG port loses the device, sysfs follows 20 ms later ──
    reset();
    run(3000, &S0);
    struct smp sag = S0; sag.uv = 3000; sag.sts = 0xb7;
    run(3, &sag);
    struct smp gone = S0; gone.hprt = 0x00001000 | (0u << 10);   // PWR only: no CONNSTS, no ENA
    int64_t t_drop = T;
    run(20, &gone);
    struct smp gone2 = gone; gone2.present = 0;
    run(300, &gone2);
    struct smp back = S0;
    int dumped = run(690, &back);
    ok(dumped == 0, "the window stays open until 1 s after the LAST event (sysfs at +20 ms → +1.020 s)");
    dumped = run(200, &back);
    ok(dumped == 1, "★ one dump, then");
    ok(strstr(OUT, "DUMP n=1 ") != NULL, "a DUMP header");
    char want[64];
    snprintf(want, sizeof want, "t0=%lld ", (long long)t_drop);
    ok(strstr(OUT, want) != NULL, "t0 = the sample where HPRT0's connect bit fell");
    ok(strstr(OUT, "why=otg-port-disconnect,usb-1-1-gone@+0.0200") != NULL, "★ both events, the sysfs one 20 ms after the port's");
    ok(strstr(OUT, "pre_us=1000000 ") != NULL || strstr(OUT, "pre_us=999") != NULL, "1 s before");
    ok(strstr(OUT, "post_us=1020000 ") != NULL, "1 s after the second event");
    ok(strstr(OUT, "med_us=1000 ") && strstr(OUT, "max_gap_us=1000 "), "the real interval in the header (median, max gap)");
    ok(strstr(OUT, "pwr_en3=46 boost_cfg=09") != NULL, "the regulator enables read at dump time");
    ok(strstr(OUT, "S -1000000 a0 dd b3 40 0e88 0556 0d02 00001405 1 0") != NULL, "★ the first sample, raw registers, 1 s before");
    ok(strstr(OUT, "S -3000 a0 dd b7 40 0e88 0556 0bb8 00001405 1 0") != NULL, "the sag 3 ms before (USB_VOL 0x0bb8, the clamp bit)");
    ok(strstr(OUT, "R 2 -1000") != NULL, "the rest of the sag as a run");
    ok(strstr(OUT, "S +0 a0 dd b3 40 0e88 0556 0d02 00001000 1 0") != NULL, "the port without its device at +0");
    ok(strstr(OUT, "S +20000 ") != NULL && strstr(OUT, " 00001000 0 0\n") != NULL, "sysfs presence gone at +20 ms");
    ok(strstr(OUT, "S +320000 a0 dd b3 40 0e88 0556 0d02 00001405 1 0") != NULL, "back at +320 ms");
    ok(strstr(OUT, "END n=1 lines=") != NULL, "an END line");
    ok(LINES < 20, "compact: identical samples collapsed into R lines");
    char *bad = strstr(OUT, "S -1000001");
    ok(bad == NULL, "nothing older than the pre window");

    // ── line levels (LNSTS) flicker: not a change ──
    reset();
    run(1500, &S0);
    OUTN = 0; LINES = 0;
    for (int i = 0; i < 500; i++) { struct smp x = S0; x.hprt = (S0.hprt & ~HPRT_LNSTS_MASK) | ((uint32_t)(i & 3) << 10); x.t = T; cap_push(&C, &x); T += 1000; }
    struct smp g = S0; g.hprt = 0x1000; run(1100, &g);
    ok(count(OUT, "\nS ") + (strncmp(OUT, "S ", 2) == 0) <= 3, "★ LNSTS flicker does not break a run (D+/D- follow the traffic)");

    // ── a second drop inside the window extends it ──
    reset();
    run(1500, &S0);
    struct smp d1 = S0; d1.present = 0; run(300, &d1);
    run(400, &S0);          // back at +0.3
    run(300, &d1);          // gone again at +0.7
    run(1500, &S0);
    ok(count(OUT, "DUMP ") == 1 && strstr(OUT, "why=usb-1-1-gone,usb-1-1-gone@+0.7000") != NULL, "★ a second drop in the window joins the same dump");
    ok(strstr(OUT, "post_us=1700000") != NULL, "and the window runs on 1 s past it");

    // ── no HPRT0 (no /dev/mem): sysfs alone triggers, '-' in place of the register ──
    reset();
    struct smp nh = S0; nh.hprt = HPRT_NONE; run(1200, &nh);
    struct smp nh0 = nh; nh0.present = 0; run(1100, &nh0);
    ok(strstr(OUT, "why=usb-1-1-gone ") != NULL && strstr(OUT, " - 1 0\n") != NULL && strstr(OUT, " - 0 0\n") != NULL,
       "without HPRT0: the sysfs presence triggers and the column reads '-'");

    // ── stopped mid-window: the dump comes out cut ──
    reset();
    run(1200, &S0);
    run(100, &d1);
    cap_stop(&C);
    ok(strstr(OUT, "cut=1") != NULL && strstr(OUT, "post_us=99000") != NULL, "★ SIGTERM mid-window: the dump is printed, marked cut");

    // ── the cap: DUMP_MAX per 10 min ──
    reset();
    run(1200, &S0);
    for (int i = 0; i < 14; i++) { run(50, &d1); run(1200, &S0); }
    ok(count(OUT, "DUMP ") == DUMP_MAX && count(OUT, "SKIP ") == 14 - DUMP_MAX, "★ at most 10 dumps per 10 min, the rest SKIP lines");
    T += DUMP_WINDOW_US;
    OUTN = 0; run(50, &d1); run(1200, &S0);
    ok(count(OUT, "DUMP n=11 ") == 1, "10 min later dumps resume");

    // ── a read error is a change of its own ──
    reset();
    run(1200, &S0);
    struct smp e = S0; e.err = 1; e.bv = e.uv = 0; e.bc = 0; run(2, &e);
    run(10, &d1); run(1100, &S0);
    ok(strstr(OUT, " 1 1\n") != NULL, "a failed PMIC read is shown (err 1)");

    printf("   %d passed, %d failed\n", P, F);
    return F ? 1 : 0;
}
