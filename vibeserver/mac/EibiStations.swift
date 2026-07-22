// EiBi shortwave-schedule download → the web client's station search (GET /stations).
//
// ★ Why the SERVER does this, not the browser: a browser cannot fetch eibispace.de itself — it
// sends no Access-Control-Allow-Origin, and unlike native code a browser enforces CORS. On the
// phone the app owned this download; a standalone VibeServer has no app, so the host fetches it and
// hands it to the core via vs_set_stations (served same-origin at /stations, so CORS never bites).
//
// A faithful port of src/services/eibi.ts: same season file, same Windows-1252 decode, same
// semicolon CSV, same active-now time/day filter. Flags are omitted here (the ITU→flag table is
// large and the web search works from the itu code and name); everything else matches.
import Foundation

enum EibiStations {
    /// Result of a refresh, so the UI can tell the truth instead of always claiming success.
    enum Result { case loaded(Int), failed(String) }

    /// Fetch (or reuse today's cache), filter to what is on air now, and publish to the core.
    /// Safe to call repeatedly — it no-ops the network when the cache is fresh for today's season.
    static func refresh() async -> Result {
        do {
            let (file, season) = seasonFile()
            let csv = try await csvForSeason(file: file, season: season)
            let active = activeNow(parse(csv))
            let json = buildJSON(active)
            json.withCString { vs_set_stations($0) }
            return .loaded(active.count)
        } catch {
            // Leave whatever the core already has; the search degrades to bookmarks + band plan.
            return .failed(error.localizedDescription)
        }
    }

    /// Clear the served list (toggle turned off).
    static func clear() { vs_set_stations("") }

    // ── Season file ─────────────────────────────────────────────────────────
    // A = summer (~last Sun Mar → last Sun Oct), B = winter. EiBi labels a season by the year it
    // started; Jan/Feb belong to the previous year's winter.
    private static func seasonFile(_ now: Date = Date()) -> (file: String, season: String) {
        var cal = Calendar(identifier: .gregorian)
        cal.timeZone = TimeZone(identifier: "UTC")!
        let c = cal.dateComponents([.year, .month, .day], from: now)
        let m = c.month!, day = c.day!, y = c.year!
        let summer: Bool
        if m > 3 && m < 10 { summer = true }
        else if m < 3 || m > 10 { summer = false }
        else if m == 3 { summer = day >= 25 }
        else { summer = day < 25 }               // m == 10
        var yy = y
        if !summer && m <= 2 { yy = y - 1 }
        let season = (summer ? "a" : "b") + String(String(yy).suffix(2))
        return ("sked-\(season).csv", season)
    }

    // ── CSV: today's cache, else download ───────────────────────────────────
    private static func cacheDir() -> URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("VibeServer", isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base
    }

    private static func csvForSeason(file: String, season: String) async throws -> String {
        let cache = cacheDir().appendingPathComponent("eibi-\(season).csv")
        // Reuse the cache if it is from today — EiBi changes at most once a day, and this keeps a
        // frequently-restarted server (or an offline one) from hammering eibispace.de.
        if let attrs = try? FileManager.default.attributesOfItem(atPath: cache.path),
           let mod = attrs[.modificationDate] as? Date,
           Calendar.current.isDateInToday(mod),
           let data = try? Data(contentsOf: cache) {
            return decodeWin1252(data)
        }
        let url = URL(string: "http://www.eibispace.de/dx/\(file)")!
        let (data, resp) = try await URLSession.shared.data(from: url)
        guard (resp as? HTTPURLResponse)?.statusCode == 200, !data.isEmpty else {
            // Fall back to any stale cache rather than failing outright.
            if let data = try? Data(contentsOf: cache) { return decodeWin1252(data) }
            throw URLError(.badServerResponse)
        }
        try? data.write(to: cache)
        return decodeWin1252(data)
    }

    // EiBi files are Windows-1252. Decoding as UTF-8 turns accented station names into U+FFFD.
    private static let cp1252Hi: [UInt8: UInt32] = [
        0x80: 0x20AC, 0x82: 0x201A, 0x83: 0x0192, 0x84: 0x201E, 0x85: 0x2026, 0x86: 0x2020,
        0x87: 0x2021, 0x88: 0x02C6, 0x89: 0x2030, 0x8A: 0x0160, 0x8B: 0x2039, 0x8C: 0x0152,
        0x8E: 0x017D, 0x91: 0x2018, 0x92: 0x2019, 0x93: 0x201C, 0x94: 0x201D, 0x95: 0x2022,
        0x96: 0x2013, 0x97: 0x2014, 0x98: 0x02DC, 0x99: 0x2122, 0x9A: 0x0161, 0x9B: 0x203A,
        0x9C: 0x0153, 0x9E: 0x017E, 0x9F: 0x0178,
    ]
    private static func decodeWin1252(_ data: Data) -> String {
        var out = String.UnicodeScalarView()
        out.reserveCapacity(data.count)
        for b in data {
            let cp = (b >= 0x80 && b <= 0x9F) ? (cp1252Hi[b] ?? UInt32(b)) : UInt32(b)
            out.append(Unicode.Scalar(cp)!)
        }
        return String(out)
    }

    // ── Parse + active-now filter (columns: kHz;Time;Days;ITU;Station;Lng;…) ──
    private struct Entry { let freqHz: Int; let time: String; let days: String; let station: String; let lang: String; let itu: String }

    private static func parse(_ csv: String) -> [Entry] {
        var out: [Entry] = []
        // ★ isNewline, NOT ($0 == "\n" || "\r"). EiBi files use CRLF, and Swift merges "\r\n" into a
        // SINGLE grapheme Character that equals neither "\n" nor "\r" — so the naive test split
        // nothing and the whole file came back as one line, parsing to zero stations. isNewline
        // recognises the CRLF grapheme (and LF, CR, and the Unicode line separators).
        for line in csv.split(whereSeparator: { $0.isNewline }) {
            let f = line.split(separator: ";", omittingEmptySubsequences: false).map(String.init)
            guard f.count >= 5, let khz = Double(f[0].trimmingCharacters(in: .whitespaces)), khz > 0 else { continue }
            let station = f[4].trimmingCharacters(in: .whitespaces)
            if station.isEmpty { continue }
            out.append(Entry(freqHz: Int((khz * 1000).rounded()),
                             time: f[1].trimmingCharacters(in: .whitespaces),
                             days: f[2].trimmingCharacters(in: .whitespaces),
                             station: station,
                             lang: f[5].trimmingCharacters(in: .whitespaces),
                             itu: f[3].trimmingCharacters(in: .whitespaces)))
        }
        return out
    }

    private static func timeActive(_ t: String, _ nowMin: Int) -> Bool {
        guard t.count == 9, t[t.index(t.startIndex, offsetBy: 4)] == "-",
              let s = Int(t.prefix(4)), let e = Int(t.suffix(4)) else { return true }
        let sm = (s / 100) * 60 + (s % 100), em = (e / 100) * 60 + (e % 100)
        if sm == em { return true }                       // continuous
        return sm < em ? (nowMin >= sm && nowMin < em) : (nowMin >= sm || nowMin < em)
    }

    private static let dayCode = ["", "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"]
    private static let dayNum: [String: Int] = ["Mo": 1, "Tu": 2, "We": 3, "Th": 4, "Fr": 5, "Sa": 6, "Su": 7]

    private static func dayActive(_ days: String, _ eibiDay: Int) -> Bool {
        let d = days.trimmingCharacters(in: .whitespaces)
        if d.isEmpty { return true }                                             // daily
        if d.allSatisfy({ $0 >= "1" && $0 <= "7" }) { return d.contains(Character(String(eibiDay))) }  // "12345"
        let parts = d.split(separator: "-").map(String.init)
        if parts.count == 2 {
            if let a = Int(parts[0]), let b = Int(parts[1]) {                    // "1-5"
                return a <= b ? (eibiDay >= a && eibiDay <= b) : (eibiDay >= a || eibiDay <= b)
            }
            if let a = dayNum[parts[0]], let b = dayNum[parts[1]] {              // "Mo-Fr"
                return a <= b ? (eibiDay >= a && eibiDay <= b) : (eibiDay >= a || eibiDay <= b)
            }
        }
        let code = dayCode[eibiDay]
        if d.split(separator: ",").allSatisfy({ dayNum[String($0)] != nil }) { return d.contains(code) }
        return true                                                             // dates / irr / alt → don't hide
    }

    private static func activeNow(_ entries: [Entry]) -> [Entry] {
        var cal = Calendar(identifier: .gregorian)
        cal.timeZone = TimeZone(identifier: "UTC")!
        let now = Date()
        let c = cal.dateComponents([.hour, .minute, .weekday], from: now)
        let nowMin = c.hour! * 60 + c.minute!
        let eibiDay = c.weekday! == 1 ? 7 : c.weekday! - 1   // Calendar: Sun=1; EiBi: Mon=1..Sun=7
        return entries.filter { timeActive($0.time, nowMin) && dayActive($0.days, eibiDay) }
    }

    // ── JSON in the shape web/client search.ts expects ──────────────────────
    private static func esc(_ s: String) -> String {
        var o = ""
        for ch in s.unicodeScalars {
            if ch == "\"" || ch == "\\" { o.append("\\"); o.append(Character(ch)) }
            else if ch.value >= 0x20 { o.append(Character(ch)) }
        }
        return o
    }
    private static func buildJSON(_ entries: [Entry]) -> String {
        var parts: [String] = []
        parts.reserveCapacity(entries.count)
        for e in entries {
            var s = "{\"name\":\"\(esc(e.station))\",\"frequency\":\(e.freqHz),\"mode\":\"am\",\"group\":\"EiBi\",\"source\":\"eibi\""
            if !e.lang.isEmpty { s += ",\"comment\":\"\(esc(e.lang))\"" }
            if !e.itu.isEmpty  { s += ",\"itu\":\"\(esc(e.itu))\"" }
            s += "}"
            parts.append(s)
        }
        return "[" + parts.joined(separator: ",") + "]"
    }
}
