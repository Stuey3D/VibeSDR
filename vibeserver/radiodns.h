#pragma once
#include <string>

/**
 * RadioDNS — the broadcaster's OWN station logo, found from what the transmitter tells us.
 *
 * ★★★ WHY THIS EXISTS. Every other logo source guesses from the station's NAME, and the name is
 *     the one thing RDS is bad at: the PS field is eight characters, so "BBC Radio 2" arrives as
 *     "BBC R2" and no database matches it. RadioDNS goes the other way — it looks the station up
 *     by the PI CODE, ECC and frequency, which is exactly what a receiver already knows and what
 *     uniquely identifies a service. The answer comes from the broadcaster, so it is their real
 *     artwork rather than a user-submitted favicon that may be a dead link.
 *     ★ Free, no patents, no licence, no account (radiodns.org) — which is what makes it usable
 *       here at all, unlike the commercial PI databases.
 *
 * ★★ AND IT HAS TO LIVE IN THE SERVER, not the web client. Two things stop a browser doing it:
 *    a page cannot make a raw DNS CNAME query, and the broadcaster's SPI host sends no CORS
 *    headers so the XML cannot be fetched cross-origin either. The daemon has neither problem —
 *    and doing it here fixes it once for EVERY client (browser, phone, watch) instead of three
 *    implementations of the same guessing game.
 *
 * The lookup, per the RadioDNS specification:
 *   1. Build   <freq>.<pi>.<gcc>.fm.radiodns.org
 *      freq = 5 digits in 100 kHz units ("08810" for 88.1 MHz), pi = 4 lowercase hex,
 *      gcc  = the PI's country nibble followed by the ECC ("c" + "e1" = "ce1").
 *   2. Resolve its CNAME — that is the broadcaster's own service host.
 *   3. Fetch  http://<host>/radiodns/spi/3.1/SI.xml  and take the logo from the service entry.
 */
namespace vsradiodns {

/** Look up the logo for one FM service.
 *  @param piHex  the RDS PI as four hex digits ("C202"); case-insensitive.
 *  @param ecc    the Extended Country Code as hex ("E1"); may be empty, in which case the lookup
 *                cannot be made — the country is part of the identity, not decoration.
 *  @param freqHz the tuned frequency in Hz.
 *  @return an https URL, or empty when the station publishes nothing (which is the common case —
 *          most stations are not in RadioDNS, and that must degrade quietly to the name search).
 *  ★ Cached in memory per service, including NEGATIVE results: a station that is not in RadioDNS
 *    must not cost a DNS query and an HTTP fetch every time somebody tunes past it. */
std::string logoFor(const std::string& piHex, const std::string& ecc, double freqHz);

/**
 * The ECC a station's country and PI nibble imply — for the stations that never send one.
 *
 * ★★★ MOST STATIONS DO NOT TRANSMIT AN ECC, AND WITHOUT IT THIS WHOLE FEATURE NEVER FIRES. It
 *     rides in RDS group 1A, which many encoders simply never send: BBC Radio 1 broadcasts 1A
 *     (11% of its groups) and gets its real artwork, while Heart on the same receiver sends only
 *     0A and 2A, so `ecc` was empty, the FQDN could not be built, and the page fell back to a
 *     generic favicon from the name database. Two stations, one receiver, and the only difference
 *     was whether the transmitter volunteered its country (Stuart, 2026-08-14, with both panels
 *     side by side).
 * ★★ A LOOKUP, NOT A GUESS. Given a country and the PI's country nibble, the ECC is whichever
 *    table row puts that country at that nibble — one answer or none. Pass the RECEIVER's own
 *    country: if the station's nibble does not correspond to it, this returns empty rather than
 *    inventing one, which is the same safety the app relies on. A wrong GCC resolves to no
 *    station anyway, which is indistinguishable from "not in RadioDNS".
 * ★ Ambiguity (one country at the same nibble in two rows) returns empty on purpose.
 *
 * @param iso   two-letter country, e.g. "GB"
 * @param piHex the PI as hex; only its first nibble is used
 */
std::string eccForIso(const std::string& iso, const std::string& piHex);

/** Where the cache lives, for the same reason the other data directories are settable. */
void setDir(const std::string& dir);

}  // namespace vsradiodns
