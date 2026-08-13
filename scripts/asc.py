#!/usr/bin/env python3
"""App Store Connect API helper — trigger Xcode Cloud builds, inspect versions.

  python3 scripts/asc.py builds vibesdr        # latest builds, CORRECTLY SORTED
  python3 scripts/asc.py builds jr
  python3 scripts/asc.py versions vibesdr
  python3 scripts/asc.py trigger ios|tv|jr     # start a Cloud build on main

Needs `pyjwt`. Key/issuer per memory: testflight_setup.md.
"""
import json, time, sys, urllib.request, urllib.error

KEY = "NG46B3P48N"
ISS = "340c3b5f-a208-4c2f-a68b-4ca12851b769"
P8  = "/Users/stuey3d/.appstoreconnect/private_keys/AuthKey_NG46B3P48N.p8"

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
        print("  %-8s %-24s %s  %s" % (a["versionString"], a["appStoreState"],
                                       a.get("releaseType"), x["id"]))

def trigger(which):
    r = call("ciBuildRuns", "POST", {"data": {"type": "ciBuildRuns", "relationships": {
        "workflow": {"data": {"type": "ciWorkflows", "id": WORKFLOWS[which]}},
        "sourceBranchOrTag": {"data": {"type": "scmGitReferences", "id": MAIN_REF}}}}})
    print("started run", r["data"]["attributes"].get("number"), "id", r["data"]["id"])

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    arg = sys.argv[2] if len(sys.argv) > 2 else ""
    if   cmd == "builds"   and arg in APPS:      builds(arg)
    elif cmd == "versions" and arg in APPS:      versions(arg)
    elif cmd == "trigger"  and arg in WORKFLOWS: trigger(arg)
    else: raise SystemExit(__doc__)
