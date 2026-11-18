#!/usr/bin/env python3
"""Prepare the patched decomp tree and generated sources.

  tree    copy decomp/{src,include} to build/tree, tag `base`, apply patches/*.patch
  export  rewrite patches/ from build/tree commits base..HEAD
  gen     run tools/gen_data.py (--headers: overlay headers and port_abs.h only)
  all     tree, then gen
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DECOMP = ROOT / "decomp"
TREE = ROOT / "build" / "tree"
PATCHES = ROOT / "patches"
VENV_PY = ROOT / "tools" / ".venv" / "bin" / "python"


def git(*args, cwd=TREE, check=True):
    return subprocess.run(["git", *args], cwd=cwd, check=check)


def require_reference_build():
    missing = [p for p in (DECOMP / "asm", DECOMP / "build" / "global.elf") if not p.exists()]
    if missing:
        sys.exit(f"prepare: missing {', '.join(str(p.relative_to(ROOT)) for p in missing)}; "
                 "run Step 2 (reference decomp build: tools/dk make setup && tools/dk make in decomp/)")


def cmd_tree(_args):
    require_reference_build()
    if TREE.exists():
        shutil.rmtree(TREE)
    TREE.mkdir(parents=True)
    for sub in ("src", "include"):
        shutil.copytree(DECOMP / sub, TREE / sub, symlinks=True)
    git("init", "-q", "-b", "main")
    git("config", "user.name", "webby-kong port")
    git("config", "user.email", "port@webby-kong.invalid")
    git("config", "core.autocrlf", "false")
    git("add", "-A")
    git("commit", "-q", "-m", "base")
    git("tag", "base")
    patches = sorted(PATCHES.glob("*.patch"))
    if patches:
        r = git("am", "-q", "--3way", "--keep-cr", *map(str, patches), check=False)
        if r.returncode != 0:
            sys.exit("prepare: git am failed; resolve in build/tree (git am --continue), then run export")
    print(f"prepare: build/tree ready, {len(patches)} patches applied")


def cmd_export(_args):
    if not (TREE / ".git").exists():
        sys.exit("prepare: build/tree missing; run `tools/prepare.py tree` first")
    PATCHES.mkdir(exist_ok=True)
    for p in PATCHES.glob("*.patch"):
        p.unlink()
    git("format-patch", "-q", "--zero-commit", "--no-signature", "--no-stat", "-N",
        "-o", str(PATCHES), "base..HEAD")
    print(f"prepare: exported {len(list(PATCHES.glob('*.patch')))} patches")


def cmd_gen(args):
    require_reference_build()
    py = str(VENV_PY) if VENV_PY.exists() else sys.executable
    cmd = [py, str(ROOT / "tools" / "gen_data.py")]
    if args.headers:
        cmd.append("--headers")
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        sys.exit(r.returncode)


def cmd_all(args):
    cmd_tree(args)
    args.headers = False
    cmd_gen(args)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("tree").set_defaults(fn=cmd_tree)
    sub.add_parser("export").set_defaults(fn=cmd_export)
    g = sub.add_parser("gen")
    g.add_argument("--headers", action="store_true")
    g.set_defaults(fn=cmd_gen)
    sub.add_parser("all").set_defaults(fn=cmd_all)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
