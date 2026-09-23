import Foundation
import CryptoKit

/// A multi-radio VibeServer (V3), from the watch's side.
///
/// ★★★ A V3 SERVER IS A FRONT DOOR THAT OWNS NO RADIO. Every radio lives behind `/r/<id>/…`, so a
///     client that asks the door for `/ws/user-spectrum` asks for a radio it does not have. The
///     door answers 503, which as a WebSocket handshake reaches us as a bare close with nothing in
///     it — indistinguishable from "server down". Jr had no idea such a server existed.
///
/// ★★ Jr is a SEPARATE APP from the phone (standalone, spike/WristSDR), so this is a second
///    implementation of the same idea rather than shared code — deliberately, and the reason the
///    field names here match the server's exactly: two implementations that disagree about the
///    wire are two bugs waiting, so neither side renames anything.
struct VibeRadio: Identifiable, Hashable {
    let id: String
    let label: String
    let driver: String
    /// Configured listener cap: 1 is a single-user radio, more is shared.
    let users: Int
    /// A locked-range profile — listeners tune inside a captured window, not the radio.
    let locked: Bool
    let restricted: Bool
    let centreHz: Double
    let mode: String
    /// ★ The lowest client protocol with controls for this radio (BRIEF-v11 §7); above
    ///   JrVersion.proto the picker greys it out. Absent on an older server = 0.
    var minProto: Int = 0
    var unsupported: Bool { minProto > JrVersion.proto }
    /// ★★★ THIS RADIO HAS A PIN OF ITS OWN. Not to be confused with `locked` three fields up:
    ///     that one means a FIXED TUNING CENTRE (a captured window you listen inside) and has
    ///     nothing whatever to do with credentials. Two booleans one word apart is exactly the
    ///     shape of "ONE RULE, TWO READERS" that keeps costing days here, so they are named after
    ///     the server's own fields and never conflated: `locked` = a dial you cannot move,
    ///     `pinLocked` = a door you cannot open.
    /// ★ Absent on every older server, which means "no per-radio PIN" — the front door's own PIN
    ///   (if any) is then the only credential, exactly as before.
    var pinLocked: Bool = false

    /// What this radio is FOR, short enough for a 41 mm screen.
    ///
    /// ★ The watch has room for about four words. "Shared" is the useful one, not the exact cap:
    ///   what decides whether you tap it is whether arriving means waiting.
    var summary: String {
        var bits: [String] = []
        if centreHz > 0 {
            let mhz = centreHz / 1_000_000
            bits.append(String(format: mhz >= 100 ? "%.1f MHz" : "%.3f MHz", mhz)
                        + (mode.isEmpty ? "" : " " + mode.uppercased()))
        }
        bits.append(users > 1 ? "shared" : "1 at a time")
        if locked { bits.append("fixed") }
        return bits.joined(separator: " · ")
    }
}

struct VibeFrontDoor {
    let name: String
    let radios: [VibeRadio]
}

enum FrontDoor {
    /// Ask a server what is behind it. `nil` means NOT a front door — an ordinary single-radio
    /// VibeServer, a Kiwi, an OpenWebRX, anything at all.
    ///
    /// ★★ Callers must read nil as "connect exactly as you always have". That is the overwhelming
    ///    majority of servers, and treating an unknown answer as a door would break every one.
    static func probe(host: String, tls: Bool, timeout: TimeInterval = 4) async -> VibeFrontDoor? {
        let scheme = tls ? "https" : "http"
        guard let url = URL(string: "\(scheme)://\(host)/vibeserver/radios?proto=\(JrVersion.proto)") else { return nil }
        var req = URLRequest(url: url)
        req.timeoutInterval = timeout
        req.cachePolicy = .reloadIgnoringLocalCacheData
        guard let (data, resp) = try? await URLSession.shared.data(for: req),
              let http = resp as? HTTPURLResponse, http.statusCode == 200,
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return nil }

        // ★★ `frontDoor` is ABSENT on a radio and on every older server, so a missing key must read
        //    as "ordinary receiver". The endpoint alone cannot tell them apart — a single-radio V3
        //    answers it too, describing only itself.
        guard (j["frontDoor"] as? Bool) == true,
              let rows = j["radios"] as? [[String: Any]] else { return nil }

        let radios: [VibeRadio] = rows.compactMap { r in
            guard let id = r["id"] as? String, !id.isEmpty else { return nil }
            return VibeRadio(
                id: id,
                label: (r["label"] as? String) ?? (r["driver"] as? String) ?? "Radio",
                driver: (r["driver"] as? String) ?? "",
                users: (r["users"] as? Int) ?? 1,
                locked: (r["locked"] as? Bool) ?? false,
                restricted: (r["restricted"] as? Bool) ?? false,
                centreHz: (r["centreHz"] as? Double) ?? Double((r["centreHz"] as? Int) ?? 0),
                mode: (r["mode"] as? String) ?? "",
                minProto: (r["minProto"] as? Int) ?? 0,
                pinLocked: (r["pinLocked"] as? Bool) ?? false)
        }
        guard !radios.isEmpty else { return nil }   // a door with nothing behind it is not a choice
        return VibeFrontDoor(name: (j["name"] as? String) ?? "VibeServer", radios: radios)
    }

    // ── Per-radio PIN ────────────────────────────────────────────────────────

    /// The VibeServer PIN handshake's arithmetic, in ONE place.
    ///
    /// ★★★ TWO IMPLEMENTATIONS THAT DISAGREE ABOUT THE WIRE ARE TWO BUGS WAITING — the note at the
    ///     top of this file, applied to ourselves. `UberClient.resolveVibeAuth` computed this
    ///     inline and is the only thing that ever had to; the radio-PIN check is the second
    ///     reader, so the sum moved here and `resolveVibeAuth` now calls it. Change the key, the
    ///     message or the case of the hex and BOTH callers change together.
    /// ★ Key = the PIN bytes; message = the nonce's ASCII-hex STRING as sent (never decoded);
    ///   lowercase hex out.
    static func authSuffix(pin: String, nonce: String) -> String {
        let mac = HMAC<SHA256>.authenticationCode(for: Data(nonce.utf8),
                                                  using: SymmetricKey(data: Data(pin.utf8)))
        let token = mac.map { String(format: "%02x", $0) }.joined()
        return "&vs_nonce=\(nonce)&vs_auth=\(token)"
    }

    /// Is `pin` the credential for this radio? 200 = yes, anything else = no.
    ///
    /// ★★★ DO NOT OPEN A STREAM TO TEST A PIN. The obvious check — connect and see whether it
    ///     works — takes a LISTENER SLOT on a one-at-a-time radio, so a wrong guess would evict
    ///     nobody but would certainly be counted, and on a shared dial it would drag the frequency
    ///     about on the way past. `/vibeserver/auth/verify` claims nothing and costs one small
    ///     request; it exists precisely so a client can ask before committing.
    /// ★★ ONE FAILURE STRING FOR EVERY FAILURE (the caller's, not ours): a wrong PIN and a radio
    ///    that is not there must read identically, or the reply becomes an oracle for which radio
    ///    ids exist and how close a guess was. So this returns a bare Bool — there is deliberately
    ///    nothing here to tell them apart with.
    /// ★ The nonce is fetched from the RADIO's own `/r/<id>/vibeserver/auth`, because a per-radio
    ///   PIN is checked by that radio: asking the door for the nonce would sign against the wrong
    ///   challenge and a correct PIN would read as wrong.
    static func verifyPin(host: String, tls: Bool, radioId: String, pin: String,
                          timeout: TimeInterval = 6) async -> Bool {
        guard !host.isEmpty, !pin.isEmpty else { return false }
        let scheme = tls ? "https" : "http"
        let base = "\(scheme)://\(host)/r/\(radioId)"
        guard let nonceURL = URL(string: "\(base)/vibeserver/auth") else { return false }
        var nreq = URLRequest(url: nonceURL)
        nreq.timeoutInterval = timeout
        nreq.cachePolicy = .reloadIgnoringLocalCacheData
        guard let (data, resp) = try? await URLSession.shared.data(for: nreq),
              (resp as? HTTPURLResponse)?.statusCode == 200,
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return false }
        // ★ A radio that says it wants no PIN is already open — treat that as a pass rather than
        //   signing an empty challenge, which the server could only refuse.
        if !((j["required"] as? Bool) ?? false) { return true }
        // ★ No nonce with `required` true means LOCKED OUT (too many tries). That is not a pass,
        //   and it is emphatically not a reason to try again — see resolveVibeAuth.
        guard let nonce = j["nonce"] as? String, !nonce.isEmpty else { return false }
        let suffix = authSuffix(pin: pin, nonce: nonce)
        // ★ `authSuffix` is built to be appended to an existing query, so it leads with "&";
        //   here it is the whole query, hence the drop.
        guard let vURL = URL(string: "\(base)/vibeserver/auth/verify?\(suffix.dropFirst())")
        else { return false }
        var vreq = URLRequest(url: vURL)
        vreq.timeoutInterval = timeout
        vreq.cachePolicy = .reloadIgnoringLocalCacheData
        guard let (_, vresp) = try? await URLSession.shared.data(for: vreq) else { return false }
        return (vresp as? HTTPURLResponse)?.statusCode == 200
    }
}
