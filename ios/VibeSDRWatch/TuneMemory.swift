import Foundation

/// Per-instance "come back where you left it" for DIRECT connections — the wrist's copy of the
/// phone app's `lsv_last_tune:<baseUrl>` behaviour (`SDRScreen.tsx`), deliberately mirrored:
///
///   - **First visit to a host: adopt what the SERVER sends us.** We are a guest; forcing our own
///     default onto a receiver we have never met lands the spectrum off-band. The phone falls back
///     to `status.frequency`/`status.mode` for exactly this reason.
///   - **After that: restore the last frequency and demodulator for THAT instance.** Per-instance,
///     not global — one remembered frequency across different receivers is meaningless, and on the
///     phone the hardcoded default "landed on the 20m FT8 squeal every launch".
///   - **Validate on the way out, not just on the way in.** Corrupt or out-of-range stored data
///     falls back to the server's tune rather than asserting nonsense.
///   - **Debounced 1s, and never before the restore has run.** Saving while connecting would
///     persist the transient default over the real memory — which is why the phone gates on
///     `lastTuneLoaded`.
///
/// ★ THE ONE THING THE PHONE DOES NOT NEED — POISON PROTECTION.
///   Kiwi refuses connections (badp / too_busy / band restrictions). A remembered frequency is
///   ASSERTED on reconnect, so a tune that got us kicked would be replayed into the same kick
///   forever: a connection loop the user cannot escape from inside the app, because every attempt
///   re-reads the same poison. So a tune is only ever promoted to storage once the connection has
///   PROVEN itself (a frame actually arrived), and a refusal before that purges the entry and
///   drops us back to the server's default.
///
///   The asymmetry is deliberate: writes are earned, deletes are free. Forgetting a good frequency
///   costs one re-tune; remembering a bad one costs the app.
///
/// ★ SCOPE — UberSDR/VibeServer and KiwiSDR ONLY (Stuart, 2026-07-19).
///   OWRX and FM-DX do no frequency changing at all: they open onto whatever the server puts them
///   on and stay there. They must NOT get tune memory — there is no user-chosen frequency to
///   remember, and asserting one would fight the server's own profile. Do not "finish the job" by
///   wiring this into OwrxClient or FmDxClient.
@MainActor
final class TuneMemory {
  /// Namespaced per instance, exactly like the phone's `lsv_last_tune:<baseUrl>`.
  private let key: String
  private let range: ClosedRange<Double>
  private let validMode: (String) -> Bool

  /// Set once the restore has run. Until then `note()` is ignored — see the gating note above.
  private var loaded = false
  /// The connection has delivered something real, so a tune is safe to persist.
  private var proven = false
  private var pending: (f: Double, m: String)?
  private var saveTask: Task<Void, Never>?

  /// - Parameters:
  ///   - host: instance identity. Use the same string the picker keys on, or two entries diverge.
  ///   - range: acceptable frequencies. Network HF SDRs stop at 30 MHz; VHF-capable backends pass
  ///     a wider bound, mirroring the phone's `hiHz` split for local hardware.
  init(host: String, range: ClosedRange<Double> = 1_000...30_000_000,
       validMode: @escaping (String) -> Bool) {
    self.key = "vibe.lasttune.\(host)"
    self.range = range
    self.validMode = validMode
  }

  /// The remembered tune, or nil to adopt whatever the server sends. Marks the restore as done, so
  /// saving may begin.
  func restore() -> (frequency: Double, mode: String?)? {
    loaded = true
    guard let d = UserDefaults.standard.dictionary(forKey: key),
          let f = d["f"] as? Double, range.contains(f) else { return nil }
    let m = d["m"] as? String
    return (f, m.flatMap { validMode($0) ? $0 : nil })
  }

  /// Call when a frame/row actually arrives. Until this happens nothing is written — that is what
  /// makes a kick-inducing frequency impossible to persist.
  func markProven() {
    guard !proven else { return }
    proven = true
    if let p = pending { note(frequency: p.f, mode: p.m) }   // flush what we held back
  }

  /// Record the current tune. Debounced, so spinning the crown writes once at rest rather than on
  /// every detent.
  func note(frequency: Double, mode: String) {
    guard loaded, range.contains(frequency) else { return }
    guard proven else { pending = (frequency, mode); return }
    pending = nil
    saveTask?.cancel()
    saveTask = Task { [key] in
      try? await Task.sleep(nanoseconds: 1_000_000_000)      // 1s, matching the phone
      guard !Task.isCancelled else { return }
      UserDefaults.standard.set(["f": frequency, "m": mode], forKey: key)
    }
  }

  /// ★ The connection was refused. Drop the memory so the next attempt takes the server's default
  ///   instead of replaying whatever we were on into the same refusal.
  func purge() {
    saveTask?.cancel()
    pending = nil
    UserDefaults.standard.removeObject(forKey: key)
  }
}
