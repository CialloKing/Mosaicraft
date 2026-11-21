#!/usr/bin/env bash
# Trace the actual process tree so a cold vcpkg run cannot quietly use an interpreter.
set -euo pipefail
log=$1
shift
mkdir -p -- "$(dirname -- "$log")"
strace -qq -f -s 4096 -e trace=execve,execveat -o "$log" -- "$@"
test -s "$log"
if grep -Ei 'execve(at)?\([^\"]*"([^\"]*/)?(python[0-9.]*|py|pypy[0-9.]*|pwsh|powershell)(\.exe)?"[ ,]|execve(at)?\([^\"]*"[^\"]+\.(py|ps1)"[ ,]' "$log"; then
    printf '%s\n' "Unexpected interpreter in $log" >&2
    exit 1
fi
