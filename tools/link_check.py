#!/usr/bin/env python3
"""RULE_LAUNCH_LINK wrapper: run the link, show its output, fail on wasm-ld signature mismatches.

Asyncify warns about ASYNCIFY_REMOVE names wasm-opt inlined away before it ran; those lines are expected noise.
Python rather than sh so the wrapper also runs where Ninja has no shell (Windows).
"""
import subprocess
import sys

NOISE = "Asyncify removelist contained a non-existing function name"


def main():
    r = subprocess.run(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    shown = [line for line in r.stdout.splitlines() if NOISE not in line]
    if any(shown):
        print("\n".join(shown), flush=True)
    if "signature mismatch" in r.stdout:
        sys.exit("link_check: function signature mismatch (fix the prototype by patch)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
