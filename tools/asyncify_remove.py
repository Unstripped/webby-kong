#!/usr/bin/env python3
"""Write an ASYNCIFY_REMOVE list: every function the given objects define.

usage: asyncify_remove.py <llvm-nm> <out.txt> <object.o>...

The objects are the RSP task HLE (display lists, audio) and the WebGL backend. They run on the host context or are
leaf helpers the game calls (RDRAM syncs); none of them reaches a fiber switch. Asyncify still instruments them
because they call printf or SDL (indirect calls), and that instrumentation costs a large share of every frame.
Names come from the objects rather than the sources, so the list is exact and follows renames; names wasm-opt
inlines before asyncify runs still draw a warning, which tools/link_check.sh drops.

A function listed here must never be on the stack when a fiber switches: both builds link with ASSERTIONS, so
asyncify traps if one is.
"""
import subprocess
import sys


def main():
    nm, out, objects = sys.argv[1], sys.argv[2], sys.argv[3:]
    symbols = subprocess.run([nm, '--defined-only', *objects], check=True, capture_output=True, text=True).stdout
    names = []
    for line in symbols.splitlines():
        fields = line.split()
        # "<address> <type> <name>"; t/T are functions
        if len(fields) == 3 and fields[1] in ('t', 'T') and fields[2] not in names:
            names.append(fields[2])
    with open(out, 'w') as f:
        f.write('\n'.join(names) + '\n')


if __name__ == '__main__':
    main()
