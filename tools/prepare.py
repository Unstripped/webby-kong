#!/usr/bin/env python3
"""Prepare the patched decomp tree and generated sources.

  tree    copy decomp/{src,include} to build/tree, tag `base`, apply patches/*.patch
  rebase OLD
          carry patches/*.patch from decomp commit OLD to the current decomp/ (after a submodule bump):
          build/tree gets OLD's src/include as `old-base` with the patches applied, then 3-way rebases them
          onto the current decomp/{src,include} (`base`); resolve conflicts there, then run export
  export  rewrite patches/ from build/tree commits base..HEAD
  gen     run tools/gen_data.py from decomp/ and port/ref (--headers: overlay headers and port_abs.h only); no ROM
  ref     regenerate port/ref from the reference decomp build (after a decomp bump; needs the ROM)
  check   apply patches/*.patch to decomp/{src,include} in a temporary tree (CI; no ROM needed)
  all     tree, then gen (no ROM needed)
"""
import argparse
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DECOMP = ROOT / "decomp"
TREE = ROOT / "build" / "tree"
PATCHES = ROOT / "patches"
VENV_PY = ROOT / "tools" / ".venv" / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")


def git(*args, cwd=TREE, check=True):
    return subprocess.run(["git", *args], cwd=cwd, check=check)


def require_reference_build():
    missing = [p for p in (DECOMP / "asm", DECOMP / "assets" / "inc", DECOMP / "bin" / "segments",
                           DECOMP / "build" / "global.elf") if not p.exists()]
    if missing:
        sys.exit(f"prepare: missing {', '.join(str(p.relative_to(ROOT)) for p in missing)}; "
                 "run the reference decomp build (tools/dk make setup && tools/dk make in decomp/), "
                 "then tools/prepare.py ref")


def remove_read_only(func, path, _exc):
    """rmtree onexc: git writes its objects read-only, which Windows refuses to delete."""
    os.chmod(path, stat.S_IWRITE)
    func(path)


def init_tree(tree=TREE):
    """Recreate build/tree (or `tree`); refuses while it holds uncommitted changes or an unfinished am/rebase/merge."""
    if (tree / ".git").exists():
        busy = [n for n in ("rebase-merge", "rebase-apply", "MERGE_HEAD", "CHERRY_PICK_HEAD") if (tree / ".git" / n).exists()]
        dirty = subprocess.run(["git", "status", "--porcelain"], cwd=tree, capture_output=True, text=True).stdout
        if busy or dirty:
            sys.exit(f"prepare: build/tree has {'an unfinished ' + busy[0] if busy else 'uncommitted changes'}; "
                     "finish or commit them (then export) or move build/tree away first")
    if tree.exists():
        shutil.rmtree(tree, onexc=remove_read_only)
    tree.mkdir(parents=True)
    git("init", "-q", "-b", "main", cwd=tree)
    git("config", "user.name", "webby-kong port", cwd=tree)
    git("config", "user.email", "port@webby-kong.invalid", cwd=tree)
    git("config", "core.autocrlf", "false", cwd=tree)


def commit_decomp_base(tree=TREE):
    """Commit decomp/{src,include} as the tree's `base`."""
    for sub in ("src", "include"):
        shutil.rmtree(tree / sub, ignore_errors=True)
        shutil.copytree(DECOMP / sub, tree / sub, symlinks=True)
    git("add", "-A", cwd=tree)
    git("commit", "-q", "-m", "base", cwd=tree)
    git("tag", "base", cwd=tree)


def cmd_tree(_args):
    init_tree()
    commit_decomp_base()
    patches = sorted(PATCHES.glob("*.patch"))
    if patches:
        r = git("am", "-q", "--3way", "--keep-cr", *map(str, patches), check=False)
        if r.returncode != 0:
            sys.exit("prepare: git am failed; resolve in build/tree (git am --continue), then run export")
    print(f"prepare: build/tree ready, {len(patches)} patches applied")


def cmd_rebase(args):
    require_reference_build()
    r = subprocess.run(["git", "-C", str(DECOMP), "rev-parse", "--verify", "--quiet", f"{args.old}^{{commit}}"],
                       capture_output=True)
    if r.returncode != 0:
        sys.exit(f"prepare: {args.old} is not a commit in decomp/")
    init_tree()
    # checkout with a throwaway index rather than `git archive | tar -x`: decomp's own checkout settings apply (with
    # core.symlinks=false, as on Windows, symlinks are written as files, the way commit_decomp_base copies them)
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["git", "-C", str(DECOMP), f"--work-tree={TREE}", "checkout", args.old, "--", "src", "include"],
                       env={**os.environ, "GIT_INDEX_FILE": str(Path(tmp) / "index")}, check=True)
    git("add", "-A")
    git("commit", "-q", "-m", f"decomp {args.old}")
    git("tag", "old-base")
    patches = sorted(PATCHES.glob("*.patch"))
    if git("am", "-q", "--keep-cr", *map(str, patches), check=False).returncode != 0:
        sys.exit(f"prepare: patches/ do not apply to decomp {args.old}")
    git("checkout", "-q", "-b", "decomp", "old-base")
    commit_decomp_base()
    git("checkout", "-q", "main")
    if git("rebase", "-q", "--onto", "base", "old-base", "main", check=False).returncode != 0:
        sys.exit("prepare: rebase stopped on conflicts; resolve in build/tree (git rebase --continue), then run export")
    print(f"prepare: {len(patches)} patches rebased onto decomp/; run export")


def cmd_export(_args):
    if not (TREE / ".git").exists():
        sys.exit("prepare: build/tree missing; run `tools/prepare.py tree` first")
    PATCHES.mkdir(exist_ok=True)
    for p in PATCHES.glob("*.patch"):
        p.unlink()
    git("format-patch", "-q", "--zero-commit", "--no-signature", "--no-stat", "-N",
        "-o", str(PATCHES), "base..HEAD")
    print(f"prepare: exported {len(list(PATCHES.glob('*.patch')))} patches")


def run_gen_data(*args):
    py = str(VENV_PY) if VENV_PY.exists() else sys.executable
    r = subprocess.run([py, str(ROOT / "tools" / "gen_data.py"), *args], cwd=ROOT)
    if r.returncode != 0:
        sys.exit(r.returncode)


def cmd_gen(args):
    run_gen_data(*(["--headers"] if args.headers else []))


def cmd_ref(_args):
    require_reference_build()
    run_gen_data("--ref")


def cmd_all(args):
    cmd_tree(args)
    args.headers = False
    cmd_gen(args)


def cmd_check(_args):
    """Apply patches/*.patch to decomp/{src,include} in a scratch tree; needs no ROM or reference build."""
    if not (DECOMP / "src").is_dir():
        sys.exit("prepare: decomp/ is empty; run `git submodule update --init`")
    patches = sorted(PATCHES.glob("*.patch"))
    with tempfile.TemporaryDirectory(prefix="webby-kong-check-") as tmp:
        tree = Path(tmp) / "tree"
        init_tree(tree)
        commit_decomp_base(tree)
        if patches and git("am", "-q", "--3way", "--keep-cr", *map(str, patches), cwd=tree, check=False).returncode != 0:
            sys.exit("prepare: patches/ do not apply to decomp/; run `tools/prepare.py tree` and resolve in build/tree")
    head = subprocess.run(["git", "-C", str(DECOMP), "rev-parse", "--short", "HEAD"],
                          capture_output=True, text=True, check=True).stdout.strip()
    print(f"prepare: {len(patches)} patches apply to decomp {head}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("tree").set_defaults(fn=cmd_tree)
    r = sub.add_parser("rebase")
    r.add_argument("old", help="decomp commit the current patches/ apply to")
    r.set_defaults(fn=cmd_rebase)
    sub.add_parser("export").set_defaults(fn=cmd_export)
    g = sub.add_parser("gen")
    g.add_argument("--headers", action="store_true")
    g.set_defaults(fn=cmd_gen)
    sub.add_parser("ref").set_defaults(fn=cmd_ref)
    sub.add_parser("all").set_defaults(fn=cmd_all)
    sub.add_parser("check").set_defaults(fn=cmd_check)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
