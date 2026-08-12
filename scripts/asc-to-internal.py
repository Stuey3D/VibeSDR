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

★★★ AND "THE NEWEST BUILD" IS THE WRONG BUILD. With more than one run in flight — which happened
    the very first time this ran, an earlier pair having built the previous version numbers and
    finished first — picking the newest upload would have put 10.0.2 into internal testing while
    reporting success. The build is matched to its RUN instead: the Cloud workflow numbers its own
    builds and the upload arrives as the ciBuildRun NUMBER (CURRENT_PROJECT_VERSION is ignored).
★ `/apps/{id}/builds` refuses `sort` and returns its pages UNORDERED, so it is paged fully and
  filtered here — a limit=10 peek once read as "the numbers only reach 18" when they reached 80+.
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


def wait_run(kind, run_id):
    while True:
        a = asc.call(f"ciBuildRuns/{run_id}")["data"]["attributes"]
        comp = a.get("completionStatus")
        if comp:
            log(f"[{kind}] run {a.get('number')} finished: {comp}")
            return comp, a.get("number")
        log(f"[{kind}] {a.get('executionProgress')}…")
        time.sleep(30)


def wait_processed(kind, app_id, run_number):
    """The build produced by THIS run, once ASC has processed it.

    ★★★ MATCHED TO THE RUN, NOT "THE NEWEST". Taking the newest upload is wrong the moment there
        is more than one run in flight — and there was: an earlier pair (89/21) built the previous
        version numbers and finished first, so "newest" would cheerfully have put 10.0.2 into
        internal testing while reporting success. The link is exact and already documented: THE
        CLOUD WORKFLOW NUMBERS ITS OWN BUILDS AND THE UPLOAD ARRIVES AS THE ciBuildRun NUMBER
        (CURRENT_PROJECT_VERSION is ignored — a 85→86 bump once arrived as 20, matching its run).
    ★ So `version` is compared as a string against the run number, and nothing else is accepted."""
    want = str(run_number)
    for _ in range(80):
        builds = asc.all_pages(f"apps/{app_id}/builds?limit=200")
        mine = [b for b in builds if str(b["attributes"].get("version")) == want]
        if mine:
            b = mine[0]
            state = b["attributes"].get("processingState")
            log(f"[{kind}] build {want} — {state}")
            if state == "VALID":
                return b
            if state in ("FAILED", "INVALID"):
                raise SystemExit(f"[{kind}] build {want} {state}")
        else:
            log(f"[{kind}] waiting for build {want} to appear…")
        time.sleep(30)
    raise SystemExit(f"[{kind}] timed out waiting for build {want}")


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
    results = {}
    for kind, run_id in runs.items():
        app_id = APPS[kind]
        try:
            comp, number = wait_run(kind, run_id)
            if comp != "SUCCEEDED":
                results[kind] = f"run {comp}"
                continue
            group = internal_group(app_id)
            build = wait_processed(kind, app_id, number)
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
