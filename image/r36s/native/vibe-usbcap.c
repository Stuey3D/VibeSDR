// vibe-usbcap — the R36S's FAST USB-drop capture (handheld81, 2026-11-20). Diagnosis only: it READS, it never writes.
//
// ★★★ WHY. On the h80 card (2026-11-19/20) the RTL-SDR on the OTG port (USB 1-1, dwc2 ff300000.usb) dropped three times
//     for 0.5 s at 179.9 / 182.9 / 186.9 s of uptime with the charger in and the battery in the CV tail (charge current
//     ~0.24 A, termination 0.20 A): "dwc2 ff300000.usb: Not connected", usb_submit_urb -19, "USB disconnect", back
//     ~0.3 s later. vibe-hwd's 20 ms trace (FastTrace) showed NOTHING change — port 4.844 V, battery 4.170 V +0.235 A,
//     cccv, CHRG_IN 0xdd, no input clamp. So whatever happens is shorter than 20 ms. This samples about every 1 ms,
//     keeps a rolling ring, and when the device drops it prints the ring around the drop for vibe-hwd to log.
// ★★ WHO RUNS IT: vibe-hwd (FastCapture), only while the cord is in, the charge current is low and a USB device is on
//    the bus — the risky state. It prints line records on stdout (READY / STAT / DUMP … S / R … END / SKIP / ERR);
//    vibe-hwd turns them into "chgtrace-fast" log lines with ITS OWN scaling (the same code as the 20 ms trace), so
//    the volts and amps here and there can never disagree. This program does no scaling at all: raw registers out.
// ★★ WHAT IS READ, every sample (addresses: mainline include/linux/mfd/rk808.h, RK817 section):
//      one I2C block read 0xE4..0xF0 (13 bytes): RK817_PMIC_CHRG_OUT 0xE4, RK817_PMIC_CHRG_IN 0xE5,
//        RK817_PMIC_CHRG_STS 0xEB (bit 7 RK817_BAT_EXS, bits 6:4 RK817_CHG_STS — mainline rk817_charger.c reads both;
//        bits 3:0 BAT_OVP / CHRG_IN_CLAMP / USB_EXS / USB_EFF are the Rockchip BSP driver's reg_fields, NOT in mainline),
//        RK817_SYS_STS 0xF0 (bit 6 RK817_PLUG_IN_STS — mainline rk817_charger.c rk817_charger_probe reads it for
//        "plugged_in"). The bytes between are read because one transfer costs less bus time than four; they are dropped.
//      one I2C block read 0x78..0x7F (8 bytes): RK817_GAS_GAUGE_BAT_VOL_H/L 0x78/79, BAT_CUR_H/L 0x7A/7B (signed),
//        USB_VOL_H/L 0x7E/7F (0x7C/7D are not defined in rk808.h and are dropped).
//      the dwc2 host port register HPRT0 (base + 0x440; mainline drivers/usb/dwc2/hw.h: bit 0 CONNSTS, 1 CONNDET,
//        2 ENA, 3 ENACHG, 4 OVRCURRACT, 5 OVRCURRCHG, 8 RST, 11:10 LNSTS (the D+/D- line levels), 12 PWR, 18:17 SPD)
//        through /dev/mem — ONE 32-bit load, the controller's own view of the port at that instant — and only when
//        GSNPSID (base + 0x40) reads 0x4F54xxxx (it is a dwc2 there) and vibe-hwd found the dwc2 driver bound to it
//        (Rockchip's dwc2 params: power_down NONE, no_clock_gating — mainline dwc2/params.c dwc2_set_rk_params — so the
//        controller is clocked whenever the driver is bound). /dev/mem refused → no HPRT0, the sysfs presence still runs.
//      the presence of each watched USB device (/sys/bus/usb/devices/<name>, one access() each).
//    Once at start and once per dump (not every sample — they are the regulators' software enables, not live states):
//      RK817_POWER_EN_REG(3) 0xB4 (mainline rk808-regulator.c: bit 1 BOOST, bit 2 OTG_SWITCH) and RK817_BOOST_OTG_CFG 0xDE.
//      VCALIB0/1 0x93..0x96 once at start (READY), for vibe-hwd's volts.
// ★★★ THE BUS IS SHARED SAFELY: every read is ONE I2C_SMBUS ioctl (I2C_SMBUS_I2C_BLOCK_DATA / BYTE_DATA) — the register
//     pointer write and the read go to the adapter as ONE i2c_transfer with a repeated start, under the adapter's bus
//     lock — exactly how vibe-hwd reads and writes (its _smbus / PmicFd). So neither vibe-hwd's writes, its 20 ms
//     reads nor the kernel's own rk808 traffic can land between our pointer write and our read (handheld62's two-call
//     reads did exactly that). This program never writes a PMIC register: vibe-hwd stays the only writer.
// ★ BOUNDED: one fixed ring (RING samples, ~8 s at 1 ms), no allocation after start, at most DUMP_MAX dumps per
//   DUMP_WINDOW_US. Its own cost (samples, real interval, I2C time, CPU) is printed every STAT_US as a STAT line.
// ★ Timing: clock_nanosleep(TIMER_ABSTIME) on CLOCK_MONOTONIC (the kernel log's clock, near enough); a late tick is not
//   caught up (no bursts of back-to-back reads). A sample's time is the middle of its reads.
// Build: gcc -O2 -Wall -Wextra -o vibe-usbcap vibe-usbcap.c (image/r36s/build-image.sh, in the trixie arm64 container).
// Test:  image/r36s/test/usbcap-test.c includes this file with USBCAP_TEST (the ring / trigger / dump on synthetic data).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RING 8192                 // samples kept (~8 s at 1 ms): the pre window + the longest post window fit easily
#define MAX_WATCH 8
#define DUMP_MAX 10
#define DUMP_WINDOW_US 600000000LL // at most DUMP_MAX dumps in 10 min (a flapping radio cannot flood the log)
#define STAT_US 30000000LL         // a STAT line every 30 s
#define HPRT_NONE 0xFFFFFFFFu      // "HPRT0 not read" (no /dev/mem, or not a dwc2)
#define HPRT_CONNSTS 0x1u
#define HPRT_LNSTS_MASK (0x3u << 10)

struct smp {
    int64_t t;                    // µs, CLOCK_MONOTONIC
    uint16_t bv, uv;              // BAT_VOL, USB_VOL raw
    int16_t bc;                   // BAT_CUR raw (signed)
    uint8_t cout, cin, sts, sys;  // CHRG_OUT, CHRG_IN, CHRG_STS, SYS_STS
    uint8_t err;                  // 1 = a PMIC read failed (the register fields are then stale/zero)
    uint32_t hprt;                // HPRT0 or HPRT_NONE
    uint32_t present;             // bit i = watch[i] present in sysfs
};

struct cap {
    // config
    int64_t period_us, pre_us, post_us, max_post_us;
    int nwatch;
    char watch[MAX_WATCH][32];
    // the ring
    struct smp ring[RING];
    size_t head, n;               // next write index; how many valid
    struct smp prev;
    int have_prev;
    // a drop being captured
    int armed;
    int64_t t0, end;
    char why[512];
    int dump_n, skipped;
    int64_t dump_t[DUMP_MAX];
    int dump_k;
    // per-dump read time (µs) and regulator enables, provided by the I/O side
    int64_t read_us_sum, read_us_n;
    // output
    void (*emit)(void *ctx, const char *line);
    void *emit_ctx;
    int (*regs)(void *ctx, uint8_t *pwr_en3, uint8_t *boost_cfg);   // 0 = ok
    void *regs_ctx;
};

static void emitf(struct cap *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void emitf(struct cap *c, const char *fmt, ...) {
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    c->emit(c->emit_ctx, b);
}

static void cap_init(struct cap *c, int64_t period_us, int64_t pre_us, int64_t post_us) {
    memset(c, 0, sizeof *c);
    c->period_us = period_us;
    c->pre_us = pre_us;
    c->post_us = post_us;
    c->max_post_us = 3 * post_us;   // a second drop inside the window extends it, never past 3× (bounded)
}

// ★ Two samples are "the same" when every register, the port state (but its line levels: LNSTS follows the D+/D-
//   wires and flickers with high-speed traffic) and the presence agree — the collapse key for R (run) lines.
static int same(const struct smp *a, const struct smp *b) {
    uint32_t ha = a->hprt == HPRT_NONE ? a->hprt : (a->hprt & ~HPRT_LNSTS_MASK);
    uint32_t hb = b->hprt == HPRT_NONE ? b->hprt : (b->hprt & ~HPRT_LNSTS_MASK);
    return a->bv == b->bv && a->uv == b->uv && a->bc == b->bc && a->cout == b->cout && a->cin == b->cin &&
           a->sts == b->sts && a->sys == b->sys && a->err == b->err && ha == hb && a->present == b->present;
}

static const struct smp *ring_at(const struct cap *c, size_t i) {   // i = 0 oldest … n-1 newest
    return &c->ring[(c->head + RING - c->n + i) % RING];
}

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

static void put_sample(struct cap *c, char kind, const struct smp *s) {
    if (s->hprt == HPRT_NONE)
        emitf(c, "%c %+" PRId64 " %02x %02x %02x %02x %04x %04x %04x - %x %d", kind, s->t - c->t0, s->cout, s->cin, s->sts,
              s->sys, s->bv, (uint16_t)s->bc, s->uv, s->present, s->err);
    else
        emitf(c, "%c %+" PRId64 " %02x %02x %02x %02x %04x %04x %04x %08x %x %d", kind, s->t - c->t0, s->cout, s->cin,
              s->sts, s->sys, s->bv, (uint16_t)s->bc, s->uv, s->hprt, s->present, s->err);
}

// The dump: every sample in [t0 − pre, end] — an S line whenever something changed, an R line for a run of the same
// ("R <count> <time of the last>"). The header carries the real sampling interval over this window.
static void cap_dump(struct cap *c, int cut) {
    static int64_t gaps[RING];
    size_t first = c->n, k = 0, i;
    for (i = 0; i < c->n; i++)
        if (ring_at(c, i)->t >= c->t0 - c->pre_us) { first = i; break; }
    size_t cnt = c->n - first;
    for (i = first + 1; i < c->n; i++)
        gaps[k++] = ring_at(c, i)->t - ring_at(c, i - 1)->t;
    int64_t med = 0, mx = 0;
    if (k) {
        qsort(gaps, k, sizeof gaps[0], cmp_i64);
        med = gaps[k / 2];
        mx = gaps[k - 1];
    }
    int64_t pre = 0, post = 0;
    if (cnt) {
        pre = c->t0 - ring_at(c, first)->t;
        post = ring_at(c, c->n - 1)->t - c->t0;
    }
    uint8_t pe = 0, bc = 0;
    int regs_ok = c->regs && c->regs(c->regs_ctx, &pe, &bc) == 0;
    char regs[64];
    if (regs_ok) snprintf(regs, sizeof regs, "pwr_en3=%02x boost_cfg=%02x", pe, bc);
    else snprintf(regs, sizeof regs, "pwr_en3=? boost_cfg=?");
    emitf(c, "DUMP n=%d t0=%" PRId64 " why=%s pre_us=%" PRId64 " post_us=%" PRId64 " samples=%zu med_us=%" PRId64
          " max_gap_us=%" PRId64 " read_us=%" PRId64 " cut=%d %s", c->dump_n, c->t0, c->why, pre, post, cnt, med, mx,
          c->read_us_n ? c->read_us_sum / c->read_us_n : 0, cut, regs);
    const struct smp *last = NULL;
    int run = 0;
    const struct smp *run_last = NULL;
    size_t lines = 0;
    for (i = first; i < c->n; i++) {
        const struct smp *s = ring_at(c, i);
        if (last && same(s, last)) { run++; run_last = s; continue; }
        if (run) { emitf(c, "R %d %+" PRId64, run, run_last->t - c->t0); lines++; run = 0; }
        put_sample(c, 'S', s);
        lines++;
        last = s;
    }
    if (run) { emitf(c, "R %d %+" PRId64, run, run_last->t - c->t0); lines++; }
    emitf(c, "END n=%d lines=%zu", c->dump_n, lines);
}

// One sample in. Returns 1 when a dump was printed.
static int cap_push(struct cap *c, const struct smp *s) {
    char ev[200];
    ev[0] = 0;
    if (c->have_prev) {
        int off = 0;
        if (c->prev.hprt != HPRT_NONE && s->hprt != HPRT_NONE && (c->prev.hprt & HPRT_CONNSTS) && !(s->hprt & HPRT_CONNSTS))
            off += snprintf(ev + off, sizeof ev - off, "%sotg-port-disconnect", off ? "," : "");
        uint32_t gone = c->prev.present & ~s->present;
        for (int w = 0; w < c->nwatch && off < (int)sizeof ev - 40; w++)
            if (gone & (1u << w))
                off += snprintf(ev + off, sizeof ev - off, "%susb-%s-gone", off ? "," : "", c->watch[w]);
    }
    c->prev = *s;
    c->have_prev = 1;
    c->ring[c->head] = *s;
    c->head = (c->head + 1) % RING;
    if (c->n < RING) c->n++;
    if (ev[0]) {
        if (c->armed) {
            // ★ a second event inside the window: noted with its offset, the window runs on (never past max_post_us)
            size_t l = strlen(c->why);
            snprintf(c->why + l, sizeof c->why - l, ",%s@%+.4f", ev, (s->t - c->t0) / 1e6);
            int64_t e = s->t + c->post_us, lim = c->t0 + c->max_post_us;
            if (e > c->end) c->end = e < lim ? e : lim;
        } else {
            int k = 0;
            for (int j = 0; j < c->dump_k; j++)
                if (s->t - c->dump_t[j] < DUMP_WINDOW_US) c->dump_t[k++] = c->dump_t[j];
            c->dump_k = k;
            if (c->dump_k >= DUMP_MAX) {
                c->skipped++;
                emitf(c, "SKIP t=%" PRId64 " why=%s dumps=%d skipped=%d", s->t, ev, DUMP_MAX, c->skipped);
            } else {
                c->dump_t[c->dump_k++] = s->t;
                c->armed = 1;
                c->dump_n++;
                c->t0 = s->t;
                c->end = s->t + c->post_us;
                snprintf(c->why, sizeof c->why, "%s", ev);
            }
        }
    }
    if (c->armed && s->t >= c->end) {
        cap_dump(c, 0);
        c->armed = 0;
        return 1;
    }
    return 0;
}

// Stopping (SIGTERM from vibe-hwd): a drop still collecting its "after" is printed with what it has (cut=1).
static void cap_stop(struct cap *c) {
    if (c->armed) {
        cap_dump(c, 1);
        c->armed = 0;
    }
}

#ifndef USBCAP_TEST
// ─────────────────────────────────────────── the device side ───────────────────────────────────────────────────────
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>

#define PMIC_ADDR 0x20

static volatile sig_atomic_t g_stop;
static void on_term(int sig) { (void)sig; g_stop = 1; }

static int64_t now_us(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// ★ ONE transfer per read (see the top): I2C_SMBUS with I2C_SMBUS_I2C_BLOCK_DATA — the same ioctl vibe-hwd's PmicFd uses.
static int i2c_block(int fd, uint8_t reg, uint8_t n, uint8_t *out) {
    union i2c_smbus_data d;
    struct i2c_smbus_ioctl_data a = {.read_write = I2C_SMBUS_READ, .command = reg, .size = I2C_SMBUS_I2C_BLOCK_DATA, .data = &d};
    d.block[0] = n;
    if (ioctl(fd, I2C_SMBUS, &a) < 0) return -1;
    memcpy(out, &d.block[1], n);
    return 0;
}

static int i2c_byte(int fd, uint8_t reg, uint8_t *out) {
    union i2c_smbus_data d;
    struct i2c_smbus_ioctl_data a = {.read_write = I2C_SMBUS_READ, .command = reg, .size = I2C_SMBUS_BYTE_DATA, .data = &d};
    if (ioctl(fd, I2C_SMBUS, &a) < 0) return -1;
    *out = d.byte;
    return 0;
}

struct io {
    int fd;
    volatile uint32_t *dwc;       // the dwc2 register page, or NULL
    char paths[MAX_WATCH][96];
    int nwatch;
};

static int io_regs(void *ctx, uint8_t *pe, uint8_t *bc) {
    struct io *io = ctx;
    return (i2c_byte(io->fd, 0xB4, pe) || i2c_byte(io->fd, 0xDE, bc)) ? -1 : 0;
}

static void io_emit(void *ctx, const char *line) {
    (void)ctx;
    // ★ whole lines, unbuffered by stdio: vibe-hwd reads them as they come (a STAT line must not sit in a buffer)
    size_t l = strlen(line);
    char b[640];
    if (l > sizeof b - 2) l = sizeof b - 2;
    memcpy(b, line, l);
    b[l++] = '\n';
    ssize_t off = 0;
    while (off < (ssize_t)l) {
        ssize_t w = write(1, b + off, l - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            _exit(0);             // vibe-hwd has gone: nothing to report to
        }
        off += w;
    }
}

static void read_sample(struct io *io, struct smp *s) {
    uint8_t a[13], g[8];
    memset(s, 0, sizeof *s);
    if (i2c_block(io->fd, 0xE4, 13, a) == 0) {
        s->cout = a[0]; s->cin = a[1]; s->sts = a[7]; s->sys = a[12];
    } else s->err = 1;
    if (i2c_block(io->fd, 0x78, 8, g) == 0) {
        s->bv = (uint16_t)(g[0] << 8 | g[1]);
        s->bc = (int16_t)(uint16_t)(g[2] << 8 | g[3]);
        s->uv = (uint16_t)(g[6] << 8 | g[7]);
    } else s->err = 1;
    s->hprt = io->dwc ? io->dwc[0x440 / 4] : HPRT_NONE;
    for (int w = 0; w < io->nwatch; w++)
        if (access(io->paths[w], F_OK) == 0) s->present |= 1u << w;
}

static void usage(void) {
    fprintf(stderr, "usage: vibe-usbcap --i2c /dev/i2c-N --watch 1-1[,1-1.2] [--dwc2 0xff300000] [--period-us 1000]\n"
                    "                   [--pre-ms 1000] [--post-ms 1000] [--sysfs /sys] [--samples N (bench: stop after N)]\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *i2c = NULL, *watch = NULL, *sysfs = "/sys";
    unsigned long long dwc2 = 0;
    long period = 1000, pre_ms = 1000, post_ms = 1000, max_samples = 0;
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) usage();
        if (!strcmp(argv[i], "--i2c")) i2c = argv[++i];
        else if (!strcmp(argv[i], "--watch")) watch = argv[++i];
        else if (!strcmp(argv[i], "--dwc2")) dwc2 = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--period-us")) period = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--pre-ms")) pre_ms = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--post-ms")) post_ms = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--sysfs")) sysfs = argv[++i];
        else if (!strcmp(argv[i], "--samples")) max_samples = strtol(argv[++i], NULL, 0);
        else usage();
    }
    if (!i2c || !watch || period < 200 || period > 100000 || pre_ms < 100 || pre_ms > 2000 || post_ms < 100 || post_ms > 2000)
        usage();
    // ★ the ring must hold the pre window and the longest post window (3 × post) at this period
    if ((pre_ms + 3 * post_ms) * 1000 / period >= RING - 16) usage();
    // ★ a diagnostic: the ordinary priority (vibe-hwd runs at nice −10 for the power button; this must not outrank the audio)
    setpriority(PRIO_PROCESS, 0, 0);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    struct sigaction sa = {0};
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    static struct cap c;
    static struct io io;
    cap_init(&c, period, pre_ms * 1000, post_ms * 1000);
    c.emit = io_emit;
    c.regs = io_regs;
    c.regs_ctx = &io;
    char wl[256];
    snprintf(wl, sizeof wl, "%s", watch);
    for (char *sp = NULL, *t = strtok_r(wl, ",", &sp); t && c.nwatch < MAX_WATCH; t = strtok_r(NULL, ",", &sp)) {
        snprintf(c.watch[c.nwatch], sizeof c.watch[0], "%s", t);
        snprintf(io.paths[c.nwatch], sizeof io.paths[0], "%s/bus/usb/devices/%s", sysfs, t);
        c.nwatch++;
    }
    io.nwatch = c.nwatch;
    io.fd = open(i2c, O_RDWR | O_CLOEXEC);
    if (io.fd < 0 || ioctl(io.fd, I2C_SLAVE_FORCE, PMIC_ADDR) < 0) {
        emitf(&c, "ERR i2c %s: %s", i2c, strerror(errno));
        return 1;
    }
    uint8_t vc[4];
    if (i2c_block(io.fd, 0x93, 4, vc) < 0) {
        emitf(&c, "ERR i2c read VCALIB: %s", strerror(errno));
        return 1;
    }
    char hp[160] = "off(no --dwc2)";
    if (dwc2) {
        int mf = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
        if (mf < 0) snprintf(hp, sizeof hp, "off(/dev/mem: %s)", strerror(errno));
        else {
            void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mf, (off_t)dwc2);
            close(mf);
            if (m == MAP_FAILED) snprintf(hp, sizeof hp, "off(mmap: %s)", strerror(errno));
            else {
                volatile uint32_t *r = m;
                uint32_t id = r[0x40 / 4];
                if ((id >> 16) != 0x4F54) {
                    snprintf(hp, sizeof hp, "off(GSNPSID %08x is not a dwc2)", id);
                    munmap(m, 4096);
                } else {
                    io.dwc = r;
                    snprintf(hp, sizeof hp, "ok(GSNPSID %08x)", id);
                }
            }
        }
    }
    uint8_t pe = 0, bc = 0;
    int rok = io_regs(&io, &pe, &bc) == 0;
    emitf(&c, "READY period_us=%ld pre_us=%ld post_us=%ld vcalib=%02x%02x,%02x%02x watch=%s hprt=%s pwr_en3=%s%02x boost_cfg=%s%02x",
          period, pre_ms * 1000, post_ms * 1000, vc[0], vc[1], vc[2], vc[3], watch, hp, rok ? "" : "?", pe, rok ? "" : "?", bc);

    int64_t next = now_us(CLOCK_MONOTONIC), stat_t0 = next, cpu0 = now_us(CLOCK_PROCESS_CPUTIME_ID);
    int64_t st_n = 0, st_gap_max = 0, st_over = 0, st_read_sum = 0, st_read_max = 0, last_t = 0, done = 0;
    while (!g_stop) {
        struct smp s;
        int64_t r0 = now_us(CLOCK_MONOTONIC);
        read_sample(&io, &s);
        int64_t r1 = now_us(CLOCK_MONOTONIC);
        s.t = (r0 + r1) / 2;
        c.read_us_sum += r1 - r0; c.read_us_n++;
        if (c.read_us_n >= 1000000) { c.read_us_sum /= 2; c.read_us_n /= 2; }
        if (last_t) {
            int64_t g = s.t - last_t;
            if (g > st_gap_max) st_gap_max = g;
            if (g > 2 * period) st_over++;
        }
        last_t = s.t;
        st_n++;
        st_read_sum += r1 - r0;
        if (r1 - r0 > st_read_max) st_read_max = r1 - r0;
        cap_push(&c, &s);
        if (r1 - stat_t0 >= STAT_US) {
            int64_t cpu = now_us(CLOCK_PROCESS_CPUTIME_ID);
            emitf(&c, "STAT wall_us=%" PRId64 " samples=%" PRId64 " mean_us=%" PRId64 " max_gap_us=%" PRId64 " over2x=%" PRId64
                  " read_us=%" PRId64 " read_max_us=%" PRId64 " cpu_us=%" PRId64, r1 - stat_t0, st_n,
                  st_n > 1 ? (r1 - stat_t0) / st_n : 0, st_gap_max, st_over, st_n ? st_read_sum / st_n : 0, st_read_max, cpu - cpu0);
            stat_t0 = r1; cpu0 = cpu;
            st_n = st_gap_max = st_over = st_read_sum = st_read_max = 0;
        }
        if (max_samples && ++done >= max_samples) break;
        next += period;
        int64_t now = now_us(CLOCK_MONOTONIC);
        if (next < now) next = now;   // ★ late: no catching up
        struct timespec ts = {.tv_sec = next / 1000000, .tv_nsec = (next % 1000000) * 1000};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !g_stop) {}
    }
    if (max_samples) {   // the bench's own summary (STAT comes only every 30 s)
        int64_t cpu = now_us(CLOCK_PROCESS_CPUTIME_ID);
        int64_t wall = now_us(CLOCK_MONOTONIC) - stat_t0;
        emitf(&c, "STAT wall_us=%" PRId64 " samples=%" PRId64 " mean_us=%" PRId64 " max_gap_us=%" PRId64 " over2x=%" PRId64
              " read_us=%" PRId64 " read_max_us=%" PRId64 " cpu_us=%" PRId64, wall, st_n, st_n > 1 ? wall / st_n : 0,
              st_gap_max, st_over, st_n ? st_read_sum / st_n : 0, st_read_max, cpu - cpu0);
    }
    cap_stop(&c);
    emitf(&c, "BYE");
    return 0;
}
#endif
