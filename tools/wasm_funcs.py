#!/usr/bin/env python3
"""Print wasm function indices and breakpoint offsets for DevTools logpoints.

usage: wasm_funcs.py <module.wasm> <regex> [--body]

One line per function whose name matches (re.search): <name>\t<funcIndex>\t<offset>.
offset is the absolute file offset of the body's first instruction (after the local
declarations), which is the columnNumber Debugger.setBreakpoint takes for wasm scripts.
--body prints the offset of the body-size LEB instead.
"""
import re
import sys


def leb(data, pos):
    result = shift = 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return result, pos


def name_str(data, pos):
    n, pos = leb(data, pos)
    return data[pos:pos + n].decode('utf-8', 'replace'), pos + n


def skip_import_desc(data, pos, kind):
    if kind == 0:  # func: typeidx
        _, pos = leb(data, pos)
    elif kind == 1:  # table: reftype + limits
        pos += 1
        pos = skip_limits(data, pos)
    elif kind == 2:  # memory: limits
        pos = skip_limits(data, pos)
    elif kind == 3:  # global: valtype + mut
        pos += 2
    elif kind == 4:  # tag: attribute + typeidx
        pos += 1
        _, pos = leb(data, pos)
    else:
        raise ValueError(f'unknown import kind {kind}')
    return pos


def skip_limits(data, pos):
    flags = data[pos]
    pos += 1
    _, pos = leb(data, pos)
    if flags & 1:
        _, pos = leb(data, pos)
    return pos


def main():
    args = [a for a in sys.argv[1:] if a != '--body']
    body_mode = '--body' in sys.argv[1:]
    if len(args) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    path, pattern = args
    data = open(path, 'rb').read()
    if data[:4] != b'\0asm':
        print(f'{path}: not a wasm module', file=sys.stderr)
        return 2

    n_imported = 0
    bodies = []  # (size-LEB offset, first-instruction offset)
    names = {}
    pos = 8
    while pos < len(data):
        sec_id = data[pos]
        size, start = leb(data, pos + 1)
        end = start + size
        if sec_id == 2:
            count, p = leb(data, start)
            for _ in range(count):
                _, p = name_str(data, p)
                _, p = name_str(data, p)
                kind = data[p]
                p = skip_import_desc(data, p + 1, kind)
                if kind == 0:
                    n_imported += 1
        elif sec_id == 10:
            count, p = leb(data, start)
            for _ in range(count):
                size_off = p
                body_size, body = leb(data, p)
                q = body
                nlocals, q = leb(data, q)
                for _ in range(nlocals):
                    _, q = leb(data, q)
                    q += 1  # valtype
                bodies.append((size_off, q))
                p = body + body_size
        elif sec_id == 0:
            sec_name, p = name_str(data, start)
            if sec_name == 'name':
                while p < end:
                    sub = data[p]
                    sub_size, sp = leb(data, p + 1)
                    if sub == 1:
                        count, q = leb(data, sp)
                        for _ in range(count):
                            idx, q = leb(data, q)
                            names[idx], q = name_str(data, q)
                    p = sp + sub_size
        pos = end

    rx = re.compile(pattern)
    found = 0
    for i, (size_off, first) in enumerate(bodies):
        idx = n_imported + i
        name = names.get(idx)
        if name is not None and rx.search(name):
            print(f'{name}\t{idx}\t{size_off if body_mode else first}')
            found += 1
    if not found:
        print(f'no function matches {pattern}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
