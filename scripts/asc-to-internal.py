#!/usr/bin/env python3
"""Wait for Xcode Cloud runs to finish, then put each new build into INTERNAL TestFlight.

    python3 scripts/asc-to-internal.py ios=<runId> jr=<runId>

★★★ WHY THIS IS A SCRIPT AND NOT A SEQUENCE OF CALLS BY HAND. Three separate things each take
    minutes and each has a trap that makes a wrong answer look like a right one:
      1. the Cloud run finishing,
      2. App Store Connect PROCESSING the upload (a build is not attachable until it is),
      3. export compliance, which silently withholds a build from testers if unanswered.
    Doing it unattended means each of those has to be waited for, not assumed.

★★★ TWO GROUPS SHARE ONE NAME. VibeSDR has "VibeSDR Test" TWICE — one internal, one EXTERNAL with
    a public link. Attaching to the wrong one publishes a build to the internet instead of to the
    owner's own devices. So the group is chosen by `isInternalGroup`, never by name.

★★ AND THE BUILD NUMBER LIES. The Cloud workflow numbers its own builds (CURRENT_PROJECT_VERSION
   is ignored), `/apps/{id}/builds` refuses `sort`, and the page it returns is NOT ordered — a
   limit=10 peek once read as "the numbers only reach 18" when they reached 80+. So: page fully,
   and pick by `uploadedDate`, never by number.
"""
import sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asc

APPS = {"ios": "6786344049", "jr": "6795507029"}
# Chosen by isInternalGroup at runtime; these are only the expected answers, for the log.
EXPECT = {"ios": "VibeSDR Test (internal)", "jr": "Internal Testing"}


def log(*a):
    print(*a, flush=True)


def internal_group(app_id):
    groups = asc.call(f"apps/{app_id}/betaGroups?limit=50")["data"]
    internal = [g for g in groups if g["attributes"].get("isInternalGroup")]
    if not internal:
        raise SystemExit(f"no internal group for app {app_id}")
    if len(internal) > 1:
        # ★ Refuse rather than guess: picking the wrong one is not recoverable by re-running.
        raise SystemExit("more than one internal group — refusing to guess: "
                         + ", ".join(g["attributes"]["name"] for g in internal))
    return internal[0]


def newest_build(app_id):
    builds = asc.all_pages(f"apps/{app_id}/builds?limit=200")
    dated = [b for b in builds if b["attributes"].get("uploadedDate")]
    if not dated:
        return None
    return max(dated, key=lambda b: b["attributes"]["uploadedDate"])


def wait_run(kind, run_id):
    while True:
        a = asc.call(f"ciBuildRuns/{run_id}")["data"]["attributes"]
        comp = a.get("completionStatus")
        if comp:
            log(f"[{kind}] run finished: {comp}")
            return comp
        log(f"[{kind}] {a.get('executionProgress')}…")
        time.sleep(30)


def wait_processed(kind, app_id, after_iso):
    """A build is not attachable until ASC has processed it."""
    for _ in range(80):
        b = newest_build(app_id)
        if b and b["attributes"]["uploadedDate"] > after_iso:
            state = b["attributes"].get("processingState")
            log(f"[{kind}] build {b['attributes'].get('version')} — {state}")
            if state == "VALID":
                return b
            if state in ("FAILED", "INVALID"):
                raise SystemExit(f"[{kind}] build {state}")
        time.sleep(30)
    raise SystemExit(f"[{kind}] timed out waiting for a processed build")


def ensure_compliance(kind, build):
    """★★ AN UNANSWERED EXPORT-COMPLIANCE QUESTION WITHHOLDS THE BUILD FROM TESTERS — quietly. It
    is attached, it is valid, and it simply never installs. Answer it if ASC has not already."""
    if build["attributes"].get("usesNonExemptEncryption") is None:
        asc.call(f"builds/{build['id']}", "PATCH",
                 {"data": {"type": "builds", "id": build["id"],
                           "attributes": {"usesNonExemptEncryption": False}}})
        log(f"[{kind}] export compliance answered (no non-exempt encryption)")


def attach(kind, group, build):
    asc.call(f"betaGroups/{group['id']}/relationships/builds", "POST",
             {"data": [{"type": "builds", "id": build["id"]}]})
    log(f"[{kind}] ✅ build {build['attributes'].get('version')} → "
        f"{group['attributes']['name']} (internal)")


def main():
    runs = dict(kv.split("=", 1) for kv in sys.argv[1:])
    if not runs:
        raise SystemExit(__doc__)
    # ★ Recorded BEFORE the wait, so "newer than this" cannot accidentally match a build that was
    #   already there when we started.
    start = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(time.time() - 3600))
    results = {}
    for kind, run_id in runs.items():
        app_id = APPS[kind]
        try:
            comp = wait_run(kind, run_id)
            if comp != "SUCCEEDED":
                results[kind] = f"run {comp}"
                continue
            group = internal_group(app_id)
            build = wait_processed(kind, app_id, start)
            ensure_compliance(kind, build)
            attach(kind, group, build)
            results[kind] = f"build {build['attributes'].get('version')} in internal testing"
        except SystemExit as e:
            results[kind] = f"FAILED: {e}"
            log(f"[{kind}] {e}")
    log("\n=== RESULT ===")
    for k, v in results.items():
        log(f"  {k}: {v}")


if __name__ == "__main__":
    main()
