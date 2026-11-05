// handheld.h — `vibeserver --handheld`: the R36S handheld's engine supervisor (docs/v12/R36S-HANDHELD-IMAGE.md §0.1-§0.2).
//
// ★★★ "A PROPER PORTABLE RADIO JUST POWERED BY VIBESDR" (Stuart, 2026-11-05). One owner, one screen, one radio at a
//     time, any of six drivers, swapped by pulling one out and plugging another in. The supervisor is what makes that
//     an appliance: it owns 127.0.0.1:48000, watches the USB bus, starts ONE radio process for whatever is plugged in
//     (the same `vibeserver` radio process every server runs, in its handheld profile), hands each connection to it
//     whole (SCM_RIGHTS, as the front door does — nothing proxied), and answers for itself when there is no radio:
//     the page, "Please connect an SDR", the device controls, the card.
// ★★ WHY A SUPERVISOR AND NOT THE RADIO PROCESS ITSELF: the shim is one radio, one driver, one process — switching an
//    Airspy for an RTL inside it would be a rewrite of its lifecycle. A process per radio is the shape the server
//    already trusts (a crash domain per radio, the device released by the kernel when it exits), and the page must keep
//    loading, and the volume overlay keep working, while no radio is plugged in at all.
// ★ Everything the owner's memory needs lives HERE, not in the radio process: the radio process dies with its radio,
//   and the save on unplug is precisely the moment it no longer exists.
#pragma once

namespace handheld {

/** True when argv asks for the supervisor (`--handheld`, or `--config` naming a file with "profile":"handheld"). */
bool wanted(int argc, char** argv);

/** Run the supervisor until SIGTERM/SIGINT or a shutdown. Returns the process exit code. */
int runSupervisor(int argc, char** argv);

}  // namespace handheld
