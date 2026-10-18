#!/usr/bin/env bash
# build-x86-debs.sh — RENAMED to build-debs-remote.sh (2026-10-18), which builds all four architectures on
# the Lenovo. Kept as a shim so the old command line (and the memory notes that quote it) still works: it
# builds the two x86 packages exactly as before. A later --arch overrides this one.
exec "$(dirname "$0")/build-debs-remote.sh" --arch 'amd64 i386' "$@"
