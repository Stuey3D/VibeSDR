#!/usr/bin/env python3
"""App Store Connect API helper — trigger Xcode Cloud builds, inspect versions.

  python3 scripts/asc.py builds vibesdr        # latest builds, CORRECTLY SORTED
  python3 scripts/asc.py builds jr
  python3 scripts/asc.py versions vibesdr
  python3 scripts/asc.py trigger ios|tv|jr     # start a Cloud build on main
  python3 scripts/asc.py distribute jr         # put the newest build into internal testing

Needs `pyjwt`. Key/issuer per memory: testflight_setup.md.
"""
import json, os, time, sys, urllib.request, urllib.error

# ★★★ THE IDENTITY LIVES OUTSIDE THE REPO. This is a PUBLIC repo, and the key id and issuer are
#     account identifiers — not secrets on their own (the .p8 is the secret, and that has never
#     been committed) but there is no reason to publish them. ~/.appstoreconnect/config holds them,
#     next to the private key that was always there.
# ★ Fail LOUDLY and say what to write. A missing identity used to be impossible; now it is a
#   first-run step, and "401 Unauthorized" would be a terrible way to discover it.
_CFG = os.path.expanduser("~/.appstoreconnect/config")

def _identity():
    vals = {}
    try:
        with open(_CFG) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                vals[k.strip()] = v.strip().strip('"').strip("'")
    except FileNotFoundError:
        sys.exit(f"!! No App Store Connect identity at {_CFG}\n"
                 f"   Create it with:\n"
                 f"     KEY_ID=<your key id>\n"
                 f"     ISSUER=<your issuer uuid>\n"
                 f"   The .p8 stays at ~/.appstoreconnect/private_keys/AuthKey_<KEY_ID>.p8")
    if not vals.get("KEY_ID") or not vals.get("ISSUER"):
        sys.exit(f"!! {_CFG} is missing KEY_ID or ISSUER")
    return vals["KEY_ID"], vals["ISSUER"]

KEY, ISS = _identity()
P8  = os.path.expanduser(f"~/.appstoreconnect/private_keys/AuthKey_{KEY}.p8")

APPS = {"vibesdr": "6786344049", "jr": "6795507029"}
WORKFLOWS = {
    "ios": "C8BEAD56-2735-423A-BF64-6B3406FDE470",   # VibeSDR — App Store Release
    "tv":  "6375ca31-bf23-4276-ac27-ce214234a821",   # VibeSDR — Apple TV Release (parked)
    "jr":  "65A79E1C-8815-4981-888C-679E44828E7B",   # WristSDRStub — Default
}
MAIN_REF = "a238c176-4830-4898-aece-244c4c614ffb"    # scmGitReferences id for `main`

_tok = None
_tok_exp = 0
def token():
    """A valid bearer token, re-minted before it expires.

    ★★★ IT USED TO BE MINTED ONCE AND CACHED FOR THE LIFE OF THE PROCESS, with a 15-minute expiry —
        fine for a quick query, and wrong for the one job that matters: asc-to-internal.py WAITS
        for a Cloud build, which takes longer than that. So the wait succeeded, the build succeeded,
        and the attach at the end died with NOT_AUTHORIZED — the work was done and the last step
        threw it away (seen on build 96, 2026-08-13). Re-minted with 60 s of headroom, so a request
        that starts just under the wire still lands.
    """
    global _tok, _tok_exp
    now = int(time.time())
    if _tok is None or now >= _tok_exp - 60:
        import jwt
        _tok_exp = now + 900
        _tok = jwt.encode({"iss": ISS, "iat": now, "exp": _tok_exp,
                           "aud": "appstoreconnect-v1"},
                          open(P8).read(), algorithm="ES256",
                          headers={"kid": KEY, "typ": "JWT"})
    return _tok

def call(path, method="GET", body=None):
    url = path if path.startswith("http") else "https://api.appstoreconnect.apple.com/v1/" + path
    req = urllib.request.Request(
        url, data=json.dumps(body).encode() if body else None, method=method,
        headers={"Authorization": "Bearer " + token(), "Content-Type": "application/json"})
    try:
        # PATCH on a relationship returns 204 with an EMPTY body — json.load would blow up on it
        # and make a SUCCESSFUL attach look like a failure.
        raw = urllib.request.urlopen(req).read()
        return json.loads(raw) if raw.strip() else {}
    except urllib.error.HTTPError as e:
        raise SystemExit("HTTP %s\n%s" % (e.code, e.read().decode()[:1200]))

def all_pages(path):
    """★★ /apps/{id}/builds REFUSES ?sort AND RETURNS ITS PAGES UNORDERED.
    A limit=10 peek showed Jr builds 9,13,1,11,16 — which reads as 'the numbers only reach 18'
    when they actually reach 80+. Always page fully and sort here."""
    out, r = [], call(path)
    while True:
        out += r["data"]
        nxt = r.get("links", {}).get("next")
        if not nxt:
            return out
        r = call(nxt)

def builds(app):
    rows = [(int(x["attributes"]["version"]), x["attributes"].get("processingState"),
             (x["attributes"].get("uploadedDate") or "")[:16], x["attributes"].get("expired"))
            for x in all_pages("apps/%s/builds?limit=200" % APPS[app])]
    rows.sort(reverse=True)
    for v, st, up, exp in rows[:12]:
        print("  build %-5s %-10s %s%s" % (v, st, up, "  (expired)" if exp else ""))

def versions(app):
    for x in call("apps/%s/appStoreVersions?limit=10" % APPS[app])["data"]:
        a = x["attributes"]
        # ★★ PRINT THE PLATFORM. Without it this list is genuinely ambiguous: a tvOS version
        #    sitting in PREPARE_FOR_SUBMISSION looks exactly like an iOS one, and the difference
        #    decides whether you can create the next iOS version at all — App Store Connect allows
        #    only ONE editable version per platform. On 2026-08-15 a stale 10.0.2 read as an iOS
        #    release that had never shipped; it is the Apple TV experiment, and iOS's real live
        #    version was 10.0.1 two rows down.
        print("  %-8s %-7s %-24s %s  %s" % (a["versionString"], a.get("platform", "?"),
                                            a["appStoreState"], a.get("releaseType"), x["id"]))

def trigger(which):
    r = call("ciBuildRuns", "POST", {"data": {"type": "ciBuildRuns", "relationships": {
        "workflow": {"data": {"type": "ciWorkflows", "id": WORKFLOWS[which]}},
        "sourceBranchOrTag": {"data": {"type": "scmGitReferences", "id": MAIN_REF}}}}})
    print("started run", r["data"]["attributes"].get("number"), "id", r["data"]["id"])

# ★★★ JR'S WORKFLOW HAS NO TESTFLIGHT POST-ACTION, so its builds stop at
#     READY_FOR_BETA_TESTING and someone has to finish the job. FIVE times out of five
#     (2026-09-02/03) I pushed Jr through by hand and called it a stall; it is not a stall and not
#     a flake, it is a missing step. The evidence is in the RUN, not the workflow:
#         iOS 247 actions:  Archive - iOS          + TestFlight Internal Testing - iOS
#         Jr   80 actions:  Archive - iOS
#     ★★ AND THE API CANNOT ADD IT. `ciWorkflows` models `actions` (ARCHIVE/BUILD/TEST/ANALYZE)
#        and has no representation of Xcode Cloud POST-actions at all — the two workflows are
#        byte-identical through the API apart from scheme and path. Adding the post-action is a
#        one-time change in the Xcode Cloud UI and cannot be scripted from here.
#     ★ So this exists to make the manual step one command rather than a hand-written API call
#       remembered from last time. It stays useful even after the post-action is added: a build
#       that misses distribution for any other reason is fixed the same way.
#  ★ hasAccessToAllBuilds is ALREADY true on that group, and export compliance is answered
#    (usesNonExemptEncryption=false) — both checked, so neither is the cause. Do not chase them.
def distribute(app, version=""):
    rows = sorted(all_pages("apps/%s/builds?limit=200" % APPS[app]),
                  key=lambda x: x["attributes"].get("uploadedDate") or "", reverse=True)
    if version:
        rows = [x for x in rows if x["attributes"]["version"] == version]
        if not rows:
            raise SystemExit("no build %s for %s" % (version, app))
    b = rows[0]
    groups = [g for g in call("apps/%s/betaGroups?limit=20" % APPS[app])["data"]
              if g["attributes"].get("isInternalGroup")]
    if not groups:
        raise SystemExit("no internal group on %s" % app)
    # ★ Named in the output, because two groups sharing a name has bitten us before
    #   (testflight_groups_trap) — the id is what actually identifies it.
    for g in groups:
        call("builds/%s/relationships/betaGroups" % b["id"], "POST",
             {"data": [{"type": "betaGroups", "id": g["id"]}]})
        print("build %s -> %s (%s)" % (b["attributes"]["version"], g["attributes"]["name"], g["id"]))
    st = call("builds/%s/buildBetaDetail" % b["id"])["data"]["attributes"].get("internalBuildState")
    print("internal state now:", st)

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    arg = sys.argv[2] if len(sys.argv) > 2 else ""
    if   cmd == "builds"   and arg in APPS:      builds(arg)
    elif cmd == "versions" and arg in APPS:      versions(arg)
    elif cmd == "trigger"  and arg in WORKFLOWS: trigger(arg)
    elif cmd == "distribute" and arg in APPS:    distribute(arg, sys.argv[3] if len(sys.argv) > 3 else "")
    else: raise SystemExit(__doc__)
