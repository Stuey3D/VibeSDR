import Foundation

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
        try data.write(to: configURL, options: .atomic)
    }
}
