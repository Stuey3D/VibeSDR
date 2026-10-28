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

// ★★★ A SEASON EiBi HAS NOT PUBLISHED YET IS NOT AN ERROR (NickB, 2026-10-27: "curl: (22) The
//     requested URL returned error: 404"). Our season switch is a date guess (25 March / 25
//     October); EiBi publishes the new file when it is ready, which can be weeks later. So: ask for
//     this season; if it is not there, use the PREVIOUS season's file, say so in words, and ask for
//     the new one again every few hours until it appears. Same rule as vibeserver/eibi.cpp and
//     src/services/eibiSeason.ts — keep all three in step.
enum EibiStations {
    /// Result of a refresh, so the UI can tell the truth instead of always claiming success.
    /// `note` is a plain sentence when the schedule is not this season's ("" when it is).
    enum Result { case loaded(Int, note: String), failed(String) }

    /// One season's file from the network. `unpublished` = EiBi has no such file yet (404/410).
    enum Fetch { case ok(Data), unpublished, failed(String) }

    /// How long after EiBi said "not yet" before asking again.
    static let retryUnpublished: TimeInterval = 6 * 3600

    /// Fetch (or reuse today's cache), filter to what is on air now, and publish to the core.
    /// Safe to call repeatedly — it no-ops the network when the cache is fresh for today's season.
    static func refresh() async -> Result {
        switch await schedule(now: Date(), fetch: fetchRemote) {
        case .failed(let e):
            // Leave whatever the core already has; the search degrades to bookmarks + band plan.
            return .failed(e)
        case .loaded(let csv, let note):
            let active = activeNow(parse(csv))
            let json = buildJSON(active)
            json.withCString { vs_set_stations($0) }
            return .loaded(active.count, note: note)
        }
    }

    /// Clear the served list (toggle turned off).
    static func clear() { vs_set_stations("") }

    // ── Season ──────────────────────────────────────────────────────────────
    // A = summer (~last Sun Mar → last Sun Oct), B = winter. EiBi labels a season by the year it
    // started, so January to 24 March belong to the previous year's winter.
    static func seasonCode(for now: Date) -> String {
        var cal = Calendar(identifier: .gregorian)
        cal.timeZone = TimeZone(identifier: "UTC")!
        let c = cal.dateComponents([.year, .month, .day], from: now)
        let m = c.month!, day = c.day!, y = c.year!
        let summer: Bool
        if m > 3 && m < 10 { summer = true }
        else if m < 3 || m > 10 { summer = false }
        else if m == 3 { summer = day >= 25 }
        else { summer = day < 25 }               // m == 10
        // ★★ m <= 3, NOT m <= 2: 1–24 March is still the winter that started the previous October
        //    ("<= 2" asked for sked-b27.csv on 1 March 2027 — a file that will never exist).
        let yy = (!summer && m <= 3) ? y - 1 : y
        return (summer ? "a" : "b") + String(format: "%02d", ((yy % 100) + 100) % 100)
    }

    /// b26 → a26, a27 → b26.
    static func previousSeason(_ code: String) -> String {
        let yy = Int(code.dropFirst()) ?? 0
        return code.hasPrefix("b") ? String(format: "a%02d", yy) : String(format: "b%02d", (yy + 99) % 100)
    }

    /// "B26 (winter)" / "A26 (summer)".
    static func seasonLabel(_ code: String) -> String {
        code.uppercased() + (code.hasPrefix("a") ? " (summer)" : " (winter)")
    }

    static func notPublishedNote(current: String, using: String) -> String {
        "EiBi hasn't published the \(seasonLabel(current)) schedule yet — using \(using.uppercased()) until it appears."
    }

    // ── Cache ───────────────────────────────────────────────────────────────
    /// ★ Settable for the test; the app uses ~/Library/Application Support/VibeServer.
    static var cacheDirOverride: URL?
    private static func cacheDir() -> URL {
        if let d = cacheDirOverride { return d }
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("VibeServer", isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base
    }
    private static func cacheFile(_ season: String) -> URL { cacheDir().appendingPathComponent("eibi-\(season).csv") }
    /// "b26 <unix time>": the season EiBi last said it has not published, and when.
    private static func triedFile() -> URL { cacheDir().appendingPathComponent("eibi-tried.txt") }

    private static func cached(_ season: String, freshOn now: Date? = nil) -> String? {
        let url = cacheFile(season)
        if let now {
            guard let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
                  let mod = attrs[.modificationDate] as? Date,
                  Calendar.current.isDate(mod, inSameDayAs: now) else { return nil }
        }
        guard let data = try? Data(contentsOf: url) else { return nil }
        let csv = decodeWin1252(data)
        return parse(csv).isEmpty ? nil : csv
    }

    private static func triedRecently(_ season: String, _ now: Date) -> Bool {
        guard let s = try? String(contentsOf: triedFile(), encoding: .utf8) else { return false }
        let p = s.split(separator: " ")
        guard p.count == 2, String(p[0]) == season, let at = Double(p[1]) else { return false }
        let age = now.timeIntervalSince1970 - at
        return age >= 0 && age < retryUnpublished
    }

    enum Schedule { case loaded(String, note: String), failed(String) }

    /// The season choice and its fallback. ★ Internal (not private) so the test can drive it with a
    /// fake fetcher and clock.
    static func schedule(now: Date, fetch: (String) async -> Fetch) async -> Schedule {
        let cur = seasonCode(for: now), prev = previousSeason(cur)
        let note = notPublishedNote(current: cur, using: prev)
        // Reuse the cache if it is from today — EiBi changes at most once a day, and this keeps a
        // frequently-restarted server (or an offline one) from hammering eibispace.de.
        if let csv = cached(cur, freshOn: now) { return .loaded(csv, note: "") }
        // ★★ EiBi said "not yet" for this season a short while ago: the previous one is the schedule.
        if triedRecently(cur, now), let csv = cached(prev) { return .loaded(csv, note: note) }

        switch await fetch(cur) {
        case .ok(let data) where !parse(decodeWin1252(data)).isEmpty:
            try? data.write(to: cacheFile(cur))
            try? FileManager.default.removeItem(at: triedFile())
            return .loaded(decodeWin1252(data), note: "")
        case .failed(let msg):
            // ★ A REAL failure says so — but a stale cache still beats an empty search.
            if let csv = cached(cur) { return .loaded(csv, note: "") }
            if let csv = cached(prev) { return .loaded(csv, note: triedRecently(cur, now) ? note : "") }
            return .failed("couldn't reach eibispace.de — \(msg)")
        case .ok, .unpublished:
            // ★ Not published (a page with no entries counts too — it must never become the list).
            try? "\(cur) \(Int(now.timeIntervalSince1970))".write(to: triedFile(), atomically: true, encoding: .utf8)
        }

        if let csv = cached(prev, freshOn: now) { return .loaded(csv, note: note) }
        switch await fetch(prev) {
        case .ok(let data) where !parse(decodeWin1252(data)).isEmpty:
            try? data.write(to: cacheFile(prev))
            return .loaded(decodeWin1252(data), note: note)
        case .failed(let msg):
            if let csv = cached(prev) { return .loaded(csv, note: note) }
            return .failed("EiBi hasn't published the \(seasonLabel(cur)) schedule yet, and the \(seasonLabel(prev)) one could not be fetched — couldn't reach eibispace.de — \(msg)")
        case .ok, .unpublished:
            if let csv = cached(prev) { return .loaded(csv, note: note) }
            return .failed("EiBi hasn't published the \(seasonLabel(cur)) schedule yet, and the \(seasonLabel(prev)) file has no entries either")
        }
    }

    private static func fetchRemote(_ season: String) async -> Fetch {
        // ★★ Plain http on purpose for now: https://www.eibispace.de serves an EXPIRED certificate
        //    (checked 2026-10-03), so https would fail every fetch. The parser treats every line as
        //    untrusted. Same note as eibi.cpp — move both to https together once the cert is fixed.
        guard let url = URL(string: "http://www.eibispace.de/dx/sked-\(season).csv") else {
            return .failed("bad URL")
        }
        do {
            let (data, resp) = try await URLSession.shared.data(from: url)
            let status = (resp as? HTTPURLResponse)?.statusCode ?? 0
            if status == 404 || status == 410 { return .unpublished }
            guard status == 200, !data.isEmpty else { return .failed("HTTP \(status)") }
            return .ok(data)
        } catch {
            return .failed(error.localizedDescription)
        }
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
            // ★★★ A BAD LINE MUST NEVER CRASH THE MENU-BAR APP. This file arrives over plain http,
            //     so every field is untrusted: f[5] is read below, so SIX columns are required (five
            //     trapped on index out of range), and Double() accepts "inf", "nan" and "1e400" —
            //     Int() of a non-finite or huge Double is a runtime trap, not an error. Finite and
            //     bounded (1 GHz, far above any shortwave schedule) before the conversion.
            guard f.count >= 6,
                  let khz = Double(f[0].trimmingCharacters(in: .whitespaces)),
                  khz.isFinite, khz > 0, khz <= 1_000_000 else { continue }
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
