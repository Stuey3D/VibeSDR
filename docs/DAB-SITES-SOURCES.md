# DAB transmitter sites: where they come from and why

★ Written 2026-10-06. Stuart asked for a worldwide search for DAB transmitter sites that we could
add to our list "without breaking any licencing agreements". He noted that muxpulse and AbracaDABra
let users import site lists, so databases of them must exist. This repository is **public**, so
anything compiled in is redistributed. The rule is therefore strict: we use a regulator's own list
only when its licence clearly allows redistribution and modification in a public repository, with
attribution we can meet. If we were in doubt, we left the source out.

## What is compiled in

| Country | Source | Licence | Attribution we give | Rows | How it is matched |
|---|---|---|---|---|---|
| UK | [Ofcom: technical parameters, DAB](https://www.ofcom.org.uk/tv-radio-and-on-demand/coverage-and-transmitters/radio-tech-parameters) | [Open Government Licence v3.0](https://www.nationalarchives.gov.uk/doc/open-government-licence/version/3/) | "Contains public sector information licensed under the Open Government Licence v3.0." (a licence condition) | 1,324 | EId + TII main/sub (the only source with TII) |
| CH | [BAKOM: Swiss radio and TV broadcasters](https://opendata.swiss/en/dataset/schweizerische-radio-und-fernsehsender) (`ch.bakom.radio-fernsehsender`) | opendata.swiss ["Open use"](https://opendata.swiss/en/terms-of-use#terms_open): commercial and non-commercial use; citing the source is recommended. The federal geodata terms add: "may be processed, analysed and published in accordance with their terms of use". | "Source: Federal Office of Communications OFCOM (BAKOM)" | 538 (331 sites × blocks) | EId country 4 + tuned block (no EId or TII in the data); the ensemble name is shown |
| CZ | [ČTÚ: Rozhlasové vysílače](https://data.ctu.gov.cz/dataset/rozhlasove-vysilace) | Terms on the data.gov.cz (NKOD) record, per distribution: [contains no copyright works](https://data.gov.cz/podmínky-užití/neobsahuje-autorská-díla/), is not a copyright database, has no sui generis database right, and contains no personal data. The catalogue maps this to CC0. | "Source: Český telekomunikační úřad" (courtesy) | 164 (97 sites) | EId + block (ČTÚ files the placeholder EId 0x211B under several regional networks). A row with no EId is matched by country 2 + block. |
| NL | [RDI: Overzicht DAB en DVB-T2 zenders](https://www.rdi.nl/documenten/2024/08/13/overzicht-dab-en-dvb-t2-zenders) | rdi.nl [copyright statement](https://www.rdi.nl/copyright): "Voor deze website geldt de Creative Commons zero-verklaring (CC0 1.0)". The exceptions are texts marked as copyrighted, and images; this file is neither. | "Source: RDI" (courtesy) | 272 (112 sites) | EId country 8 + tuned block (the file has no ensemble, EId or TII); the province is shown |

The importers are `scripts/dab-sites/import_{ch_bakom,cz_ctu,nl_rdi}.py`, with shared code in
`dabsites.py`. Each one writes `android/app/src/main/cpp/vibe_dab_txdb_<cc>.h`, and that header
carries the source, licence and attribution in its first lines. Every row carries a source id
(`kDabSrc*` in `vibe_dab_txdb.h`). The UK's generator is still `tools/gen-dab-txdb.py`. The four
tables come to about 200 KB of source and 135 KB compiled (measured at -O2). The three new
countries account for about 57 KB of that.

Run the importers again to refresh, one at a time. Each makes one or two requests, from a named
user agent:

```
python3 scripts/dab-sites/import_ch_bakom.py   # BAKOM regenerates its file daily; the dataset says annual
python3 scripts/dab-sites/import_cz_ctu.py     # daily
python3 scripts/dab-sites/import_nl_rdi.py     # irregular; finds the current dated .txt on the page
python3 scripts/dab-sites/test_dabsites.py     # fixtures only, no network
```

## Power and height (added 2026-11-25)

Stuart asked for each site's power and aerial height in the DAB panel, so that it is obvious why a
farther site can be the stronger one (12B: Daventry at 12 mi is 24 dB, Geddington at 13 mi is 4 dB).
These are only ever the regulator's own published figures, from the same files and under the same
licences as the sites. A field the source does not publish is left out, never estimated.

| Country | Source | ERP? | Height? | Units, and what the height is measured from |
|---|---|---|---|---|
| UK | Ofcom `txparamsdab.csv` | Yes: "In-Use ERP Total" | Yes: "In-Use Ae Ht" | ERP in kW. Height is the aerial above the ground at the site, in metres. |
| CH | BAKOM GeoJSON | Only where a site carries one service: "power" | No | ERP as "270 W" or "1.2 kW". There is one figure per site, so at a mixed FM and DAB site it is not the DAB figure (Bettingen St. Chrischona reads 180 kW). It is kept only when the site lists one service, which is 156 of 335 sites. |
| CZ | ČTÚ CSV | Yes: "ERP W" | Yes: "Výška nad mořem" | ERP in W. Height is the site's ground above sea level, in metres. Praha Město, the Žižkov tower, reads 258 m, which is the hill and not the aerial. |
| NL | RDI text file | Yes: "ERP(kW)" | Yes: "Eff.Hoogte" | ERP in kW. Height is in metres and labelled "effective height", which is RDI's own term. |

The UK figures were refreshed from Ofcom's official file on 2026-11-25:
<https://www.ofcom.org.uk/siteassets/resources/documents/spectrum/tv-transmitter-guidance/tech-parameters/txparamsdab.csv>,
linked from the
[technical parameters page](https://www.ofcom.org.uk/tv-radio-and-on-demand/coverage-and-transmitters/radio-tech-parameters),
which was last updated on 16 November 2026. The licence is OGL v3.0, as before, and the site rows
did not change. Ofcom writes the ERP as `9.549,926`. Read as kW with extra digits, that is
9.549926 kW. Read in continental notation it is 9 549,926 W. Both readings give the same 9.55 kW.
The DAB file gives no units. In the workbook version of the same data (`txparams.xlsx`), the MF and
VHF sheets call the same height column "In-Use Aerial Ht". The MF sheet gives its power in kW
("In-Use EMRP (kW)").

The panel says what each height is measured from:

- "aerial 106 m above ground" for Ofcom and for an owner's list
- "site 402 m above sea level" for ČTÚ
- "effective height 332 m" for RDI

Every field is shown on every layout. A Transmitters or Licensed sites line that is wider than the
panel scrolls, as these lines already did.

Examples from BBC National DAB, 12B (CE15), as the panel shows them:

| Site | TII | ERP | Aerial above ground |
|---|---|---|---|
| Daventry (Northants) | 17/02 | 9.55 kW | 106 m (site 199 m above sea level) |
| Northampton | 17/0F | 2 kW | 30 m (site 127 m) |
| Geddington (North Northamptonshire) | 17/05 | 5 kW | 49 m (site 118 m) |

## How it is shown

In the DAB panel (the app and the web client), the **Licensed sites** row now ends with whose record
the site is: "Ofcom record", "BAKOM record", "ČTÚ record", "RDI record", or "your list". Only
Ofcom's rows carry a TII code, so only those show one. A server too old to send `src` only ever
listed Ofcom's sites, and still reads "Ofcom record". The sources and licences are credited in the
app's About, in the web client's ABOUT, and in the README's Credits table.

No source other than Ofcom publishes TII codes. The other countries' sites can therefore name the
**licensed** transmitters of the multiplex you are on, but they can never name the transmitter that
a TII hit came from.

## Surveyed and left out

| Country | What exists | Licence | Why it is not in |
|---|---|---|---|
| FR | ANFR, "installations radioélectriques de plus de 5 watts" (data.gouv.fr): 1,106 "RDF T‑DAB" emitters | Licence Ouverte v2.0 (OK) | Every row's band is "174–223 MHz". There is no block, ensemble or TII, so a site cannot be tied to a multiplex, and listing every French DAB site near you as "licensed" would be wrong. Arcom's FM/DAB+ list has no stated licence. |
| PL | UKE permit lists `pozwolenia_dab_h/_r` (bip.uke.gov.pl), with multiplex, block, site, ERP | Statutory reuse terms (credit source and dates, say the data was processed), stated for "uke.gov.pl". No named licence. | UNCLEAR: the terms name a different host from the files, and no licence is named. Ask UKE to confirm. Probably usable. |
| HR | HAKOM WFS `GSPO_DVB_T2_DAB_P` | Otvorena dozvola (OK) | 40 rows with **no site names**: there would be nothing to show. |
| DK | Mastedatabasen API (`teknologi=33`) | CC0 1.0 (OK) | Sites only: no block or ensemble, so they cannot be matched. |
| SK | Regulator geoserver `uprekaps:dab_miestne_muxy` (12 local muxes, with EId) | CC BY 4.0 is declared on the record for the **FM** layer only | UNCLEAR for the DAB layer. |
| IT | Comune di Milano, AGCOM-derived list (12 DAB rows); no national AGCOM register | CC BY 4.0 (Milan) | One city's re-publication of AGCOM data whose own terms are not stated. Too small, and the provenance is uncertain. |
| AU | ACMA Licensed Broadcasting Transmitter Data (data.gov.au) | CC BY 3.0 AU (OK on paper) | ACMA's server reset every connection from here. The columns (EId? TII?) are unverified, and we will not import what we cannot read. The next candidate. |
| AT | RTR Senderkataster | Liability disclaimer only | UNCLEAR. |
| NO | Nkom Finnsenderen | Map front end only; "No conditions apply" is on the *application's* record | UNCLEAR, and nothing to download. |
| FI | Traficom radio stations (CC BY 4.0) | OK | No DAB rows: all below 108 MHz. |
| DE, BE, SE, SI, IE, ES, PT, RS, MT, CY, LU, LT, LV, EE, GR, HU, TN, KW and others | — | — | No downloadable regulator dataset found. Germany's BNetzA and the Länder publish none, and Ireland has had no national DAB since 2021. |
| — | **FMLIST, wohnort.org, community lists** | Not licensed for redistribution | ✗ Never compiled in, never fetched by us. |

## Your own list (FMLIST / AbracaDABra users)

A server owner can place their **own** copy of the TII list that FMLIST exports, the
`dab-tx-list.csv` that AbracaDABra and muxpulse read, in the VibeServer data directory. The file is
`;`-separated, starts with a header line, and has these columns:
`id;country;channel;label;EId;TII;location;lat;lon;alt;height;pol;MHz;kW`.

The TII column is decimal, main × 100 + sub. The server reads the file at start
(`vibe_dab_txdb.h loadTxListFile`). It is never uploaded, fetched or redistributed by us, and its
rows show as "your list". When the `height` and `kW` columns are filled in, they are shown as the
aerial height above ground and the ERP. The older `dab-tii-<ecc>.csv` format
(`eid,main,sub,site,area,lat,lon`, hex) still works. It now also takes two optional columns after
`lon`: `erp_kw` (the ERP in kW) and `height_m` (the aerial height above ground, in metres). For
example, `eid,main,sub,site,area,lat,lon,erp_kw,height_m`. Either column may be left empty, and a
file with seven columns loads exactly as before.

Not built yet: an in-app "Import TII list…" button. Choosing a file on the phone would have to be
sent to the server that is doing the decoding. That would mean a new authenticated upload route,
with a size cap and the admin PIN, which is more than a file picker. Until then, an owner on Linux
or a Pi copies the file into the data directory.
