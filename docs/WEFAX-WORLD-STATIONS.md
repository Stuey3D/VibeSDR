# World HF WEFAX (radiofacsimile) broadcasters: station table for auto-align testing

Compiled 2026-10-07 for VibeSDR's WEFAX auto-align work. Text research only: no images were downloaded.
Status is "as of" the newest dated source cited; confidence is given per row.

## Conventions used below

- **Dial / USB**: most official lists give the *assigned (centre) frequency*. For an SSB receiver in USB the
  *carrier / dial* frequency is **assigned − 1.9 kHz** (black 1500 Hz, white 2300 Hz audio). NWS says this is the
  default for every entry in its worldwide PDF unless noted, and the US schedules state it explicitly
  ("CARRIER FREQUENCY IS 1.9 kHz BELOW THE ASSIGNED FREQUENCY"). Exceptions are flagged.
- **Line width at IOC 576** = 576·π ≈ **1809.6 px per line**. Converting a line-rate error to slant:
  **slant (px/line) ≈ ppm × 1.81 × 10⁻³**.
- **Slant (ppm)** is Gough Lui's kiwifax.py `--sr-coeff` value (units: ppm, "positive if the lines are too short";
  measured on GPS-locked KiwiSDRs, "trial and error … close enough", Feb 2019). **Cross-check:** GYA +34 ppm →
  0.0615 px/line, which matches our own GYA measurement of −0.06 px/line. So his figures look like the
  transmitter's real line-rate error, with the opposite sign to ours (our px/line ≈ −ppm × 1.81e-3).
- **WMO/standard APT**: start tone 300 Hz (IOC 576) or 675 Hz (IOC 288), about 5–10 s; phasing about 30 s;
  stop tone 450 Hz, about 5–15 s. KMA documents its own exactly: 300 Hz for 10 s, phasing 30 s, stop 15 s.
- **Licence**: "PD" means a US federal work (17 U.S.C. §105), which is public domain. Everything else is copyrighted
  unless an open licence is stated.

---

## A. Station table: active or probably active

| Call | Operator / location | Assigned freqs kHz (dial = −1.9 unless noted) | LPM/IOC | Start / phasing / stop | Chart styles, margins and frames | Known slant / timing quirks | Status (2025–26), confidence | Licence of products | Sources |
|---|---|---|---|---|---|---|---|---|---|
| **NMF** | USCG CAMSLANT for NWS OPC/NHC, Boston MA (tx Camp Edwards) | 4235 (0230–1039Z), 6340.5, 9110, 12750 (1400–2239Z) | 120/576 for all products | Standard tones and phasing. Test pattern at 0230, 0745, 1400 and 1720Z | Bilevel line charts on white paper. **Black margins in the image area** and **"data dots" coded into the first few image lines** (Gough Lui). Surface analyses come in two parts (NE and NW Atlantic), some rotated so they are read sideways. **GOES IR satellite images, greyscale**: area 6 has a black bar at the bottom, area 5 has **black bars top and bottom**. Text-page products: schedule ×2, Request for Comments, Product Notice Bulletin. Ice chart (USCG IIP / N. American Ice Service) | −4 ppm (≈0.007 px/line), small | **Active**, but NWS **PNS 26-48 (9 Jun 2026) proposes discontinuation "towards the end of 2026"**. Comments closed 13 Jul 2026. No SCN yet as of 6 Oct 2026. mwlist log 2026-06-01. High confidence | **PD** (NWS). Ice charts from the joint US/Canada NAIS may carry Canadian Crown copyright, so leave them out | [hfmarsh.txt](https://tgftp.nws.noaa.gov/fax/hfmarsh.txt), [rfax.pdf](https://www.weather.gov/media/marine/rfax.pdf), [PNS 26-48](https://www.weather.gov/media/notification/pdf_2026/pns26-48_commentsHF__RadioFax_Broadcast.pdf), [Gough Lui NMF](https://goughlui.com/2019/02/03/radiofax-nmf-boston-massachusetts-national-weather-service-noaa-usa/) |
| **NMG** | USCG for NWS NHC/TAFB, New Orleans LA (Belle Chasse) | 4317.9, 8503.9, 12789.9, 17146.4 (1200–2045Z). Dial 4316.0, 8502.0, 12788.0, 17144.5 | 120/576 | Standard. Test pattern at 0000, 0600, 1200 and 1800Z | As NMF (NWS family). **GOES IR tropical satellite, greyscale.** **High Seas Forecast sent as a TEXT PAGE** (English) at 0245, 0845, 1445 and 2045Z | −4 ppm | **Active**; same PNS 26-48 proposal. High confidence | **PD** | [hfgulf.txt](https://tgftp.nws.noaa.gov/fax/hfgulf.txt), [rfaxmex.txt](https://tgftp.nws.noaa.gov/fax/rfaxmex.txt), PNS 26-48 |
| **NMC** | USCG for NWS OPC/NHC, Point Reyes CA | 4346 (0140–1608Z), 8682, 12786, 17151.2, 22527 (1840–2356Z) | 120/576 | Standard. Test pattern at 0140, 0655, 1120, 1400, 1820 and 2320Z | NWS family. **Three GOES IR satellite products, greyscale.** SST analyses. Text pages: schedule ×2, RFC, product notice | −4 ppm | **Active**; PNS 26-48 proposal. High confidence | **PD** | [hfreyes.txt](https://tgftp.nws.noaa.gov/fax/hfreyes.txt), [rfaxpac.txt](https://tgftp.nws.noaa.gov/fax/rfaxpac.txt) |
| **NOJ** | USCG for NWS Alaska Region, Kodiak AK | 2054, 4298, 8459, 12412.5 (all times) | 120/576 | Standard. Test pattern at 0340, 0950, 1540 and 2150Z | NWS family. **Sea-ice analysis and forecast charts** (heavier fill), Cook Inlet ice, GOES IR satellite, "Symbols and contractions/schedule" text page | −4 ppm | **Active**; PNS 26-48 proposal. High confidence | **PD** | [hfak.txt](https://tgftp.nws.noaa.gov/fax/hfak.txt), [rfaxak.txt](https://tgftp.nws.noaa.gov/fax/rfaxak.txt) |
| **KVM70** | US Navy transmitter for NWS WFO Honolulu, Honolulu HI | 9982.5 (0519–1556Z), 11090, 16135 (1719–0356Z) | 120/576. The schedule lists the 0615/1815 surface analysis and the 1042/2242 cyclone chart as **"120/570"**, almost certainly a typo for 576 | Standard. Test pattern at 0519 and 1719Z | NWS family. **Streamline analysis**, **two GOES IR satellite sectors plus a tropical one** (greyscale), SST, schedule ×2 text pages | −4 ppm | **Active**: raphnet received it 30 Dec 2025. PNS 26-48 proposal. High confidence | **PD** | [hfhi.txt](https://tgftp.nws.noaa.gov/fax/hfhi.txt), [rfaxhi.txt](https://tgftp.nws.noaa.gov/fax/rfaxhi.txt), [raphnet](https://www.raphnet.net/radio/wefax/index_en.php) |
| **DDH3 / DDK3 / DDK6** | Deutscher Wetterdienst, Hamburg/Pinneberg | 3855, 7880, 13882.5. Dial 3853.1, 7878.1, 13880.6. DWD: "white +425 Hz, black −425 Hz" (F1C), so the shift is **±425 Hz**, slightly wider than JMH's ±400 | 120/576 for every product | Standard start, phasing and stop. Schedule chart at 1425 (dxinfocentre "TC @ 1425") | **Our reference:** white paper, map in a **thin black frame on a white border**. Surface analysis with significant weather; wave charts; ice charts (Baltic, Arctic, and **Canadian Ice Service / IIP** NW Atlantic); SST North Sea; schedule ×2 text pages | −12 ppm (≈0.022 px/line) | **Active.** DWD's notices (2025–26) mention only the 5905 kHz voice outage, and the Mediterranean text report ended 1 Apr 2025. High confidence | DWD: **CC BY 4.0 / GeoNutzV with attribution** for DWD geodata (open-data policy). The charts carry "© DWD". The CIS ice-chart rebroadcast is Canadian Crown copyright, so leave it out | [rfax.pdf V-2/3](https://www.weather.gov/media/marine/rfax.pdf), [DWD notices](https://www.dwd.de/DE/fachnutzer/schifffahrt/funkausstrahlung/rttyinfo.html), [DWD schedule EN](https://www.dwd.de/EN/specialusers/shipping/broadcast_en/broadcast_fax_102020.pdf?__blob=publicationFile&v=1) |
| **GYA** | Royal Navy / Met Office JOMOC, Northwood UK | 2618.5 (2200–0500Z), 4610 (24 h), 8040 (24 h alternative), 11086.5 (0600–2000Z) | 120/576 | dxinfocentre: **"No TC"** (no test chart). Tone behaviour is not documented in a citable source; treat start and stop detection as unreliable | **Our reference:** **black vertical margin line ~40 px in.** Dense back-to-back schedule every 12 min. Surface analysis and prognoses, sea and swell, **"Ocean fronts"**, SST, gale-warning chart, precipitation and visibility | **+34 ppm ≈ 0.0615 px/line, matching our measured −0.06** | **Active** (dxinfocentre Mar 2025, and our own recordings). Gough Lui reported "inactive" in 2019, but that was the Persian Gulf GYA outstation. High confidence | **Crown copyright (MOD/JOMOC).** No explicit OGL statement for the fax products: **NOT clearly licensed** | [rfax.pdf V-4/5](https://www.weather.gov/media/marine/rfax.pdf), [dxinfocentre](https://www.dxinfocentre.com/rafax.htm), [Gough Lui slant](https://goughlui.com/2019/02/10/radiofax-tips-modify-kiwifax-py-for-better-dx-use-slant-correction-factors-status/) |
| **JMH / JMH2 / JMH4** | Japan Meteorological Agency, Tokyo | 3622.5, 7795, 13988.5. Dial 3620.6, 7793.1, 13986.6. JMA: "F3C WHITE:+400 BLACK:−400" | 120/576 | "slightly longer start tone, a standard phasing interval". White tone to aid tuning. **TEST CHART at 0103 and 1303Z** (greyscale steps, resolution bars) | **Black margins**; charts "do not fill the entire width of the paper". **No scale bars or coded dots.** **Himawari IR satellite pictures, 4 a day** (0110, 0710, 1310 and 1910Z), **16-level greyscale with a 17-segment grey bar**. Some products are portrait-sent with landscape-formatted content. "Radio prediction" chart on the 20th and 21st | 0.0 ppm (very stable) | **Active**: raphnet 8 Dec 2025. Schedule effective 19 Jan 2022. High confidence | **JMA terms: Public Data License v1.0, compatible with CC BY 4.0**, attribution required. Strictly this covers website content; JMH charts and Himawari images are JMA works. Fairly clear | [JMH-ENG.pdf](https://www.jma-net.go.jp/common/177jmh/JMH-ENG.pdf), [JMA terms](https://www.jma.go.jp/jma/en/copyright.html), [Gough Lui JMH](https://goughlui.com/2019/01/27/radiofax-jmh-jmh2-jmh4-japan-meteorological-agency/) |
| **JFX** | Kagoshima fisheries radio, Japan | 4274, 8658, 13074, 16907.5, 22559.6. Different products on different frequencies (NWS PDF, updated 29 Jan 2025) | 120 (not stated explicitly) | "generous white-tone" before the fax; **"inverted phasing ticks"**; stop tone used | **Black image margins.** Mostly **Japanese TEXT pages** (navigational warnings, fish and tuna market prices, "JFX NOAA information"), plus 8-panel surface prog charts and SST | 0.0 ppm, "very stable" | **Active** (NWS schedule updated 29 Jan 2025; dxinfocentre 2025 "new"). Medium confidence | Copyrighted (operator/JMA content). Not licensed | [rfax.pdf I-2](https://www.weather.gov/media/marine/rfax.pdf), [Gough Lui JFX](https://goughlui.com/2019/02/10/radiofax-jfx-kagoshima-japan-fisheries/) |
| **JJC** (formerly also **9VF** Singapore) | Kyodo News, Japan | **16971** is the only confirmed working frequency (2022–25). Historical: 4316, 8467.5, 12745.5, 17069.6, 22542; 9VF 16035 and 17430 | **★ 60/576.** About 1 s per line, 1809 px | Tone behaviour not documented | **Newspaper TEXT pages** (Japanese, plus an English edition), navigational warnings, ocean-current map, **sumo tables**. Effectively all text | +3.2 ppm | **Active**: raphnet 11 Dec 2025, "most likely … the last service of the type in the world". NWS schedule updated 30 Jan 2025. High confidence | **© Kyodo News.** Not licensed; never commit | [raphnet](https://www.raphnet.net/radio/wefax/index_en.php), [Gough Lui JJC](https://goughlui.com/2020/01/18/radiofax-update-jjc-kyodo-still-alive-in-2020/), [rfax.pdf I-6](https://www.weather.gov/media/marine/rfax.pdf) |
| **HLL2** | Korea Meteorological Administration, Seoul | 3585 (1200–0000Z), 5857.5, 7433.5, 9165, 13570 (0000–1200Z). Dial 9163.1 and so on | 120/576 | **Documented by KMA:** 300 Hz start for 10 s, phasing 30 s, stop 15 s. "Slightly long start tone, slightly short phasing and stop tones" (Gough Lui) | **Black margins** (white margins until about 2017, black since 2019). **Lots of Korean TEXT pages**: observation reports, lighthouse reports, special weather report, warnings, schedule. Upper-air charts, wave charts, SST. Greyscale typhoon and sea-ice products. **Short charts** mixed with standard-length ones | −18 ppm (≈0.033 px/line) | **Active**: raphnet 31 Dec 2025 (best on 13570). High confidence | KMA content. Licence not verified (possibly KOGL). **Not clearly licensed** | [rfax.pdf I-4](https://www.weather.gov/media/marine/rfax.pdf), [Gough Lui HLL2](https://goughlui.com/2019/02/09/radiofax-hll2-seoul-south-korea-korean-meteorological-administration/), [raphnet](https://www.raphnet.net/radio/wefax/index_en.php) |
| **XSG** | Shanghai Coast Radio for the CMA National Met Centre / Shanghai Met Bureau | 4170, 8302, 12382, 16559 | 120/576 | White tuning tone, WMO start and stop tones. **Phasing "nearly 50/50 black and white", unlike any other station.** "Trailing black tone" | **Black image margins.** Tall "divided seascape" charts (two panels). Very fine detail (ocean-current arrows). **IR satellite images.** Times in the schedule are given in local time | 0.0 ppm, "practically perfect" | **Active**: raphnet 10 Dec 2025. High confidence | CMA. Not licensed | [Gough Lui XSG](https://goughlui.com/2019/03/07/radiofax-xsg-shanghai-china-national-meterological-centre-cma/), [dxinfocentre](https://www.dxinfocentre.com/rafax.htm) |
| **XSQ** | Guangzhou Coast Radio for the Guangdong Met Observatory / South China Sea Forecast Centre | 4199.75, 8412.5, 12629.25, 16826.25 (dxinfocentre also lists 6826.25) | 120/576 (assumed) | Schedule chart (MANAM) at 0000Z | Surface, South China Sea, wave and typhoon forecast charts. Bilingual headings | Unknown | **Active** (dxinfocentre Mar 2025; NWS schedule effective Aug 2022). Medium confidence | CMA/Guangdong. Not licensed | [rfax.pdf I-7/8](https://www.weather.gov/media/marine/rfax.pdf), [dxinfocentre](https://www.dxinfocentre.com/rafax.htm) |
| **HSW64** | Thai Meteorological Department, Bangkok (tx Udon Thani) | Listed as 7395.0; NWS says **"may refer to carrier frequency"**; centre 7396.8–7396.9 (Gough Lui, weatherfax.com). **Dial 7395.0** | 120/576 | Test chart at 0050Z | "Quite strong similarities" to SVJ4 and CBV: a **white-to-black gradient bar before the chart**, scale bar. English "Forecast for shipping" **TEXT pages**; surface, 850/700/500 mb charts | −11 ppm (≈0.020 px/line) | **Active** (dxinfocentre Mar 2025). Weak, low power. Medium confidence | TMD. Not licensed | [rfax.pdf I-5](https://www.weather.gov/media/marine/rfax.pdf), [Gough Lui HSW64](https://goughlui.com/2019/01/27/radiofax-hsw64-weather-forecast-division-thai-meteorological-department/) |
| **VMC / VMW** | Bureau of Meteorology: Charleville QLD (VMC) and Wiluna WA (VMW) | VMC 2628 (0900–1900Z), 5100, 11030, 13920, 20469 (1900–0900Z). VMW 5755 (1100–2100Z), 7535, 10555, 15615, 18060 (2100–1100Z). The NWS PDF treats these as assigned (dial −1.9); not verified, because the BoM technical guide returns 403 to automated fetches | 120/576 | Schedule pages at 0015 and 1215Z (dxinfocentre "TC") | MSLP analyses and progs (Lambert), **polar-stereographic Southern Hemisphere and Indian Ocean charts**, SST, wind and swell waves. Text pages: schedule ×2, information notice, **"IPS recommended frequencies" charts**, voice-broadcast info. 11030 kHz carries directional repeats to Tasmania | **Jan 2019 fault, confirmed by BoM:** "the chart periodically loses phase alignment and 'jumps'"; uncorrelated jump points on different frequencies. Whether it was fixed is unknown | **Active**: raphnet VMC 30 Dec 2025 ("very weak"); mwlist VMW logged 30 Dec 2024. High confidence | **© Commonwealth of Australia (BoM).** BoM's default terms are personal, non-commercial, unaltered use. **Not licensed** | [rfax.pdf IV-1/2](https://www.weather.gov/media/marine/rfax.pdf), [Gough Lui 2019](https://goughlui.com/2019/01/28/radiofax-news-bmf-confirmed-offline-vmc-vmw-transmission-fault-radiofax-status/), [raphnet](https://www.raphnet.net/radio/wefax/index_en.php) |
| **CBV / CBM** | Armada de Chile: Valparaíso Playa Ancha (CBV) and Punta Arenas Magallanes (CBM) | CBV 4228, 8677, 17146.4. CBM 4322, 8696. NWS lists them J3C with no convention note, so dial −1.9 | 120/576 | "relatively short start tone and phasing". Test chart and schedule at 1100Z (CBV) and 1550Z (CBM). **Late starts reported** | **Gradient bar white→black** with text "#0001" at left (similar kit to SVJ4 and HSW64). Bold-lined surface charts in portrait with a scale at bottom left. **Greyscale SATELLITE images interleaved with nearly every chart** (CBV 4 a day, CBM 4 a day). Spanish | **−90 ppm ≈ 0.163 px/line, the largest of any active station** | **Active** (dxinfocentre Mar 2025). Schedule info dated 2010. Medium-low confidence | Armada de Chile. Not licensed | [rfax.pdf II-1](https://www.weather.gov/media/marine/rfax.pdf), [Gough Lui CBV](https://goughlui.com/2019/01/27/radiofax-cbv-cbm-armada-de-chile-servicio-meterologico/) |
| **SVJ4** | Hellenic National Meteorological Service (HNMS), Athens | NWS: 4481 and 8105, "*Center Frequency is 1.9 kHz higher*". **So 4481 and 8105 are the dial frequencies**; centre 4482.9 and 8106.9 | 120/576 | "relatively short start and end tone, short phasing". One block from about 0845 to 1100Z daily | **White margins.** Continuous **gradient bar** white→black before the chart (prefix "SWA"). Landscape-sent. Wave charts with **filled contour shading** (large solid areas). Greek labels | Unknown | **Active** (dxinfocentre Mar 2025). Schedule info 01/2019. Medium confidence | HNMS. Not licensed | [rfax.pdf V-1](https://www.weather.gov/media/marine/rfax.pdf), [Gough Lui SVJ4](https://goughlui.com/2019/01/26/radiofax-svj4-hellenic-national-meteorological-service-athens-marine-meteorological-centre-greece/) |
| **RBW41 / UDK2 / RKS72** | Roshydromet Murmansk (Murmansk UGMS) | NWS: RBW41 5336, 6445.5, 7908.8 (1900–0600Z); RBW48 10130 (0600–1900Z). Listener reports: **8444** (NWS, 18 Oct 2024), **6328.5** (dxinfocentre, "active, TC @ 1850"). 5336 and 10130 inactive | **Mixed: 120/576 charts, but the broadcast schedule at 1850 is sent at ★ 90/576** (NWS) | **"doesn't use APT tones at all"** (Gough Lui 2019) | Barents Sea surface prog, sea state, ice/iceberg charts, SST | −17 ppm (≈0.031 px/line) | **Active**: iceberg prognosis heard on 8444 at 2000Z on 18 Oct 2024 (NWS). Medium confidence | Roshydromet. Not licensed | [rfax.pdf V-1](https://www.weather.gov/media/marine/rfax.pdf), [dxinfocentre](https://www.dxinfocentre.com/rafax.htm) |
| **(ROSKON) UGC?** | St Petersburg, North-West UGMS (?) | 2640 (fax). 4212 is SITOR-B | 120 | **"no start tone … launching from noise into phasing tone directly"** | One **ice chart at 1300Z**, updated every 2 days. Landscape chart **extends right into the black synchronisation margins** | "a slight slant" (not quantified) | Last documented Mar 2021; listed "reported" in 2025. **Low confidence** | Not licensed | [Gough Lui St Petersburg](https://goughlui.com/2021/03/21/radiofax-st-petersburg-russia-roskon-fgbu-north-west-ugms/) |
| **UBR2** | Vanino (Russian Far East) | 3560, 6455 (weatherfax.com: 6456.9) | ? | ? | ? | ? | dxinfocentre 2025: "Reported active". **Low confidence** | Not licensed | [dxinfocentre](https://www.dxinfocentre.com/rafax.htm) |

## B. Ceased, inactive or unknown (do not plan test data around these)

| Call | Operator / location | Status | Confidence | Source |
|---|---|---|---|---|
| **ZKLF** | MetService, Auckland/Wellington NZ | **Ceased 1 July 2023** | High | [rfax.pdf IV-2](https://www.weather.gov/media/marine/rfax.pdf), [raphnet](https://www.raphnet.net/radio/wefax/index_en.php) |
| **VCO** (Sydney NS ice charts, 4416 and 6915.1) | CCG MCTS Sydney / Canadian Ice Service | **Discontinued 1 Oct 2024** (CCG RAMN 2026 §5.15.7). The NWS PDF still lists it from 2014 data | High | [CCG RAMN 2026](https://www.canada.ca/en/canadian-coast-guard/corporate/publications/radio-aids-marine-navigation/environment-climate-change-marine-ice-warning-forecast-programs.html) |
| **VFF** Iqaluit, **VFR** Resolute, **VFA** Inuvik | CCG Arctic | **Discontinued 2019** (NWS PDF citing CCG RAMN 2024). dxinfocentre still shows VFA as "on request" | High | [rfax.pdf III-1](https://www.weather.gov/media/marine/rfax.pdf) |
| **CFH** | Canadian Forces METOC, Halifax | Placed **"in abeyance" 2 Sep 2010**, "may be reinstated … without warning". dxinfocentre: decommissioned | High | [Milcom 2011](http://mt-milcom.blogspot.com/2011/06/halifax-nova-scotia-high-frequency.html), [dxinfocentre](https://www.dxinfocentre.com/rafax.htm) |
| **BMF** | Central Weather Bureau, Taipei | **Terminated Oct 2013** "due to old equipment" (CWB reply to Gough Lui) | High | [Gough Lui 2019](https://goughlui.com/2019/01/28/radiofax-news-bmf-confirmed-offline-vmc-vmw-transmission-fault-radiofax-status/) |
| **PWZ33** | Brazilian Navy, Rio (12665, 16978) | **Decommissioned Jan 2021** (archived NWS PDF). Last log on mwlist 2015 | Medium | [docksideradio copy of rfax.pdf](https://docksideradio.com/media/marine/rfax.pdf), [mwlist](https://www.mwlist.org/fax.php) |
| **ZSJ** | SA Navy, Cape Naval (4014, 7508, 13538, 18238) | "Alive but unreliable" Feb 2019 (7508 only). **60 LPM**, +100 ppm slant, severe echo smear. No later evidence; not in the NWS 2025 PDF | Low (presumed ceased) | [Gough Lui ZSJ](https://goughlui.com/2019/02/16/radiofax-the-quest-for-zsj-cape-naval-south-africa-updates/) |
| **BAF** Beijing, **BDF2** Shanghai (3241) | CMA | Not in NWS 2025 or dxinfocentre 2025 as active. BDF2 has never been logged on mwlist | Low (presumed ceased) | [mwlist](https://www.mwlist.org/fax.php) |
| **ATP** New Delhi (IMD) | IMD | Reported inactive / not heard (Gough Lui 2019). Not in NWS 2025 | Medium | [Gough Lui 2019](https://goughlui.com/2019/01/28/radiofax-news-bmf-confirmed-offline-vmc-vmw-transmission-fault-radiofax-status/) |
| **LRO/LOR** Argentina, Turkey, Singapore met fax, **9VF** Kyodo relay | — | No current evidence of activity found. None appear in the 2025 lists | Low | — |
| **UIW Pevek** (148 kHz, LF, **90/576**, ice) | Russia | NWS info dated 11/97. LF, not HF. mwlist: never logged | Low | [rfax.pdf I-3](https://www.weather.gov/media/marine/rfax.pdf) |
| **UFH** Petropavlovsk, **UFZ** Vladivostok, **UGE** Arkhangelsk | Russia | Out of service, or status unknown (dxinfocentre 2025) | Low | [dxinfocentre](https://www.dxinfocentre.com/rafax.htm) |
| **LSB** Marambio (Antarctica, 2401) | Argentina | Listed on mwlist, never logged | Low | [mwlist](https://www.mwlist.org/fax.php) |

**IOC 288:** no current station is documented as using IOC 288. Every schedule found says x/576. **Non-120 LPM in current use:**
JJC **60/576** throughout; Murmansk **90/576** for its schedule chart; ZSJ 60 (probably dead); Pevek 90 (LF, probably dead).

---

## C. NOAA product files on https://tgftp.nws.noaa.gov/fax/ (verified against the directory listing, 2026-10-07)

All names come from the official `rfax*.txt` index files and were checked to be present in the directory listing,
with that day's timestamps unless marked. Charts are "G4(T4) format … enveloped in TIFF", so they are **bilevel**.
Most also exist as `.gif`. Satellite images are greyscale `.jpg`. Base URL: `https://tgftp.nws.noaa.gov/fax/`.
Index files: [rfaxatl.txt](https://tgftp.nws.noaa.gov/fax/rfaxatl.txt) (NMF), [rfaxmex.txt](https://tgftp.nws.noaa.gov/fax/rfaxmex.txt) (NMG),
[rfaxpac.txt](https://tgftp.nws.noaa.gov/fax/rfaxpac.txt) (NMC), [rfaxak.txt](https://tgftp.nws.noaa.gov/fax/rfaxak.txt) (NOJ),
[rfaxhi.txt](https://tgftp.nws.noaa.gov/fax/rfaxhi.txt) (KVM70).

**Caveat:** the schedule and test-pattern TIFs named in the index files (PLAZ01–04, PLBZ01–09, PLEZ01–03, PZZZ93/94/95,
NOJTEST.TIF) are **not present** in the directory. The only text schedules on the server are the `hf*.txt` files.

| Station | Product | URL |
|---|---|---|
| NMF | Surface analysis Part 1, NE Atlantic (latest) | https://tgftp.nws.noaa.gov/fax/PYAA11.TIF |
| NMF | Surface analysis Part 2, NW Atlantic (latest) | https://tgftp.nws.noaa.gov/fax/PYAA12.TIF |
| NMF | Preliminary surface analysis (latest) | https://tgftp.nws.noaa.gov/fax/PYAD10.TIF |
| NMF | 500 mb analysis (latest) | https://tgftp.nws.noaa.gov/fax/PPAA10.TIF |
| NMF | 24 h surface forecast (latest) | https://tgftp.nws.noaa.gov/fax/PPAE10.TIF |
| NMF | Wind/wave analysis (latest) | https://tgftp.nws.noaa.gov/fax/PWAA90.TIF |
| NMF | Sea state analysis 12Z | https://tgftp.nws.noaa.gov/fax/PJAA99.TIF |
| NMF/NMG | Tropical cyclone danger area / 48 h high wind-wave (latest) | https://tgftp.nws.noaa.gov/fax/PWEK11.TIF |
| NMF | GOES IR satellite, W Atlantic (latest) | https://tgftp.nws.noaa.gov/fax/evnt99.jpg |
| NMF | USCG International Ice Patrol ice chart (seasonal) | https://tgftp.nws.noaa.gov/fax/PIEA88.TIF |
| NMF | Schedule (text) | https://tgftp.nws.noaa.gov/fax/hfmarsh.txt |
| NMG | US/Tropical surface analysis, W half (latest) | https://tgftp.nws.noaa.gov/fax/PYEB11.TIF |
| NMG | Tropical surface analysis, E half (latest) | https://tgftp.nws.noaa.gov/fax/PYEA11.TIF |
| NMG | Sea state analysis (latest) | https://tgftp.nws.noaa.gov/fax/PJEA11.TIF |
| NMG | 24 h wind/wave forecast (latest) | https://tgftp.nws.noaa.gov/fax/PWEE11.TIF |
| NMG | 24 h tropical surface forecast (latest) | https://tgftp.nws.noaa.gov/fax/PYEE10.TIF |
| NMG | GOES IR satellite, 12S–44N (latest) | https://tgftp.nws.noaa.gov/fax/evst99.jpg |
| NMG | **High Seas Forecast TEXT PAGE** (latest). **Stale: last updated 14 Jul 2025**, as are PLEA86–89 | https://tgftp.nws.noaa.gov/fax/PLEA10.TIF |
| NMG | Schedule (text) | https://tgftp.nws.noaa.gov/fax/hfgulf.txt |
| NMC | Surface analysis Part 1, NE Pacific (latest) | https://tgftp.nws.noaa.gov/fax/PYBA90.TIF |
| NMC | Surface analysis Part 2, NW Pacific (latest) | https://tgftp.nws.noaa.gov/fax/PYBA91.TIF |
| NMC/NOJ | 500 mb analysis (latest) | https://tgftp.nws.noaa.gov/fax/PPBA10.TIF |
| NMC | Sea state analysis 00Z | https://tgftp.nws.noaa.gov/fax/PJBA99.TIF |
| NMC | Wind/wave analysis (latest) | https://tgftp.nws.noaa.gov/fax/PWBA90.TIF |
| NMC/KVM70 | East Pacific tropical surface analysis (latest) | https://tgftp.nws.noaa.gov/fax/PYFA90.TIF |
| NMC | SST chart 40N–53N | https://tgftp.nws.noaa.gov/fax/PTBA88.TIF |
| NMC | 72 h tropical cyclone danger area (latest) | https://tgftp.nws.noaa.gov/fax/PWFK11.TIF |
| NMC/NOJ | GOES IR satellite, Pacific (latest) | https://tgftp.nws.noaa.gov/fax/evpn99.jpg |
| NMC | GOES IR satellite, tropical E Pacific (latest) | https://tgftp.nws.noaa.gov/fax/evpn10.jpg |
| NMC | Schedule (text) | https://tgftp.nws.noaa.gov/fax/hfreyes.txt |
| NOJ | Arctic/Alaska surface analysis (latest) | https://tgftp.nws.noaa.gov/fax/PYCA10.TIF |
| NOJ/NMC | 24 h wind/wave forecast (latest) | https://tgftp.nws.noaa.gov/fax/PWBE10.TIF |
| NOJ | Sea-ice analysis | https://tgftp.nws.noaa.gov/fax/PTCA89.TIF |
| NOJ | 5-day sea-ice forecast (dated 5 Oct 2026) | https://tgftp.nws.noaa.gov/fax/PTCO89.TIF |
| NOJ | Cook Inlet sea ice | https://tgftp.nws.noaa.gov/fax/PTCA87.TIF |
| NOJ | SST analysis 40N–60N | https://tgftp.nws.noaa.gov/fax/PTCA88.TIF |
| NOJ | Schedule (text) | https://tgftp.nws.noaa.gov/fax/hfak.txt |
| KVM70 | Pacific surface analysis EQ–50N (latest) | https://tgftp.nws.noaa.gov/fax/PPBA11.TIF |
| KVM70 | Pacific streamline analysis (latest) | https://tgftp.nws.noaa.gov/fax/PWFA11.TIF |
| KVM70 | 24 h Pacific surface forecast (latest) | https://tgftp.nws.noaa.gov/fax/PYFE11.TIF |
| KVM70 | 72 h tropical cyclone danger area (latest) | https://tgftp.nws.noaa.gov/fax/PWFK12.TIF |
| KVM70 | Tropical sea state analysis (latest) | https://tgftp.nws.noaa.gov/fax/PKFA10.TIF |
| KVM70 | Pacific SST chart (dated 27 Aug 2026) | https://tgftp.nws.noaa.gov/fax/PTFA88.TIF |
| KVM70 | E Pacific satellite (latest) | https://tgftp.nws.noaa.gov/fax/evpz11.jpg |
| KVM70 | SW Pacific satellite (latest) | https://tgftp.nws.noaa.gov/fax/evps11.jpg |
| KVM70 | Schedule (text) | https://tgftp.nws.noaa.gov/fax/hfhi.txt |

Stale HFO central-Pacific wind/wave files: PJFB10, PJFB89, PJFD89 and PWFE11 were last updated in May 2025.
Prefer PKFA10 or PWFI10, which are current. The directory also holds UK Met Office charts relayed by NWS
(PPVA89, PJGA93 and others in [otherfax.txt](https://tgftp.nws.noaa.gov/fax/otherfax.txt)). Those are **Crown copyright, not PD**,
even though they sit on a NOAA server.

---

## D. Risks for an auto-aligner (DDK white-paper frame + GYA 40 px black line are the only tested cases)

1. **Black side margins are the norm, not the exception.** JMH, HLL2 (since about 2019), XSG, JFX, the St Petersburg
   station, and the NWS family (NMF/NMG/NMC/NOJ/KVM70, "black margins in the image area" plus coded "data dots" in the
   first lines) all put black sync/margin strips in the picture. An aligner that looks for a white border (DDK-style) or
   a single thin black line (GYA-style) will lock onto the wrong edge. The St Petersburg ice chart even **extends into
   the black sync margins**. Only SVJ4 (and DDK) are documented with white margins.
2. **Greyscale satellite images with large black areas or bars:** NMF (black bars top and bottom on area 5), NMG,
   NMC (×3), NOJ, KVM70 (×3), JMH (Himawari 4 a day, 16-level with a grey bar), CBV/CBM (a satellite image after almost
   every chart), XSG (IR). Dark IR ocean backgrounds give no white margin to find.
3. **Filled or shaded charts:** SVJ4 wave charts (filled contours), NOJ sea-ice charts, HLL2 greyscale typhoon and
   ice products. Gradient bars (white→black ramp) sit before the chart on SVJ4, HSW64 and CBV/CBM and will look like
   an edge.
4. **Text-only pages:** JJC (everything, at 60 LPM), JFX (mostly), HLL2 (many Korean reports), HSW64 ("Forecast for
   shipping"), NMG (High Seas Forecast), plus schedules, notices and test charts from almost everyone (JMH test chart
   at 0103/1303Z; NWS test patterns). Text has no frame and few long verticals.
5. **Non-120 LPM:** **JJC 60/576** (live, daily). **Murmansk schedule at 90/576** while its charts are 120. Historical:
   ZSJ 60, Pevek 90. **No current IOC 288 found**, but keep the 675 Hz start-tone path working.
6. **Missing or odd tones and phasing:** RBW41 Murmansk sends **no APT tones at all**. St Petersburg has **no start
   tone** and goes straight into phasing. GYA has no test chart and undocumented tones. JFX uses **inverted phasing ticks**
   (polarity flipped). XSG's phasing is **about 50 % black, 50 % white**. CBV/CBM **start late**. VMC/VMW had a
   **phase-jump fault** in 2019 (alignment "jumps" mid-chart). The aligner must cope with a phase *step* inside an image,
   not just a constant slant.
7. **Known line-rate error (kiwifax ppm → px/line at IOC 576):** CBV/CBM **−90 (0.16)**, GYA +34 (0.06, matches ours),
   VCO −24 (now ceased), HLL2 −18 (0.033), RBW41 −17 (0.031), DDK −12 (0.022), HSW64 −11 (0.020), ZKLF +11 (ceased),
   US NWS family −4 (0.007), JJC +3.2, JMH, JFX and XSG 0.0. The search range must reach at least ±0.2 px/line, both
   signs, to cover CBV/CBM.
8. **Dial convention traps:** SVJ4's published 4481/8105 are **already the dial (carrier) frequencies**, and HSW64's 7395.0
   probably is too. Everyone else publishes assigned/centre (dial −1.9). DDK's shift is ±425 Hz and JMH's ±400 Hz.
9. **Test-data licensing:** only the five **US NWS stations are clearly public domain**, and they may go off air at the
   end of 2026 (PNS 26-48). Capture off-air US recordings now if wanted. JMH (JMA, CC BY 4.0-compatible) and DWD (CC BY
   4.0 with attribution) are the only other clearly licensed sources. GYA (Crown/MOD), BoM, KMA, Kyodo and the rest are
   not clearly licensed. Ice charts rebroadcast by NMF and DDK may be Canadian Ice Service (Crown) content.

---

## E. Harness results on the current aligner (main 7aa713bb, 2026-10-07)

★ `node --no-warnings scripts/wefax-world/harness.ts` (add `WEFAX_WORLD_LOCAL=<dir>` for an uncommitted corpus,
`--quick`, `--only <text>`, `--verbose`). It imports `src/utils/wefaxAlign.ts` from the tree it runs in, so re-run it
after any change to the aligner. Exit 1 on any FAIL — which is why it is not in `scripts/run-tests.sh` yet.

**Verdicts.** PASS: seam in paper/margin/strip (or within 12 px of the true edge) on ≥ 95 % of lines and slant within
0.01. LEFT: the aligner did nothing (shift 0, the station's slant) — drawn as received, no harm done. WARN: slant
0.01–0.03 off. FAIL: the aligner moved the seam into the picture, or drew a slant ≥ 0.03 px/line wrong.

**Geometry.** A NOAA product is a 1728-px scan; the line at 120/576 is 1809 px. Each published image is run as
`full` (scaled edge to edge), `blank5` (+ 81 px white) and `black5` (+ 81 px black — the NWS/JMH/HLL2/XSG/JFX
on-air look, risk 1 above; the likeliest real geometry for most of the world).

| Charts (runs each) | full PASS/LEFT/WARN/FAIL | blank5 | black5 |
|---|---|---|---|
| NMF ×3 (12) | 4/12/0/20 | 24/0/0/12 | 1/2/0/33 |
| NMG ×4 (12) | 26/12/4/6 | 30/8/4/6 | 13/11/7/17 |
| NMC ×3 (12) | 14/4/0/18 | 33/2/0/1 | 9/4/4/19 |
| NOJ ×2 (12) | 12/7/0/5 | 23/0/1/0 | 5/2/0/17 |
| KVM70 ×3 (12) | 1/11/1/23 | 7/11/4/14 | 0/7/0/29 |
| CBV slant 0.16 on PWBA90 (12) | 8/2/2/0 | 8/2/2/0 | 0/0/11/1 |
| DDK off air ×3, local | 30/4/2/0 | — | — |
| GYA off air ×3, local | 30/0/0/6 | — | — |
| synthetic: edge map / @GYA / black edge / black framed / satellite / framed satellite / text | 6/4/0/2 · 8/2/0/2 · 6/6/0/0 · 8/4/0/0 · 6/2/0/4 · 4/0/7/1 · 12/0/0/0 | | |

**Whose rule breaks.**
1. **Edge-to-edge maps read as bordered** — `findGutter`/`bandOf` + `BORDER_MIN` (90) + `GUTTER_EDGE` growth, and
   `findChartAlign` trying the border FIRST. An open-sea column band on a NOAA surface analysis (PYAA12: x≈1137, the
   strip between two curved isobars, no grid line) passes for DDK's white border, and a PHASED chart is re-cut through
   the middle of the Atlantic. 44 `full` + 47 `black5` FAILs are this path. The same path takes a blank patch near the
   bottom of a Northwood chart on a late join (GYA 6 FAILs: the border search runs before the margin search, so the
   margin is never consulted).
2. **A straight meridian is a Northwood margin** — `marginPeak` (`MARGIN_INK` 140, `narrow` ≥ 40, not `framed`).
   NOAA's polar-stereographic charts carry a truly vertical meridian full height (PYAA12 at x≈903); solid, narrow and
   with chart on both sides, it is indistinguishable from GYA's margin and the chart is cut there (17 + 13 FAILs).
3. **A wide black strip drives the margin slant search to its end** — `findMarginSlant` maximises `narrow`, and an
   81-px solid strip scores HIGHER the more a wrong slant smears its sides (the ±25–40 px comparison windows slide out
   of the strip into paper), so the "best" slant is the search's edge, ±0.054 (`SLANT_SEARCH` 0.05 + the 0.004 fine
   step). 43 `black5` FAILs. This is the commonest real geometry outside Europe.
4. **KVM70 11090 kHz gets Northwood's −0.06** — `wefaxPreset` matches any dial within 5 kHz of 11086.5; KVM70's dial
   is 11088.1. Every KVM70 11090 chart is drawn leaning 0.06 px/line (~70 px over a chart) unless a slant is measured.
   (BMF 4616 would have collided with 4610, but BMF is off air.)
5. **Satellite images** — `findBorder`'s wide slant search (`BORDER_SLANT_SEARCH` 0.15) lines up cloud texture:
   full-width synthetic satellite measured +0.05…+0.13 on a straight chart (`BORDER_LINE_INK` 80 is cleared by bright
   cloud edges on a dark sea, read as "ink" against paper). Real NOAA GOES images fared better (NMG evst99 mostly PASS)
   because of their white surround.
6. **Not modelled yet, known from the research:** CBV/CBM's ≈0.16 px/line is only reachable by the border search
   (±0.15 + fine); a margin/strip chart at that slant cannot be measured (`SLANT_SEARCH` ±0.05). VMC/VMW phase STEPS
   mid-chart. NWS "data dots" in the first lines. SVJ4/HSW64/CBV grey-ramp bars before the chart. JJC at 60 LPM:
   `ALIGN_CHECK_LINES` (300/600) and `CHART_END_FLAT` (40) are LINE counts, so 5 and 10 minutes, 40 s at 60 LPM — and
   the native decoder accepts lpm 60/90/120/240 but detects only the 300 Hz IOC 576 start tone (no 675 Hz IOC 288;
   no current station uses 288).

**After the conservative aligner (2026-10-07, a1708d76): 1 FAIL of 768** (was 254 on the same 64 charts, with the
local DDK/GYA set and three of Stuart's phased HF+ DDK copies — two schedule pages and a surface chart). `findChartAlign`
now moves a chart only when it is positively one of: a white border confirmed by DDK's header bar, or bounded by a
frame solid down the whole look on a white-border station (`wefaxFormat`: DDK, SVJ4) — rule 1; a black margin strip,
recognised by content and with its slant measured from its edges — rule 3 and CBV's 0.16; Northwood's margin, on
Northwood only — rule 2. Presets match within 0.5 kHz — rule 4. Satellite "strips" must be black, not dark grey —
rule 5. The one left: `SYN-edge@GYA` joined for its last 900 lines — a synthetic edge-to-edge map with no margin on
Northwood's dial, where the margin search takes a vertical line; Northwood sends no chart without its margin.

**Corpus.** Committed: `scripts/wefax-world/corpus/noaa/*.png` — 15 NOAA/NWS products (public domain, 17 U.S.C. §105),
manifest `corpus/corpus.json`. Synthetic styles: `scripts/wefax-world/synthetic.json`. Local only (not committed,
not licensed for redistribution): 3 GYA + 3 DDK charts from Stuart's UberSDR (Crown copyright / DWD), NOAA PIEA88
(North American Ice Service). The UberSDR archive holds only 4610 and 7880 kHz.
