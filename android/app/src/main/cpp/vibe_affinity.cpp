// vibe_affinity.cpp — the app's threads on the right cores, at the right priority (Android client).
//
// ★★★ STUART'S PI 2 MOVE, ON THE PHONE (2026-10-12): "break down its entire workload into individual threads and
//   spread them across the cores with proper priority levels". Perfetto on the XCover 4s (2× A73 + 6× A53, default
//   skin, main SDR screen, hands-off, 12 s): the RenderThread ran two-thirds of its frame time on an A53, and the UI
//   thread and RenderThread each took ~14 ms of a 16.7 ms frame. Pinning both to the A73s: janky frames 92.9 → 43.7 /
//   45.6 % (two runs), p50 23 → 15 ms, RenderThread 14.4 → 9.2 ms a frame, GPU-buffer waits 3.0 → 0.5 ms.
//   What was tried and dropped, measured the same way:
//   ✗ JS → the A53s only: JS at 74 % of an A53, tuning LAGGY and the network meter at 1/3 (it is measured in JS).
//   ✗ RenderThread only: JS drifted onto the A73s beside it — 55 % janky, p99 53 ms.
//
// ★★ WHAT THE SWEEP DOES, every SWEEP_MS while armed (the SDR screen, in the foreground):
//   · UI thread + RenderThread → the FRAME cores. (SkiaRender — every Skia canvas's draw, our react-native-skia
//     patch — is left FREE: see the sweep.)
//   · ★ A thread STARTED BY the UI thread INHERITS its mask (Linux). Pinning the UI thread put the tunnel WebSocket,
//     AudioTrack, vibesdr-ext and Okio on the two A73s with it — 11 threads — and the tuning drums felt "slower and
//     heavier" (Stuart). Every other thread found with the frame mask is handed back to the OTHER cores.
//   · ★ Android moves an app between cpusets on background/foreground and that RESETS every mask — the next sweep
//     after the return puts them back. Disarmed in the background: nothing runs there.
//   · Priorities — the Pi 2's order, NETWORK > AUDIO > SPECTRUM > DECODERS (vibe_thread.h): an OkHttp connection
//     thread (its name ends "/...": OkHttp redacts the URL) is raised to NET_NICE; Fresco's image decoders are
//     lowered to BULK_NICE. Audio threads already run at urgent-audio priority and are not touched.
//
// ★★ WHICH CORES ARE THE FRAME CORES:
//   · big.LITTLE: every core of at least half the strongest's capacity (prime + mid on a 1+3+4 phone, never the
//     prime alone). Capacity from the kernel's cpu_capacity (Moto G35, Tab A9); where there is none, the CPU part
//     from /proc/cpuinfo (the XCover's Samsung 4.4 kernel — and its A73s' max clock is LOWER than its A53s', so the
//     clock cannot decide). Other threads: EVERY core — JS left free measured best.
//   · ★ ALL CORES ALIKE (an all-A53 phone — Stuart: "for phones without any defined Big cores"): the highest two are
//     RESERVED for the UI thread and RenderThread, and every other thread of ours is kept OFF them. That is the Pi 2
//     layout itself. Fewer than four cores: nothing is done.
//   · Fewer than two frame cores: the strongest two.
//
// sched_setaffinity and a RAISED priority on our OWN threads need no permission (an app may take its own threads to
// urgent-audio priority); nothing outside this process is touched.
#include <jni.h>
#include <sched.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

constexpr int SWEEP_MS  = 2000;
constexpr int NET_NICE  = -10;   // above the UI thread's 0, below audio's -19 (vibe_thread.h's order, client side)
constexpr int BULK_NICE = 10;    // image decoding: bytes somebody is waiting for, nobody hears late

bool readLine(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[256] = {0};
    const bool ok = fgets(buf, sizeof buf, f) != nullptr;
    fclose(f);
    if (!ok) return false;
    out = buf;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return true;
}

// Little cores' CPU parts: A53, A35, A55, A510, A520.
bool littlePart(unsigned part) {
    return part == 0xd03 || part == 0xd04 || part == 0xd05 || part == 0xd46 || part == 0xd80;
}

struct Plan {
    std::vector<bool> frame, other;   // per core
    std::string summary;
    bool ok = false;
};

std::string coreList(const std::vector<bool>& m) {
    std::string s;
    for (size_t i = 0; i < m.size(); i++) if (m[i]) s += std::to_string(i);
    return s.empty() ? "-" : s;
}

// ✗ Tried and dropped: JS on the other cores + ONE frame core (FM, XCover: 88 % janky, p99 38 ms — no better).
Plan makePlan() {
    Plan p;
    const int n = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (n < 4) { p.summary = std::to_string(n) + " cores — nothing pinned"; return p; }
    std::vector<int> cap(n, -1);
    bool allCap = true;
    for (int i = 0; i < n; i++) {
        std::string s;
        if (readLine("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpu_capacity", s)) cap[i] = atoi(s.c_str());
        else allCap = false;
    }
    std::string how = "cpu_capacity";
    if (!allCap) {
        how = "cpu part";
        FILE* f = fopen("/proc/cpuinfo", "r");
        if (!f) { p.summary = "no /proc/cpuinfo"; return p; }
        char line[256];
        int cur = -1;
        std::vector<int> part(n, -1);
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "processor", 9)) { const char* c = strchr(line, ':'); cur = c ? atoi(c + 1) : -1; }
            else if (!strncmp(line, "CPU part", 8) && cur >= 0 && cur < n) {
                const char* c = strchr(line, ':');
                if (c) part[cur] = (int)strtoul(c + 1, nullptr, 0);
            }
        }
        fclose(f);
        for (int i = 0; i < n; i++) {
            if (part[i] < 0) { p.summary = "cpu part unreadable"; return p; }
            cap[i] = littlePart((unsigned)part[i]) ? 1 : 4;   // ★ not 1 : 2 — "at least half the strongest" let 1 × 2 ≥ 2 through: every core a frame core
        }
    }
    const int mx = *std::max_element(cap.begin(), cap.end());
    const int mn = *std::min_element(cap.begin(), cap.end());
    p.frame.assign(n, false);
    p.other.assign(n, true);
    std::string layout;
    if (mx == mn) {
        // ★ All alike: reserve the highest two for the frame threads, keep ours off them.
        p.frame[n - 1] = p.frame[n - 2] = true;
        p.other[n - 1] = p.other[n - 2] = false;
        layout = "all alike — reserved pair";
    } else {
        int nf = 0;
        for (int i = 0; i < n; i++) if (cap[i] * 2 >= mx) { p.frame[i] = true; nf++; }
        if (nf < 2) {   // a lone prime core: the strongest two
            std::vector<int> idx(n);
            for (int i = 0; i < n; i++) idx[i] = i;
            std::sort(idx.begin(), idx.end(), [&](int a, int b) { return cap[a] > cap[b] || (cap[a] == cap[b] && a > b); });
            p.frame.assign(n, false);
            p.frame[idx[0]] = p.frame[idx[1]] = true;
        }
        layout = "big.LITTLE";
    }
    p.ok = true;
    p.summary = layout + " (" + how + "): frame " + coreList(p.frame) + " · others " + coreList(p.other);
    return p;
}

bool maskOf(pid_t tid, std::vector<bool>& out, int n) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(tid, sizeof set, &set) != 0) return false;
    out.assign(n, false);
    for (int i = 0; i < n; i++) out[i] = CPU_ISSET(i, &set);
    return true;
}

bool pin(pid_t tid, const std::vector<bool>& want) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (size_t i = 0; i < want.size(); i++) if (want[i]) CPU_SET(i, &set);
    return sched_setaffinity(tid, sizeof set, &set) == 0;
}

bool endsWith(const std::string& s, const char* tail) {
    const size_t t = strlen(tail);
    return s.size() >= t && s.compare(s.size() - t, t, tail) == 0;
}

std::mutex g_mx;
std::condition_variable g_cv;
bool g_armed = false;
bool g_threadUp = false;
bool g_havePlan = false;
Plan g_plan;
std::string g_last = "never swept";
std::unordered_set<pid_t> g_niced;   // threads already given their priority (sweep thread only)

std::string sweep(const Plan& p) {
    const int n = (int)p.frame.size();
    const pid_t pid = getpid();
    int frame = 0, freed = 0, net = 0, bulk = 0, fail = 0;
    std::vector<bool> cur;
    DIR* d = opendir("/proc/self/task");
    if (!d) return "no /proc/self/task";
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const pid_t tid = (pid_t)atoi(e->d_name);
        std::string comm;
        if (!readLine(std::string("/proc/self/task/") + e->d_name + "/comm", comm)) continue;
        if (!maskOf(tid, cur, n)) continue;
        // SkiaRender: our react-native-skia patch draws every canvas there (patches/@shopify+react-native-skia).
        // ★ NOT SkiaRender (our react-native-skia patch: every canvas draws there). Started by the UI thread it
        //   inherits the frame mask and is freed like any other — measured on the XCover's DAB screen (three runs
        //   each, thermally throttled phone, so ±15 points a run): SkiaRender ON the A73s 84 % janky (it crowded the
        //   RenderThread: 142 % of 200 % on two cores); free, it settles on the A53s and the average was ~73 %
        //   against ~93 % with pinning alone.
        const bool frameThread = tid == pid || comm == "RenderThread";
        const std::vector<bool>& want = frameThread ? p.frame : p.other;
        if (frameThread) {
            if (cur != want) { if (pin(tid, want)) frame++; else fail++; }
        } else if (cur != want &&
                   (cur == p.frame || std::all_of(cur.begin(), cur.end(), [](bool b) { return b; }))) {
            // Inherited the frame mask, or still on every core where ours must keep off a reserved pair. A mask
            // somebody else chose deliberately (anything else) is left alone.
            if (pin(tid, want)) freed++; else fail++;
        }
        if (!g_niced.count(tid)) {
            if (endsWith(comm, "/...")) {
                if (setpriority(PRIO_PROCESS, tid, NET_NICE) == 0) net++;
                g_niced.insert(tid);
            } else if (!strncmp(comm.c_str(), "FrescoDecode", 12)) {
                if (setpriority(PRIO_PROCESS, tid, BULK_NICE) == 0) bulk++;
                g_niced.insert(tid);
            }
        }
    }
    closedir(d);
    char buf[160];
    snprintf(buf, sizeof buf, "pinned %d · freed %d · net %d · bulk %d · failed %d", frame, freed, net, bulk, fail);
    return buf;
}

void loop() {
    prctl(PR_SET_NAME, "vibe-affinity");
    setpriority(PRIO_PROCESS, 0, BULK_NICE);   // housekeeping: never in anybody's way
    std::unique_lock<std::mutex> lk(g_mx);
    for (;;) {
        g_cv.wait(lk, [] { return g_armed; });
        const Plan p = g_plan;
        lk.unlock();
        const std::string r = p.ok ? sweep(p) : p.summary;
        lk.lock();
        g_last = r;
        g_cv.wait_for(lk, std::chrono::milliseconds(SWEEP_MS), [] { return !g_armed; });
    }
}

}  // namespace

/** Arm (the SDR screen is up and in front) or disarm the sweep. Returns the plan, for the log. */
extern "C" JNIEXPORT jstring JNICALL
Java_com_vibesdr_app_VibeAffinity_arm(JNIEnv* env, jclass, jboolean on) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (on && !g_havePlan) {
        g_plan = makePlan();
        g_havePlan = true;
    }
    g_armed = on;
    if (on && !g_threadUp) { std::thread(loop).detach(); g_threadUp = true; }
    g_cv.notify_all();
    return env->NewStringUTF(g_plan.summary.c_str());
}

/** The last sweep's counts, for the log. */
extern "C" JNIEXPORT jstring JNICALL
Java_com_vibesdr_app_VibeAffinity_lastSweep(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mx);
    return env->NewStringUTF(g_last.c_str());
}
