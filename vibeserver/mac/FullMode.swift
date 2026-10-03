import Foundation
import Security

// ── FULL MODE: the app as a launcher, not as the server ──────────────────────
//
// ★★★ SIMPLE AND FULL ARE TWO DIFFERENT SERVERS, AND THAT IS DELIBERATE.
//
// Simple mode runs the core IN-PROCESS (`vs_start`): one radio, one port, plug it in and press
// Start. That is the product most people want and it must not change — so nothing in this file
// touches it.
//
// Full mode is multi-process by design, exactly as on Linux: a FRONT DOOR that owns no radio and
// holds the public port, with one process per radio behind it. Rather than re-implement that model
// inside a Swift app, the app writes the same config file the Pi reads and starts the SAME binary
// the Pi runs. "Full mode behaves identically to Linux" is then true by construction rather than
// by maintenance — and every fix to the front door arrives on macOS for free.
//
// ★★ THE TWO SETTINGS STORES ARE NOT MERGED, AND MUST NOT BE.
//    Simple's settings live in UserDefaults (@AppStorage); Full's live in the config file the
//    BROWSER edits. Switching mode hides and ignores the other store — it never copies or deletes
//    it — so a user can move between modes without losing the receiver they had set up. A
//    "helpful" sync in either direction is what turns a mode switch into data loss: the browser's
//    carefully built public server flattening the GUI's remembered local one, or the reverse.
//    ★ The admin password and the PIN are the one exception: both modes ask for the same
//      credential, so they are passed through rather than kept twice.

enum FullMode {

    /// ★ NOT /etc. The Linux default path needs root and does not exist on a Mac; the core already
    ///   honours VIBESERVER_CONFIG, so the app points it at the standard per-user location instead
    ///   of the core growing a second notion of where its config lives.
    static var configDirectory: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("VibeServer", isDirectory: true)
    }
    static var configURL: URL { configDirectory.appendingPathComponent("config.json") }

    /// The front-door binary shipped inside the bundle (see build-app.sh).
    /// ★ Looked up rather than hard-coded so the app works wherever the user drags it.
    /// ★★★ "vibeserver-engine", NOT "vibeserver". macOS filesystems are case-insensitive, so a
    ///     binary called `vibeserver` in Contents/MacOS IS `VibeServer` — the app itself. Shipping
    ///     it under that name overwrote the app with the CLI at build time.
    static var binaryURL: URL? {
        Bundle.main.url(forAuxiliaryExecutable: "vibeserver-engine")
            ?? Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/vibeserver-engine")
    }

    /// One radio as the GUI knows it: what the hardware says, plus the owner's decision to serve it.
    struct Radio: Identifiable, Equatable {
        var id: String { serial.isEmpty ? "\(driver)#\(index)" : serial }
        var index: Int
        var driver: String
        var serial: String
        var name: String
        var serve: Bool
    }

    /// Every radio attached right now, straight from the core's own detection.
    /// ★ ALL TICKED BY DEFAULT, as the Linux TUI does: someone who plugged three radios in wants
    ///   three receivers, and making them opt each one in asks a question their hands have already
    ///   answered. `previous` keeps the owner's un-ticks across a rescan.
    static func detect(preserving previous: [Radio] = []) -> [Radio] {
        vs_radios_refresh()
        let n = Int(vs_radio_count())
        return (0..<n).map { i in
            let serial = String(cString: vs_radio_serial(Int32(i)))
            let driver = String(cString: vs_radio_driver(Int32(i)))
            let name   = String(cString: vs_radio_name(Int32(i)))
            let id     = serial.isEmpty ? "\(driver)#\(i)" : serial
            let kept   = previous.first { $0.id == id }
            return Radio(index: i, driver: driver, serial: serial, name: name,
                         serve: kept?.serve ?? true)
        }
    }

    /// Non-zero when two attached radios report the same serial — they cannot then be told apart,
    /// so each one's settings could follow the other. The TUI warns about this and so must a GUI.
    static var serialsCollide: Bool { vs_radio_serials_collide() != 0 }

    // ── Writing the config the front door reads ──────────────────────────────

    /// ★★★ PATCH, NEVER REPLACE. The browser setup page owns almost everything in this file — name,
    ///     location, sharing, limits, per-radio settings — and the GUI owns only three things. If
    ///     Start rewrote the file from what the GUI knows, every press would silently discard
    ///     whatever the owner had configured in the browser. So: read what is there, change only
    ///     our three, write it back.
    /// ★ A radio already in the file keeps its settings when it is re-ticked; `enabled` is the only
    ///   field the GUI touches, which is exactly the TUI's split between "serve this" (enabled) and
    ///   "I have said what it should do" (configured).
    static func writeConfig(radios: [Radio], adminPassword: String, pin: String) throws {
        try FileManager.default.createDirectory(at: configDirectory,
                                                withIntermediateDirectories: true)
        var root: [String: Any] = [:]
        if let data = try? Data(contentsOf: configURL),
           let existing = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            root = existing
        }

        var existingRadios = (root["radios"] as? [[String: Any]]) ?? []

        // Update or append each detected radio, keeping everything the browser set.
        for r in radios {
            let match = existingRadios.firstIndex {
                // ★ Serial is the identity. Fall back to driver+label only for a radio whose
                //   driver reports no serial at all — matching on INDEX would re-point settings at
                //   different hardware the moment something is unplugged.
                if !r.serial.isEmpty { return ($0["serial"] as? String) == r.serial }
                return ($0["driver"] as? String) == r.driver && ($0["serial"] as? String ?? "").isEmpty
            }
            if let i = match {
                existingRadios[i]["enabled"] = r.serve
            } else {
                existingRadios.append([
                    "serial": r.serial, "driver": r.driver, "label": r.name,
                    "enabled": r.serve,
                    // ★ NOT configured. It has not been set up in the browser yet, and both gates
                    //   must be true before it goes on air — otherwise a brand-new radio lands on
                    //   whatever the defaults happen to be.
                    "configured": false,
                ])
            }
        }
        // ★ A radio that is no longer attached is left in the file, NOT removed: unplugging a
        //   dongle for an evening must not throw away how it was set up.
        root["radios"] = existingRadios

        // The three the GUI owns. Everything else in this file belongs to the browser.
        root["adminPass"] = adminPassword
        root["pin"] = pin
        // ★ Full mode IS the configured state as far as the machine is concerned: the front door
        //   must come up and serve the setup page even before any radio has been set up, or there
        //   is nowhere to do the setting up.
        root["configured"] = true

        let data = try JSONSerialization.data(withJSONObject: root,
                                              options: [.prettyPrinted, .sortedKeys])
        // ★ Atomic: a half-written config read by the front door mid-start is a server that comes
        //   up wrong, which is much harder to diagnose than one that fails to come up.
        // ★★★ AND 0600 FROM THE FIRST BYTE (2026-10-03 security pass). This file holds the admin
        //     password and the PIN in clear, and `Data.write(.atomic)` created it 0644 — readable
        //     by every account on the Mac. Same discipline as the core's own save
        //     (vibeserver_config.cpp): temp file created owner-only, then renamed into place, which
        //     also tightens a config.json an earlier build left world-readable.
        try writePrivate(data, to: configURL)
    }

    /// Owner-only atomic write: O_CREAT|O_EXCL|O_NOFOLLOW at mode 0600 into a temp name beside the
    /// target, fsync, rename. A symlink or a leftover file at the temp name makes the open fail
    /// rather than be written through.
    static func writePrivate(_ data: Data, to url: URL) throws {
        let tmp = url.path + ".tmp"
        unlink(tmp)
        let fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        var ok = fchmod(fd, 0o600) == 0
        if ok {
            ok = data.withUnsafeBytes { raw -> Bool in
                guard var p = raw.baseAddress else { return true }   // empty data: nothing to write
                var left = raw.count
                while left > 0 {
                    let n = write(fd, p, left)
                    if n <= 0 { return false }
                    p += n; left -= n
                }
                return true
            }
        }
        if ok { ok = fsync(fd) == 0 }
        let closed = close(fd) == 0
        guard ok, closed, rename(tmp, url.path) == 0 else {
            let e = errno
            unlink(tmp)
            throw POSIXError(POSIXErrorCode(rawValue: e) ?? .EIO)
        }
    }
}

// ── SECRETS IN THE KEYCHAIN, NOT IN UserDefaults (2026-10-03 security pass) ────
//
// ★★★ The admin password and the PIN were @AppStorage — a plain-text plist in ~/Library/Preferences
//     that any process running as the user (and every backup) can read. They now live in the login
//     Keychain as generic passwords, encrypted at rest and readable only by this app without a
//     prompt.
// ★★ ONE-TIME MIGRATION: the first read finds the Keychain empty and the old UserDefaults value
//    present, copies it in, and DELETES the plist copy — but only once the Keychain write has
//    succeeded, so a failure never loses the password the owner set.
// ★ The legacy (file-based) login keychain, NOT kSecUseDataProtectionKeychain: the data-protection
//   keychain needs a keychain-access-groups entitlement and a provisioning profile, which a
//   Developer-ID app built with swiftc does not have (errSecMissingEntitlement). The item's ACL
//   trusts the app's designated requirement, so a Developer-ID update reads it silently; an AD-HOC
//   local build (`codesign --sign -`) has a new identity every build and macOS will ask once.
enum SecretStore {
    private static let service = "com.stuey3d.vibeserver"

    private static func baseQuery(_ account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: account]
    }

    /// The stored value, or nil if there is none (or the Keychain refused).
    static func read(_ account: String) -> String? {
        var q = baseQuery(account)
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess,
              let data = out as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    /// Store `value`; an EMPTY value deletes the item (an empty secret means "none set").
    @discardableResult
    static func write(_ account: String, _ value: String) -> Bool {
        let q = baseQuery(account)
        if value.isEmpty {
            let st = SecItemDelete(q as CFDictionary)
            return st == errSecSuccess || st == errSecItemNotFound
        }
        let data = Data(value.utf8)
        let upd = SecItemUpdate(q as CFDictionary, [kSecValueData as String: data] as CFDictionary)
        if upd == errSecSuccess { return true }
        guard upd == errSecItemNotFound else { return false }
        var add = q
        add[kSecValueData as String] = data
        // ★ Readable only while the Mac is unlocked, and never synced to iCloud or another Mac.
        add[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
        return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
    }

    /// Read `account`, migrating a value left in UserDefaults under `legacyKey` on first use.
    static func load(_ account: String, legacyKey: String) -> String {
        if let v = read(account) {
            // ★ Already migrated; a plist copy can only be a stale leftover — remove it.
            if UserDefaults.standard.object(forKey: legacyKey) != nil {
                UserDefaults.standard.removeObject(forKey: legacyKey)
            }
            return v
        }
        let legacy = UserDefaults.standard.string(forKey: legacyKey) ?? ""
        if legacy.isEmpty {
            UserDefaults.standard.removeObject(forKey: legacyKey)
            return ""
        }
        if write(account, legacy) { UserDefaults.standard.removeObject(forKey: legacyKey) }
        return legacy
    }
}
