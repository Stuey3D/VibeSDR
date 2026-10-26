// decoders/drm/drm_params.h — the DRM (Digital Radio Mondiale) transmission structure: robustness modes A–D, spectrum
// occupancies, pilot cells and the cell map of a transmission super frame. Everything here is from the PUBLISHED
// STANDARD, ETSI ES 201 980 V4.3.1 (2023-11), clause 8 (and clause 7.7 for the MSC cell counts it is checked against).
//
// ★★★ ONE SAMPLE RATE INSIDE: 24 kHz. Every robustness mode's useful part and guard interval are a whole number of
//     samples there (table 47 × 24 000): A 576 + 64, B 512 + 128, C 352 + 128, D 224 + 176. Carrier k is FFT bin k
//     (spacing = 1/Tu), so no carrier ever falls between bins. 24 kHz holds spectrum occupancies 0–3 (≤ 10 kHz, ±4.75 kHz
//     about the reference frequency) with room for the tuning search; 18/20 kHz (4, 5) need 48 kHz and are not built —
//     nobody broadcasts them on short wave (docs/v12/DRM.md).
// ★★ Robustness mode E (DRM+, 100 kHz, VHF) is NOT here: a different sample rate, a 244-cell FAC and its own pilot
//    algebra (§8.4.4.3.6). It would reuse the coding layer below unchanged — see the report in docs/v12/DRM.md.
//
// ★ The cell map is built ONCE per (mode, occupancy) by the rules of §8.4–8.6 in the order of precedence the standard
//   gives (time/frequency references over gain references; then SDC, FAC, MSC). Its counts are checked against tables 25
//   (SDC cells), 41–44 (MSC cells per super frame) and §8.5.2.1 (65 FAC cells) by test-drm — a wrong table entry shows up
//   as a wrong count, which is how this file proves itself.
#pragma once
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace vibedrm {

using cf = std::complex<float>;

constexpr int kFs = 24000;                 // ★ the internal sample rate (see above)

enum class Mode : int8_t { A = 0, B = 1, C = 2, D = 3, None = -1 };
inline char modeLetter(Mode m) { return m == Mode::None ? '-' : char('A' + int(m)); }

struct ModeInfo {
    int tu, tg;            // samples at kFs
    int ns;                // symbols per transmission frame
    int x, y, k0;          // gain reference pattern (table 58/60)
    int nSdcSym;           // SDC symbols at the start of each super frame (§8.5.3.1)
    int fp[3];             // frequency reference carriers (table 51)
    int fpPh[3];           // their phase index at s = 0 (table 52)
    int q1024;             // gain-reference phase constant (§8.4.4.3.x)
};

inline const ModeInfo& modeInfo(Mode m) {
    static const ModeInfo t[4] = {
        { 576,  64, 15, 4, 5, 2, 2, {18, 54, 72}, {205, 836, 215}, 36 },
        { 512, 128, 15, 2, 3, 1, 2, {16, 48, 64}, {331, 651, 555}, 12 },
        { 352, 128, 20, 2, 2, 1, 3, {11, 33, 44}, {214, 392, 242}, 12 },
        { 224, 176, 24, 1, 3, 1, 3, { 7, 21, 28}, {788, 1014, 332}, 14 },
    };
    return t[int(m)];
}

/** Carrier range per mode and spectrum occupancy (table 49); kmin > kmax = not defined. */
inline bool carrierRange(Mode m, int so, int& kmin, int& kmax) {
    static const int lo[4][6] = { {2, 2, -102, -114, -98, -110}, {1, 1, -91, -103, -87, -99},
                                  {99, 99, 99, -69, 99, -67},    {99, 99, 99, -44, 99, -43} };
    static const int hi[4][6] = { {102, 114, 102, 114, 314, 350}, {91, 103, 91, 103, 279, 311},
                                  {0, 0, 0, 69, 0, 213},          {0, 0, 0, 44, 0, 135} };
    if (m == Mode::None || so < 0 || so > 5) return false;
    kmin = lo[int(m)][so]; kmax = hi[int(m)][so];
    return kmin <= kmax;
}

/** ★ What this receiver builds: occupancies 0–3 (≤ 10 kHz). 4 and 5 are outside the 24 kHz window. */
inline bool occupancySupported(Mode m, int so) {
    int a, b;
    return so >= 0 && so <= 3 && carrierRange(m, so, a, b);
}

inline bool carrierUnused(Mode m, int k) {
    if (m == Mode::A) return k >= -1 && k <= 1;
    return k == 0;
}

/** Time reference cells (tables 53–56): carrier, phase index. */
struct KPh { int16_t k; int16_t ph; };
inline const std::vector<KPh>& timeRefs(Mode m) {
    static const std::vector<KPh> a = { {17,973},{18,205},{19,717},{21,264},{28,357},{29,357},{32,952},{33,440},
        {39,856},{40,88},{41,88},{53,68},{54,836},{55,836},{56,836},{60,1008},{61,1008},{63,752},{71,215},{72,215},{73,727} };
    static const std::vector<KPh> b = { {14,304},{16,331},{18,108},{20,620},{24,192},{26,704},{32,44},{36,432},{42,588},
        {44,844},{48,651},{49,651},{50,651},{54,460},{56,460},{62,944},{64,555},{66,940},{68,428} };
    static const std::vector<KPh> c = { {8,722},{10,466},{11,214},{12,214},{14,479},{16,516},{18,260},{22,577},{24,662},
        {28,3},{30,771},{32,392},{33,392},{36,37},{38,37},{42,474},{44,242},{45,242},{46,754} };
    static const std::vector<KPh> d = { {5,636},{6,124},{7,788},{8,788},{9,200},{11,688},{12,152},{14,920},{15,920},
        {17,644},{18,388},{20,652},{21,1014},{23,176},{24,176},{26,752},{27,496},{28,332},{29,432},{30,964},{32,452} };
    static const std::vector<KPh> none;
    switch (m) { case Mode::A: return a; case Mode::B: return b; case Mode::C: return c; case Mode::D: return d; default: return none; }
}

/** FAC cell positions (tables 62–65): per symbol, the carriers. */
inline const std::vector<std::vector<int>>& facCells(Mode m) {
    static const std::vector<std::vector<int>> a = { {}, {}, {26,46,66,86}, {10,30,50,70,90}, {14,22,34,62,74,94},
        {26,38,58,66,78}, {22,30,42,62,70,82}, {26,34,46,66,74,86}, {10,30,38,50,58,70,78,90}, {14,22,34,42,62,74,82,94},
        {26,38,46,66,86}, {10,30,50,70,90}, {14,34,74,94}, {38,58,78}, {} };
    static const std::vector<std::vector<int>> b = { {}, {}, {13,25,43,55,67}, {15,27,45,57,69}, {17,29,47,59,71},
        {19,31,49,61,73}, {9,21,33,51,63,75}, {11,23,35,53,65,77}, {13,25,37,55,67,79}, {15,27,39,57,69,81},
        {17,29,41,59,71,83}, {19,31,43,61,73}, {21,33,45,63,75}, {23,35,47,65,77}, {} };
    static const std::vector<std::vector<int>> c = { {}, {}, {}, {9,21,45,57}, {23,35,47}, {13,25,37,49}, {15,27,39,51},
        {5,17,29,41,53}, {7,19,31,43,55}, {9,21,45,57}, {23,35,47}, {13,25,37,49}, {15,27,39,51}, {5,17,29,41,53},
        {7,19,31,43,55}, {9,21,45,57}, {23,35,47}, {13,25,37,49}, {15,27,39,51}, {} };
    static const std::vector<std::vector<int>> d = { {}, {}, {}, {9,18,27}, {10,19}, {11,20,29}, {12,30}, {13,22,31},
        {5,14,23,32}, {6,15,24,33}, {16,25,34}, {8,17,26,35}, {9,18,27,36}, {10,19,37}, {11,20,29}, {12,30}, {13,22,31},
        {5,14,23,32}, {6,15,24,33}, {16,25,34}, {8,17,26,35}, {9,18,27,36}, {10,19,37}, {} };
    static const std::vector<std::vector<int>> none;
    switch (m) { case Mode::A: return a; case Mode::B: return b; case Mode::C: return c; case Mode::D: return d; default: return none; }
}

/** Gain reference phase index ϑ1024[s,k] for a cell that IS a gain reference (§8.4.4.3.1, modes A–D). */
inline int gainRefPhase(Mode m, int s, int k) {
    static const int wA[5][3] = { {228,341,455},{455,569,683},{683,796,910},{910,0,114},{114,228,341} };
    static const int zA[5][3] = { {0,81,248},{18,106,106},{122,116,31},{129,129,39},{33,32,111} };
    static const int wB[3][5] = { {512,0,512,0,512},{0,512,0,512,0},{512,0,512,0,512} };
    static const int zB[3][5] = { {0,57,164,64,12},{168,255,161,106,118},{25,232,132,233,38} };
    static const int wC[2][10] = { {465,372,279,186,93,0,931,838,745,652},{931,838,745,652,559,465,372,279,186,93} };
    static const int zC[2][10] = { {0,76,29,76,9,190,161,248,33,108},{179,178,83,253,127,105,101,198,250,145} };
    static const int wD[3][8] = { {366,439,512,585,658,731,805,878},{731,805,878,951,0,73,146,219},{73,146,219,293,366,439,512,585} };
    static const int zD[3][8] = { {0,240,17,60,220,38,151,101},{110,7,78,82,175,150,106,25},{165,7,252,124,253,177,197,142} };
    const ModeInfo& mi = modeInfo(m);
    const int n = s % mi.y, mm = s / mi.y;
    const int p = (k - mi.k0 - n * mi.x) / (mi.x * mi.y);
    int w = 0, z = 0;
    switch (m) {
        case Mode::A: w = wA[n][mm]; z = zA[n][mm]; break;
        case Mode::B: w = wB[n][mm]; z = zB[n][mm]; break;
        case Mode::C: w = wC[n][mm]; z = zC[n][mm]; break;
        case Mode::D: w = wD[n][mm]; z = zD[n][mm]; break;
        default: break;
    }
    long long v = 4LL * z + (long long)p * w + (long long)p * p * (1 + s) * mi.q1024;
    v %= 1024; if (v < 0) v += 1024;
    return int(v);
}

inline bool isGainRef(Mode m, int s, int k) {
    const ModeInfo& mi = modeInfo(m);
    const int r = k - mi.k0 - mi.x * (s % mi.y);
    const int per = mi.x * mi.y;
    return ((r % per) + per) % per == 0;
}

/** Boosted (power 4, amplitude 2) gain references at the band edges (table 59). */
inline bool isBoosted(Mode m, int so, int k) {
    int kmin, kmax;
    if (!carrierRange(m, so, kmin, kmax)) return false;
    // ★ The table lists, per edge, the outermost two gain-reference carriers: kmin, kmin+step, kmax-step, kmax, where the
    //   step is the gain pattern's carrier spacing x — except mode A SO 0/1 whose lower edge is 2, 6 (x = 4) and mode D
    //   whose step is 1. Rather than derive it, the table itself:
    static const int A[6][4] = { {2,6,98,102},{2,6,110,114},{-102,-98,98,102},{-114,-110,110,114},{-98,-94,310,314},{-110,-106,346,350} };
    static const int B[6][4] = { {1,3,89,91},{1,3,101,103},{-91,-89,89,91},{-103,-101,101,103},{-87,-85,277,279},{-99,-97,309,311} };
    static const int C[6][4] = { {0,0,0,0},{0,0,0,0},{0,0,0,0},{-69,-67,67,69},{0,0,0,0},{-67,-65,211,213} };
    static const int D[6][4] = { {0,0,0,0},{0,0,0,0},{0,0,0,0},{-44,-43,43,44},{0,0,0,0},{-43,-42,134,135} };
    const int (*t)[4] = m == Mode::A ? A : m == Mode::B ? B : m == Mode::C ? C : D;
    for (int i = 0; i < 4; ++i) if (t[so][i] == k && k != 0) return true;
    return false;
}

inline cf phasor(int ph1024, float amp) {
    const double a = 2.0 * M_PI * double(ph1024) / 1024.0;
    return cf(float(amp * std::cos(a)), float(amp * std::sin(a)));
}

enum CellType : uint8_t { kUnused = 0, kFreqRef, kTimeRef, kGainRef, kFac, kSdc, kMsc, kMscDummy };

/** ★ The cell map of one transmission SUPER FRAME (3 frames × Ns symbols × K carriers). Built once per (mode, SO). */
struct CellMap {
    Mode mode = Mode::None;
    int so = -1;
    int kmin = 0, kmax = -1, nk = 0;          // carriers kmin..kmax, index j = k - kmin
    int ns = 0;                               // symbols per frame
    std::vector<uint8_t> type;                // [(frame*ns + s)*nk + j]
    std::vector<cf> pilot;                    // the reference value for pilot cells, 0 elsewhere
    int nFac = 0;                             // per frame
    int nSdc = 0;                             // per super frame
    int nMscAvail = 0;                        // per super frame (NSFA)
    int nMux = 0;                             // MSC cells per multiplex frame (NMUX = floor(NSFA / 3))
    int nDummy = 0;                           // NL

    int idx(int frame, int s, int k) const { return (frame * ns + s) * nk + (k - kmin); }

    bool build(Mode m, int occ) {
        if (!occupancySupported(m, occ)) return false;
        mode = m; so = occ;
        carrierRange(m, occ, kmin, kmax);
        nk = kmax - kmin + 1;
        const ModeInfo& mi = modeInfo(m);
        ns = mi.ns;
        type.assign(size_t(3 * ns * nk), kUnused);
        pilot.assign(type.size(), cf(0, 0));
        const float r2 = std::sqrt(2.0f);
        const auto& tr = timeRefs(m);
        const auto& fac = facCells(m);
        nFac = 0; nSdc = 0; nMscAvail = 0;
        for (int f = 0; f < 3; ++f)
            for (int s = 0; s < ns; ++s)
                for (int k = kmin; k <= kmax; ++k) {
                    const int i = idx(f, s, k);
                    if (carrierUnused(m, k)) continue;
                    // 1. time references (symbol 0 of every frame) — they carry the frequency references' phases too
                    bool done = false;
                    if (s == 0)
                        for (const KPh& t : tr) if (t.k == k) { type[i] = kTimeRef; pilot[i] = phasor(t.ph, r2); done = true; break; }
                    if (done) {
                        // a time reference that is also a frequency reference stays a frequency reference for tracking
                        for (int q = 0; q < 3; ++q) if (mi.fp[q] == k) type[i] = kFreqRef;
                        continue;
                    }
                    // 2. frequency references (every symbol)
                    for (int q = 0; q < 3; ++q)
                        if (mi.fp[q] == k) {
                            int ph = mi.fpPh[q];
                            if (m == Mode::D && (k == 7 || k == 21) && (s & 1)) ph = (ph + 512) % 1024;
                            type[i] = kFreqRef; pilot[i] = phasor(ph, r2); done = true; break;
                        }
                    if (done) continue;
                    // 3. gain references
                    if (isGainRef(m, s, k)) {
                        type[i] = kGainRef;
                        pilot[i] = phasor(gainRefPhase(m, s, k), isBoosted(m, occ, k) ? 2.0f : r2);
                        continue;
                    }
                    // 4. SDC: the first symbols of the super frame
                    if (f == 0 && s < mi.nSdcSym) { type[i] = kSdc; ++nSdc; continue; }
                    // 5. FAC
                    bool isFac = false;
                    for (int kk : fac[size_t(s)]) if (kk == k) { isFac = true; break; }
                    if (isFac) { type[i] = kFac; if (f == 0) ++nFac; continue; }
                    // 6. MSC
                    type[i] = kMsc; ++nMscAvail;
                }
        nMux = nMscAvail / 3;
        nDummy = nMscAvail - 3 * nMux;
        // ★ The NL dummy cells are the LAST MSC cells of the super frame (§7.7, table 46).
        int left = nDummy;
        for (int i = int(type.size()) - 1; i >= 0 && left > 0; --i)
            if (type[size_t(i)] == kMsc) { type[size_t(i)] = kMscDummy; --left; }
        return true;
    }

    /** ★ Is a gain reference of the TIME-INTERPOLATION grid here? Every gain reference position, whatever its phase
     *  came from (§8.4.4.3.0: a time/frequency reference there takes precedence for the PHASE, the cell is still on the
     *  scattered grid). Used by the channel estimator. */
    bool onGainGrid(int s, int k) const { return !carrierUnused(mode, k) && isGainRef(mode, s, k); }
};

/** SDC cells per super frame from the standard (table 25), for the self-check. */
inline int sdcCellsTable(Mode m, int so) {
    static const int t[4][6] = { {167,190,359,405,754,846},{130,150,282,322,588,662},{0,0,0,288,0,607},{0,0,0,152,0,332} };
    return m == Mode::None ? 0 : t[int(m)][so];
}
/** MSC cells per multiplex frame from the standard (tables 41–44). */
inline int muxCellsTable(Mode m, int so) {
    static const int t[4][6] = { {1259,1422,2632,2959,5464,6118},{966,1110,2051,2337,4249,4774},{0,0,0,1844,0,3867},{0,0,0,1226,0,2606} };
    return m == Mode::None ? 0 : t[int(m)][so];
}
/** SDC data field length in bytes (table 21) — a cross-check on the coding arithmetic. */
inline int sdcDataBytesTable(Mode m, int sdcMode, int so) {
    static const int t[4][2][6] = { {{37,43,85,97,184,207},{17,20,41,47,91,102}}, {{28,33,66,76,143,161},{13,15,32,37,70,79}},
                                    {{0,0,0,68,0,147},{0,0,0,32,0,72}}, {{0,0,0,33,0,78},{0,0,0,15,0,38}} };
    return m == Mode::None ? 0 : t[int(m)][sdcMode][so];
}

}  // namespace vibedrm
