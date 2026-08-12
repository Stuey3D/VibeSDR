import Foundation

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
        guard let url = URL(string: "\(scheme)://\(host)/vibeserver/radios") else { return nil }
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
                mode: (r["mode"] as? String) ?? "")
        }
        guard !radios.isEmpty else { return nil }   // a door with nothing behind it is not a choice
        return VibeFrontDoor(name: (j["name"] as? String) ?? "VibeServer", radios: radios)
    }
}
