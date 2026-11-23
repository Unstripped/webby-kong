#!/bin/sh
# RULE_LAUNCH_LINK wrapper: run the link, show its output, fail on wasm-ld signature mismatches.
# Asyncify warns about ASYNCIFY_REMOVE names wasm-opt inlined away before it ran; those lines are expected noise.
out=$("$@" 2>&1)
status=$?
shown=$(printf '%s\n' "$out" | grep -v 'Asyncify removelist contained a non-existing function name')
[ -n "$shown" ] && printf '%s\n' "$shown"
if printf '%s\n' "$out" | grep -q 'signature mismatch'; then
    echo "link_check: function signature mismatch (fix the prototype by patch)" >&2
    exit 1
fi
exit $status
