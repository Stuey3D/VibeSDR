#pragma once

/** ★★★ DIE WITH THE SUPERVISOR — so a radio process can never outlive the front door.
 *
 *  A radio outliving its supervisor is the worst failure this design has: it keeps the SDR
 *  claimed, so nothing can restart it, while the process that was meant to own it is gone. The
 *  user sees a receiver that cannot be started and hardware that looks broken.
 *
 *  On LINUX the front door sets `prctl(PR_SET_PDEATHSIG, SIGKILL)` before exec and it survives
 *  execve, so the child needs to do nothing and this is a no-op.
 *
 *  ★★ macOS HAS NO SUCH FACILITY. There is nothing the parent can set that outlives exec, so the
 *     child must arm the watch ITSELF once it knows it is supervised (`--radio` with `--serve`).
 *     kqueue's EVFILT_PROC/NOTE_EXIT is the native way to watch another process: unlike polling
 *     getppid() it costs nothing while the parent lives and fires the moment it does not.
 *
 *  ★ Call it BEFORE claiming the radio. Arming it afterwards leaves a window in which we hold an
 *    SDR with nobody supervising us — which is the exact state this exists to prevent.
 *  ★ Safe to call when already orphaned: it exits immediately rather than running on.
 */
namespace vibe {

/** Arm the watch on our current parent. Returns false if the platform needs no watch (Linux,
 *  where PR_SET_PDEATHSIG already covers it) — not a failure. */
bool dieWithParent();

/** Testing seam: watch an ARBITRARY pid rather than getppid(), and call `onExit` instead of
 *  terminating. Lets the behaviour be tested without the test having to kill itself.
 *  ★ The production path is a thin wrapper over this, so a passing test exercises the real
 *    mechanism rather than a copy of it. */
bool watchPidForExit(int pid, void (*onExit)());

}  // namespace vibe
