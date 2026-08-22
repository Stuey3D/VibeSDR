// Listing this server in the public VibeServer directory at vibeserver.vibesdr.net.
//
// ★★★ THE SAME PROTOCOL THE ANDROID APP ALREADY SPEAKS — register, then ping to renew, delist to
//     leave. Deliberately not a second dialect: the directory verifies an address by challenging
//     it (a nonce out, an HMAC back), and the answer is produced by the shim we already link, so
//     there is exactly one implementation of the part that matters for identity.
//
// ★★ WHAT IT PUBLISHES IS READ FROM OUR OWN ENDPOINTS OVER LOOPBACK, not assembled from config.
//    /vibeserver.json and /vibeserver/radios are what a listener sees, so a listing built from
//    them cannot describe a server that does not exist — and the front door already answers for
//    radios owned by other processes, which no amount of config-reading here could do.
#pragma once

#include <string>

namespace vibedir {

/** Where the listing's id and key are kept between restarts. Default /var/lib/vibeserver. */
void setStateDir(const std::string& dir);

struct Settings {
    bool        listed = false;      ///< the owner's switch
    std::string name;                ///< public name; the address is derived from it
    std::string locator;             ///< Maidenhead — where the pin goes
    int         port = 0;            ///< our own port, for the loopback status reads
    long long   shareForSec = -1;    ///< >0 temporary, 0 permanent, -1 leave unchanged (renewal)
    /** ★ An address the owner already has — DDNS, a port forward, a reverse proxy. Empty means
     *  "make one for me", which is the Quick Tunnel path. */
    std::string publicUrl;
};

/** Apply the owner's wishes. Starts, updates or stops the listing as needed; returns immediately
 *  and does the work on its own thread. */
void apply(const Settings& s);

/** Leave the directory now, rather than at the next expiry. */
void stop();

/** What the setup page shows: listed, the address to share, and why not if not. */
std::string statusJson();

/** How this machine names its OS — "Debian 13", "macOS 26.1". Empty when it cannot say. */
std::string platformName();

/** The machine itself — "Raspberry Pi 500", "Mac mini (M4)". Empty when it cannot say. */
std::string hostModel();

}  // namespace vibedir
