// vibe_dab_uep.h — GENERATED. Unequal Error Protection, from EN 300 401 tables 8 and 15.
//
// ★★★ THE BBC'S NATIONAL SERVICES ARE UEP, NOT EEP. Decoding the real 12B multiplex showed every
//     Layer II service using the SHORT FORM of FIG 0/1 — a 6-bit table index rather than an
//     explicit size — so without these tables the station list decodes perfectly and not one
//     service can be played. Table 8's index 31 is 112 kbit/s and index 40 is 160 kbit/s, which
//     is Radio 3; both were read off the air before this file existed.
//
// ★★ Machine-extracted from the spec PDF, like every other table here. The consistency check is
//    exact and the test asserts it: for each profile, 4·Σ Li·(8+PIi) + 12 + padding must equal
//    the sub-channel size in capacity units × 64.
#pragma once

#include <cstdint>

namespace vibedab {

/** Table 8: the short-form index gives size, protection level and bit rate outright. */
struct UepIndex { int sizeCu; int protLevel; int bitrateKbps; };

inline constexpr UepIndex kUepIndex[64] = {
    {   16, 5,  32 },   // index 0
    {   21, 4,  32 },   // index 1
    {   24, 3,  32 },   // index 2
    {   29, 2,  32 },   // index 3
    {   35, 1,  32 },   // index 4
    {   24, 5,  48 },   // index 5
    {   29, 4,  48 },   // index 6
    {   35, 3,  48 },   // index 7
    {   42, 2,  48 },   // index 8
    {   52, 1,  48 },   // index 9
    {   29, 5,  56 },   // index 10
    {   35, 4,  56 },   // index 11
    {   42, 3,  56 },   // index 12
    {   52, 2,  56 },   // index 13
    {   32, 5,  64 },   // index 14
    {   42, 4,  64 },   // index 15
    {   48, 3,  64 },   // index 16
    {   58, 2,  64 },   // index 17
    {   70, 1,  64 },   // index 18
    {   40, 5,  80 },   // index 19
    {   52, 4,  80 },   // index 20
    {   58, 3,  80 },   // index 21
    {   70, 2,  80 },   // index 22
    {   84, 1,  80 },   // index 23
    {   48, 5,  96 },   // index 24
    {   58, 4,  96 },   // index 25
    {   70, 3,  96 },   // index 26
    {   84, 2,  96 },   // index 27
    {  104, 1,  96 },   // index 28
    {   58, 5, 112 },   // index 29
    {   70, 4, 112 },   // index 30
    {   84, 3, 112 },   // index 31
    {  104, 2, 112 },   // index 32
    {   64, 5, 128 },   // index 33
    {   84, 4, 128 },   // index 34
    {   96, 3, 128 },   // index 35
    {  116, 2, 128 },   // index 36
    {  140, 1, 128 },   // index 37
    {   80, 5, 160 },   // index 38
    {  104, 4, 160 },   // index 39
    {  116, 3, 160 },   // index 40
    {  140, 2, 160 },   // index 41
    {  168, 1, 160 },   // index 42
    {   96, 5, 192 },   // index 43
    {  116, 4, 192 },   // index 44
    {  140, 3, 192 },   // index 45
    {  168, 2, 192 },   // index 46
    {  208, 1, 192 },   // index 47
    {  116, 5, 224 },   // index 48
    {  140, 4, 224 },   // index 49
    {  168, 3, 224 },   // index 50
    {  208, 2, 224 },   // index 51
    {  232, 1, 224 },   // index 52
    {  128, 5, 256 },   // index 53
    {  168, 4, 256 },   // index 54
    {  192, 3, 256 },   // index 55
    {  232, 2, 256 },   // index 56
    {  280, 1, 256 },   // index 57
    {  160, 5, 320 },   // index 58
    {  208, 4, 320 },   // index 59
    {  280, 2, 320 },   // index 60
    {  192, 5, 384 },   // index 61
    {  280, 3, 384 },   // index 62
    {  416, 1, 384 },   // index 63
};

/** Table 15: the four block groups and their puncturing indices. */
struct UepProfile { int L[4]; int PI[4]; int padding; bool valid; };

struct UepRow { int bitrateKbps; int level; int L[4]; int PI[4]; int padding; };

inline constexpr UepRow kUepProfiles[] = {
    {  32, 5, {   3,   4,  17,   0 }, {  5,  3,  2,  0 }, 0 },
    {  32, 4, {   3,   3,  18,   0 }, { 11,  6,  5,  0 }, 0 },
    {  32, 3, {   3,   4,  14,   3 }, { 15,  9,  6,  8 }, 0 },
    {  32, 2, {   3,   4,  14,   3 }, { 22, 13,  8, 13 }, 0 },
    {  32, 1, {   3,   5,  13,   3 }, { 24, 17, 12, 17 }, 4 },
    {  48, 5, {   4,   3,  26,   3 }, {  5,  4,  2,  3 }, 0 },
    {  48, 4, {   3,   4,  26,   3 }, {  9,  6,  4,  6 }, 0 },
    {  48, 3, {   3,   4,  26,   3 }, { 15, 10,  6,  9 }, 4 },
    {  48, 2, {   3,   4,  26,   3 }, { 24, 14,  8, 15 }, 0 },
    {  48, 1, {   3,   5,  25,   3 }, { 24, 18, 13, 18 }, 0 },
    {  56, 5, {   6,  10,  23,   3 }, {  5,  4,  2,  3 }, 0 },
    {  56, 4, {   6,  10,  23,   3 }, {  9,  6,  4,  5 }, 0 },
    {  56, 3, {   6,  12,  21,   3 }, { 16,  7,  6,  9 }, 0 },
    {  56, 2, {   6,  10,  23,   3 }, { 23, 13,  8, 13 }, 8 },
    {  64, 5, {   6,   9,  31,   2 }, {  5,  3,  2,  3 }, 0 },
    {  64, 4, {   6,   9,  33,   0 }, { 11,  6,  5,  0 }, 0 },
    {  64, 3, {   6,  12,  27,   3 }, { 16,  8,  6,  9 }, 0 },
    {  64, 2, {   6,  10,  29,   3 }, { 23, 13,  8, 13 }, 8 },
    {  64, 1, {   6,  11,  28,   3 }, { 24, 18, 12, 18 }, 4 },
    {  80, 5, {   6,  10,  41,   3 }, {  6,  3,  2,  3 }, 0 },
    {  80, 4, {   6,  10,  41,   3 }, { 11,  6,  5,  6 }, 0 },
    {  80, 3, {   6,  11,  40,   3 }, { 16,  8,  6,  7 }, 0 },
    {  80, 2, {   6,  10,  41,   3 }, { 23, 13,  8, 13 }, 8 },
    {  80, 1, {   6,  10,  41,   3 }, { 24, 17, 12, 18 }, 4 },
    {  96, 5, {   7,   9,  53,   3 }, {  5,  4,  2,  4 }, 0 },
    {  96, 4, {   7,  10,  52,   3 }, {  9,  6,  4,  6 }, 0 },
    {  96, 3, {   6,  12,  51,   3 }, { 16,  9,  6, 10 }, 4 },
    {  96, 2, {   6,  10,  53,   3 }, { 22, 12,  9, 12 }, 0 },
    {  96, 1, {   6,  13,  50,   3 }, { 24, 18, 13, 19 }, 0 },
    { 112, 5, {  14,  17,  50,   3 }, {  5,  4,  2,  5 }, 0 },
    { 112, 4, {  11,  21,  49,   3 }, {  9,  6,  4,  8 }, 0 },
    { 112, 3, {  11,  23,  47,   3 }, { 16,  8,  6,  9 }, 0 },
    { 112, 2, {  11,  21,  49,   3 }, { 23, 12,  9, 14 }, 4 },
    { 128, 5, {  12,  19,  62,   3 }, {  5,  3,  2,  4 }, 0 },
    { 128, 4, {  11,  21,  61,   3 }, { 11,  6,  5,  7 }, 0 },
    { 128, 3, {  11,  22,  60,   3 }, { 16,  9,  6, 10 }, 4 },
    { 128, 2, {  11,  21,  61,   3 }, { 22, 12,  9, 14 }, 0 },
    { 128, 1, {  11,  20,  62,   3 }, { 24, 17, 13, 19 }, 8 },
    { 160, 5, {  11,  19,  87,   3 }, {  5,  4,  2,  4 }, 0 },
    { 160, 4, {  11,  23,  83,   3 }, { 11,  6,  5,  9 }, 0 },
    { 160, 3, {  11,  24,  82,   3 }, { 16,  8,  6, 11 }, 0 },
    { 160, 2, {  11,  21,  85,   3 }, { 22, 11,  9, 13 }, 0 },
    { 160, 1, {  11,  22,  84,   3 }, { 24, 18, 12, 19 }, 0 },
    { 192, 5, {  11,  20, 110,   3 }, {  6,  4,  2,  5 }, 0 },
    { 192, 4, {  11,  22, 108,   3 }, { 10,  6,  4,  9 }, 0 },
    { 192, 3, {  11,  24, 106,   3 }, { 16, 10,  6, 11 }, 0 },
    { 192, 2, {  11,  20, 110,   3 }, { 22, 13,  9, 13 }, 8 },
    { 192, 1, {  11,  21, 109,   3 }, { 24, 20, 13, 24 }, 0 },
    { 224, 5, {  12,  22, 131,   3 }, {  8,  6,  2,  6 }, 4 },
    { 224, 4, {  12,  26, 127,   3 }, { 12,  8,  4, 11 }, 0 },
    { 224, 3, {  11,  20, 134,   3 }, { 16, 10,  7,  9 }, 0 },
    { 224, 2, {  11,  22, 132,   3 }, { 24, 16, 10, 15 }, 0 },
    { 224, 1, {  11,  24, 130,   3 }, { 24, 20, 12, 20 }, 4 },
    { 256, 5, {  11,  24, 154,   3 }, {  6,  5,  2,  5 }, 0 },
    { 256, 4, {  11,  24, 154,   3 }, { 12,  9,  5, 10 }, 4 },
    { 256, 3, {  11,  27, 151,   3 }, { 16, 10,  7, 10 }, 0 },
    { 256, 2, {  11,  22, 156,   3 }, { 24, 14, 10, 13 }, 8 },
    { 256, 1, {  11,  26, 152,   3 }, { 24, 19, 14, 18 }, 4 },
    { 320, 5, {  11,  26, 200,   3 }, {  8,  5,  2,  6 }, 4 },
    { 320, 4, {  11,  25, 201,   3 }, { 13,  9,  5, 10 }, 8 },
    { 320, 2, {  11,  26, 200,   3 }, { 24, 17,  9, 17 }, 0 },
    { 384, 5, {  11,  27, 247,   3 }, {  8,  6,  2,  7 }, 0 },
    { 384, 3, {  11,  24, 250,   3 }, { 16,  9,  7, 10 }, 4 },
    { 384, 1, {  12,  28, 245,   3 }, { 24, 20, 14, 23 }, 8 },
};

inline constexpr int kUepProfileCount = int(sizeof(kUepProfiles) / sizeof(kUepProfiles[0]));

/** Look up the profile for a bit rate and protection level. */
inline UepProfile uepProfile(int bitrateKbps, int level) {
    for (int i = 0; i < kUepProfileCount; ++i) {
        const UepRow& r = kUepProfiles[i];
        if (r.bitrateKbps == bitrateKbps && r.level == level)
            return { { r.L[0], r.L[1], r.L[2], r.L[3] },
                     { r.PI[0], r.PI[1], r.PI[2], r.PI[3] }, r.padding, true };
    }
    return { {0,0,0,0}, {0,0,0,0}, 0, false };
}

/** Coded (transmitted) bits for a UEP profile: four block groups, the 12 tail bits, and padding.
 *  ★ This must equal the sub-channel's capacity in bits, and the test checks every profile. */
inline int uepCodedBits(const UepProfile& p) {
    if (!p.valid) return 0;
    int n = 0;
    for (int i = 0; i < 4; ++i) n += p.L[i] * (8 + p.PI[i]);
    return 4 * n + 12 + p.padding;
}

}  // namespace vibedab
