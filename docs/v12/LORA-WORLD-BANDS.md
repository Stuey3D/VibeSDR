# LoRa / mesh / ISM world bands — and the ISM-mode BLOCK LIST

Research only (2026-11-12, branch `loraworld`). Nothing in the decoder has been changed. The machine-readable
version is `data/lora-world-bands.json`: regions → channels `{name, kind, hz, bw, sfs, cr, sync, default, tier}`,
the sensor bands, the blocks, and a country → block landing map. Both files were generated from the sources below
by one script, so the tables and the JSON agree.

Stuart asked for the tuning to work like DAB blocks ("11A and 11N overlap"). So ISM mode should step between NAMED
CAPTURE BLOCKS, each about 2 MHz wide, and not between the four bands it has today. Blocks may overlap.

## Sources (every row cites one of these)

| Key | Source |
|---|---|
| RP002 | LoRa Alliance **RP002-1.0.4 Regional Parameters** (2022): Table 2 (dynamic plans: default channels, RX2), Table 3 (US915/AU915 fixed plans), Table 39 (CN470 common join channels), §2.6 (CN779 deprecated), §1.2 Table 1 (country cross-reference), line 4489 ("SyncWord 0x34 (Public)") |
| MT_FW | **meshtastic/firmware** `src/mesh/RadioInterface.cpp` `regions[]` (RDEF band/duty/profile/override slot), `hash()` (djb2), slot formula lines ~1399–1441; `src/mesh/MeshRadio.h` `modemPresetToParams()` (BW/SF/CR); `src/DisplayFormatters.cpp` (preset names that get hashed); `RadioLibInterface.h` `syncWord = 0x2b`. Branch develop = **v2.8.2 prerelease (2026-11-04)**; compared with **v2.7.26 (last stable, 2026-06-24)** |
| MT_DOC | meshtastic.org/docs/configuration/radio/lora (the region table matches MT_FW) |
| MC_API | the **MeshCore app's preset list**, `suggested_radio_settings` from `api.meshcore.nz/api/v1/config` (fetched 2026-11-12). It is the same list as `meshcore-dev/MeshCore docs/radio_presets.md` |
| MC_FW | **meshcore-dev/MeshCore** `platformio.ini` (`LORA_FREQ=869.618, LORA_BW=62.5, LORA_SF=8`); `variants/*/target.cpp` → `radio.begin(…, RADIOLIB_SX126X_SYNC_WORD_PRIVATE /*0x12*/, power, 16 /*preamble*/ …)` |
| TTN_FP | **TheThingsNetwork/lorawan-frequency-plans**: `frequency-plans.yml` (plan → ISO country-codes) and the per-plan YAML (IL_917_920_TTN, MA_869_870_DRAFT, UZ_923_1, RU_864_870_TTN …) |
| TTN_CTRY | thethingsnetwork.org/docs/lorawan/frequencies-by-country (a summary only; it disagrees with TTN_FP in places, e.g. Bolivia) |

## How Meshtastic picks its frequency (MT_FW)

`slotWidth = spacing + 2·padding + BW` and `numSlots = round((freqEnd − freqStart + spacing) / slotWidth)`.
With channel slot 0 (the default), `slot = djb2(primaryChannelName) % numSlots`, where an empty channel name means
the PRESET'S DISPLAY NAME ("LongFast", "MediumFast", "LongMod", …). Then
`freq = freqStart + BW/2 + padding + slot·slotWidth`. So each preset has its own default frequency in each region.
The formula was checked against the three published values: US LongFast **906.875**, EU_868 **869.525**, EU_433
**433.875**. Profiles: STD and EU868 have spacing 0 and padding 0. LITE (EU_866) has 0.4 MHz spacing and 37.5 kHz
padding, giving 865.7/866.3/866.9/867.5, which matches the firmware comment. NARROW (EU_N_868) has 10.4 kHz padding,
giving 869.442/869.525/869.608, which also matches. In 2.8 the ham profiles use fixed override slots.

Meshtastic modem presets (MT_FW `modemPresetToParams`): sub-GHz BW kHz / SF / CR 4/x. "Wide" is LORA_24 on SX128x.

| Preset | BW | SF | CR | Wide BW |
|---|---|---|---|---|
| ShortTurbo | 500 | 7 | 5 | 1625 |
| ShortFast | 250 | 7 | 5 | 812.5 |
| ShortSlow | 250 | 8 | 5 | 812.5 |
| MediumFast | 250 | 9 | 5 | 812.5 |
| MediumSlow | 250 | 10 | 5 | 812.5 |
| MediumTurbo | 500 | 9 | 5 | 1625 |
| **LongFast** (default) | 250 | 11 | 5 | 812.5 |
| LongTurbo | 500 | 11 | 8 | 1625 |
| LongMod(erate) | 125 | 11 | 8 | 406.25 |
| LongSlow | 125 | 12 | 8 | 406.25 |
| LiteFast / LiteSlow (2.8, EU_866 only) | 125 | 9 / 10 | 5 | — |
| NarrowFast / NarrowSlow (2.8, EU_N_868 + ham 70 cm/1.25 m) | 62.5 | 7 / 8 | 6 | — |
| TinyFast / TinySlow (2.8, ham 2 m) | 15.6 | 7 / 8 | 5 / 6 | — |

Sync word **0x2B** (SX127x byte; it is 0x24B4 on SX126x). The preamble is 16 symbols.

## Meshtastic regions — default slot per preset (computed from MT_FW)

| Region | Band (MHz) | Duty % | Slots (LongFast) | LongFast centre (MHz) | Other presets' default centres (MHz) | Firmware |
|---|---|---|---|---|---|---|
| US | 902–928 | 100 | 104 | **906.875** (LongFast, BW 250) | LongSlow 905.312, MediumSlow 914.875, MediumFast 913.125, ShortSlow 920.625, ShortFast 918.875, LongMod 902.688, ShortTurbo 926.75, LongTurbo 908.75, MediumTurbo 925.25 | 2.7 + 2.8 |
| EU_433 | 433–434 | 10 | 4 | **433.875** (LongFast, BW 250) | LongSlow 433.312, MediumSlow 433.875, MediumFast 433.125, ShortSlow 433.625, ShortFast 433.875, LongMod 433.688, ShortTurbo 433.75, LongTurbo 433.75, MediumTurbo 433.25 | 2.7 + 2.8 |
| EU_868 | 869.4–869.65 | 10 | 1 | **869.525** (LongFast, BW 250) | LongSlow 869.462, MediumSlow 869.525, MediumFast 869.525, ShortSlow 869.525, ShortFast 869.525, LongMod 869.587 | 2.7 + 2.8 |
| EU_866 | 865.6–867.6 | 2.5 | 4 | **866.3** (LiteFast, BW 125) | LiteSlow 865.7 | 2.8 pre |
| EU_N_868 | 869.4–869.65 | 10 | 3 | **869.525** (NarrowSlow, BW 62.5) | NarrowFast 869.525 | 2.8 pre |
| CN | 470–510 | 100 | 160 | **478.875** (LongFast, BW 250) | LongSlow 493.312, MediumSlow 504.875, MediumFast 507.125, ShortSlow 492.625, ShortFast 494.875, LongMod 476.688, ShortTurbo 502.75, LongTurbo 470.75, MediumTurbo 495.25 | 2.7 + 2.8 |
| JP | 920.5–923.5 | 100 | 12 | **923.375** (LongFast, BW 250) | LongSlow 920.812, MediumSlow 921.375, MediumFast 920.625, ShortSlow 923.125, ShortFast 922.375, LongMod 922.188, ShortTurbo 923.25, LongTurbo 923.25, MediumTurbo 922.75 | 2.7 + 2.8 |
| ANZ | 915–928 | 100 | 52 | **919.875** (LongFast, BW 250) | LongSlow 918.312, MediumSlow 927.875, MediumFast 926.125, ShortSlow 920.625, ShortFast 918.875, LongMod 915.688, ShortTurbo 926.75, LongTurbo 921.75, MediumTurbo 925.25 | 2.7 + 2.8 |
| ANZ_433 | 433.05–434.79 | 100 | 7 | **434.425** (LongFast, BW 250) | LongSlow 434.613, MediumSlow 434.425, MediumFast 434.425, ShortSlow 433.425, ShortFast 433.425, LongMod 433.738, ShortTurbo 434.3, LongTurbo 434.3, MediumTurbo 433.8 | 2.7 + 2.8 |
| RU | 868.7–869.2 | 100 | 2 | **869.075** (LongFast, BW 250) | LongSlow 869.013, MediumSlow 869.075, MediumFast 868.825, ShortSlow 868.825, ShortFast 869.075, LongMod 868.888, ShortTurbo 868.95, LongTurbo 868.95, MediumTurbo 868.95 | 2.7 + 2.8 |
| KR | 920–923 | 100 | 12 | **922.875** (LongFast, BW 250) | LongSlow 920.312, MediumSlow 920.875, MediumFast 920.125, ShortSlow 922.625, ShortFast 921.875, LongMod 921.688, ShortTurbo 922.75, LongTurbo 922.75, MediumTurbo 922.25 | 2.7 + 2.8 |
| TW | 920–925 | 100 | 20 | **923.875** (LongFast, BW 250) | LongSlow 923.312, MediumSlow 924.875, MediumFast 922.125, ShortSlow 922.625, ShortFast 924.875, LongMod 921.688, ShortTurbo 922.75, LongTurbo 920.75, MediumTurbo 920.25 | 2.7 + 2.8 |
| IN | 865–867 | 100 | 8 | **865.875** (LongFast, BW 250) | LongSlow 866.312, MediumSlow 865.875, MediumFast 866.125, ShortSlow 865.625, ShortFast 865.875, LongMod 865.688, ShortTurbo 865.75, LongTurbo 865.75, MediumTurbo 866.25 | 2.7 + 2.8 |
| NZ_865 | 864–868 | 100 | 16 | **864.875** (LongFast, BW 250) | LongSlow 867.312, MediumSlow 866.875, MediumFast 865.125, ShortSlow 866.625, ShortFast 864.875, LongMod 866.688, ShortTurbo 864.75, LongTurbo 864.75, MediumTurbo 865.25 | 2.7 + 2.8 |
| TH | 920–925 | 10 | 20 | **923.875** (LongFast, BW 250) | LongSlow 923.312, MediumSlow 924.875, MediumFast 922.125, ShortSlow 922.625, ShortFast 924.875, LongMod 921.688, ShortTurbo 922.75, LongTurbo 920.75, MediumTurbo 920.25 | 2.7 + 2.8 |
| UA_433 | 433–434.7 | 10 | 7 | **434.375** (LongFast, BW 250) | LongSlow 434.562, MediumSlow 434.375, MediumFast 434.375, ShortSlow 433.375, ShortFast 433.375, LongMod 433.688, ShortTurbo 434.25, LongTurbo 434.25, MediumTurbo 433.75 | 2.7 + 2.8 |
| UA_868 | 868–868.6 | 1 | 2 | **868.375** (LongFast, BW 250) | LongSlow 868.188, MediumSlow 868.375, MediumFast 868.125, ShortSlow 868.125, ShortFast 868.375, LongMod 868.438, ShortTurbo 868.25, LongTurbo 868.25, MediumTurbo 868.25 | 2.7 only (removed 2.8) |
| MY_433 | 433–435 | 100 | 8 | **433.875** (LongFast, BW 250) | LongSlow 434.312, MediumSlow 433.875, MediumFast 434.125, ShortSlow 433.625, ShortFast 433.875, LongMod 433.688, ShortTurbo 433.75, LongTurbo 433.75, MediumTurbo 434.25 | 2.7 + 2.8 |
| MY_919 | 919–924 | 100 | 20 | **922.875** (LongFast, BW 250) | LongSlow 922.312, MediumSlow 923.875, MediumFast 921.125, ShortSlow 921.625, ShortFast 923.875, LongMod 920.688, ShortTurbo 921.75, LongTurbo 919.75, MediumTurbo 919.25 | 2.7 + 2.8 |
| SG_923 | 917–925 | 100 | 32 | **917.875** (LongFast, BW 250) | LongSlow 924.312, MediumSlow 919.875, MediumFast 922.125, ShortSlow 923.625, ShortFast 917.875, LongMod 923.688, ShortTurbo 917.75, LongTurbo 917.75, MediumTurbo 918.25 | 2.7 + 2.8 |
| PH_433 | 433–434.7 | 100 | 7 | **434.375** (LongFast, BW 250) | LongSlow 434.562, MediumSlow 434.375, MediumFast 434.375, ShortSlow 433.375, ShortFast 433.375, LongMod 433.688, ShortTurbo 434.25, LongTurbo 434.25, MediumTurbo 433.75 | 2.7 + 2.8 |
| PH_868 | 868–869.4 | 100 | 6 | **869.375** (LongFast, BW 250) | LongSlow 869.312, MediumSlow 868.875, MediumFast 868.125, ShortSlow 869.125, ShortFast 868.375, LongMod 868.562, ShortTurbo 869.25, LongTurbo 869.25, MediumTurbo 868.75 | 2.7 + 2.8 |
| PH_915 | 915–918 | 100 | 12 | **917.875** (LongFast, BW 250) | LongSlow 915.312, MediumSlow 915.875, MediumFast 915.125, ShortSlow 917.625, ShortFast 916.875, LongMod 916.688, ShortTurbo 917.75, LongTurbo 917.75, MediumTurbo 917.25 | 2.7 + 2.8 |
| KZ_433 | 433.075–434.775 | 100 | 7 | **434.45** (LongFast, BW 250) | LongSlow 434.637, MediumSlow 434.45, MediumFast 434.45, ShortSlow 433.45, ShortFast 433.45, LongMod 433.762, ShortTurbo 434.325, LongTurbo 434.325, MediumTurbo 433.825 | 2.7 + 2.8 |
| KZ_863 | 863–868 | 100 | 20 | **866.875** (LongFast, BW 250) | LongSlow 866.312, MediumSlow 867.875, MediumFast 865.125, ShortSlow 865.625, ShortFast 867.875, LongMod 864.688, ShortTurbo 865.75, LongTurbo 863.75, MediumTurbo 863.25 | 2.7 + 2.8 |
| NP_865 | 865–868 | 100 | 12 | **867.875** (LongFast, BW 250) | LongSlow 865.312, MediumSlow 865.875, MediumFast 865.125, ShortSlow 867.625, ShortFast 866.875, LongMod 866.688, ShortTurbo 867.75, LongTurbo 867.75, MediumTurbo 867.25 | 2.7 + 2.8 |
| BR_902 | 902–907.5 | 100 | 22 | **903.875** (LongFast, BW 250) | LongSlow 903.312, MediumSlow 907.375, MediumFast 906.625, ShortSlow 905.125, ShortFast 904.375, LongMod 906.688, ShortTurbo 905.75, LongTurbo 906.25, MediumTurbo 902.75 | 2.7 + 2.8 |


2.8 prerelease ham regions, which need licensed mode, use fixed override slots: ITU1_2M 144.510, ITU2_2M 145.010
and ITU3_2M 144.650 (TinyFast, 15.6 kHz); ITU2_125CM 223.650; ITU1/2/3_70CM **433.650** (NarrowSlow 62.5 kHz
SF8 CR4/6). The 70 cm slot is the same frequency as MeshCore's EU 433 preset, so the 433 block covers it.
LORA_24 (2.4 GHz) LongFast is 2420.71875 MHz (BW 812.5, slot 26/103).

## MeshCore presets (MC_API; firmware default from MC_FW)

The firmware build default with no app is **869.618 MHz / BW 62.5 / SF8 / CR5**. The sync word is **0x12** (RadioLib
private) and the preamble is 16. The app list has no sync-word field, so every preset uses 0x12. Every preset is a
community default for its area, so every non-deprecated preset is tier 0.

| MeshCore preset | MHz | BW kHz | SF | CR 4/x |
|---|---|---|---|---|
| Australia | 915.800 | 250 | 10 | 5 |
| Australia (Narrow) | 916.575 | 62.5 | 7 | 7 |
| Australia (Mid) | 915.075 | 125 | 9 | 5 |
| Australia: SA, WA | 923.125 | 62.5 | 8 | 8 |
| Australia: QLD | 923.125 | 62.5 | 8 | 5 |
| Brazil | 923.125 | 62.5 | 8 | 8 |
| Canada | 910.525 | 62.5 | 7 | 5 |
| Costa Rica | 910.525 | 125 | 11 | 5 |
| EU/UK (Narrow) | 869.618 | 62.5 | 8 | 8 |
| EU/UK (Deprecated) | 869.525 | 250 | 11 | 5 |
| Czech Republic (Narrow) | 869.432 | 62.5 | 7 | 5 |
| EU 433MHz (Long Range) | 433.650 | 250 | 11 | 5 |
| EU 433MHz (Narrow) | 433.650 | 62.5 | 8 | 8 |
| Hungary | 869.618 | 62.5 | 7 | 5 |
| Netherlands | 869.618 | 62.5 | 7 | 5 |
| Netherlands (Limburg) | 869.618 | 62.5 | 8 | 8 |
| New Zealand (Narrow) | 917.375 | 62.5 | 7 | 5 |
| New Zealand (Gisborne) | 917.375 | 250 | 11 | 5 |
| Portugal 433 | 433.375 | 62.5 | 9 | 6 |
| Portugal 868 | 869.618 | 62.5 | 7 | 6 |
| Slovakia | 869.618 | 62.5 | 7 | 5 |
| Switzerland | 869.618 | 62.5 | 8 | 8 |
| Thailand | 921 | 62.5 | 7 | 5 |
| USA | 910.525 | 62.5 | 7 | 5 |
| USA - Southern California | 927.875 | 62.5 | 7 | 5 |
| Vietnam (Narrow) | 920.250 | 62.5 | 8 | 5 |
| Vietnam (Deprecated) | 920.250 | 250 | 11 | 5 |

## LoRaWAN regional plans (RP002-1.0.4 unless marked)

Uplinks are 125 kHz, the sync word is **0x34** (public; private networks use 0x12), and the preamble is 8.
Downlinks use inverted IQ.

| Plan | Band (MHz) | Default / join uplinks (MHz) | RX2 (MHz, DR) | Notes | Countries (TTN_FP codes) |
|---|---|---|---|---|---|
| EU868 | 863–870 | 868.1, 868.3, 868.5 | 869.525 SF12 (TTN: SF9) | 867.1–867.9 are the usual CFList additions (TTN_FP), not RP002 defaults; duty < 1 % / 10 % at 869.4–869.65 | EU, GB, CH, ZA, AE, SA, TR, RU, PH … (59 codes in TTN_FP) |
| EU433 | 433–434 | 433.175, 433.375, 433.575 | 434.665 SF12 | rare in practice | R1 433 users |
| US915 | 902–928 | 64 × 125 k 902.3–914.9 (0.2 step) + 8 × 500 k 903.0–914.2; **FSB2 903.9–905.3 + 904.6** is the one TTN and most public networks use | 923.3 (500 k, DR8 = SF12) | down 8 × 500 k 923.3–927.5 (0.6 step); 400 ms dwell | US CA MX PR CR PA EC GY |
| AU915 | 915–928 | 64 × 125 k 915.2–927.8 + 8 × 500 k 915.9–927.1; **FSB2 916.8–918.2 + 917.5** (TTN) | 923.3 (500 k) | down as US915 | AU NZ AR BR CL CO PE UY PY BO SR DO (+ CA) |
| AS923-1 | 915–928 | 923.2, 923.4 | 923.2 SF10 | AU operators recommend AS923-1 (RP002 fn. 4) | JP SG TW TH MY HK (AU, NZ) |
| AS923-2 | 915–928 | 921.4, 921.6 | 921.4 SF10 | | ID VN |
| AS923-3 | 915–928 | 916.6, 916.8 | 916.6 SF10 | | PH CU (and the 915–918/921 rows of RP002 Table 1) |
| AS923-4 | 917–920 | 917.3, 917.5 | 917.3 SF10 | TTN IL plan adds 917.7, 917.9, 919.3–919.9 | IL |
| KR920 | 920.9–923.3 | 922.1, 922.3, 922.5 | 921.9 SF12 | | KR |
| IN865 | 865–867 | 865.0625, 865.4025, 865.985 | 866.55 SF10 | | IN |
| RU864 | 864–870 | 868.9, 869.1 | 869.1 SF12 | TTN adds 864.1–864.9 | RU |
| CN470 | 470–510 | 20 common join channels: 470.9, 472.5, 474.1, 475.7, 504.1, 505.7, 507.3, 508.9, 479.9, 499.9, 470.3, 472.3 … 488.3 (Table 39) | per sub-plan (Tables 47/48) | **spread over 40 MHz: a scan list, not a block** (only 479.9 sits in block `479`); SF12 dropped in 1.0.x | CN |
| CN779 | 779–787 | *not given in 1.0.4 (defined in LoRaWAN 1.0.1)* | — | **deprecated**; no new devices after 2021-01-01 | CN |
| MA869 *(TTN draft)* | 869–870 | 869.1, 869.3 (+869.7, 869.9) | 869.5 | not in RP002, so unconfirmed | MA |
| UZ923 *(TTN draft)* | 915–928 | 926.2, 926.4, 926.6, 926.8 | 926.6 | not in RP002, so unconfirmed | UZ |
| IL915 | — | none: RP002 says Israel is AS923-4 (917–920) | | the name "IL915" does not exist | IL |

## Sensor bands (non-LoRa ISM/SRD)

| Band | Range (MHz) | Where | Source |
|---|---|---|---|
| 315 | 310–320 (FCC 15.231 covers 260–470); JP 312–315.25 | US, CA, JP, KR | FCC 47 CFR 15.231; JP range from ARIB STD-T93, **unconfirmed** |
| 426 | ~426.0–426.15 | JP "specified low power" telemetry | **unconfirmed** (ARIB STD-T67 not checked) |
| 433.92 | 433.05–434.79 | ITU R1 (RR 5.138), plus AU/NZ (LIPD), BR, MY and much of R2/R3 | ITU RR 5.138; ERC/REC 70-03 |
| 868/869 | 863–870; sensors at 868.3, wM-Bus T1/C1 868.95; 869.4–869.65 at 10 % / 500 mW | CEPT | ERC/REC 70-03 Annex 1; 2006/771/EC; EN 300 220 |
| 865–867 | 865–867 | IN | RP002 Table 1 |
| 902–928 | 902–928 (915 sensors) | ITU R2 (RR 5.150) | ITU RR 5.150; FCC 15.247/15.249 |
| 915–928 | AU/NZ 915–928; BR 902–907.5 + 915–928 | AU NZ BR | ACMA LIPD; MT_FW BR_902 comment; RP002 Table 1 |
| 920–923/925 | JP 920.5–928 (T108), KR 917–923.5, AS 920–925 | JP KR TW TH SG MY VN ID | RP002 Table 1 |
| 470–510 | 470–510 | CN | RP002 §2.9 |

## ★ THE BLOCK LIST (like DAB blocks — they overlap)

Rule: a channel is inside a block when **|f − centre| + BW/2 ≤ 0.9 MHz** (2.048 MS/s, ±0.9 MHz usable). The
decoder's own `choose()` edge is 0.45 × rate = ±921.6 kHz, so every row below passes it. "Centre" is where the
RADIO is tuned, so its DC lands at the centre. Every centre was placed off the busy channels (433.92, 868.95 and the
mesh defaults). The script checks every **default** channel in every region; the result is **0 uncovered** (CN470's
40 MHz join list is a scan list, as noted). Fourteen blocks are core (315 plus 13 LoRa); six are optional.

| Block | Centre (MHz) | Label | Landing for | Default channels inside | Sensor note |
|---|---|---|---|---|---|
| `315` | 315.25 | 315 US/JP sensors | US, CA, JP | — | 315.00 keeps today's 250 kHz DC offset (radio at 315.25) |
| `433` | 434.00 | 433.9 ISM + 433 mesh | ITU R1 433, MY_433, UA, PH, KZ, ANZ_433 | MT EU_433 LongFast; MT ANZ_433 LongFast; MT UA_433 LongFast; MT MY_433 LongFast; MT PH_433 LongFast; MT KZ_433 LongFast; MT ITU1_70CM NarrowSlow; MT ITU2_70CM NarrowSlow; MT ITU3_70CM NarrowSlow; MC EU 433MHz (Long Range); MC EU 433MHz (Narrow); MC Portugal 433; LW EU433 433.175; LW EU433 433.375; LW EU433 433.575 | 433.92 sits 80 kHz below DC: keep it out of the DC notch; 433.05-434.79 fully inside except the bottom 50 kHz (433.05-433.10) |
| `479` | 479.30 | 479 CN | CN | MT CN LongFast; LW CN470 479.9 | no sensor band here (CN SRD only) |
| `865` | 865.60 | 865 IN/NZ | IN, NZ_865 | MT EU_866 LiteFast; MT IN LongFast; MT NZ_865 LongFast; LW IN865 865.0625; LW IN865 865.4025; LW IN865 865.985 | IN 865-867 SRD |
| `867` | 867.20 | 867 EU866/KZ/NP | NP, KZ | MT KZ_863 LongFast; MT NP_865 LongFast | lower 863-870 SRD |
| `869` | 868.85 | 869 EU/UK/RU mesh | EU, GB, RU, PH_868, UA_868 | MT EU_868 LongFast; MT EU_N_868 NarrowSlow; MT RU LongFast; MT UA_868 LongFast; MT PH_868 LongFast; MC EU/UK (Narrow); MC Czech Republic (Narrow); MC Hungary; MC Netherlands; MC Netherlands (Limburg); MC Portugal 868; MC Slovakia; MC Switzerland; LW EU868 868.1; LW EU868 868.3; LW EU868 868.5; LW RU864 868.9; LW RU864 869.1; LW MA869 (draft) 869.1; LW MA869 (draft) 869.3 | 868.3 and 868.95 wM-Bus inside; DC at 868.85 is 100 kHz below 868.95 |
| `904` | 904.60 | 904.6 US-LoRaWAN/BR | BR (Meshtastic BR_902) | MT BR_902 LongFast; LW US915 903.9; LW US915 904.1; LW US915 904.3; LW US915 904.5; LW US915 904.7; LW US915 904.9; LW US915 905.1; LW US915 905.3 | 902-928 sensors partially (915 is NOT in this block) |
| `907` | 906.60 | 906.9 US Meshtastic | US, CA, MX | MT US LongFast | — |
| `910` | 910.40 | 910.5 US/CA MeshCore | US (MeshCore), CA, CR | MC Canada; MC Costa Rica; MC USA | — |
| `913` (optional) | 913.40 | 913 US MediumFast | — | — | 915 US sensors fit (915.0 is 600 kHz above centre) |
| `915` | 915.80 | 915.8 AU MeshCore/915 ISM | AU (MeshCore) | MC Australia; MC Australia (Narrow); MC Australia (Mid); LW AS923-3 916.6 | 915.00 US/AU sensors inside (800 kHz below centre) |
| `917` | 917.40 | 917.4 AU/NZ/AS923-3/4/PH/SG | NZ (MeshCore), PH, SG, IL, AU (LoRaWAN) | MT SG_923 LongFast; MT PH_915 LongFast; MC Australia (Narrow); MC New Zealand (Narrow); MC New Zealand (Gisborne); LW AU915 916.8; LW AU915 917.0; LW AU915 917.2; LW AU915 917.4; LW AU915 917.6; LW AU915 917.8; LW AU915 918.0; LW AU915 918.2; LW AS923-3 916.6; LW AS923-3 916.8; LW AS923-4 917.3; LW AS923-4 917.5; LW IL917 (TTN, AS923-4 base) 917.3; LW IL917 (TTN, AS923-4 base) 917.5; LW IL917 (TTN, AS923-4 base) 917.7; LW IL917 (TTN, AS923-4 base) 917.9 | — |
| `920` | 920.10 | 920 ANZ Meshtastic/VN/TH | AU/NZ (Meshtastic), VN, TH (MeshCore) | MT ANZ LongFast; MC Vietnam (Narrow); LW IL917 (TTN, AS923-4 base) 919.3; LW IL917 (TTN, AS923-4 base) 919.5; LW IL917 (TTN, AS923-4 base) 919.7; LW IL917 (TTN, AS923-4 base) 919.9 | — |
| `922` | 921.70 | 921.7 AS923-2/KR LoRaWAN | ID, KR (LoRaWAN) | MC Thailand; LW AS923-2 921.4; LW AS923-2 921.6; LW KR920 922.1; LW KR920 922.3; LW KR920 922.5 | — |
| `923` | 923.15 | 923 BR/AU/AS/JP/KR mesh | BR (MeshCore), JP, KR, TW, TH, MY, AU SA/WA/QLD | MT JP LongFast; MT KR LongFast; MT TW LongFast; MT TH LongFast; MT MY_919 LongFast; MC Australia: SA, WA; MC Australia: QLD; MC Brazil; LW AS923-1 923.2; LW AS923-1 923.4; LW KR920 922.5 | — |
| `925` (optional) | 925.40 | 925 US/AU LoRaWAN downlinks | — | LW UZ923 (draft) 926.2 | — |
| `927` (optional) | 927.05 | 927.9 SoCal/UZ | US-SoCal (MeshCore), UZ | MC USA - Southern California; LW UZ923 (draft) 926.4; LW UZ923 (draft) 926.6; LW UZ923 (draft) 926.8 | — |
| `144` (optional) | 144.80 | 2 m ham Meshtastic (licensed) | — | MT ITU1_2M TinyFast; MT ITU2_2M TinyFast; MT ITU3_2M TinyFast | — |
| `223` (optional) | 223.65 | 1.25 m ham Meshtastic (R2) | — | MT ITU2_125CM NarrowSlow | — |
| `2420` (optional) | 2420.72 | 2.4 GHz LORA_24 | — | MT LORA_24 LongFast | — |


Design notes:

- **`433` at 434.00** keeps all of 433.05–434.79 except its bottom 50 kHz. 433.92 is 80 kHz below the DC point,
  so the sensors stay clear of the notch. EU433 LoRaWAN 433.175 fits with a 12 kHz margin. A 2.4 MS/s capture
  gives it plenty of room.
- **`869` at 868.85** holds EU868 LoRaWAN 868.1–868.5, the whole 869.4–869.65 mesh sub-band (Meshtastic EU_868 and
  EU_N_868, all the MeshCore EU presets), RU864 and RU Meshtastic, PH_868, and wM-Bus 868.3/868.95. The EU 867.x
  LoRaWAN additions live in `867`.
  ★ **Today's `868` band (868.6, DC offset 0) does not reach Meshtastic LongFast 869.525 at 2.048 MS/s:**
  |869.525 − 868.6| + 0.125 = 1.05 MHz, more than the 0.92 MHz edge. It works only at 2.4 MS/s or more.
- **`923` at 923.15** is the busiest block in the world: MeshCore BR/AU 923.125, AS923-1, and Meshtastic JP, KR,
  TW, TH and MY_919. TW/TH LongFast 923.875 fits with 50 kHz to spare. KR LoRaWAN 922.1 does not fit, so it is in
  `922`.
- **US needs three blocks**, because its defaults are spread out: Meshtastic 906.875, MeshCore 910.525 and LoRaWAN
  FSB2 at 903.9–905.3. The other presets hash elsewhere; MediumFast lands on 913.125, so `913` is optional.
- **AU needs `915` (MeshCore 915.8 and AU Narrow), `917` (LoRaWAN AU915 FSB2 and NZ MeshCore), `920`
  (Meshtastic ANZ 919.875) and `923` (MeshCore SA/WA/QLD 923.125).**

Non-default channels outside every block (the JSON lists them): most other Meshtastic preset slots in US, CN, ANZ,
KZ_863, SG_923 and BR_902; the US/AU 500 kHz downlinks at 923.9, 924.5 and 926.3; RU 864.1–864.7; MA 869.7/869.9;
and CN RX2 492.5. They can be added as optional blocks, or reached with the exact-frequency box.

### Feasibility per radio

| Block(s) | RTL-SDR (24–1766) | Airspy R2/Mini (24–1800) | SDRplay RSP (1 k–2000) | HackRF (1–6000) | USRP (B2xx 70–6000) | Airspy HF+ |
|---|---|---|---|---|---|---|
| 315, 433, 479, 865–927 | yes | yes | yes | yes | yes | **no** (HF+ stops at 260 MHz; `kMinRateHz` already excludes it) |
| 144, 223 (ham, optional) | yes | yes | yes | yes | yes | no |
| 2420 (LORA_24, optional) | **no** | **no** | **no** | yes | yes | no |

A 2 MHz span is the narrowest rate on every radio here. A wider capture, such as RSP 8–10 MHz, HackRF 20 MHz or
USRP 56 MHz, can hold several adjacent blocks at once: `920`+`922`+`923`+`925`, or `904`+`907`+`910`. That is what
DAB "11A + 11N" means in practice.

## LoRa decoder fit (read from `vibe_lora.h` / `vibe_lora_service.h` at beta70)

The decoder today: CSS demod for **SF 7–12**, **any BW** (the Channeliser takes any `bw` and resamples to exactly the
chip rate), **CR 4/5–4/8** read from the explicit header, the payload CRC, LDRO by the datasheet rule (symbol over
16 ms), a sync-word filter per channel, inverted IQ per channel, and preambles up to about 72 windows. The plans in
`planFor()` are eu868, eu433 and us915 (sub-band 8, 914.3–914.9).

Every combination needed worldwide:

| BW kHz | SF | CR | Sync | Who | Supported? |
|---|---|---|---|---|---|
| 125 | 7–12 | 4/5 | 0x34 (0x12 private) | LoRaWAN uplinks, all plans | yes |
| 125 | 7–12 | 4/5 | 0x34, inverted IQ | LoRaWAN RX2 (EU, AS, KR, IN, RU, EU433) | yes (plan entry needed) |
| 500 | 7–12 (DR8–13) | 4/5 | 0x34, inverted IQ | US915/AU915 downlinks, RX2 923.3 | yes (no plan entries yet; 4× an SF's cost at 125 k) |
| 500 | 8 (US DR4) / 8 (AU DR6) | 4/5 | 0x34 | US904.6 / AU917.5 wide uplinks | yes (no plan entry) |
| 250 | 7–11 | 4/5 | 0x2B | Meshtastic Short/Medium/LongFast | yes |
| 500 | 7, 9, 11 | 4/5, 4/8 | 0x2B | Meshtastic Short/Medium/LongTurbo | yes (no plan entries) |
| 125 | 9–12 | 4/5, 4/8 | 0x2B | Meshtastic LongMod/LongSlow, LiteFast/Slow (EU_866) | yes |
| 62.5 | 7, 8 | 4/6 | 0x2B | Meshtastic NarrowFast/Slow (EU_N_868, ham 70 cm/1.25 m) | yes in code, **no plan entry, untested** |
| **15.6** | 7, 8 | 4/5, 4/6 | 0x2B | Meshtastic TinyFast/Slow (ham 2 m, 2.8) | **untested**: SF8 at 15.6 k is 16.4 ms, so LDRO is on; ~13 Channeliser bins; carrier-offset tolerance is tight |
| 62.5 | 7–9 | 4/5–4/8 | 0x12 | MeshCore narrow presets (incl. AU Narrow CR 4/7) | yes |
| 125 | 9, 11 | 4/5 | 0x12 | MeshCore AU Mid, Costa Rica | yes (no plan entry) |
| 250 | 10, 11 | 4/5 | 0x12 | MeshCore AU 915.8 SF10, EU 433 LR, NZ Gisborne, deprecated EU/VN | yes |
| 812.5 / 406.25 / 1625 | 11, 12 … | | SX128x | Meshtastic LORA_24 | **gap**: unreachable on RTL/Airspy/RSP; SX128x framing not verified |

**Gaps, in order of value:**
1. **Plans, not DSP.** The blocks above need `planFor()` entries: AU915/AS923/KR/IN/RU/CN, Meshtastic outside
   US/EU, every MeshCore preset, EU_866 LiteFast/Slow, EU_N_868 NarrowSlow at 869.525 (62.5 k, 0x2B; not the same
   channel as MeshCore 869.618), and US915 **FSB2**. Today's `us915` plan is sub-band 8 (914.3–914.9); TTN and
   most public US networks use FSB2.
2. **BW 15.6 kHz** (Meshtastic ham 2 m, 2.8 prerelease): needs a test vector. No code change is expected.
3. **Implicit header**: the decoder is explicit-header only. LoRaWAN **Class B beacons** (EU 869.525 SF9, AS923
   and others) use implicit header and no CRC, so they are not decodable. This is minor.
4. **LR-FHSS** (EU868 DR8–11, US915 DR5–6) and **FSK** (EU868 DR7 at 868.8): these are not LoRa CSS, so they are
   out of scope.
5. **SF5/SF6**: no preset or plan uses them (Meshtastic only mentions them for custom settings), so they are not
   needed.
6. Sync words: 0x34, 0x12 and 0x2B cover everything found. No other community sync word was found; this is
   unconfirmed beyond these three projects.

There was **no BW 31.25 or 41.7 kHz** in any preset or plan. Meshtastic's `bwKHzToCode()` accepts them, but only for
custom settings.

## Country → block (by ISO code)

`data/lora-world-bands.json` → `countryLanding.byIso` gives 102 ISO codes, each with an ordered block list; the
first is the landing block. Examples:

- GB/EU/CH: `869, 433, 867`
- US: `907, 910, 904, 315, 913`
- CA: `910, 907, 904, 315`
- BR: `923, 904, 433`
- AU: `915, 920, 917, 923, 433`
- NZ: `917, 920, 865, 433`
- IN: `865`
- CN: `479`
- JP: `923, 315`
- KR: `923, 922`
- ID: `922`
- VN: `920, 922`
- IL: `917, 920`
- RU: `869, 433`
- KZ: `867, 433`
- NP: `867, 865`

Any other country falls back by ITU region: R1 → `869, 433`; R2 → `907, 910, 315`; R3 → `923, 433`.

The map was built from the Meshtastic region labels, the MeshCore preset titles and the TTN_FP country-codes (the
LoRaWAN lists), with RP002 Table 1 spot checks. **Lat/lon polygons are not needed**: no country here needs splitting
by area, except AU (MeshCore SA/WA/QLD use 923.125 while the rest of AU uses 915.8) and US Southern California
(927.875). Those two are best done by giving the owner a choice of block, not by a polygon.

## Unconfirmed / not checked against a primary source

- **RP002 Table 1 as a whole** (country × band × plan, about 250 rows): the PDF table did not extract cleanly. Only
  rows checked by hand are relied on: IN, ID, IL, KR, US, CA, MX, BR, SG, TW, TH, MY, PH, VN, GB, HU, ZA, RU, KZ, CU,
  EG, HK. The full table should be transcribed by hand before the country list is trusted for the 140 countries
  not mapped here.
- **Latin America (AR, CL, CO, PE, UY, PY, BO, SR, DO, EC)**: TTN_FP puts most of them on AU915, and the map lands
  them on `917`. **Which Meshtastic region they use (US vs ANZ) was not confirmed**; if it is US, add `907`.
  TTN_CTRY says Bolivia is US902 while TTN_FP says AU915.
- The JP 315 MHz range, the JP 426 MHz band and the IN GSR 564(E) text were not read.
- CN779 default channels (only in LoRaWAN 1.0.1; deprecated).
- MA869 and UZ923 are **TTN drafts**, not RP002.
- Meshtastic **EU_866, EU_N_868 and the ham regions exist only in the 2.8 prereleases**. **UA_868** (868.0–868.6,
  LongFast 868.375) exists in 2.7.26 and was removed in 2.8.
- "US communities moving to MediumFast (913.125)": block `913` is optional for that reason. Its real-world use was
  not confirmed.
- The MeshCore preset list is server-fetched by the app and changes over time. The copy here is from 2026-11-12.
