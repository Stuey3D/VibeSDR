// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the OpenRSP authors
//
// This file is part of OpenRSP.
//
// OpenRSP is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// OpenRSP is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with OpenRSP. If
// not, see <https://www.gnu.org/licenses/>.

// Fixed-point coefficient and mixer tables of the RSP stream DSP. Full tap arrays: index i
// multiplies x[n - i] (newest sample first).
#pragma once

#include <cstdint>

namespace sdrplay {
namespace dsp {
namespace tables {

// FIR coefficients. Frequencies are fractions of the filter's input rate fs; the pass bands
// given are the -3 dB edges of the response these coefficients produce. Each table is
// round(S * h) for the design given with it: remez = scipy.signal.remez (Parks-McClellan,
// desired 1/0, unit weights), firls = least-squares linear-phase FIR (the scipy.signal.firls
// design, desired 1/0, unit weights), band edges in fs. tools/derive_fir/derive_fir.py
// (ctest derive_fir_check) regenerates every table from these designs.
//
// HB1: 23-tap half-band low-pass, Q15, unity DC gain, pass band to about 0.22 fs. Down-converter
// half-band paths (decimate by 2), ADS-B modes 2/3 (full rate), user decimation wideBandSignal = 1, M = 32.
// Design: scipy.signal.remez(23, [0, 25/256, 103/256, 1/2], [1, 0], grid_density=32), S = 32766.
extern const std::int32_t kHb1[23];
// HB2: 51-tap half-band low-pass, Q15, pass band to about 0.24 fs. Down-converter
// decimate-by-4 path, second decimate-by-2 stage.
// Design: scipy.signal.remez(51, [0, 25/128, 39/128, 1/2], [1, 0], grid_density=32), S = 32766.
extern const std::int32_t kHb2[51];
// HB3: 31-tap half-band low-pass, Q15, pass band to about 0.23 fs. User decimation
// wideBandSignal >= 2, the decimate-by-2 stages before FIR108b.
// Design: scipy.signal.remez(31, [0, 0.165, 0.335, 1/2], [1, 0], grid_density=32), S = 32766.
extern const std::int32_t kHb3[31];
// H58: 58-tap symmetric low-pass, Q15, pass band to about 0.14 fs. Down-converter
// 1620 kHz path (decimate by 3).
// Design: firls, 58 taps, pass 0-760 kHz, stop 1.1-3 MHz at fs = 6 MHz, S = 32700.
extern const std::int32_t kH58[58];
// FIR108: 108-tap symmetric low-pass, Q15 (taps sum to 32750), pass band to about 0.23 fs.
// User decimation wideBandSignal = 1, final decimate-by-2 stage.
// Design: scipy.signal.remez(108, [0, 0.225, 0.2495, 1/2], [1, 0]), S = 32651.
extern const std::int32_t kFir108[108];
// FIR108b: 108-tap symmetric low-pass, Q16 (taps sum to 65528), pass band to about 0.24 fs.
// User decimation wideBandSignal >= 2, final decimate-by-2 stage.
// Design: firls, 108 taps, pass 0-0.2125, stop 0.2875-0.5, S = 65536.
extern const std::int32_t kFir108b[108];
// ADS-B mode 1: 51-tap symmetric low-pass, Q15, pass band to about 0.11 fs, full rate after
// the fs/4 mix.
// Design: firls, 51 taps, pass 0-0.1125, stop 0.1375-0.5, S = 31768.
extern const std::int32_t kAdsbLpf51[51];
// ADS-B mode 2: 51-tap symmetric band-pass centred on fs/4 (about 0.14-0.36 fs), Q15, applied
// to the real input. Non-zero taps only at the centre and at offsets 2, 6, 10, ... 22 from it.
// Design: firls, 51 taps, stop 0-0.1125, pass 0.1375-0.3625, stop 0.3875-0.5, S = 31768.
extern const std::int32_t kAdsbBpf2[51];
// ADS-B mode 3: 51-tap symmetric band-pass (about 0.07-0.43 fs), Q15, applied to the real
// input. Non-zero taps only at the centre and at even offsets up to 24 from it.
// Design: firls, 51 taps, stop 0-0.05, pass 0.075-0.425, stop 0.45-0.5, S = 31768.
extern const std::int32_t kAdsbBpf3[51];

// Mixer tables, amplitude 32767 (Q15), k = table index:
// COS40[k] = trunc(32767 * cos(2 pi * 9k / 40)): 9/40 of fs = 450 kHz at 2 MHz.
extern const std::int32_t kCos40[40];
// SIN40[k] = trunc(32767 * sin(2 pi * 9k / 40)) = COS40[(k + 30) % 40].
extern const std::int32_t kSin40[40];
// T300[k] = 32767 * cos(2 pi * 79k / 300) rounded to integers: 79/300 of fs = 1.58 MHz at
// 6 MHz. The Q mixer reads it 75 entries (a quarter period) ahead, which gives the sine.
extern const std::int32_t kT300[300];

} // namespace tables
} // namespace dsp
} // namespace sdrplay
