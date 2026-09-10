#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <sys/types.h>

/**
 * Running an external program WITHOUT A SHELL.
 *
 * ★★★ WHY THIS EXISTS. Every fetch in the daemon used to build a shell command line and hand it to
 *  `popen`/`system`, with the arguments wrapped in single quotes. That is safe only while nothing
 *  in the string can contain a single quote — and on the RadioDNS path it can: the DAB service and
 *  ensemble identifiers come OFF THE AIR, and the hostnames come out of DNS answers. One quote in
 *  any of them closes ours and the rest of the value is read by /bin/sh as commands, on the
 *  receiver, as the vibeserver user. CodeQL flagged it as cpp/command-line-injection and it was
 *  right: this was a real remote command-injection hole, not a false positive.
 *
 * ★★★ QUOTING IS NOT THE FIX. Escaping is a rule that has to be applied correctly at every call
 *  site for ever, and the flagged sites already believed they were quoting properly — asndb.cpp
 *  even carries a hard-won note about quoting the destination path. The fix is to remove the shell:
 *  `fork` + `execvp` takes an ARGUMENT VECTOR, so an argument is one argument no matter what bytes
 *  are in it. There is nothing left to escape and nothing left to get wrong.
 *
 * ★ Deliberately small: capture stdout, or send stdout to a file. Anything wanting a pipeline
 *   should run the two programs in turn through a temporary file — a pipeline is a shell feature
 *   and this header exists to not have one.
 */
namespace vibeproc {

/** Run `argv[0]` with `argv`, no shell. stdout is appended to `*out` when `out` is non-null;
 *  stderr always goes to /dev/null. Returns the child's exit status, or -1 if it could not be
 *  started or did not exit normally. ★ An empty argv is -1, never a shell. */
int run(const std::vector<std::string>& argv, std::string* out = nullptr);

/** As `run`, but the child's stdout is written straight to `path` (created/truncated, 0644).
 *  ★ For downloads: the bytes never pass through our address space, and there is still no shell
 *    redirection involved — the child's fd 1 IS the file. */
int runToFile(const std::vector<std::string>& argv, const std::string& path);

/** A child still running, with a stream on its stdout — for output that must be read as it
 *  arrives rather than collected at the end. ★ `pid` is kept so the caller can signal it directly
 *  instead of matching a command line. */
struct Child {
    FILE* out = nullptr;
    pid_t pid = -1;
    bool  ok() const { return out != nullptr && pid > 0; }
};

/** Start `argv` and return a stream on its stdout. `mergeStderr` sends the child's stderr to the
 *  same stream (some tools announce themselves there). Close with `reap`. */
Child spawn(const std::vector<std::string>& argv, bool mergeStderr = false);

/** Close the stream and wait for the child. Safe to call twice. Returns the exit status or -1. */
int reap(Child& c);

}  // namespace vibeproc
