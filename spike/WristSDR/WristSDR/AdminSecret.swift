import Foundation
import Security

/**
 * The owner's admin password, remembered per server.
 *
 * ★★★ WHY THIS EXISTS. Typing a password on a 40 mm screen is genuinely unpleasant — scribble or
 *     dictation, one character at a time, for something that must be exact — and the owner of a
 *     receiver may need it every time they pick up the watch (Stuart, 2026-08-13: "so that the next
 *     time a user doesnt have to struggle to enter it on a tiny screen"). A password nobody can
 *     face typing is one they stop using, and then the protections behind it stop being used too.
 *
 * ★★★ THE KEYCHAIN, NOT UserDefaults. The PIN is saved in the favourites list as plain text and
 *     that is defensible — it decides who may LISTEN. This decides who may CHANGE THE RADIO and
 *     lift every limit on the server, so it is stored where the OS encrypts it and where a backup
 *     or a file dump does not carry it away. kSecAttrAccessibleWhenUnlockedThisDeviceOnly: it never
 *     leaves this watch and is unreadable while locked.
 *
 * ★★ PER HOST. One watch may know several receivers, and one shared password across them would be
 *    a credential silently offered to a server it was never meant for.
 * ★ Saving is the OWNER'S CHOICE and reversible — forget() removes it completely.
 */
enum AdminSecret {

  private static let service = "net.vibesdr.jr.admin"

  private static func query(_ host: String) -> [String: Any] {
    [kSecClass as String:       kSecClassGenericPassword,
     kSecAttrService as String: service,
     kSecAttrAccount as String: host]
  }

  /// Remember (or replace) the password for this host.
  static func save(_ password: String, host: String) {
    guard !host.isEmpty else { return }
    guard !password.isEmpty else { forget(host: host); return }
    var q = query(host)
    SecItemDelete(q as CFDictionary)                       // replace, never duplicate
    q[kSecValueData as String] = Data(password.utf8)
    q[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
    SecItemAdd(q as CFDictionary, nil)
  }

  /// The saved password for this host, or "" if there is none.
  static func load(host: String) -> String {
    guard !host.isEmpty else { return "" }
    var q = query(host)
    q[kSecReturnData as String] = true
    q[kSecMatchLimit as String]  = kSecMatchLimitOne
    var out: CFTypeRef?
    guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess,
          let d = out as? Data, let s = String(data: d, encoding: .utf8) else { return "" }
    return s
  }

  static func forget(host: String) {
    guard !host.isEmpty else { return }
    SecItemDelete(query(host) as CFDictionary)
  }

  static func has(host: String) -> Bool { !load(host: host).isEmpty }
}
