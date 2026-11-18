#!/bin/sh
# RULE_LAUNCH_LINK wrapper: run the link, show its output, fail on wasm-ld signature mismatches.
out=$("$@" 2>&1)
status=$?
[ -n "$out" ] && printf '%s\n' "$out"
if printf '%s\n' "$out" | grep -q 'signature mismatch'; then
    echo "link_check: function signature mismatch (fix the prototype by patch)" >&2
    exit 1
fi
exit $status
