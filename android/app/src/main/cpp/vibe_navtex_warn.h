// vibe_navtex_warn.h — V12 2026-11-01 THE NAVTEX MAP, stage 2: the WARNINGS (Stuart: "so a picture of a shipwreck or a
// wind turbine with no nav light"; "unexploded ordinance would look good too").
//
// ★★★ THE GALLERY IS THE SOURCE OF TRUTH. Every NAVTEX message is already a gallery item. This keeps a SMALL APPEND-ONLY
//     INDEX beside it — gallery/navtex-warn.idx — one line per message: its file name, time, id, a hash of its text, the
//     decode quality, the hazard class, its series number, what it cancels, and its positions. NEVER the text again (the
//     card fetches the gallery file when a marker is opened). The index is brought up to date from the gallery's LIVE view
//     (Store::forEachLive — never the raw index.jsonl): a message saved since is parsed once and appended; the first time
//     (a new server, a missing or stale index) that is the BACKFILL, done in chunks — `sync(budget)` parses at most
//     `budget` new messages a call and says how far it got, so a map opened during it shows "Building … 42 %". A gallery
//     item evicted since is simply not live: its line is skipped, and compact() drops such lines once they are a third of
//     the file. RAM: the 64-bit hashes of the indexed names (8 B a message) while a sync runs; nothing held after.
// ★★ ONE MARKER PER WARNING, ON THE MAP ONLY (the gallery list keeps every reception — Stuart): receptions are folded by
//    their id (B1B2B3B4) AND a normalised-text hash; a different text under the same id within 30 days whose positions
//    match is the same warning (a garbled copy); otherwise, or after 30 days, a new one. The copy shown is the best one
//    (fewest error characters). A cancellation — "CANCEL WZ 476/26 (EA77)(VA08)", "CANCEL THIS MESSAGE 302200 UTC NOV"
//    — ends the warning it names; one not heard for 72 h is EXPIRED. ALL shows those faded, never as current.
// ★★ STRICT POSITIONS (navtex::positions, the resolver's own parser): degrees, minutes (two digits), optional decimals, a
//    hemisphere letter, a clean gap. "39-56.45NK000-00.90E", "39-53:54N 000-0.(1E", "39-5.89N 200-00.02E" are refused —
//    a false position is worse than none (test-navtex-warn).
// ★ Bounded: a request answers the newest kMaxMarkers warnings ("showing the newest 1,000 of N"), groups ≤ kMaxGroups.
#pragma once
#include "vibe_navtex_map.h"
#include "vibe_navtex_met.h"   // ★ the met labels: a forecast's areas are parsed once, beside its index line

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vibe {
namespace navwarn {

static constexpr size_t kMaxMarkers = 1000;
static constexpr size_t kMaxGroups = 20000;
static constexpr int64_t kExpireSec = 72 * 3600;
static constexpr int64_t kReuseSec = 30 * 86400;
static constexpr size_t kMaxPts = 24;
static constexpr const char* kPrefix = "v6\t";   // ★ kVersion's line prefix
static constexpr int kVersion = 6;   // ★ 6 (2026-11-01): cables/pipelines in five, platforms, named lists (one marker an item), REPLACES. 5: the submarine class (was folded into firing). 4: + the shape the text names, its date-time group   // ★ the index's format: a line of another version is ignored (and the index rebuilt)

// ── the hazard classes (the Lenovo's 3 924 messages decided them — scripts/navtex-survey numbers in the commit) ─────────
/** Conservative keyword rules on the message body, whole words, in this order (the first that matches wins); when none
 *  does, "general" — never a guess. B2 decides SAR (D), ice (C) and met (B, E). `unlit` is a MODIFIER: a structure or a
 *  buoy whose light has failed is that icon with the unlit mark. */
struct Class { std::string id; bool unlit = false; std::string hit, lang; };
/** ★ Every pattern compiled ONCE (2026-11-01: a regex built per message per rule was most of the backfill's time — and a
 *  Pi 2 builds them ten times slower). Keyed by the literal's address: the patterns are string literals. */
inline const std::regex& rx(const char* re) {
    static std::mutex mu; static std::unordered_map<const char*, std::unique_ptr<std::regex>> m;
    std::lock_guard<std::mutex> lk(mu);
    auto& r = m[re]; if (!r) r.reset(new std::regex(re));
    return *r;
}
inline bool word(const std::string& U, const char* re) { return std::regex_search(U, rx(re)); }
/** ★★ THE MULTILINGUAL LEXICON (2026-11-01; Stuart: "can we translate navtex messages so we can put the icons on the map
 *  correctly"). Not a translation — the words a hazard is written with in the languages the Lenovo's 4 514 messages and
 *  the UberSDR's 1 041 are in (English, then Italian, Dutch, Croatian, Spanish, German, French, Greek, Portuguese,
 *  Estonian, Polish), uppercase without accents (NAVTEX has none), whole words. Deterministic, offline, cheap. The rule
 *  that matched is kept (`hit`, `lang`) for the card's one-line summary: "Unlit light (from French: FEU ETEINT)". */
struct Rule { const char* cls; const char* lang; const char* re; };
inline const std::vector<Rule>& rules() {
    static const std::vector<Rule> r = {
        {"sar", "en", R"(\bOVERDUE\b|\bSEARCH AND RESCUE\b|\bPERSONS? (IN|OVERBOARD)\b|\bMAN OVERBOARD\b|\bMISSING (VESSEL|PERSON|DIVER|CRAFT|BOAT)S?\b|\bMAYDAY\b)"},
        {"sar", "fr", R"(\bPORTES? DISPARU|\bHOMME A LA MER\b|\bRECHERCHES?\b)"}, {"sar", "es", R"(\bDESAPARECIDOS?\b|\bBUSQUEDA\b)"},
        {"sar", "it", R"(\bDISPERSO\b|\bDISPERSI\b|\bRICERCHE\b)"}, {"sar", "de", R"(\bVERMISST\b|\bSUCHE\b)"}, {"sar", "nl", R"(\bVERMIST\b)"},
        {"sar", "hr", R"(\bNESTA(O|LI|LA)\b|\bTRAGANJE\b)"},
        // ★ SUBMARINES before mines/firing (Stuart, 2026-11-01: "you are missing a Submarine for submarine operations"):
        //   a dived-submarine warning names TORPEDO or EXERCISES too, and the submarine is what a mariner must look out for.
        {"submarine", "en", R"(\bSUBMARINES?\b(?!\s+(CABLES?|PIPELINES?|POWER|TELECOM\w*|COMMUNICATIONS?)\b)|\bSUBFACTS\b|\bDIVED SUBS?\b|\bSUBS AND GUNS\b)"},
        // ★ French puts the adjective after: CABLES SOUS-MARINS is an undersea cable. Only the NOUN — a submarine — counts.
        {"submarine", "fr", R"(\b(EXERCICES?|ACTIVITES?|PRESENCE|UN|DES|LES)\s+(DE\s+|D\s)?SOUS[- ]MARINS?\b|\bSOUS[- ]MARINS?\s+EN\s+PLONGEE\b)"}, {"submarine", "es", R"(\bSUBMARINOS?\b)"}, {"submarine", "it", R"(\bSOMMERGIBIL[EI]\b|\bSOTTOMARIN[OI]\b)"},
        {"submarine", "de", R"(\bU-?BOOTE?\b|\bUNTERSEEBOOT\w*\b)"}, {"submarine", "nl", R"(\bONDERZEEBO{1,2}T(EN)?\b)"},
        {"submarine", "hr", R"(\bPODMORNIC\w*\b)"}, {"submarine", "el", R"(\bYPOVRYCHI\w*\b)"},
        {"uxo", "en", R"(\bUNEXPLODED\b|\bUXO\b|\bMINES?\b|\bORDNANCE\b|\bEXPLOSIVES?\b|\bMUNITIONS?\b|\bTORPEDO(ES)?\b)"},
        {"uxo", "fr", R"(\bENGINS? (EXPLOSIFS?|DE GUERRE)\b|\bEXPLOSIFS?\b)"}, {"uxo", "it", R"(\bORDIGN[IO]\b|\bESPLOSIV[IO]\b)"},
        {"uxo", "es", R"(\bEXPLOSIVOS?\b|\bARTEFACTOS?\b)"}, {"uxo", "de", R"(\bMUNITION\b|\bKAMPFMITTEL\b|\bBLINDGAENGER\b)"},
        {"uxo", "nl", R"(\bMUNITIE\b|\bEXPLOSIEVEN\b)"}, {"uxo", "hr", R"(\bNEEKSPLODIRAN\w*\b|\bMINSKO\b|\bUBOJIT\w*\b)"},
        {"wreck", "en", R"(\bWRECKS?\b|\bSUNKEN\b|\bSUNK\b)"}, {"wreck", "fr", R"(\bEPAVES?\b)"}, {"wreck", "es", R"(\bPECIOS?\b|\bNAUFRAGIO\b)"},
        {"wreck", "it", R"(\bRELITT[OI]\b)"}, {"wreck", "de", R"(\bWRACKS?\b)"}, {"wreck", "nl", R"(\bWRAK(KEN)?\b)"}, {"wreck", "hr", R"(\bOLUPIN[AE]\b)"},
        {"wreck", "el", R"(\bNAYAGIO\b|\bNAVAGIO\b)"},
        {"firing", "en", R"(\bFIRING\b|\bGUNNERY\b|\bGUNFACTS\b|\bMISSILE\b|\bROCKET LAUNCH|\bLIVE AMMUNITION\b|\bNAVAL (EXERCISES?|OPERATIONS)\b|\bMILITARY (EXERCISES?|OPERATIONS|ACTIVITY)\b|\bSHIPS EXERCISES\b)"},
        {"firing", "fr", R"(\bTIRS?\b|\bEXERCICES? MILITAIRES?\b)"}, {"firing", "es", R"(\bEJERCICIOS? (DE TIRO|MILITAR(ES)?)\b|\bTIRO\b)"},
        {"firing", "it", R"(\bESERCITAZION[EI]\b|\bTIRO\b)"}, {"firing", "de", R"(\bSCHIESS\w*\b|\bUEBUNGSGEBIET\b)"}, {"firing", "nl", R"(\bSCHIET\w*\b)"},
        {"firing", "hr", R"(\bGA(D|DJ)ANJ\w*\b|\bVJEZB\w*\b)"}, {"firing", "el", R"(\bASKISEIS PYRON\b|\bVOLES\b)"},
        {"adrift", "en", R"(\bADRIFT\b|\bDRIFTING\b|\bDERELICT\b|\bCONTAINERS? (LOST|OVERBOARD)\b|\bFLOATING (OBJECT|DEBRIS|LOG|TIMBER)S?\b)"},
        {"adrift", "fr", R"(\bA LA DERIVE\b)"}, {"adrift", "es", R"(\bA LA DERIVA\b)"}, {"adrift", "it", R"(\bALLA DERIVA\b)"},
        {"adrift", "de", R"(\bTREIBEND\w*\b)"}, {"adrift", "nl", R"(\bDRIJVEND\w*\b)"}, {"adrift", "hr", R"(\bPLUTA\w*\b)"},
        // ★ v6 (2026-11-01, Stuart): WIND FARMS stay `structure`; oil and gas PLATFORMS, drilling RIGS (towed ones too) and
        //   WELLHEADS are `platform` — their own icon (a steel jacket and derrick), most of them "UNLIT AND ALL NAVAIDS
        //   INOPERATIVE". RIG is a whole word (never TRIG / BRIGHT / RIGHT / ORIGIN). "CONSULT NEARBY OFFSHORE PLATFORMS"
        //   (an anchoring notice) is not one — classify() strips that phrase before these rules look.
        {"structure", "en", R"(\bWIND ?FARMS?\b|\bWIND ?TURBINES?\b|\bTURBINES?\b|\bMET(EOROLOGICAL)? MASTS?\b)"},
        {"structure", "fr", R"(\bPARC EOLIEN\b|\bEOLIENNES?\b)"}, {"structure", "es", R"(\bPARQUE EOLICO\b|\bAEROGENERADOR\w*\b)"},
        {"structure", "it", R"(\bPARCO EOLICO\b|\bAEROGENERATOR\w*\b)"}, {"structure", "de", R"(\bWINDPARKS?\b|\bWINDENERGIE\w*\b|\bWINDKRAFT\w*\b)"},
        {"structure", "nl", R"(\bWINDPARKS?\b|\bWINDTURBINES?\b|\bWINDMOLEN\w*\b)"},
        {"platform", "en", R"(\bPLATFORMS?\b|\bDRILLING RIGS?\b|\bJACK.?UP( RIGS?)?\b|\bRIGS?\b|\bWELL ?HEADS?\b|\bFPSO\b|\bOFFSHORE INSTALLATIONS?\b|\b(SUSPENDED|ABANDONED) WELLS?\b)"},
        {"platform", "fr", R"(\bPLATE.?FORMES?\b|\bTETES? DE PUITS\b)"}, {"platform", "es", R"(\bPLATAFORMAS?\b|\bCABEZA DE POZO\b)"},
        {"platform", "it", R"(\bPIATTAFORM[AE]\b|\bTESTA POZZO\b)"}, {"platform", "de", R"(\bPLATTFORM\w*\b|\bBOHRINSEL\w*\b)"},
        {"platform", "nl", R"(\bBOORPLATFORM\w*\b|\bBOOREILAND\w*\b|\bPLATFORMS?\b)"}, {"platform", "hr", R"(\bPLATFORM[AEU]\b)"},
        {"navaid", "en", R"(\bBUOYS?\b|\bBEACONS?\b|\bLIGHTHOUSE\b|\bLIGHT ?VESSEL\b|\bLIGHT ?FLOAT\b|\bLANBY\b|\bRACON\b|\bDGNSS\b|\bDGPS\b|\bAIS\b|\bFOG ?SIGNAL\b|\bFOGHORN\b|\bLEADING LIGHTS?\b|\bLIGHTS?\b)"},
        {"navaid", "fr", R"(\bBOUEES?\b|\bBALISES?\b|\bPHARES?\b|\bFEUX?\b)"}, {"navaid", "es", R"(\bBOYAS?\b|\bBALIZAS?\b|\bFAROS?\b|\bLUZ\b)"},
        {"navaid", "it", R"(\bBOA\b|\bBOE\b|\bMEDA\b|\bFARO\b|\bFANALE\b|\bLUCE\b)"}, {"navaid", "de", R"(\bTONNEN?\b|\bBAKEN\b|\bLEUCHTFEUER\b|\bFEUER\b)"},
        {"navaid", "nl", R"(\bBOEIEN?\b|\bBAKENS?\b|\bVUURTOREN\b|\bLICHT\b|\bMISTHOORN\b)"}, {"navaid", "hr", R"(\bPLUTAC\w*\b|\bSVJETIONIK\w*\b|\bSVJETL\w*\b|\bBOJA\b)"},
        {"navaid", "el", R"(\bFAROS\b|\bFANOS\b|\bSIMADOUR[AE]S?\b)"},
        // ★★ v6 (2026-11-01, Stuart): CABLES AND PIPELINES IN FIVE — gas pipeline, power cable, telecom cable, pipeline, and a
        //   cable whose kind the text does not say — from the message's own words, the specific ones FIRST. A "submarine
        //   cable" stays a cable (the submarine rule refuses it). "KEEP A SAFE DISTANCE OF 2 CABLES" is a distance — classify()
        //   rewrites a number of cables to CBLS before these rules look.
        {"pipeline_gas", "en", R"(\bGAS ?PIPE ?LINES?\b|\bGAS ?PIPES?\b|\bGAS TRUNK ?LINES?\b)"}, {"pipeline_gas", "fr", R"(\bGAZODUCS?\b)"},
        {"pipeline_gas", "it", R"(\bGASDOTT[OI]\b|\bMETANODOTT[OI]\b)"}, {"pipeline_gas", "es", R"(\bGASODUCTOS?\b)"},
        {"pipeline_gas", "de", R"(\bGASLEITUNG\w*\b|\bGASPIPELINE\w*\b|\bERDGAS\w*\b)"}, {"pipeline_gas", "nl", R"(\bGASLEIDING\w*\b|\bAARDGAS\w*\b)"},
        {"pipeline_gas", "hr", R"(\bPLINOVOD\w*\b)"},
        {"cable_power", "en", R"(\bPOWER ?CABLES?\b|\bEXPORT CABLES?\b|\bINTER-?ARRAY\b|\bINTERCONNECTORS?\b|\bHVDC\b|\bHVAC CABLES?\b|\bELECTRIC(AL)? CABLES?\b|\bHIGH.?VOLTAGE CABLES?\b)"},
        {"cable_power", "fr", R"(\bCABLES? ELECTRIQUES?\b|\bLIAISON ELECTRIQUE\b|\bRACCORDEMENT ELECTRIQUE\b)"},
        {"cable_power", "it", R"(\bCAV[OI] ELETTRIC[OI]\b|\bELETTRODOTT[OI]\b)"}, {"cable_power", "es", R"(\bCABLES? ELECTRICOS?\b|\bTENDIDO ELECTRICO\b)"},
        {"cable_power", "de", R"(\bSTROMKABEL\w*\b|\bENERGIEKABEL\w*\b|\bHOCHSPANNUNGSKABEL\w*\b)"},
        {"cable_power", "nl", R"(\bHOOGSPANNINGSKABEL\w*\b|\bSTROOMKABEL\w*\b|\bEXPORTKABEL\w*\b)"}, {"cable_power", "hr", R"(\bENERGETSK\w* KABEL\w*\b)"},
        {"cable_telecom", "en", R"(\bTELECOM(MUNICATIONS?)? CABLES?\b|\bFIB(RE|ER)[- ]?OPTICS?\b|\bDATA CABLES?\b|\bTELEPHONE CABLES?\b|\bCOMMUNICATIONS? CABLES?\b)"},
        {"cable_telecom", "fr", R"(\bFIBRES? OPTIQUES?\b|\bCABLES? (DE )?TELECOM\w*\b)"}, {"cable_telecom", "it", R"(\bFIBRA OTTICA\b|\bCAV[OI] (DI )?TELECOMUNICAZION\w*\b)"},
        {"cable_telecom", "es", R"(\bFIBRA OPTICA\b|\bCABLES? DE TELECOMUNICACION\w*\b)"}, {"cable_telecom", "de", R"(\bGLASFASER\w*\b|\bDATENKABEL\w*\b|\bTELEKOMMUNIKATIONSKABEL\w*\b)"},
        {"cable_telecom", "nl", R"(\bGLASVEZEL\w*\b|\bTELECOMKABEL\w*\b)"},
        {"pipeline", "en", R"(\b\w*PIPE ?LINES?\b|\bPIPE.?LAYING\b|\bOUTFALL PIPES?\b)"}, {"pipeline", "fr", R"(\bOLEODUCS?\b|\bCANALISATIONS?\b)"},
        {"pipeline", "es", R"(\bOLEODUCTOS?\b|\bTUBERIAS?\b|\bCANALIZACION\w*\b)"}, {"pipeline", "it", R"(\bCONDOTT[AE]\b|\bOLEODOTT[OI]\b)"},
        {"pipeline", "de", R"(\bROHRLEITUNG\w*\b)"}, {"pipeline", "nl", R"(\bPIJPLEIDING\w*\b|\bZINKERLEIDING\w*\b|\bLEIDINGEN\b)"},
        {"pipeline", "hr", R"(\bCJEVOVOD\w*\b)"},
        {"cable", "en", R"(\bCABLES?\b)"}, {"cable", "fr", R"(\bCABLES? SOUS.?MARINS?\b)"},
        {"cable", "es", R"(\bCABLE SUBMARINO\b)"}, {"cable", "it", R"(\bCAV[OI]\b)"},
        {"cable", "nl", R"(\bKABELWERK\w*\b|\bKABELS?\b)"}, {"cable", "de", R"(\bKABEL\w*\b)"},
        {"cable", "hr", R"(\bKABEL\w*\b)"}, {"cable", "el", R"(\bKALOD(IO|IA|IOY)\b)"},
        {"survey", "en", R"(\bSURVEY(S|ING)?\b|\bSEISMIC\b|\bHYDROGRAPHIC\b|\bTOWED ARRAY\b)"}, {"survey", "fr", R"(\bLEVES? HYDROGRAPHIQUES?\b|\bSISMIQUE\b)"},
        {"survey", "es", R"(\bLEVANTAMIENTO\b|\bSISMIC[OA]\b)"}, {"survey", "it", R"(\bRILIEV[OI]\b|\bSISMIC[AH]E?\b)"}, {"survey", "de", R"(\bVERMESSUNG\w*\b)"},
        {"survey", "nl", R"(\bPEILING\w*\b|\bSURVEYWERK\w*\b)"}, {"survey", "hr", R"(\bHIDROGRAF\w*\b)"},
        {"towing", "en", R"(\bTOW(ING|ED)?\b|\bIN TOW\b)"}, {"towing", "fr", R"(\bREMORQU\w*\b)"}, {"towing", "es", R"(\bREMOLQU\w*\b)"},
        {"towing", "it", R"(\bRIMORCHI\w*\b)"}, {"towing", "de", R"(\bSCHLEPP\w*\b)"}, {"towing", "nl", R"(\bSLEEP\w*\b)"}, {"towing", "hr", R"(\bTEGLJ\w*\b)"},
        {"works", "en", R"(\bDREDG(ING|ER|ERS)\b|\bWORKS\b|\bCONSTRUCTION\b|\bDIVING OPERATIONS\b|\bDIVERS?\b)"}, {"works", "fr", R"(\bTRAVAUX\b|\bDRAGAGES?\b|\bPLONGEE\b)"},
        {"works", "es", R"(\bOBRAS\b|\bDRAGADO\b|\bBUCEO\b)"}, {"works", "it", R"(\bLAVORI\b|\bDRAGAGGIO\b|\bSUBACQUE\w*\b)"}, {"works", "de", R"(\bARBEITEN\b|\bBAGGER\w*\b|\bTAUCH\w*\b)"},
        {"works", "nl", R"(\bWERKZAAMHEDEN\b|\bBAGGER\w*\b|\bDUIK\w*\b)"}, {"works", "hr", R"(\bRADOVI\b|\bRONJENJ\w*\b|\bJARUZANJ\w*\b)"},
        {"obstruction", "en", R"(\bOBSTRUCTIONS?\b|\bOBSTACLES?\b|\bANCHOR AND CHAIN\b|\bSHOAL(S|ING)?\b|\bLESS DEPTH\b|\bREDUCED DEPTH\b|\bUNCHARTED\b|\bROCKS?\b)"},
        {"obstruction", "fr", R"(\bOBSTRUCTIONS?\b|\bHAUT.?FOND\b)"}, {"obstruction", "es", R"(\bOBSTACULOS?\b|\bBAJO\b)"}, {"obstruction", "it", R"(\bOSTACOL[OI]\b|\bSECCA\b)"},
        {"obstruction", "de", R"(\bHINDERNIS\w*\b|\bUNTIEFE\b)"}, {"obstruction", "nl", R"(\bOBSTAKEL\w*\b|\bONDIEPTE\b)"}, {"obstruction", "hr", R"(\bPREPREK\w*\b|\bPLICIN\w*\b)"},
    };
    return r;
}
inline const std::vector<Rule>& unlitRules() {
    static const std::vector<Rule> r = {
        {"", "en", R"(\bLIGHTHOUSE\b[\s\S]{0,160}?\b(IS )?(NOT WORKING|INOPERATIVE|UNLIT|EXTINGUISHED|OUT OF (ORDER|SERVICE)|UNRELIABLE)\b|\bUNLIT\b|\bEXTINGUISHED\b|\bLIGHTS?\b[^.\n]{0,40}\b(INOPERATIVE|UNRELIABLE|OUT OF (ORDER|SERVICE)|NOT (WORKING|OPERATING|EXHIBITED|BURNING)|FAILED|IRREGULAR|DEFECTIVE)\b)"},
        {"", "fr", R"(\bFEUX? ETEINTS?\b|\bETEINTS?\b|\bHORS SERVICE\b)"}, {"", "es", R"(\bAPAGAD[OA]S?\b|\bFUERA DE SERVICIO\b|\bINOPERATIV[OA]\b)"},
        {"", "it", R"(\bSPENT[OA]\b|\bFUORI SERVIZIO\b|\bNON FUNZIONANTE\b|\bAVARIAT[OA]\b)"}, {"", "de", R"(\bNICHT IN BETRIEB\b|\bAUSGEFALLEN\b|\bGELOESCHT\b)"},
        {"", "nl", R"(\bGEDOOFD\b|\bBUITEN (WERKING|GEBRUIK)\b|\bDEFECT\b)"}, {"", "hr", R"(\bNE RADI\b|\bUGASEN\w*\b|\bNE SVIJETLI\b)"},
        {"", "el", R"(\bDEN LEITOURGEI\b|\bSVISMEN\w*\b)"},
    };
    return r;
}
inline Class classify(char b2, const std::string& text) {
    std::string U = navtex::upperAscii(text);
    const size_t z = U.find("ZCZC"); if (z != std::string::npos) U = U.substr(z);
    const size_t nn = U.find("NNNN"); if (nn != std::string::npos) U.resize(nn);
    // ★ v6: "KEEP A SAFE DISTANCE OF 2 CABLES", "2 KABELS AFSTAND" — a distance, not a cable
    U = std::regex_replace(U, rx(R"(\b(\d+(?:[.,]\d+)?|ONE|TWO|THREE|FOUR|FIVE|SIX|TEN)\s*(CABLES?|CABLE LENGTHS?|KABELS?|ENCABLURES?)\b)"), "$1 CBLS");
    // ★ v6: "CONSULT NEARBY OFFSHORE PLATFORMS" (an anchoring notice) names no platform hazard
    //   (and its garbled tail "…ORE PLATFORMS TO VERIFY THAT YOUR INTENDED ANCHORAGE")
    const std::string Up = std::regex_replace(U, rx(R"(\bCONSULT(\s+\w+){0,3}\s+PLATFORMS?\b|\bPLATFORMS? TO VERIFY\b)"), "CONSULT");
    // ★ v6: "A PIPELINE … MARKED WITH NORTH CARDINAL BUOY 'JDN'" — the hazard is the pipeline; the buoy only marks it. The
    //   navaid rules do not read what follows MARKED WITH / MARKED BY.
    const std::string Un = std::regex_replace(U, rx(R"(\b(MARKED|MARKS|MARKING)\s+(WITH|BY)\b[^.\n]{0,50})"), "MARKED");
    // ★ v6: Spanish "TENDIDO DE CABLE SUBMARINO" is an undersea cable, never a submarine (the French had this already)
    const std::string Us = std::regex_replace(U, rx(R"(\b(CABLES?|TUBERIAS?|CONDUCCION(ES)?|EMISARIOS?)\s+SUBMARIN[OA]S?\b)"), "$1");
    Class c;
    std::smatch m;
    for (const auto& r : unlitRules()) if (std::regex_search(U, m, rx(r.re))) { c.unlit = true; if (std::string(r.lang) != "en") { c.hit = m.str(0).substr(0, 40); c.lang = r.lang; } break; }
    if (b2 == 'C') { c.id = "ice"; return c; }
    if (b2 == 'B' || b2 == 'E') { c.id = "met"; return c; }   // ★ a forecast or a met warning: the WEATHER layer's, never a hazard icon
    for (const auto& r : rules()) {
        if (b2 == 'D' && std::string(r.cls) != "sar") { c.id = "sar"; return c; }
        const std::string cls = r.cls;
        const std::string& in = cls == "platform" ? Up : cls == "navaid" ? Un : cls == "submarine" ? Us : U;
        if (std::regex_search(in, m, rx(r.re))) {
            if (std::string(r.cls) == "adrift" && word(U, R"(\b(BUOY|BOUEE|BOYA|BOA|TONNE|BOEI)\b[^.\n]{0,30}\b(ADRIFT|A LA DERIVE|A LA DERIVA|ALLA DERIVA|TREIBEND|DRIJVEND)\b)")) continue;   // ★ a buoy adrift is a navaid
            c.id = r.cls;
            if (std::string(r.lang) != "en" && c.hit.empty()) { c.hit = m.str(0).substr(0, 40); c.lang = r.lang; }
            // ★ a navaid that is a BUOY or a beacon (any language's word for it) draws the buoy, not the lighthouse
            if (c.id == "navaid" && word(Un, R"(\b(BUOYS?|BEACONS?|LANBY|LIGHT ?FLOAT|BOUEES?|BALISES?|BOYAS?|BALIZAS?|BOA|BOE|MEDA|TONNEN?|BAKEN|BOEIEN?|BAKENS?|BOJA|PLUTAC\w*|SIMADOUR[AE]S?|\w+BOEI(EN)?|\w+TONNEN?)\b)")) c.id = "buoy";   // ★ compounds: LICHTBOEI, LEUCHTTONNE
            return c;
        }
    }
    if (b2 == 'D') { c.id = "sar"; return c; }
    if (b2 == 'B' || b2 == 'E') { c.id = "met"; return c; }
    c.id = "general";
    return c;
}

// ── the message's own references ────────────────────────────────────────────────────────────────────────────────────
/** The body between ZCZC's line and NNNN. */
inline std::string body(const std::string& file) {
    const size_t z = file.find("ZCZC");
    if (z == std::string::npos) return std::string();
    const size_t eol = file.find('\n', z);
    std::string b = file.substr(eol == std::string::npos ? file.size() : eol + 1);
    const size_t nn = b.find("NNNN"); if (nn != std::string::npos) b.resize(nn);
    return b;
}
/** Its own series number in the first lines, with the word before it — "WZ 649/26" → "WZ 649/26", "AVURNAV BREST 370/26"
 *  → "BREST 370/26", "NR-2594/2026" → "NR 2594/26". (seriesKey compares them tolerantly.) */
inline std::string seriesOf(const std::string& b) {
    std::string head = navtex::upperAscii(b.substr(0, 240));
    std::smatch m;
    static const std::regex re(R"(\b(\d{1,4})\s?/\s?(\d{2}|\d{4})\b)");
    for (auto it = std::sregex_iterator(head.begin(), head.end(), re); it != std::sregex_iterator(); ++it) {
        const size_t at = (size_t)it->position(0);
        const size_t ls = head.rfind('\n', at);
        const std::string line = head.substr(ls == std::string::npos ? 0 : ls + 1, at - (ls == std::string::npos ? 0 : ls + 1));
        if (line.find("CANCEL") != std::string::npos) continue;
        std::string y = (*it)[2]; if (y.size() == 4) y = y.substr(2);
        // the word just before the number (letters only)
        size_t e = at; while (e > 0 && !std::isalpha((unsigned char)head[e - 1]) && head[e - 1] != '\n') --e;
        size_t b = e; while (b > 0 && std::isalpha((unsigned char)head[b - 1])) --b;
        const std::string w = head.substr(b, e - b);
        return (w.empty() ? std::string() : w + " ") + std::string((*it)[1]) + "/" + y;
    }
    return std::string();
}
/** ★ The same warning's number across stations and bands: the series word's first three letters (a garbled letter later
 *  in the word is tolerated — "AVURNAV"/"AVUCNAV" both precede "BREST"), the number exact. "" when there is none. */
inline std::string seriesKey(const std::string& series) {
    const size_t sp = series.rfind(' ');
    if (sp == std::string::npos || sp == 0) return std::string();
    return series.substr(0, std::min<size_t>(3, sp)) + "|" + series.substr(sp + 1);
}
/** The letters-only word just before position `at` ("WZ" in "CANCEL WZ 476/26"), on the same line. */
inline std::string wordBefore(const std::string& s, size_t at) {
    size_t e = at; while (e > 0 && !std::isalpha((unsigned char)s[e - 1]) && s[e - 1] != '\n') --e;
    size_t b = e; while (b > 0 && std::isalpha((unsigned char)s[b - 1])) --b;
    return s.substr(b, e - b);
}
struct Cancels { std::vector<std::string> series, ids, replaces; int64_t at = 0; bool self = false; };
/** "CANCEL WZ 476/26 (EA77)(VA08)" → series 476/26, ids EA77 VA08; "CANCEL THIS MSG 302200 UTC NOV" → at (UTC s, the
 *  month and day of the message year — resolved against the message time `t`). */
inline Cancels cancelsOf(const std::string& b, int64_t t) {
    Cancels c;
    const std::string U = navtex::upperAscii(b);
    static const std::regex line(R"((CANCEL|ANNULE|ANULA|ANNULLA|AUFGEHOBEN|VERVALT|AKYRON|PONISTAVA)[^\n]*)"), ser(R"(\b(\d{1,4})\s?/\s?(\d{2}|\d{4})\b)"), id(R"(\(([A-Z]{2}\d{2})\))"),
                            self(R"((?:CANCEL|AND) THIS (MSG|MESSAGE|WARNING)\.?\s+(\d{2})(\d{2})(\d{2})\s*(UTC|Z)?\s*([A-Z]{3}))"),
                            selfNow(R"((?:CANCEL|AND) THIS (MSG|MESSAGE|WARNING)\b(?!\.?\s*((AT|ON)\s+)?\d))");
    for (auto it = std::sregex_iterator(U.begin(), U.end(), line); it != std::sregex_iterator(); ++it) {
        const std::string l = it->str();
        std::smatch m;
        if (std::regex_search(l, m, self)) {
            static const char* mon[] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
            int mo = -1; for (int k = 0; k < 12; ++k) if (m[6] == mon[k]) mo = k;
            if (mo >= 0) {
                std::time_t tt = (std::time_t)t; std::tm g{}; gmtime_r(&tt, &g);
                std::tm e{}; e.tm_year = g.tm_year; e.tm_mon = mo; e.tm_mday = std::atoi(m[2].str().c_str());
                e.tm_hour = std::atoi(m[3].str().c_str()); e.tm_min = std::atoi(m[4].str().c_str());
                int64_t at = (int64_t)timegm(&e);
                if (at < t - 180 * 86400) { e.tm_year++; at = (int64_t)timegm(&e); }   // ★ across a new year
                c.at = at;
            }
            // ★ v6: no `continue` — "CANCEL WZ 621/26 AND THIS MSG 302200 UTC OCT" names a series too (read below)
        } else if (std::regex_search(l, m, selfNow)) c.self = true;   // ★ v6: "CANCEL WZ 621/26 (GA12) AND THIS MSG." — a notice, never a hazard
        for (auto s2 = std::sregex_iterator(l.begin(), l.end(), ser); s2 != std::sregex_iterator(); ++s2) {
            std::string y = (*s2)[2]; if (y.size() == 4) y = y.substr(2);
            const std::string w = wordBefore(l, (size_t)s2->position(0));
            if (c.series.size() < 8) c.series.push_back((w.empty() ? std::string() : w + " ") + std::string((*s2)[1]) + "/" + y);
        }
        for (auto s3 = std::sregex_iterator(l.begin(), l.end(), id); s3 != std::sregex_iterator(); ++s3)
            if (c.ids.size() < 8) c.ids.push_back((*s3)[1]);
    }
    // ★ "COSTAVURNAV 510/26 OCT 24 IS CANCELED", "COASTAL WARNING 510/26 (CANCELED)": the message IS a cancellation of the
    //   series number it carries — its own position is not a hazard any more
    static const std::regex canceled(R"(\b(\d{1,4})\s?/\s?(\d{2}|\d{4})\b[^\n]{0,40}\(?\b(CANCELL?ED|ANNULEE?S?|ANULADO|ANULADA|CANCELLAT[OA]|ANNULLAT[OA]|AUFGEHOBEN|VERVALLEN|INGETROKKEN|PONISTEN[OA]?|AKYRONETAI|AKYROSI)\b)");
    for (auto it = std::sregex_iterator(U.begin(), U.end(), canceled); it != std::sregex_iterator(); ++it) {
        std::string y = (*it)[2]; if (y.size() == 4) y = y.substr(2);
        const std::string w = wordBefore(U, (size_t)it->position(0));
        if (c.series.size() < 8) c.series.push_back((w.empty() ? std::string() : w + " ") + std::string((*it)[1]) + "/" + y);
        c.self = true;
    }
    // ★★ v6 (Stuart: "the icons are based on the latest received message"): "THIS MESSAGE REPLACES WZ 612/26", "REMPLACE
    //   AVURNAV 370/26", "SUSTITUYE", "SOSTITUISCE", "ERSETZT", "VERVANGT" — the warning it names is SUPERSEDED, not drawn
    static const std::regex repl(R"(\b(REPLACES|REPLACING|SUPERSEDES|REMPLACE|SUSTITUYE|SOSTITUISCE|ERSETZT|VERVANGT)\b[^\n]{0,40})");
    for (auto it = std::sregex_iterator(U.begin(), U.end(), repl); it != std::sregex_iterator(); ++it) {
        const std::string l = it->str();
        for (auto s2 = std::sregex_iterator(l.begin(), l.end(), ser); s2 != std::sregex_iterator(); ++s2) {
            std::string y = (*s2)[2]; if (y.size() == 4) y = y.substr(2);
            const std::string w = wordBefore(l, (size_t)s2->position(0));
            if (c.replaces.size() < 8) c.replaces.push_back((w.empty() ? std::string() : w + " ") + std::string((*s2)[1]) + "/" + y);
        }
    }
    return c;
}
/** ★ The coordinate formats navtex::positions does not read, made into one it does — strictly: "LAT 40 23.4'N LONG
 *  018 18.4'E" (an apostrophe minutes mark, the words LAT / LONG) → "40 23.4N 018 18.4E". Nothing else is changed. */
/** ★ A Greek warning (transliterated): its own words. Only then are Greek hemisphere letters read. */
inline bool greek(const std::string& U) {
    return std::regex_search(U, rx(R"(\b(STATHMOS|STATHMOY|PERIOCHI|PERIOXH|NISOS|NISOY|AKRA|AKROTIRIO|LIMENAS|LIMANI|THALASSIA|PROSOXI|NAYTILIA)\b)"));
}
inline std::string posText(const std::string& b) {
    std::string U = navtex::upperAscii(b);
    /* ★★ GREEK hemisphere letters — B (Βόρειο) north, N (Νότιο) SOUTH, A (Ανατολικό) east, D/Δ (Δυτικό) west: "35-28.30B
     *  024-14.10A". ONLY in a message identified as Greek by its own words (an English "N" is north). */
    if (greek(U)) {
        U = std::regex_replace(U, rx(R"((\b\d{2}-\d{2}(?:[.,]\d{1,3})?)\s?N\b)"), "$1\x01");   // south first (N would read as north)
        U = std::regex_replace(U, rx(R"((\b\d{2}-\d{2}(?:[.,]\d{1,3})?)\s?B\b)"), "$1N");
        U = std::regex_replace(U, rx(R"((\b\d{3}-\d{2}(?:[.,]\d{1,3})?)\s?A\b)"), "$1E");
        U = std::regex_replace(U, rx(R"((\b\d{3}-\d{2}(?:[.,]\d{1,3})?)\s?D\b)"), "$1W");
        for (auto& c : U) if (c == '\x01') c = 'S';
    }
    U = std::regex_replace(U, rx(R"((\d)'([NSEW])\b)"), "$1$2");
    U = std::regex_replace(U, rx(R"(\bLAT\.?\s+(\d))"), "$1");
    U = std::regex_replace(U, rx(R"(([NS])\s+LONG\.?\s+(\d))"), "$1 $2");
    return U;
}
/** ★★ THE WARNING'S OWN WINDOW (2026-11-01): "APO 01 IAN 26 EOS 31 DEK 26", "FROM 01 NOV TO 15 NOV 2026", "DU 01 NOV AU
 *  15 NOV", "DESDE 01 NOV HASTA 15 NOV" … → its END (UTC s, end of that day); "UNTIL FURTHER NOTICE" (and its French,
 *  Spanish, Italian, German, Dutch forms) → `ufn`. Only a clean date range is read; recurring day/time windows are not.
 *  Months: English, Greek (IAN FEV MAR APR MAI IOUN IOUL AUG SEP OKT NOE DEK), French, Spanish, Italian, German, Dutch. */
struct Window { int64_t to = 0; bool ufn = false; };
inline int monthOf(const std::string& m) {
    static const std::vector<std::pair<const char*, int>> t = {
        {"JAN",0},{"IAN",0},{"JANV",0},{"ENE",0},{"GEN",0},{"FEB",1},{"FEV",1},{"FEVR",1},{"MAR",2},{"MRZ",2},{"MAA",2},{"APR",3},{"AVR",3},{"ABR",3},
        {"MAY",4},{"MAI",4},{"MAG",4},{"MEI",4},{"JUN",5},{"IOUN",5},{"JUIN",5},{"GIU",5},{"JUL",6},{"IOUL",6},{"JUIL",6},{"LUG",6},{"AUG",7},{"AOU",7},{"AGO",7},
        {"SEP",8},{"SEPT",8},{"SET",8},{"OCT",9},{"OKT",9},{"OTT",9},{"NOV",10},{"NOE",10},{"DEC",11},{"DEK",11},{"DIC",11},{"DEZ",11}};
    for (auto& x : t) if (m == x.first) return x.second;
    return -1;
}
inline Window windowOf(const std::string& b, int64_t t) {
    Window w;
    const std::string U = navtex::upperAscii(b);
    if (std::regex_search(U, rx(R"(\bUNTIL FURTHER NOTICE\b|\bUFN\b|\bJUSQU.A NOUVEL AVIS\b|\bHASTA NUEVO AVISO\b|\bFINO A NUOVO AVVISO\b|\bBIS AUF WEITERES\b|\bTOT NADER BERICHT\b|\bMECHRI NEOTERAS\b)"))) w.ufn = true;
    static const std::regex range(R"(\b(FROM|APO|DU|DESDE|DAL|VOM|VAN)\s+(\d{1,2})(?:\d{4})?\s*(?:UTC\s*)?([A-Z]{3,4})?\s*(\d{2}|\d{4})?\s+(TO|EOS|AU|HASTA|AL|BIS|TOT|TILL|UNTIL|THRU)\s+(\d{1,2})(?:\d{4})?\s*(?:UTC\s*)?([A-Z]{3,4})\.?\s*(\d{2}|\d{4})?\b)");
    std::smatch m;
    if (std::regex_search(U, m, range)) {
        const int mo = monthOf(m[7]);
        const int d = std::atoi(m[6].str().c_str());
        if (mo >= 0 && d >= 1 && d <= 31) {
            std::time_t tt = (std::time_t)t; std::tm g{}; gmtime_r(&tt, &g);
            std::tm e{}; e.tm_mday = d; e.tm_mon = mo; e.tm_hour = 23; e.tm_min = 59;
            const std::string y = m[8].str();
            e.tm_year = y.empty() ? g.tm_year : (y.size() == 2 ? 100 + std::atoi(y.c_str()) : std::atoi(y.c_str()) - 1900);
            int64_t to = (int64_t)timegm(&e);
            if (y.empty() && to < t - 180 * 86400) { e.tm_year++; to = (int64_t)timegm(&e); }
            if (to > t - 400LL * 86400 && to < t + 800LL * 86400) w.to = to;
        }
    }
    return w;
}
/** A hash of the text as the station sent it: letters and digits only, so line breaks, spacing and the odd garbled
 *  punctuation do not make a copy a different warning; '*' and '_' (lost characters) ignored. 64-bit FNV-1a. */
inline uint64_t textHash(const std::string& b) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : b) {
        const char c = (char)std::toupper(ch);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) continue;
        h ^= (unsigned char)c; h *= 1099511628211ull;
    }
    return h;
}
/** Error characters: '*' / '_' the decoder printed for what it lost. */
inline int lostChars(const std::string& b) { int n = 0; for (char c : b) n += c == '*' || c == '_'; return n; }

/** ★★ v6 A LIST OF NAMED ITEMS (2026-11-01, Stuart): "FOLLOWING PLATFORMS HAVE DEFECTS:" then one line an item —
 *  "K6-N 53-41.0N 003-44.0E FOGHORN INOPERATIVE". Each is its OWN marker with its own name and defect (never one shape
 *  across the North Sea). Strict: every position of the message is on a line of its own (one position a line, two lines
 *  or more), and each line has words besides its position (a corner list "A) 51-00N 002-00E" has none). The words
 *  before the position are its name, after it its defect; `u` when the defect says its light is out. */
struct Item { std::string name, defect; bool unlit = false; };
inline std::string itemClean(std::string s, size_t cap) {
    for (auto& ch : s) if (ch == '|' || ch == '~' || ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
    s = std::regex_replace(s, rx(R"(^[\s,;:.\-]*(\d{1,2}[.)]\s*|[A-Z][)]\s*)?)"), "");
    s = std::regex_replace(s, rx(R"(\b(IN )?(POSITION|POSN|PSN|POS)\b:?\s*$)"), "");
    s = std::regex_replace(s, rx(R"(\s+)"), " ");
    while (!s.empty() && (s.back() == ' ' || s.back() == ',' || s.back() == '.' || s.back() == ';' || s.back() == ':' || s.back() == '-')) s.pop_back();
    while (!s.empty() && (s[0] == ' ' || s[0] == ',' || s[0] == '-' || s[0] == ':')) s.erase(0, 1);
    if (s.size() > cap) s.resize(cap);
    return s;
}
inline bool meaningful(const std::string& s) {
    static const std::regex filler(R"(^(AND|OR|TO|FROM|AT|IN|THE|BETWEEN|THEN|POSITION|POS|PSN|LAT|LONG|N|S|E|W)$)");
    std::string w; int letters = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size() && std::isalnum((unsigned char)s[i])) { w += s[i]; continue; }
        if (!w.empty() && !std::regex_match(w, filler)) for (char ch : w) letters += std::isalpha((unsigned char)ch) ? 1 : 0;
        w.clear();
    }
    return letters >= 2;
}
inline std::vector<Item> namedItems(const std::string& b, size_t nAll) {
    std::vector<Item> out;
    if (nAll < 2) return out;
    static const std::regex coord(R"(\d{1,3}[- ]?\d{1,2}(?:[.,]\d{1,3})?\s?[NSEW]\b)");
    size_t p = 0;
    const std::string U = navtex::upperAscii(b);
    while (p <= U.size()) {
        const size_t e = U.find('\n', p);
        const std::string line = U.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? U.size() + 1 : e + 1;
        const std::string pl = posText(line);
        const size_t n = navtex::positions(pl).size();
        if (n == 0) continue;
        if (n > 1) return {};
        auto it = std::sregex_iterator(pl.begin(), pl.end(), coord);
        size_t a = std::string::npos, z = 0;
        for (; it != std::sregex_iterator(); ++it) { if (a == std::string::npos) a = (size_t)it->position(0); z = (size_t)(it->position(0) + it->length(0)); }
        if (a == std::string::npos) return {};
        Item i; i.name = itemClean(pl.substr(0, a), 40); i.defect = itemClean(pl.substr(z), 60);
        if (!meaningful(i.name + " " + i.defect)) return {};
        if (i.name.empty()) { i.name = i.defect; i.defect.clear(); }
        i.unlit = std::regex_search(i.defect.empty() ? i.name : i.defect, rx(R"(\bUNLIT\b|\bBLACK ?OUT\b|\bEXTINGUISHED\b|\bLIGHTS?\b[^,;]{0,20}\b(INOPERATIVE|UNRELIABLE|OUT|FAILED|DEFECTIVE)\b|\bNAV ?AIDS?\b[^,;]{0,20}\b(INOPERATIVE|UNRELIABLE)\b|\bETEINTS?\b|\bSPENT[OA]\b|\bGEDOOFD\b|\bAPAGAD[OA]\b)"));
        out.push_back(i);
    }
    if (out.size() != nAll) return {};
    return out;
}
/** ★★ WHAT THE POINTS ARE (2026-11-01, the map's lines and areas): the text's own words. An AREA ("AREA BOUNDED BY",
 *  "WITHIN AREA", "ZONE", "ZONA", "GEBIET", "GEBIED", "PODRUCJE") with three points or more = a polygon; a ROUTE ("ALONG",
 *  "ROUTE", "TRACK", "LINE JOINING", "BETWEEN … AND", cable or pipeline LAYING, a tow FROM … TO) with two or more = a
 *  line; otherwise separate points. 'a' / 'r' / '' — never guessed from the points' layout. */
inline char shapeOf(const std::string& b, size_t npts) {
    if (npts < 2) return 0;
    const std::string U = navtex::upperAscii(b);
    static const std::regex area(R"(\bAREAS? (BOUNDED|ENCLOSED|DEFINED|BETWEEN|WITHIN)\b|\bBOUNDED BY\b|\bWITHIN (AN |THE )?AREA\b|\bIN (AN |THE )?AREA\b|\bAREA:|\bZONES?\b|\bPOLYGON\b|\bZONA\b|\bGEBIET\b|\bGEBIED\b|\bPODRUCJ\w*\b|\bPERIOCHI\b|\bSECTOR\b)");
    static const std::regex route(R"(\bALONG\b|\bROUTE\b|\bTRACK\b|\bLINE (JOINING|BETWEEN|CONNECTING)\b|\bJOINING\b|\b(CABLE|PIPE ?LINE|PIPE)[ -]?(LAYING|LAY|ROUTE)\b|\bLE LONG\b|\bTRAJET\b|\bDERROTA\b|\bROTTA\b|\bTRASA\b|\bLINIE\b|\bLIJN\b|\bLINEA\b)");
    if (npts >= 3 && std::regex_search(U, area)) return 'a';
    if (std::regex_search(U, route)) return 'r';
    if (npts >= 3 && std::regex_search(U, rx(R"(\bAREA\b)"))) return 'a';
    return 0;
}
/** The message's own date-time group in its first lines — "301500 UTC OCT 26", "011234 UTC NOV 26", "010710 UTC OCT" —
 *  as UTC seconds, or 0. The year from the message time when it is not given (and across a new year). */
inline int64_t issuedOf(const std::string& b, int64_t t) {
    const std::string head = navtex::upperAscii(b.substr(0, 160));
    static const std::regex dtg(R"((?:^|\n)\s*(\d{2})(\d{2})(\d{2})\s*(?:UTC|Z)\s+([A-Z]{3})\.?(?:\s+(\d{2}|\d{4}))?)");
    std::smatch m;
    if (!std::regex_search(head, m, dtg)) return 0;
    const int mo = monthOf(m[4]);
    const int d = std::atoi(m[1].str().c_str()), hh = std::atoi(m[2].str().c_str()), mi = std::atoi(m[3].str().c_str());
    if (mo < 0 || d < 1 || d > 31 || hh > 23 || mi > 59) return 0;
    std::time_t tt = (std::time_t)t; std::tm g{}; gmtime_r(&tt, &g);
    std::tm e{}; e.tm_mday = d; e.tm_mon = mo; e.tm_hour = hh; e.tm_min = mi;
    const std::string y = m[5].str();
    e.tm_year = y.empty() ? g.tm_year : (y.size() == 2 ? 100 + std::atoi(y.c_str()) : std::atoi(y.c_str()) - 1900);
    int64_t at = (int64_t)timegm(&e);
    if (y.empty() && at > t + 2 * 86400) { e.tm_year--; at = (int64_t)timegm(&e); }   // ★ a December message heard in January
    return at > t - 400LL * 86400 && at < t + 2 * 86400 ? at : 0;   // ★ a garbled group is dropped, never shown
}

// ── one index line ───────────────────────────────────────────────────────────────────────────────────────────────────
struct Entry {
    std::string file, id, cls, series, cIds, cSeries;   // cIds / cSeries: what it cancels, space-separated
    int64_t t = 0, cancelAt = 0; double khz = 0;
    uint64_t hash = 0; int lost = 0; bool unlit = false;
    char b1 = 0, b2 = 0;
    std::vector<std::pair<double, double>> pts;
    std::string hit, lang;   // ★ the native word that classified it, and its language (the card's summary)
    int64_t validTo = 0; bool ufn = false;   // ★ its own window's end; "until further notice"
    char shape = 0; int64_t issued = 0;       // ★ v4: 'a' area / 'r' route / 0 points; its own date-time group
    std::string rSeries;                       // ★ v6: what it REPLACES (';'-separated series)
    std::vector<Item> items;                   // ★ v6: a named list — one per point, in the points' order
};
inline uint64_t nameHash(const std::string& s) { uint64_t h = 1469598103934665603ull; for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } return h; }
/** name~defect~u|… — the named list's index field (itemClean took '|' '~' tab and newline out) */
inline std::string itemsField(const std::vector<Item>& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) o += (i ? "|" : "") + v[i].name + "~" + v[i].defect + "~" + (v[i].unlit ? "1" : "0");
    return o;
}
inline std::vector<Item> itemsOf(const std::string& f) {
    std::vector<Item> v; size_t p = 0;
    while (p < f.size()) {
        size_t q = f.find('|', p); if (q == std::string::npos) q = f.size();
        const std::string one = f.substr(p, q - p);
        const size_t a = one.find('~'), b2 = a == std::string::npos ? a : one.find('~', a + 1);
        if (a != std::string::npos && b2 != std::string::npos) { Item i; i.name = one.substr(0, a); i.defect = one.substr(a + 1, b2 - a - 1); i.unlit = one.substr(b2 + 1) == "1"; v.push_back(i); }
        p = q + 1;
    }
    return v;
}
/** v1 \t file \t t \t khz \t id \t hash(hex) \t lost \t class \t unlit \t series \t cancelIds \t cancelSeries \t cancelAt \t pts */
inline std::string format(const Entry& e) {
    char b[160];
    std::string o = "v" + std::to_string(kVersion) + "\t" + e.file;
    std::snprintf(b, sizeof b, "\t%lld\t%g\t%s\t%016llx\t%d\t", (long long)e.t, e.khz, e.id.c_str(), (unsigned long long)e.hash, e.lost);
    o += b;
    o += e.cls + "\t" + (e.unlit ? "1" : "0") + "\t" + e.series + "\t" + e.cIds + "\t" + e.cSeries + "\t" + std::to_string((long long)e.cancelAt) + "\t";
    for (size_t i = 0; i < e.pts.size(); ++i) { std::snprintf(b, sizeof b, "%s%.4f,%.4f", i ? ";" : "", e.pts[i].first, e.pts[i].second); o += b; }
    std::string hit = e.hit; for (auto& ch : hit) if (ch == '\t' || ch == '\n') ch = ' ';
    return o + "\t" + hit + "\t" + e.lang + "\t" + std::to_string((long long)e.validTo) + "\t" + (e.ufn ? "1" : "0")
         + "\t" + (e.shape ? std::string(1, e.shape) : std::string()) + "\t" + std::to_string((long long)e.issued)
         + "\t" + e.rSeries + "\t" + itemsField(e.items) + "\n";
}
inline bool parse(const std::string& l, Entry& e) {
    std::vector<std::string> f; size_t p = 0;
    for (;;) { const size_t q = l.find('\t', p); if (q == std::string::npos) { f.push_back(l.substr(p)); break; } f.push_back(l.substr(p, q - p)); p = q + 1; }
    if (f.size() != 22 || f[0] != "v" + std::to_string(kVersion)) return false;
    e.rSeries = f[20]; e.items = itemsOf(f[21]);
    e.hit = f[14]; e.lang = f[15]; e.validTo = std::atoll(f[16].c_str()); e.ufn = f[17] == "1";
    e.shape = f[18] == "a" ? 'a' : f[18] == "r" ? 'r' : 0; e.issued = std::atoll(f[19].c_str());
    e.file = f[1]; e.t = std::atoll(f[2].c_str()); e.khz = std::atof(f[3].c_str()); e.id = f[4];
    e.hash = std::strtoull(f[5].c_str(), nullptr, 16); e.lost = std::atoi(f[6].c_str()); e.cls = f[7]; e.unlit = f[8] == "1";
    e.series = f[9]; e.cIds = f[10]; e.cSeries = f[11]; e.cancelAt = std::atoll(f[12].c_str());
    if (e.id.size() == 4) { e.b1 = e.id[0]; e.b2 = e.id[1]; }
    std::string pts = f[13];
    while (!pts.empty()) {
        const size_t sc = pts.find(';');
        const std::string one = pts.substr(0, sc);
        double la, lo;
        if (std::sscanf(one.c_str(), "%lf,%lf", &la, &lo) == 2 && std::fabs(la) <= 90 && std::fabs(lo) <= 180) e.pts.push_back({la, lo});
        if (sc == std::string::npos) break;
        pts = pts.substr(sc + 1);
    }
    return !e.file.empty();
}
/** One gallery NAVTEX message (its index line and its file's text) → its entry. */
inline Entry fromMessage(const std::string& file, int64_t t, double hz, const std::string& text) {
    Entry e; e.file = file; e.t = t; e.khz = navtex::bandKhz(hz); if (!e.khz) e.khz = hz / 1000.0;
    const size_t z = text.find("ZCZC ");
    if (z != std::string::npos && z + 9 <= text.size()) {
        const std::string id = text.substr(z + 5, 4);
        if (std::isupper((unsigned char)id[0]) && std::isupper((unsigned char)id[1]) && std::isdigit((unsigned char)id[2]) && std::isdigit((unsigned char)id[3])) e.id = id;
    }
    if (e.id.size() == 4) { e.b1 = e.id[0]; e.b2 = e.id[1]; }
    const std::string b = body(text);
    e.hash = textHash(b); e.lost = lostChars(b);
    const Class c = classify(e.b2, b); e.cls = c.id; e.unlit = c.unlit; e.hit = c.hit; e.lang = c.lang;
    e.series = seriesOf(b);
    const Cancels cx = cancelsOf(b, t);
    if (cx.self) e.cls = "cancel";   // ★ a cancellation: applied, never plotted
    const Window w = windowOf(b, t); e.validTo = w.to; e.ufn = w.ufn;
    for (auto& s : cx.ids) e.cIds += (e.cIds.empty() ? "" : " ") + s;
    for (auto& s : cx.series) e.cSeries += (e.cSeries.empty() ? "" : ";") + s;   // ★ ';' — a series holds a space
    e.cancelAt = cx.at;
    for (auto& s : cx.replaces) e.rSeries += (e.rSeries.empty() ? "" : ";") + s;
    auto pts = navtex::positions(posText(b));
    // ★ v6: a named list — kept beside its points through the filters below (a dropped point drops its item)
    std::vector<Item> items = shapeOf(b, pts.size()) == 0 ? namedItems(b, pts.size()) : std::vector<Item>();
    /* ★ A position with a digit lost ("02-44.67N" for 62-44.67N in a Norwegian chart area) can still parse. Not plotted
     *  when it is further than a NAVTEX service area (3 000 km MF, 5 000 km HF) from EVERY transmitter using this letter on
     *  this band — such a point is damage, not a hazard; it carries no weight either way. */
    if (e.b1 && e.khz) {
        size_t n = 0; const navtex::Station* tb = navtex::stations(n);
        const double reach = e.khz > 1000 ? 5000 : 3000;
        std::vector<std::pair<double, double>> keep; std::vector<Item> ik;
        for (auto& p : pts) {
            bool nearOne = false, any = false;
            for (size_t i = 0; i < n && !nearOne; ++i) if (tb[i].b1 == e.b1 && tb[i].khz == e.khz) { any = true; nearOne = navtex::km(p.first, p.second, tb[i].lat, tb[i].lon) <= reach; }
            if (nearOne || !any) { if (!items.empty()) ik.push_back(items[(size_t)(&p - &pts[0])]); keep.push_back(p); }
        }
        pts.swap(keep); if (!items.empty()) items.swap(ik);
    }
    /* ★ One point of a list far from all the others ("24-20.6N" among seven at 64°N — a 6 read as a 2): with three or
     *  more, a point further than 300 km and five times the list's own spread from its median is damage, not a corner. */
    if (pts.size() >= 3) {
        std::vector<double> la, lo; for (auto& p : pts) { la.push_back(p.first); lo.push_back(p.second); }
        std::nth_element(la.begin(), la.begin() + la.size() / 2, la.end()); std::nth_element(lo.begin(), lo.begin() + lo.size() / 2, lo.end());
        const double mla = la[la.size() / 2], mlo = lo[lo.size() / 2];
        std::vector<double> d; for (auto& p : pts) d.push_back(navtex::km(p.first, p.second, mla, mlo));
        std::vector<double> ds = d; std::nth_element(ds.begin(), ds.begin() + ds.size() / 2, ds.end());
        const double lim = std::max(300.0, 5 * ds[ds.size() / 2]);
        std::vector<std::pair<double, double>> keep; std::vector<Item> ik;
        for (size_t i = 0; i < pts.size(); ++i) if (d[i] <= lim) { keep.push_back(pts[i]); if (!items.empty()) ik.push_back(items[i]); }
        pts.swap(keep); if (!items.empty()) items.swap(ik);
    }
    if (pts.size() > kMaxPts) pts.resize(kMaxPts);
    if (items.size() > pts.size()) items.resize(pts.size());
    if (items.size() < 2) items.clear();
    e.pts = pts; e.items = items;
    e.shape = shapeOf(b, e.pts.size());
    if (e.shape) e.items.clear();
    e.issued = issuedOf(b, t);
    return e;
}

// ── the index file ───────────────────────────────────────────────────────────────────────────────────────────────────
using LiveFn = std::function<void(const std::function<void(const std::string& line)>&)>;   ///< the gallery's live NAVTEX lines
using TextFn = std::function<std::string(const std::string& file)>;                       ///< a gallery file's text (bounded)
struct SyncResult { size_t live = 0, indexed = 0, added = 0, pending = 0; };
/** ★ Bring the index up to date: every live NAVTEX item not yet in it is parsed (at most `budget` this call) and APPENDED.
 *  One sequential read of the index (the names' hashes), one walk of the live view, one append. */
inline SyncResult sync(const std::string& idxPath, const LiveFn& live, const TextFn& text, size_t budget,
                       std::unordered_set<uint64_t>* liveNames = nullptr, size_t* idxLines = nullptr, const std::string& metPath = std::string()) {
    SyncResult r;
    std::unordered_set<uint64_t> have;
    if (FILE* f = std::fopen(idxPath.c_str(), "r")) {
        char buf[8192];
        while (std::fgets(buf, sizeof buf, f)) {
            const char* t1 = std::strchr(buf, '\t'); if (!t1 || std::strncmp(buf, kPrefix, std::strlen(kPrefix)) != 0) continue;
            const char* t2 = std::strchr(t1 + 1, '\t'); if (!t2) continue;
            have.insert(nameHash(std::string(t1 + 1, t2)));
            if (idxLines) ++*idxLines;
        }
        std::fclose(f);
    }
    struct Todo { std::string file; int64_t t; double hz; };
    std::vector<Todo> todo;
    live([&](const std::string& l) {
        r.live++;
        std::string f; double t = 0, hz = 0;
        if (!navmap::jstr(l, "f", f) || !navmap::jnum(l, "t", t) || !navmap::jnum(l, "hz", hz)) return;
        if (liveNames) liveNames->insert(nameHash(f));
        if (have.count(nameHash(f))) { r.indexed++; return; }
        if (todo.size() < budget) todo.push_back({f, (int64_t)t, hz}); else r.pending++;
    });
    if (todo.empty()) return r;
    std::string out, met;
    // ★ the met index is rebuilt with this one: a fresh warnings index (none of this version's lines) starts it again
    if (!metPath.empty() && have.empty()) if (FILE* f = std::fopen(metPath.c_str(), "w")) std::fclose(f);
    for (const auto& d : todo) {
        const std::string tx = text(d.file);
        const Entry e = fromMessage(d.file, d.t, d.hz, tx);
        out += format(e); r.added++;
        if (!metPath.empty() && e.cls == "met") for (const auto& a : navmet::parse(body(tx))) met += navmet::format(d.file, d.t, a);
    }
    if (FILE* f = std::fopen(idxPath.c_str(), "a")) { std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); }
    if (!met.empty()) if (FILE* f = std::fopen(metPath.c_str(), "a")) { std::fwrite(met.data(), 1, met.size(), f); std::fclose(f); }
    r.indexed += r.added;
    if (idxLines) *idxLines += r.added;
    return r;
}
/** file-name hash → series key, from the index (the stations map's same-warning link). */
inline std::unordered_map<uint64_t, std::string> seriesKeys(const std::string& idxPath) {
    std::unordered_map<uint64_t, std::string> m;
    navmap::fileLines(idxPath)([&](const std::string& l) { Entry e; if (parse(l, e)) { const std::string k = seriesKey(e.series); if (!k.empty()) m[nameHash(e.file)] = k; } });
    return m;
}
/** ★ Drop the lines of messages the gallery no longer holds (and duplicates) — streamed, tmp + rename. Called when the
 *  dead lines are a third of the file. */
inline bool compact(const std::string& idxPath, const std::unordered_set<uint64_t>& liveNames) {
    FILE* f = std::fopen(idxPath.c_str(), "r");
    if (!f) return false;
    const std::string tmp = idxPath + ".tmp";
    FILE* o = std::fopen(tmp.c_str(), "w");
    if (!o) { std::fclose(f); return false; }
    std::unordered_set<uint64_t> wrote;
    char buf[8192];
    while (std::fgets(buf, sizeof buf, f)) {
        const char* t1 = std::strchr(buf, '\t'); if (!t1 || std::strncmp(buf, kPrefix, std::strlen(kPrefix)) != 0) continue;
        const char* t2 = std::strchr(t1 + 1, '\t'); if (!t2) continue;
        const uint64_t h = nameHash(std::string(t1 + 1, t2));
        if (!liveNames.count(h) || !wrote.insert(h).second) continue;
        std::fputs(buf, o);
    }
    std::fclose(f); std::fclose(o);
    return std::rename(tmp.c_str(), idxPath.c_str()) == 0;
}

// ── the query: one marker per warning ───────────────────────────────────────────────────────────────────────────────
struct Group {
    std::string id, cls, series, best;       // best = the file of the best copy
    char b1 = 0, b2 = 0; double khz = 0;
    int64_t first = 0, last = 0, cancelAt = 0; uint64_t hash = 0; int lost = 1 << 30; bool unlit = false;
    uint32_t n = 0; std::vector<std::pair<double, double>> pts;
    std::string cancelledBy;                  // the id that cancelled it
    std::string hit, lang; int64_t validTo = 0; bool ufn = false;
    char shape = 0; int64_t issued = 0;
    int64_t newest = 0; int newestLost = 0;  // ★ v6: the newest copy (its content is what is drawn)
    std::vector<Item> items;
    std::string supersededBy;                 // ★ v6: the id of the newer message that replaced it (a REPLACES, or a re-issue)
};
struct Query {
    std::vector<Group> shown;                 // newest first, ≤ kMaxMarkers
    size_t total = 0, receptions = 0, withPos = 0, live = 0, indexed = 0, pending = 0, groups = 0;
    std::map<std::string, size_t> byClass;
    size_t cancelled = 0, superseded = 0, expired = 0;   // ★ v6: of the warnings with a position — none of them drawn
};
inline bool samePts(const std::vector<std::pair<double, double>>& a, const std::vector<std::pair<double, double>>& b) {
    if (a.empty() || a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (std::fabs(a[i].first - b[i].first) > 0.05 || std::fabs(a[i].second - b[i].second) > 0.05) return false;
    return true;
}
/** ★ Read the index (streamed), keep the live messages heard since `since`, fold them into warnings, apply
 *  cancellations, and answer the newest kMaxMarkers that have a position. */
inline Query query(const std::string& idxPath, const std::unordered_set<uint64_t>& liveNames, int64_t since, int64_t now) {
    Query q;
    std::vector<Group> gs;
    std::multimap<std::string, size_t> byId;            // id → groups
    std::multimap<std::string, size_t> bySeries;        // series key → every group carrying it
    struct Cx { std::string ids, series, repl; char b1; int64_t t; std::string by; };
    std::vector<Cx> cxs;
    FILE* f = std::fopen(idxPath.c_str(), "r");
    if (f) {
        char buf[8192];
        std::string cur;
        std::unordered_set<uint64_t> seen;
        while (std::fgets(buf, sizeof buf, f)) {
            cur += buf;
            if (cur.back() != '\n') { if (cur.size() > 65536) cur.clear(); continue; }
            cur.pop_back();
            Entry e;
            if (!parse(cur, e)) { cur.clear(); continue; }
            cur.clear();
            const uint64_t nh = nameHash(e.file);
            if (!liveNames.count(nh) || !seen.insert(nh).second) continue;   // ★ evicted from the gallery, or a duplicate line
            if (e.t < since || e.id.empty()) continue;
            q.receptions++;
            if (!e.cIds.empty() || !e.cSeries.empty() || !e.rSeries.empty()) cxs.push_back({e.cIds, e.cSeries, e.rSeries, e.b1, e.t, e.id});
            if (e.cls == "cancel") continue;   // ★ a cancellation is applied below, never a marker
            // ★ the same warning: same id, same text — or a garbled copy (same id, same positions) within 30 days
            size_t gi = SIZE_MAX;
            auto range = byId.equal_range(e.id);
            for (auto it = range.first; it != range.second; ++it) {
                Group& g = gs[it->second];
                if (e.t - g.last > kReuseSec) continue;
                if (g.hash == e.hash || samePts(g.pts, e.pts)) { gi = it->second; break; }
            }
            if (gi == SIZE_MAX) {
                if (gs.size() >= kMaxGroups) continue;
                Group g; g.id = e.id; g.b1 = e.b1; g.b2 = e.b2; g.khz = e.khz; g.first = e.t; g.hash = e.hash; g.cls = e.cls;
                gs.push_back(g); gi = gs.size() - 1; byId.insert({e.id, gi});
            }
            Group& g = gs[gi];
            g.n++; g.first = std::min(g.first, e.t); g.last = std::max(g.last, e.t);
            // ★★ v6 LATEST WINS (Stuart: "the icons are based on the latest received message"): what is DRAWN — the class,
            //    the points, the shape, the named list, the unlit mark — is the NEWEST copy's. The text shown is the newest
            //    copy's too, unless it is garbled: then the copy with the fewest lost characters.
            if (e.t >= g.newest) {
                g.newest = e.t; g.newestLost = e.lost;
                g.pts = e.pts; g.cls = e.cls; g.unlit = e.unlit; g.shape = e.shape; g.items = e.items; g.hit = e.hit; g.lang = e.lang; g.hash = e.hash;
                if (!seriesKey(e.series).empty() || g.series.empty()) g.series = e.series;
                if (e.lost == 0 || e.lost <= g.lost) { g.lost = e.lost; g.best = e.file; }
            } else if (e.lost < g.lost && g.newestLost > 0) { g.lost = e.lost; g.best = e.file; }   // ★ a cleaner older copy's text
            if (e.issued && (!g.issued || e.issued < g.issued)) g.issued = e.issued;
            if (e.cancelAt) g.cancelAt = e.cancelAt;
            if (e.validTo) g.validTo = std::max(g.validTo, e.validTo);
            g.ufn = g.ufn || e.ufn;
        }
        std::fclose(f);
    }
    for (size_t i = 0; i < gs.size(); ++i) { const std::string k = seriesKey(gs[i].series); if (!k.empty()) bySeries.insert({k, i}); }   // ★ the authority's number, whoever relayed it
    // ★ cancellations: by NAVTEX id (named in brackets), or by the series number of the same station's earlier message
    for (const auto& c : cxs) {
        size_t p = 0;
        while (p < c.ids.size()) {
            const size_t sp = c.ids.find(' ', p); const std::string id = c.ids.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
            auto range = byId.equal_range(id);
            for (auto it = range.first; it != range.second; ++it) if (gs[it->second].last <= c.t && gs[it->second].cancelledBy.empty()) gs[it->second].cancelledBy = c.by;
            if (sp == std::string::npos) break; p = sp + 1;
        }
        // ★ "WZ 476/26" with its word (a bare number names no authority: skipped). v6: every group carrying it, not just one.
        for (int pass = 0; pass < 2; ++pass) {
            const std::string& list = pass ? c.repl : c.series;
            p = 0;
            while (p < list.size()) {
                const size_t sp = list.find(';', p); const std::string s = list.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
                const std::string k = seriesKey(s);
                auto range = k.empty() ? std::make_pair(bySeries.end(), bySeries.end()) : bySeries.equal_range(k);
                for (auto it = range.first; it != range.second; ++it) {
                    Group& g = gs[it->second];
                    if (g.id == c.by || g.first > c.t || !g.cancelledBy.empty() || !g.supersededBy.empty()) continue;
                    if (pass) g.supersededBy = c.by; else if (g.last <= c.t) g.cancelledBy = c.by;
                }
                if (sp == std::string::npos) break; p = sp + 1;
            }
        }
    }
    // ★★ v6 RE-ISSUED: the same authority's number heard again with a DIFFERENT text (a new id, or a changed position) —
    //    only the newest version is drawn. Same station letter, or (another station relaying it) a position within 100 km.
    for (auto it = bySeries.begin(); it != bySeries.end();) {
        auto range = bySeries.equal_range(it->first);
        std::vector<size_t> v; for (auto j = range.first; j != range.second; ++j) v.push_back(j->second);
        it = range.second;
        if (v.size() < 2) continue;
        std::sort(v.begin(), v.end(), [&](size_t a, size_t b) { return gs[a].newest > gs[b].newest; });
        for (size_t k = 1; k < v.size(); ++k) {
            Group& old = gs[v[k]];
            if (!old.supersededBy.empty()) continue;
            for (size_t j = 0; j < k; ++j) {
                const Group& nw = gs[v[j]];
                bool near = nw.b1 == old.b1;
                for (size_t a = 0; !near && a < nw.pts.size(); ++a) for (size_t b2 = 0; !near && b2 < old.pts.size(); ++b2)
                    near = navtex::km(nw.pts[a].first, nw.pts[a].second, old.pts[b2].first, old.pts[b2].second) <= 100;
                if (near && nw.newest > old.newest) { old.supersededBy = nw.id; break; }
            }
        }
    }
    q.groups = gs.size();
    std::vector<size_t> withPos;
    for (size_t i = 0; i < gs.size(); ++i) if (!gs[i].pts.empty() && gs[i].cls != "met") {
        withPos.push_back(i); q.byClass[gs[i].cls]++;
        const Group& g = gs[i];
        if (!g.cancelledBy.empty() || (g.cancelAt && g.cancelAt <= now)) q.cancelled++;
        else if (!g.supersededBy.empty()) q.superseded++;
        else if (g.validTo ? now > g.validTo : g.ufn ? now - g.last > kReuseSec : now - g.last > kExpireSec) q.expired++;
    }
    q.withPos = withPos.size();
    std::sort(withPos.begin(), withPos.end(), [&](size_t a, size_t b) { return gs[a].last > gs[b].last; });
    q.total = withPos.size();
    for (size_t k = 0; k < withPos.size() && k < kMaxMarkers; ++k) q.shown.push_back(gs[withPos[k]]);
    return q;
}
/** ★ `placed`: the stations map's placement of each message (navmap::scan's Placed) — the card's station and decode
 *  quality. Absent = neither sent. */
inline std::string toJson(const Query& q, int64_t now, const SyncResult& s, const std::unordered_map<uint64_t, navmap::Placed>* placed = nullptr) {
    char b[420];
    std::snprintf(b, sizeof b, "{\"now\":%lld,\"total\":%zu,\"shown\":%zu,\"receptions\":%zu,\"warnings\":%zu,\"live\":%zu,\"indexed\":%zu,\"pending\":%zu,\"cancelledN\":%zu,\"supersededN\":%zu,\"expiredN\":%zu,\"classes\":{",
                  (long long)now, q.total, q.shown.size(), q.receptions, q.groups, s.live, s.indexed, s.pending, q.cancelled, q.superseded, q.expired);
    std::string o = b;
    bool f1 = true; for (auto& c : q.byClass) { if (!f1) o += ','; f1 = false; o += "\"" + c.first + "\":" + std::to_string(c.second); }
    o += "},\"warnings\":[";
    for (size_t i = 0; i < q.shown.size(); ++i) {
        const Group& g = q.shown[i];
        if (i) o += ',';
        const bool cancelled = !g.cancelledBy.empty() || (g.cancelAt && g.cancelAt <= now);
        // ★ its own window when it gave one; "until further notice" 30 days unheard; else 72 h unheard
        const bool expired = cancelled || !g.supersededBy.empty() || (g.validTo ? now > g.validTo : g.ufn ? now - g.last > kReuseSec : now - g.last > kExpireSec);
        std::snprintf(b, sizeof b, "{\"id\":\"%s\",\"khz\":%g,\"first\":%lld,\"last\":%lld,\"n\":%u,\"lost\":%d,\"unlit\":%d,\"expired\":%d,\"cls\":\"",
                      navmap::jesc(g.id).c_str(), g.khz, (long long)g.first, (long long)g.last, g.n, g.lost, g.unlit ? 1 : 0, expired ? 1 : 0);
        o += b; o += navmap::jesc(g.cls) + "\",\"f\":\"" + navmap::jesc(g.best) + "\",\"series\":\"" + navmap::jesc(g.series) + "\"";
        if (!g.cancelledBy.empty()) o += ",\"cancelledBy\":\"" + navmap::jesc(g.cancelledBy) + "\"";
        if (g.cancelAt) { std::snprintf(b, sizeof b, ",\"cancelAt\":%lld", (long long)g.cancelAt); o += b; }
        if (g.validTo) { std::snprintf(b, sizeof b, ",\"validTo\":%lld", (long long)g.validTo); o += b; }
        if (g.ufn) o += ",\"ufn\":1";
        if (!g.hit.empty()) o += ",\"hit\":\"" + navmap::jesc(g.hit) + "\",\"lang\":\"" + navmap::jesc(g.lang) + "\"";
        if (g.shape) { o += ",\"shape\":\""; o += g.shape == 'a' ? "area" : "route"; o += "\""; }
        if (g.issued) { std::snprintf(b, sizeof b, ",\"issued\":%lld", (long long)g.issued); o += b; }
        if (!g.cancelledBy.empty() || (g.cancelAt && g.cancelAt <= now)) o += ",\"cancelled\":1";
        if (!g.supersededBy.empty()) o += ",\"supersededBy\":\"" + navmap::jesc(g.supersededBy) + "\"";
        if (!g.items.empty() && g.items.size() == g.pts.size()) {   // ★ v6: a named list — one marker an item
            o += ",\"items\":[";
            for (size_t k = 0; k < g.items.size(); ++k) o += std::string(k ? "," : "") + "{\"n\":\"" + navmap::jesc(g.items[k].name) + "\",\"d\":\"" + navmap::jesc(g.items[k].defect) + "\",\"u\":" + (g.items[k].unlit ? "1" : "0") + "}";
            o += "]";
        }
        if (placed) {
            auto p = placed->find(navmap::fileHash(g.best));
            if (p != placed->end()) {
                if (!p->second.name.empty()) o += ",\"st\":\"" + navmap::jesc(p->second.name) + "\",\"sc\":\"" + navmap::jesc(p->second.conf) + "\",\"iso\":\"" + navmap::jesc(p->second.iso) + "\"";
                if (!p->second.q.empty()) o += ",\"q\":\"" + navmap::jesc(p->second.q) + "\"";
            }
        }
        o += ",\"pts\":[";
        for (size_t k = 0; k < g.pts.size(); ++k) { std::snprintf(b, sizeof b, "%s[%.4f,%.4f]", k ? "," : "", g.pts[k].first, g.pts[k].second); o += b; }
        o += "]}";
    }
    return o + "]}";
}

} // namespace navwarn
} // namespace vibe
