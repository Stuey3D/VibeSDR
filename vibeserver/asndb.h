// IP -> ASN (autonomous system) and the network's name.
//
// Separate from geoip.h on purpose: they answer different questions from different data. geoip
// says which COUNTRY a block was allocated to (the registries' own record); this says which
// NETWORK currently announces it (derived from BGP). An address can be allocated to a Dutch
// holder and announced by an American hosting company, and both answers are correct.
#pragma once

#include <cstdint>
#include <string>

namespace asndb {

void setDir(const std::string& dir);

/** Load the parsed cache. False when there is none. */
bool load();

/** Download and rebuild. ~9 MB compressed, ~43 MB of text — never call this on a request. */
bool refresh(std::string& err);

bool stale(int maxAgeDays);

/** Which network announces this address?
 *  @return false when unknown — unrouted space, a private address, or no data yet. The caller
 *          must render that as "unknown", never as AS0. */
bool lookup(const std::string& ip, uint32_t& asn, std::string& name);

int count();
long long updated();

}  // namespace asndb
