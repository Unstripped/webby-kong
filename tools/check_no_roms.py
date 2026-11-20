#!/usr/bin/env python3
"""Refuse ROMs and ROM-derived files in the git index.

Checks every regular file staged in the index of the repository in the current directory (after a CI checkout the
index equals HEAD; in the pre-commit hook it is the snapshot about to be committed). A file is rejected when it has
a ROM extension (.z64 .n64 .v64 .u64 .rom .ndd), is build output (.wasm, which embeds ROM-derived data), starts with
an N64 ROM header in any byte order (z64, v64, n64), or is larger than 2 MiB. Exits 1 if anything is rejected.

Use as a hook in a clone: git config core.hooksPath .githooks
"""
import subprocess
import sys
from pathlib import PurePosixPath

ROM_SUFFIXES = {".z64", ".n64", ".v64", ".u64", ".rom", ".ndd"}
ROM_MAGICS = {bytes.fromhex("80371240"), bytes.fromhex("37804012"), bytes.fromhex("40123780")}
MAX_SIZE = 2 * 1024 * 1024


def staged_blobs():
    """(path, sha) of every regular file in the index; gitlinks (submodules) and symlinks are skipped."""
    out = subprocess.run(["git", "ls-files", "-s", "-z"], capture_output=True, check=True).stdout
    blobs = []
    for entry in out.split(b"\0"):
        if not entry:
            continue
        meta, path = entry.split(b"\t", 1)
        mode, sha, _stage = meta.split(b" ")
        if mode in (b"100644", b"100755"):
            blobs.append((path.decode("utf-8", "surrogateescape"), sha.decode()))
    return blobs


def read_blobs(shas):
    """Contents of the given blobs, read through one `git cat-file --batch` process."""
    if not shas:
        return []
    out = subprocess.run(["git", "cat-file", "--batch"], input=b"".join(s.encode() + b"\n" for s in shas),
                         capture_output=True, check=True).stdout
    contents, pos = [], 0
    for _ in shas:
        eol = out.index(b"\n", pos)
        _sha, kind, size = out[pos:eol].split(b" ")
        if kind != b"blob":
            raise RuntimeError(f"check_no_roms: unexpected object type {kind.decode()}")
        start = eol + 1
        end = start + int(size)
        contents.append(out[start:end])
        pos = end + 1
    return contents


def reason(path, data):
    suffix = PurePosixPath(path).suffix.lower()
    if suffix in ROM_SUFFIXES:
        return "ROM file extension"
    if suffix == ".wasm":
        return "build output (contains ROM-derived data)"
    if data[:4] in ROM_MAGICS:
        return "N64 ROM header"
    if len(data) > MAX_SIZE:
        return "larger than 2 MiB"
    return None


def main():
    blobs = staged_blobs()
    contents = read_blobs([sha for _path, sha in blobs])
    rejected = 0
    for (path, _sha), data in zip(blobs, contents):
        why = reason(path, data)
        if why:
            rejected += 1
            print(f"check_no_roms: {path}: {why}", file=sys.stderr)
    if rejected:
        print(f"check_no_roms: {rejected} file(s) rejected; ROMs and ROM-derived data must never be committed",
              file=sys.stderr)
        sys.exit(1)
    print(f"check_no_roms: {len(blobs)} files ok")


if __name__ == "__main__":
    main()
