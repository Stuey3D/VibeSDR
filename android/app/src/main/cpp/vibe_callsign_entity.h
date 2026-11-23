// vibe_callsign_entity.h — V12 beta92 WHO A CALLSIGN BELONGS TO, AND WHERE THAT CAN BE. docs/v12/DIGI-MAP-FILTER.md.
//
// ★★★ WHY (Stuart's Pi 500, 2026-11-22): the 24 h DIGI MODES MAP placed stations in the Southern Ocean that were FALSE
//     FT8 DECODES — "132XQA" PC46 and "1H7IJH" EB72 (no country at all), "7Q8FAZ" (Malawi) at -74.5°, "YE9HMP"
//     (Indonesia) at -65.5°, "N6LCO" (USA) at -61.5° in open sea, "FQ2HMC" PD93. A false decode passes FT8's CRC by
//     chance about once in 2^14 tries of the OSD/deep passes, so on a busy all-band server a few a day get through; their
//     call and grid are random bits shaped like a call and a grid. Two plain facts catch them:
//
//  (a) A REAL CALL'S PREFIX IS ALLOCATED. Every amateur callsign starts with a series the ITU allocated to an
//      administration (Radio Regulations Appendix 42, "Table of allocation of international call sign series"). "13…",
//      "1H…", "Q…", "0…" belong to nobody → not a station. This table is the WHOLE of Appendix 42 (plus the four series
//      amateurs use outside it: 1A SMOM, 1S Spratly, S0 Western Sahara, Z6 Kosovo) — not the app's ~300-row display
//      table (vibe_callsign_country.h), which does not know 9N, ZD7, 3Y or V3 and must never decide who is real.
//  (b) A REAL GRID IS WHERE THAT ADMINISTRATION IS. Each administration has a generous region: a few boxes covering its
//      home territory AND every overseas territory, island and DXpedition entity under its series (the US boxes take in
//      Alaska, Hawaii, the Caribbean and the Pacific islands; France's take in Polynesia, New Caledonia, Réunion, the
//      Kerguelens; the UK's the Falklands, South Georgia, St Helena, Chagos …), each box padded by kLatPad/kLonPad. A
//      box may cross the antimeridian (w > e: Russia, Alaska, Fiji, Kiribati, NZ's Chathams).
//      ★ ANTARCTICA (south of -60°) is plausible only for an administration with an Antarctic programme AND within
//        kAntarcticKm of a station (the list below — bases, not open sea): N6LCO at OC08 is 600 km from Mirny and Casey.
//      ★ /MM (maritime) and /AM (aeronautical) may be anywhere. 4U (UN), 4Y (ICAO) and C7 (WMO) may be anywhere.
//      ★ A portable call with another administration's prefix ("EA8/G4ABC", "G4ABC/EA8", "VK0/K1ABC") is judged by that
//        prefix; a trailing /P /M /QRP /R /A or a district digit ("W1AW/7") changes nothing.
//
// What the callers do with a verdict (the store, the summary, the live spot, PSK Reporter):
//   kNoEntity    → not a station: never stored, never placed, never reported (counted as a set-aside decode).
//   kImplausible → the call is real-shaped and allocated but the grid is not where it can be: KEEP the station, DROP the
//                  grid ("heard without a locator") — never placed at a guessed spot.
//   kOk          → as heard.
// ⚖ The trade-off (said, not hidden): a sailor sending his ocean grid with a plain home call (FT8 cannot carry /MM and a
//   grid in one message) loses his locator here. PSK Reporter would show him; we show him unplaced. Rare, and the cost
//   of never drawing a random point.
// ★ BOUNDED: two static tables (~330 series, ~330 boxes, ~70 stations) parsed once, ~20 KB; a judgement is a few
//   hundred compares, no allocation beyond the call's upper-casing.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vibe {
namespace callentity {

enum Flags : uint8_t { kAntarctic = 1, kAnywhere = 2 };
static constexpr double kLatPad = 2.0, kLonPad = 3.0, kAntarcticKm = 350.0;

/** One administration: its name, its series (space-separated: "K", "AA-AL", "A2", "3DA-3DM") and its boxes
 *  ("south north west east;…" in degrees, w > e = across the antimeridian). */
struct AdminRow { const char* name; uint8_t flags; const char* series; const char* boxes; };

inline const AdminRow* adminRows(size_t& n) {
    static const AdminRow t[] = {
        // ── the single-letter and big blocks ──
        { "United States", kAntarctic, "K N W AA-AL",
          "24 50 -125 -66;51 72 172 -129;18 29 -179 -154;17 19 -68 -64;18 20.5 -76 -74;13 21 144 146.5;-15 -11 -172 -168;"
          "19 20 166 167.5;16 17.5 -170 -169;5.5 7 -163 -161.5;-0.5 1 -177 -176;-1 0 -160.5 -159.5" },
        { "France", kAntarctic, "F HW-HY TH TK TM TO-TQ TV-TX",
          "41.3 51.1 -5.2 9.6;14.3 18.2 -63.2 -60.8;2.1 5.8 -54.6 -51.6;46.7 47.2 -56.5 -56.1;-23 -11 39.5 56;-50 -48.5 68.5 70.6;"
          "-46.5 -46 50 52.4;-39 -37.7 77.4 77.7;-22.8 -19.5 163.5 168.2;-20 -19 158 159;-14.4 -13.2 -178.2 -176;"
          "-28 -7.8 -155 -134;9.8 10.8 -109.7 -108.7" },
        { "United Kingdom", kAntarctic, "G M 2 VP-VQ VS ZB-ZJ ZN-ZO ZQ",
          "49 61 -8.7 2;36 36.2 -5.4 -5.3;34.5 35.2 32.7 33.9;32.2 32.4 -64.9 -64.6;17 22 -82 -61;-52.5 -51 -61.5 -57.6;"
          "-55 -53.9 -38.1 -35.7;-59.5 -56.3 -28.2 -26.2;-16 -15.9 -5.8 -5.6;-8 -7.9 -14.5 -14.3;-40.4 -37 -12.7 -9.8;"
          "-7.5 -5 71 72.6;-25 -24 -130.8 -124.7" },
        { "Italy", kAntarctic, "I", "35.4 47.1 6.6 18.6" },
        { "Russia", kAntarctic, "R UA-UI", "41 82 19.5 -169" },
        { "China", kAntarctic, "B VR XS XX 3H-3U", "18 54 73 135;21.8 25.5 119.3 122.1;14.5 16 117 118.5;15.5 17.5 111 113" },
        // ── A ──
        { "Spain", kAntarctic, "AM-AO EA-EH", "35 44 -10 5;27 30 -19 -13" },
        { "Pakistan", kAntarctic, "AP-AS 6P-6S", "23 37.5 60.5 78" },
        { "India", kAntarctic, "AT-AW VT-VW 8T-8Y", "6 36 68 98" },
        { "Australia", kAntarctic, "AX VH-VN VZ",
          "-44 -10 112 154;-32 -31 158.5 159.5;-29.5 -28.5 167.5 168.5;-18 -17 155.5 156.5;-11 -10 105 106;-12.5 -11.8 96.5 97;"
          "-53.5 -52.5 73 74;-55 -54 158.5 159.5" },
        { "Argentina", kAntarctic, "AY-AZ LO-LW L2-L9", "-55.5 -21.5 -73.6 -53.5" },
        { "Botswana", 0, "A2 8O", "-27 -17.5 20 29.5" },
        { "Tonga", 0, "A3", "-22.5 -15.5 -176 -173.5" },
        { "Oman", 0, "A4", "16.5 26.5 52 60" },
        { "Bhutan", 0, "A5", "26.5 28.5 88.7 92.2" },
        { "United Arab Emirates", 0, "A6", "22.5 26.5 51 56.5" },
        { "Qatar", 0, "A7", "24.4 26.3 50.7 51.7" },
        { "Liberia", 0, "A8 D5 EL 5L-5M 6Z", "4.3 8.6 -11.5 -7.3" },
        { "Bahrain", 0, "A9", "25.5 26.5 50.3 50.9" },
        // ── C ──
        { "Chile", kAntarctic, "CA-CE XQ-XR 3G",
          "-56 -17.5 -76 -66.4;-27.5 -26.8 -109.5 -109;-34 -33.5 -81 -78.5;-28 -25 -110 -105;-26.5 -26 -80.5 -79.5" },
        { "Canada", 0, "CF-CK CY-CZ VA-VG VO VX-VY XJ-XO", "41 84 -141 -52" },
        { "Cuba", 0, "CL-CM CO T4", "19.8 23.3 -85 -74.1" },
        { "Morocco", 0, "CN 5C-5G", "27.6 36 -13.2 -1;21 28 -17.5 -8.5" },
        { "Bolivia", 0, "CP", "-23 -9.6 -69.7 -57.4" },
        { "Portugal", 0, "CQ-CU", "36.5 42.5 -10 -6;36.5 40 -32 -24.5;32 34 -17.5 -16" },
        { "Uruguay", kAntarctic, "CV-CX", "-35 -30 -58.5 -53" },
        { "Nauru", 0, "C2", "-1 0 166.5 167.5" },
        { "Andorra", 0, "C3", "42.4 42.7 1.4 1.8" },
        { "Cyprus", 0, "C4 H2 P3 5B", "34.5 35.7 32.2 34.6" },
        { "Gambia", 0, "C5", "13 13.9 -16.9 -13.8" },
        { "Bahamas", 0, "C6", "20.9 27.3 -79.3 -72.7" },
        { "World Meteorological Organization", kAnywhere, "C7", "" },
        { "Mozambique", 0, "C8-C9", "-27 -10.4 30.2 40.9" },
        // ── D ──
        { "Germany", kAntarctic, "DA-DR Y2-Y9", "47.2 55.1 5.8 15.1" },
        { "South Korea", kAntarctic, "DS-DT D7-D9 HL 6K-6N", "33 38.7 124.5 131.9" },
        { "Philippines", 0, "DU-DZ 4D-4I", "4.5 21.2 116.9 126.7" },
        { "Angola", 0, "D2-D3", "-18.1 -4.3 11.6 24.1" },
        { "Cape Verde", 0, "D4", "14.8 17.3 -25.4 -22.6" },
        { "Comoros", 0, "D6", "-12.5 -11.3 43.2 44.6" },
        // ── E ──
        { "Ireland", 0, "EI-EJ", "51.4 55.5 -10.7 -5.9" },
        { "Armenia", 0, "EK", "38.8 41.3 43.4 46.7" },
        { "Ukraine", kAntarctic, "EM-EO UR-UZ", "44.3 52.4 22.1 40.3" },
        { "Iran", 0, "EP-EQ 9B-9D", "25 39.8 44 63.4" },
        { "Moldova", 0, "ER", "45.4 48.5 26.6 30.2" },
        { "Estonia", 0, "ES", "57.5 59.7 21.7 28.3" },
        { "Ethiopia", 0, "ET 9E-9F", "3.4 15 33 48" },
        { "Belarus", kAntarctic, "EU-EW", "51.2 56.2 23.1 32.8" },
        { "Kyrgyzstan", 0, "EX", "39.1 43.3 69.2 80.3" },
        { "Tajikistan", 0, "EY", "36.6 41.1 67.3 75.2" },
        { "Turkmenistan", 0, "EZ", "35.1 42.8 52.4 66.7" },
        { "Thailand", 0, "E2 HS", "5.6 20.5 97.3 105.7" },
        { "Eritrea", 0, "E3", "12.3 18 36.4 43.2" },
        { "Palestine", 0, "E4", "31.2 32.6 34.2 35.6" },
        { "Cook Islands", 0, "E5", "-22 -8.9 -166 -157.3" },
        { "Niue", 0, "E6", "-19.2 -18.9 -170 -169.7" },
        { "Bosnia-Herzegovina", 0, "E7", "42.5 45.3 15.7 19.7" },
        // ── H ──
        { "Hungary", 0, "HA HG", "45.7 48.6 16.1 22.9" },
        { "Switzerland", 0, "HB HE", "45.8 47.8 5.9 10.5" },
        { "Ecuador", kAntarctic, "HC-HD", "-5 1.5 -81.1 -75.2;-1.5 1.7 -92 -89.2" },
        { "Poland", kAntarctic, "HF SN-SR 3Z", "49 54.9 14.1 24.2" },
        { "Haiti", 0, "HH 4V", "18 20.1 -74.5 -71.6" },
        { "Dominican Republic", 0, "HI", "17.5 20 -72 -68.3" },
        { "Colombia", 0, "HJ-HK 5J-5K", "-4.3 12.5 -79.1 -66.8;3.5 16.5 -82 -79" },
        { "North Korea", 0, "HM P5-P9", "37.6 43 124.2 130.7" },
        { "Iraq", 0, "HN YI", "29 37.4 38.8 48.6" },
        { "Panama", 0, "HO-HP H3 H8-H9 3E-3F", "7.2 9.7 -83.1 -77.2" },
        { "Honduras", 0, "HQ-HR", "13 17.5 -89.4 -83.1" },
        { "Nicaragua", 0, "HT H6-H7 YN", "10.7 15.1 -87.7 -82.6" },
        { "El Salvador", 0, "HU YS", "13.1 14.5 -90.2 -87.6" },
        { "Vatican", 0, "HV", "41.8 42 12.4 12.5" },
        { "Saudi Arabia", 0, "HZ 7Z 8Z", "16.3 32.2 34.5 55.7" },
        { "Solomon Islands", 0, "H4", "-12.4 -5 155.5 170.2" },
        // ── J ──
        { "Japan", kAntarctic, "JA-JS 7J-7N 8J-8N",
          "30 45.6 128.5 146;24 30 122.9 131.4;24 28 141 142.5;24 24.5 153.8 154.1;20.3 20.5 136 136.2" },
        { "Mongolia", 0, "JT-JV", "41.5 52.2 87.7 119.9" },
        { "Norway", kAntarctic, "JW-JX LA-LN 3Y", "57.9 71.2 4.5 31.2;74 81 10 34;70.8 71.2 -9.1 -7.9;-54.5 -54.3 3.3 3.5" },
        { "Jordan", 0, "JY", "29.2 33.4 34.9 39.3" },
        { "Indonesia", 0, "JZ PK-PO YB-YH 7A-7I 8A-8I", "-11.1 6.1 94.9 141.1" },
        { "Djibouti", 0, "J2", "10.9 12.8 41.7 43.5" },
        { "Grenada", 0, "J3", "11.9 12.6 -61.9 -61.4" },
        { "Greece", 0, "J4 SV-SZ", "34.8 41.8 19.3 29.7" },
        { "Guinea-Bissau", 0, "J5", "10.8 12.7 -16.8 -13.6" },
        { "St. Lucia", 0, "J6", "13.7 14.1 -61.1 -60.8" },
        { "Dominica", 0, "J7", "15.2 15.7 -61.5 -61.2" },
        { "St. Vincent", 0, "J8", "12.5 13.4 -61.5 -61.1" },
        // ── L, O, P ──
        { "Luxembourg", 0, "LX", "49.4 50.2 5.7 6.6" },
        { "Lithuania", 0, "LY", "53.9 56.5 20.9 26.9" },
        { "Bulgaria", kAntarctic, "LZ", "41.2 44.3 22.3 28.7" },
        { "Peru", kAntarctic, "OA-OC 4T", "-18.4 0 -81.4 -68.6" },
        { "Lebanon", 0, "OD", "33 34.7 35.1 36.7" },
        { "Austria", 0, "OE", "46.3 49.1 9.5 17.2" },
        { "Finland", kAntarctic, "OF-OJ", "59.7 70.1 19.4 31.6" },
        { "Czechia", kAntarctic, "OK-OL", "48.5 51.1 12.1 18.9" },
        { "Slovakia", 0, "OM", "47.7 49.7 16.8 22.6" },
        { "Belgium", kAntarctic, "ON-OT", "49.5 51.6 2.5 6.4" },
        { "Denmark", 0, "OU-OZ XP 5P-5Q", "54.5 57.8 8 15.2;61.3 62.4 -7.7 -6.2;59.7 83.7 -73.3 -11.3" },
        { "Netherlands", kAntarctic, "PA-PI PJ", "50.7 53.6 3.3 7.3;11.9 12.4 -69.2 -68.2;17.4 18.1 -63.3 -62.9" },
        { "Brazil", kAntarctic, "PP-PY ZV-ZZ",
          "-33.8 5.3 -74 -34.7;-4 -3.8 -32.6 -32.3;0.8 1 -29.4 -29.2;-20.6 -20.4 -29.4 -28.8" },
        { "Suriname", 0, "PZ", "1.8 6.1 -58.1 -53.9" },
        { "Papua New Guinea", 0, "P2", "-11.7 -0.8 140.8 159.5" },
        { "Aruba", 0, "P4", "12.4 12.7 -70.1 -69.8" },
        // ── S ──
        { "Sweden", kAntarctic, "SA-SM 7S 8S", "55.3 69.1 11 24.2" },
        { "Egypt", 0, "SSA-SSM SU 6A-6B", "22 31.7 24.7 36.9" },
        { "Sudan", 0, "SSN-STZ 6T-6U", "8.6 22.3 21.8 38.6" },
        { "Bangladesh", 0, "S2-S3", "20.6 26.7 88 92.7" },
        { "Slovenia", 0, "S5", "45.4 46.9 13.4 16.6" },
        { "Singapore", 0, "S6 9V", "1.1 1.5 103.6 104.1" },
        { "Seychelles", 0, "S7", "-10.3 -3.7 46.2 56.3" },
        { "South Africa", kAntarctic, "S8 ZR-ZU", "-34.9 -22.1 16.4 32.9;-47 -46.6 37.5 38.1" },
        { "Sao Tome & Principe", 0, "S9", "-0.1 1.8 6.4 7.5" },
        { "Western Sahara", 0, "S0", "20.7 27.7 -17.2 -8.6" },
        // ── T ──
        { "Turkey", kAntarctic, "TA-TC YM", "35.8 42.1 25.6 44.9" },
        { "Guatemala", 0, "TD TG", "13.7 17.9 -92.3 -88.2" },
        { "Costa Rica", 0, "TE TI", "8 11.3 -86 -82.5;5.4 5.7 -87.2 -86.9" },
        { "Iceland", 0, "TF", "63.3 66.6 -24.6 -13.4" },
        { "Cameroon", 0, "TJ", "1.6 13.1 8.4 16.2" },
        { "Central African Republic", 0, "TL", "2.2 11.1 14.4 27.5" },
        { "Congo", 0, "TN", "-5.1 3.8 11.1 18.7" },
        { "Gabon", 0, "TR", "-4 2.4 8.6 14.6" },
        { "Tunisia", 0, "TS 3V", "30.2 37.6 7.5 11.7" },
        { "Chad", 0, "TT", "7.4 23.5 13.4 24.1" },
        { "Ivory Coast", 0, "TU", "4.3 10.8 -8.7 -2.4" },
        { "Benin", 0, "TY", "6.2 12.5 0.7 3.9" },
        { "Mali", 0, "TZ", "10.1 25.1 -12.3 4.3" },
        { "Tuvalu", 0, "T2", "-10.8 -5.6 176 179.9" },
        { "Kiribati", 0, "T3", "-2.7 3.4 172.6 177;-1 0 169 170;-4.8 -2.6 -174.6 -170.7;-11.5 4.8 -162.4 -150.2" },
        { "Somalia", 0, "T5 6O", "-1.7 12 40.9 51.5" },
        { "Afghanistan", 0, "T6 YA", "29.3 38.5 60.4 74.9" },
        { "San Marino", 0, "T7", "43.8 44 12.4 12.6" },
        { "Palau", 0, "T8", "2.8 8.2 131.1 134.8" },
        // ── U, V ──
        { "Uzbekistan", 0, "UJ-UM", "37.1 45.6 55.9 73.2" },
        { "Kazakhstan", 0, "UN-UQ", "40.5 55.5 46.4 87.4" },
        { "Antigua & Barbuda", 0, "V2", "16.9 17.8 -62 -61.6" },
        { "Belize", 0, "V3", "15.8 18.5 -89.3 -87.4" },
        { "St. Kitts & Nevis", 0, "V4", "17.1 17.5 -62.9 -62.5" },
        { "Namibia", 0, "V5", "-29 -16.9 11.7 25.3" },
        { "Micronesia", 0, "V6", "1 10.1 137.3 163.1" },
        { "Marshall Islands", 0, "V7", "4.5 14.7 160.7 172.2" },
        { "Brunei", 0, "V8", "4 5.1 114 115.4" },
        // ── X, Y, Z ──
        { "Mexico", 0, "XA-XI 4A-4C 6D-6J", "14 33 -118 -86" },
        { "Burkina Faso", 0, "XT", "9.4 15.1 -5.6 2.4" },
        { "Cambodia", 0, "XU", "10 14.7 102.3 107.7" },
        { "Vietnam", 0, "XV 3W", "8.4 23.4 102.1 109.5" },
        { "Laos", 0, "XW", "13.9 22.5 100 107.7" },
        { "Myanmar", 0, "XY-XZ", "9.6 28.6 92.2 101.2" },
        { "Vanuatu", 0, "YJ", "-20.3 -13 166.5 170.3" },
        { "Syria", 0, "YK 6C", "32.3 37.4 35.7 42.4" },
        { "Latvia", 0, "YL", "55.6 58.1 20.9 28.3" },
        { "Romania", 0, "YO-YR", "43.6 48.3 20.2 29.8" },
        { "Serbia", 0, "YT-YU YZ 4N", "42.2 46.2 18.8 23" },
        { "Venezuela", 0, "YV-YY 4M", "0.6 12.3 -73.4 -59.8;15.6 15.8 -63.7 -63.5" },
        { "Albania", 0, "ZA", "39.6 42.7 19.2 21.1" },
        { "New Zealand", kAntarctic, "ZK-ZM",
          "-47.3 -34.4 166.4 178.6;-44.4 -43.4 -177 -176.1;-31.4 -29.2 -179 -177.8;-52.6 -50.5 165.8 169.3;"
          "-49.8 -47.7 178.7 179.1;-9.5 -8.5 -172.6 -171.1" },
        { "Paraguay", 0, "ZP", "-27.6 -19.3 -62.7 -54.3" },
        { "Zimbabwe", 0, "Z2", "-22.4 -15.6 25.2 33.1" },
        { "North Macedonia", 0, "Z3", "40.8 42.4 20.4 23" },
        { "Kosovo", 0, "Z6", "41.8 43.3 20 21.8" },
        { "South Sudan", 0, "Z8", "3.5 12.3 23.4 36" },
        // ── digits first ──
        { "Sov. Mil. Order of Malta", 0, "1A", "35.4 47.1 6.6 18.6;35.8 36.1 14.1 14.6" },
        { "Spratly Islands", 0, "1S", "7 12 111 117" },
        { "Monaco", 0, "3A", "43.7 43.8 7.4 7.5" },
        { "Mauritius", 0, "3B", "-21 -10 56.5 64" },
        { "Equatorial Guinea", 0, "3C", "-1.5 3.8 5.6 11.4" },
        { "Eswatini", 0, "3DA-3DM", "-27.3 -25.7 30.8 32.1" },
        { "Fiji", 0, "3DN-3DZ 3D0-3D9", "-21.1 -12.4 176.8 -178.2;-22 -21.5 174.4 174.8" },
        { "Guinea", 0, "3X", "7.2 12.7 -15.1 -7.6" },
        { "Azerbaijan", 0, "4J-4K", "38.4 41.9 44.8 50.4" },
        { "Georgia", 0, "4L", "41 43.6 40 46.7" },
        { "Montenegro", 0, "4O", "41.8 43.6 18.4 20.4" },
        { "Sri Lanka", 0, "4P-4S", "5.9 9.9 79.6 81.9" },
        { "United Nations", kAnywhere, "4U", "" },
        { "Timor-Leste", 0, "4W", "-9.5 -8.1 124 127.3" },
        { "Israel", 0, "4X 4Z", "29.5 33.3 34.2 35.9" },
        { "ICAO", kAnywhere, "4Y", "" },
        { "Libya", 0, "5A", "19.5 33.2 9.4 25.2" },
        { "Tanzania", 0, "5H-5I", "-11.8 -1 29.3 40.5" },
        { "Nigeria", 0, "5N-5O", "4.2 13.9 2.7 14.7" },
        { "Madagascar", 0, "5R-5S 6X", "-25.7 -11.9 43.2 50.5" },
        { "Mauritania", 0, "5T", "14.7 27.3 -17.1 -4.8" },
        { "Niger", 0, "5U", "11.7 23.5 0.2 16" },
        { "Togo", 0, "5V", "6.1 11.2 -0.2 1.8" },
        { "Samoa", 0, "5W", "-14.1 -13.4 -172.8 -171.4" },
        { "Uganda", 0, "5X", "-1.5 4.2 29.6 35" },
        { "Kenya", 0, "5Y-5Z", "-4.7 5 33.9 41.9" },
        { "Senegal", 0, "6V-6W", "12.3 16.7 -17.6 -11.3" },
        { "Jamaica", 0, "6Y", "17.7 18.6 -78.4 -76.2" },
        { "Yemen", 0, "7O", "12.1 19 42.5 54.5" },
        { "Lesotho", 0, "7P", "-30.7 -28.6 27 29.5" },
        { "Malawi", 0, "7Q", "-17.2 -9.3 32.6 35.9" },
        { "Algeria", 0, "7R 7T-7Y", "18.9 37.1 -8.7 12" },
        { "Barbados", 0, "8P", "13 13.4 -59.7 -59.4" },
        { "Maldives", 0, "8Q", "-0.7 7.2 72.6 73.8" },
        { "Guyana", 0, "8R", "1.1 8.6 -61.4 -56.4" },
        { "Croatia", 0, "9A", "42.3 46.6 13.4 19.5" },
        { "Ghana", 0, "9G", "4.7 11.2 -3.3 1.2" },
        { "Malta", 0, "9H", "35.8 36.1 14.1 14.6" },
        { "Zambia", 0, "9I-9J", "-18.1 -8.2 21.9 33.7" },
        { "Kuwait", 0, "9K", "28.5 30.1 46.5 48.5" },
        { "Sierra Leone", 0, "9L", "6.9 10 -13.3 -10.3" },
        { "Malaysia", 0, "9M 9W", "0.8 6.8 99.6 104.5;0.8 7.4 109.5 119.3" },
        { "Nepal", 0, "9N", "26.3 30.5 80 88.2" },
        { "DR Congo", 0, "9O-9T", "-13.5 5.4 12.2 31.3" },
        { "Burundi", 0, "9U", "-4.5 -2.3 29 30.9" },
        { "Rwanda", 0, "9X", "-2.9 -1 28.8 30.9" },
        { "Trinidad & Tobago", 0, "9Y-9Z", "10 11.4 -61.9 -60.5" },
    };
    n = sizeof(t) / sizeof(t[0]);
    return t;
}

/** Antarctic stations (year-round and the big summer ones), lat/lon. A grid south of -60° is plausible only within
 *  kAntarcticKm of one, and only for an administration with an Antarctic programme (kAntarctic). */
struct Station { float lat, lon; };
inline const Station* antarcticStations(size_t& n) {
    static const Station t[] = {
        { -77.85f, 166.67f },  // McMurdo (US), Scott Base (NZ) beside it
        { -90.0f, 0.0f },      // Amundsen-Scott South Pole (US)
        { -64.77f, -64.05f },  // Palmer (US)
        { -78.46f, 106.84f },  // Vostok (RU)
        { -66.55f, 93.0f },    // Mirny (RU)
        { -69.38f, 76.39f },   // Progress (RU), Zhongshan (CN), Bharati (IN) beside it
        { -70.77f, 11.82f },   // Novolazarevskaya (RU), Maitri (IN)
        { -62.2f, -58.96f },   // King George Island: Bellingshausen (RU), Frei (CL), Great Wall (CN), King Sejong (KR),
                               //   Artigas (UY), Carlini (AR), Arctowski (PL), Ferraz (BR), Machu Picchu (PE)
        { -62.45f, -59.74f },  // Maldonado (EC), Prat (CL)
        { -62.66f, -60.39f },  // Livingston Island: Juan Carlos I (ES), St Kliment Ohridski (BG)
        { -62.98f, -60.68f },  // Deception Island: Gabriel de Castilla (ES)
        { -63.32f, -57.9f },   // O'Higgins (CL), Esperanza (AR)
        { -63.8f, -57.88f },   // Mendel (CZ)
        { -64.24f, -56.63f },  // Marambio (AR)
        { -65.25f, -64.26f },  // Vernadsky (UA)
        { -67.57f, -68.13f },  // Rothera (UK), Dirck Gerritsz (NL)
        { -68.13f, -67.1f },   // San Martin (AR)
        { -67.8f, -67.3f },    // Horseshoe Island (TR)
        { -75.6f, -26.2f },    // Halley (UK)
        { -60.71f, -45.6f },   // South Orkneys: Signy (UK), Orcadas (AR)
        { -77.87f, -34.63f },  // Belgrano II (AR)
        { -79.77f, -82.9f },   // Union Glacier (CL)
        { -67.6f, 62.87f },    // Mawson (AU)
        { -68.58f, 77.97f },   // Davis (AU)
        { -66.28f, 110.53f },  // Casey (AU)
        { -66.66f, 140.0f },   // Dumont d'Urville (FR)
        { -75.1f, 123.33f },   // Concordia (FR/IT)
        { -74.69f, 164.11f },  // Mario Zucchelli (IT), Jang Bogo (KR), Qinling (CN)
        { -72.01f, 2.53f },    // Troll (NO)
        { -68.8f, -90.6f },    // Peter I Island (NO)
        { -69.0f, 39.58f },    // Syowa (JP)
        { -77.32f, 39.7f },    // Dome Fuji (JP)
        { -80.42f, 77.12f },   // Kunlun (CN)
        { -73.86f, 76.98f },   // Taishan (CN)
        { -70.67f, -8.27f },   // Neumayer III (DE)
        { -75.0f, 0.07f },     // Kohnen (DE)
        { -71.95f, 23.35f },   // Princess Elisabeth (BE)
        { -71.67f, -2.84f },   // SANAE IV (ZA)
        { -73.05f, -13.4f },   // Aboa (FI), Wasa (SE)
        { -67.66f, 46.15f },   // Vecherny (BY)
        { -70.4f, 25.75f },    // Jinnah (PK)
        { -62.08f, -58.39f },  // Admiralty Bay (BR, PL)
    };
    n = sizeof(t) / sizeof(t[0]);
    return t;
}

struct Box { float s, n, w, e; };
struct Series { char from[4], to[4]; uint8_t len; uint16_t admin; };
struct Tables {
    std::vector<Series> series;
    std::vector<std::vector<Box>> boxes;   ///< per admin row
    const AdminRow* rows = nullptr;
    size_t nRows = 0;
};
inline const Tables& tables() {
    static const Tables t = [] {
        Tables o;
        o.rows = adminRows(o.nRows);
        o.boxes.resize(o.nRows);
        for (size_t i = 0; i < o.nRows; ++i) {
            const char* p = o.rows[i].series;
            while (*p) {
                while (*p == ' ') ++p;
                const char* e = p; while (*e && *e != ' ') ++e;
                if (e > p) {
                    const std::string tok(p, (size_t)(e - p));
                    const size_t dash = tok.find('-');
                    const std::string a = dash == std::string::npos ? tok : tok.substr(0, dash);
                    const std::string b = dash == std::string::npos ? tok : tok.substr(dash + 1);
                    if (a.size() == b.size() && a.size() >= 1 && a.size() <= 3) {
                        Series s{}; std::memcpy(s.from, a.c_str(), a.size()); std::memcpy(s.to, b.c_str(), b.size());
                        s.len = (uint8_t)a.size(); s.admin = (uint16_t)i;
                        o.series.push_back(s);
                    }
                }
                p = e;
            }
            const char* q = o.rows[i].boxes;
            while (*q) {
                char* end = nullptr;
                float v[4]; int k = 0;
                for (; k < 4; ++k) { v[k] = std::strtof(q, &end); if (end == q) break; q = end; }
                if (k == 4) o.boxes[i].push_back({ v[0], v[1], v[2], v[3] });
                while (*q && *q != ';') ++q;
                if (*q == ';') ++q;
                if (k != 4 && !*q) break;
            }
        }
        return o;
    }();
    return t;
}

/** The administration a prefix designator (or a whole call) belongs to: its admin row, -1 = none. Longest series wins. */
inline int adminOfPrefix(const std::string& p) {
    const Tables& t = tables();
    for (int len = 3; len >= 1; --len) {
        if ((int)p.size() < len) continue;
        const std::string k = p.substr(0, (size_t)len);
        for (const Series& s : t.series)
            if (s.len == len && std::strncmp(k.c_str(), s.from, (size_t)len) >= 0 && std::strncmp(k.c_str(), s.to, (size_t)len) <= 0) return s.admin;
    }
    return -1;
}

struct Parsed {
    std::string designator;     ///< what decides the administration ("EA8" in EA8/G4ABC, else the call)
    bool mobile = false;        ///< /MM or /AM — anywhere
    int admin = -1;
};
inline bool isModifier(const std::string& s) {
    static const char* const m[] = { "P", "M", "MM", "AM", "QRP", "QRPP", "A", "R", "B", "LH" };
    for (const char* w : m) if (s == w) return true;
    if (!s.empty() && s.size() <= 2) { bool digits = true; for (char c : s) if (c < '0' || c > '9') digits = false; if (digits) return true; }
    return false;
}
/** Split a call into its parts and decide which one names the administration. */
inline Parsed parse(const std::string& in) {
    Parsed o;
    std::string c;
    for (char ch : in) { const char u = (char)std::toupper((unsigned char)ch); if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '/') c += u; }
    std::vector<std::string> parts;
    { std::string part; for (char ch : c) { if (ch == '/') { if (!part.empty()) parts.push_back(part); part.clear(); } else part += ch; } if (!part.empty()) parts.push_back(part); }
    if (parts.empty() || parts.size() > 3) return o;
    // ★ modifiers count only AFTER the first part ("MM/W1AW" = W1AW in Scotland; "W1AW/MM" = maritime mobile)
    std::vector<std::string> keep;
    keep.push_back(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
        if (parts[i] == "MM" || parts[i] == "AM") { o.mobile = true; continue; }
        if (isModifier(parts[i])) continue;
        keep.push_back(parts[i]);
    }
    if (keep.size() == 1) o.designator = keep[0];
    else if (keep.size() == 2) o.designator = keep[0].size() < keep[1].size() ? keep[0] : keep[1].size() < keep[0].size() ? keep[1] : keep[0];
    else return o;
    o.admin = adminOfPrefix(o.designator);
    return o;
}

enum Verdict : uint8_t { kOk = 0, kNoEntity = 1, kImplausible = 2 };
inline const char* verdictWord(Verdict v) { return v == kOk ? "ok" : v == kNoEntity ? "no-entity" : "implausible"; }

/** The administration's name ("" = none). */
inline const char* adminName(int admin) {
    const Tables& t = tables();
    return admin >= 0 && (size_t)admin < t.nRows ? t.rows[admin].name : "";
}
inline const char* nameOf(const std::string& call) { return adminName(parse(call).admin); }

inline double gcKm(double la1, double lo1, double la2, double lo2) {
    const double r = M_PI / 180.0;
    const double a = std::sin((la2 - la1) * r / 2), c = std::sin((lo2 - lo1) * r / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * c * c;
    return 2 * 6371.0 * std::asin(std::min(1.0, std::sqrt(h)));
}
/** A Maidenhead locator's centre (4 or 6 characters, any case). False if it is not one. */
inline bool gridCentre(const std::string& g, double& lat, double& lon) {
    if (g.size() != 4 && g.size() != 6) return false;
    const char a = (char)std::toupper((unsigned char)g[0]), b = (char)std::toupper((unsigned char)g[1]);
    if (a < 'A' || a > 'R' || b < 'A' || b > 'R' || g[2] < '0' || g[2] > '9' || g[3] < '0' || g[3] > '9') return false;
    lon = (a - 'A') * 20.0 - 180.0 + (g[2] - '0') * 2.0;
    lat = (b - 'A') * 10.0 - 90.0 + (g[3] - '0') * 1.0;
    if (g.size() == 6) {
        const char c = (char)std::toupper((unsigned char)g[4]), d = (char)std::toupper((unsigned char)g[5]);
        if (c < 'A' || c > 'X' || d < 'A' || d > 'X') return false;
        lon += (c - 'A') * (2.0 / 24.0) + 1.0 / 24.0; lat += (d - 'A') * (1.0 / 24.0) + 0.5 / 24.0;
    } else { lon += 1.0; lat += 0.5; }
    return true;
}
inline bool inBox(const Box& b, double lat, double lon) {
    if (lat < b.s - kLatPad || lat > b.n + kLatPad) return false;
    const double w = b.w - kLonPad, e = b.e + kLonPad;
    if (b.w <= b.e) {   // ordinary — but the pad may itself cross ±180
        if (lon >= w && lon <= e) return true;
        if (w < -180 && lon >= w + 360) return true;
        if (e > 180 && lon <= e - 360) return true;
        return false;
    }
    return lon >= w || lon <= e;   // across the antimeridian
}

/** ★★ THE JUDGEMENT: may `call` be a station, and may it be at `grid`? ("" grid = only the call is judged.) */
inline Verdict judge(const std::string& call, const std::string& grid, int* adminOut = nullptr) {
    const Parsed p = parse(call);
    if (adminOut) *adminOut = p.admin;
    if (p.admin < 0) return kNoEntity;
    if (grid.empty() || p.mobile) return kOk;
    const Tables& t = tables();
    const AdminRow& a = t.rows[p.admin];
    if (a.flags & kAnywhere) return kOk;
    double lat, lon;
    if (!gridCentre(grid, lat, lon)) return kImplausible;
    if (lat < -60.0) {
        if (!(a.flags & kAntarctic)) return kImplausible;
        size_t n = 0; const Station* st = antarcticStations(n);
        for (size_t i = 0; i < n; ++i) if (gcKm(lat, lon, st[i].lat, st[i].lon) <= kAntarcticKm) return kOk;
        return kImplausible;
    }
    for (const Box& b : t.boxes[p.admin]) if (inBox(b, lat, lon)) return kOk;
    return kImplausible;
}

} // namespace callentity
} // namespace vibe
