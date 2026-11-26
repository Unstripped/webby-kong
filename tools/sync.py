#!/usr/bin/env python3
"""Bring this checkout's build up to date with its source; safe to rerun at any time (`mise run sync`).

Every step is skipped when its inputs are unchanged, so a second run only asks ninja whether anything is stale:
  pull     fast-forward the current branch from its upstream (--no-pull skips). An unreachable remote, a diverged
           branch or conflicting local changes only warn: the checked-out source is built.
  decomp   reset the submodule's core.worktree when it points elsewhere (left behind when a checkout moves), then
           check out the commit the index pins (older or newer). A decomp/ that port/ref was generated from (a bump
           in progress after tools/prepare.py ref) is built as is; local changes in a decomp/ that must move stop it.
  venv     tools/.venv with decomp/requirements.txt, tools/requirements.txt (libclang over the decomp's clang) and
           tools/requirements-server.txt; recreated when broken, reinstalled when a requirements file changes.
  prepare  tools/prepare.py all when decomp/, patches/ or prepare.py changed; tools/prepare.py gen when only
           port/ref, gen_data.py or build/tree did. build/tree is only recreated when it holds nothing patches/ lacks:
           an unfinished am/rebase, uncommitted changes or unexported commits stop the sync instead.
  build    configure build/web with emcmake when it is unconfigured or was configured with another Emscripten, then
           cmake --build build/web.
Fingerprints of the last successful prepare live in build/sync.json.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DECOMP = ROOT / "decomp"
TREE = ROOT / "build" / "tree"
GEN = ROOT / "build" / "gen"
WEB = ROOT / "build" / "web"
PATCHES = ROOT / "patches"
VENV = ROOT / "tools" / ".venv"
VENV_PY = VENV / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")
VENV_STAMP = VENV / "sync-requirements.sha256"
STATE = ROOT / "build" / "sync.json"
REQUIREMENTS = (DECOMP / "requirements.txt", ROOT / "tools" / "requirements.txt",
                ROOT / "tools" / "requirements-server.txt")


def log(msg):
    print(f"sync: {msg}", flush=True)


def fail(msg):
    sys.exit(f"sync: {msg}")


def run(*args, cwd=ROOT, check=True, capture=False, input=None):
    r = subprocess.run([str(a) for a in args], cwd=cwd, text=True, input=input,
                       capture_output=capture or input is not None)
    if check and r.returncode != 0:
        detail = f":\n{r.stderr.strip()}" if r.stderr else ""
        fail(f"`{' '.join(map(str, args))}` failed{detail}")
    return r


def out(*args, cwd=ROOT):
    return run(*args, cwd=cwd, capture=True).stdout.strip()


def ok(*args, cwd=ROOT):
    return run(*args, cwd=cwd, check=False, capture=True).returncode == 0


def digest(*parts):
    """SHA-256 over strings and the paths and contents of files (directories recursively; a missing path counts)."""
    h = hashlib.sha256()
    for part in parts:
        if not isinstance(part, Path):
            h.update(f"{part}\0".encode())
            continue
        files = sorted(f for f in part.rglob("*") if f.is_file()) if part.is_dir() else [part]
        for f in files:
            h.update(f"{f.relative_to(ROOT)}\0".encode())
            h.update(f.read_bytes() if f.exists() else b"<missing>")
            h.update(b"\0")
    return h.hexdigest()


def repair_decomp_worktree():
    """A submodule's gitdir records its checkout in core.worktree; moving a checkout leaves it pointing elsewhere."""
    dotgit = DECOMP / ".git"
    if not dotgit.is_file():
        return
    gitdir = (DECOMP / dotgit.read_text().strip().removeprefix("gitdir:").strip()).resolve()
    config = gitdir / "config"
    have = run("git", "config", "-f", config, "core.worktree", check=False, capture=True).stdout.strip()
    if have and (gitdir / have).resolve() != DECOMP.resolve():
        want = os.path.relpath(DECOMP, gitdir)
        run("git", "config", "-f", config, "core.worktree", want)
        log(f"decomp: core.worktree pointed at {have}; reset to {want}")


def pull():
    if not ok("git", "rev-parse", "--verify", "--quiet", "@{upstream}"):
        log("pull: skipped, the current branch has no upstream")
        return
    if run("git", "fetch", "--quiet", check=False).returncode != 0:
        log("pull: fetch failed; building the checked-out source")
        return
    behind = int(out("git", "rev-list", "--count", "HEAD..@{upstream}"))
    if not behind:
        log("pull: up to date")
    elif run("git", "merge", "--ff-only", "--quiet", "@{upstream}", check=False).returncode != 0:
        log(f"pull: cannot fast-forward {behind} upstream commit(s) (diverged branch or conflicting local changes); "
            "building the checked-out source")
    else:
        log(f"pull: fast-forwarded {behind} commit(s)")


def sync_decomp():
    pinned = out("git", "rev-parse", ":decomp")  # the index, so a staged bump counts as pinned
    if (DECOMP / ".git").exists():
        head = out("git", "rev-parse", "HEAD", cwd=DECOMP)
        if head == pinned:
            return
        sys.path.insert(0, str(ROOT / "tools"))
        from gen_data import decomp_fingerprint
        stamp = ROOT / "port" / "ref" / "decomp_fingerprint.txt"
        ref = stamp.read_text().splitlines() if stamp.exists() else []
        if decomp_fingerprint() in ref:
            log(f"decomp: at {head[:8]}, not the pinned {pinned[:8]}, but port/ref was generated from it "
                "(decomp bump in progress?); building it as is")
            return
        if out("git", "status", "--porcelain", "--untracked-files=no", cwd=DECOMP):
            fail(f"decomp/ has local changes and must move from {head[:8]} to the pinned {pinned[:8]}; game changes "
                 "belong in build/tree and patches/. Discard or stash them in decomp/, then rerun")
    run("git", "submodule", "update", "--init", "decomp")
    log(f"decomp: checked out {pinned[:8]}")


def sync_venv():
    want = digest(*REQUIREMENTS)
    healthy = VENV_PY.exists() and ok(VENV_PY, "-c", "")
    if healthy and VENV_STAMP.exists() and VENV_STAMP.read_text() == want:
        return
    if not healthy:
        shutil.rmtree(VENV, ignore_errors=True)
        run(sys.executable, "-m", "venv", VENV)
    pip = (VENV_PY, "-m", "pip", "install", "--quiet", "--disable-pip-version-check")
    run(*pip, "-r", REQUIREMENTS[0])
    run(*pip, "-r", REQUIREMENTS[1], "--force-reinstall", "--no-deps")
    run(*pip, "-r", REQUIREMENTS[2])
    VENV_STAMP.write_text(want)
    log("venv: requirements installed")


def tree_head():
    if not (TREE / ".git").exists():
        return None
    return run("git", "rev-parse", "--verify", "--quiet", "HEAD", cwd=TREE, check=False, capture=True).stdout.strip()


def tree_holds_only_patches():
    """True when build/tree's commits since `base` are exactly patches/*.patch (compared by stable patch id)."""
    def ids(text):
        r = run("git", "patch-id", "--stable", input=text)
        return sorted(line.split()[0] for line in r.stdout.splitlines())
    commits = out("git", "log", "-p", "--format=commit %H", "base..HEAD", cwd=TREE)
    exported = "".join(p.read_text() for p in sorted(PATCHES.glob("*.patch")))
    return ids(commits + "\n") == ids(exported)


def require_disposable_tree(state):
    if not (TREE / ".git").exists():
        return
    busy = [n for n in ("rebase-apply", "rebase-merge", "MERGE_HEAD", "CHERRY_PICK_HEAD") if (TREE / ".git" / n).exists()]
    if busy:
        fail(f"build/tree has an unfinished {busy[0]}; finish it and run tools/prepare.py export, or abort it "
             "(git -C build/tree am --abort / rebase --abort), then rerun")
    if out("git", "status", "--porcelain", cwd=TREE):
        fail("build/tree has uncommitted changes; commit them there and run tools/prepare.py export, or discard them, "
             "then rerun")
    if tree_head() != state.get("tree_head") and not tree_holds_only_patches():
        fail("build/tree has commits patches/ does not hold; run tools/prepare.py export (and commit patches/), or "
             "delete build/tree if you do not need them, then rerun")


def save(state):
    STATE.parent.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps(state, indent=2) + "\n")


def sync_prepare():
    state = json.loads(STATE.read_text()) if STATE.exists() else {}
    decomp_head = out("git", "rev-parse", "HEAD", cwd=DECOMP)
    tree_fp = digest(decomp_head, PATCHES, ROOT / "tools" / "prepare.py")

    def gen_fp():
        dirty = out("git", "diff", "HEAD", "--binary", cwd=TREE) if tree_head() else ""
        return digest(decomp_head, tree_head(), dirty, ROOT / "port" / "ref", ROOT / "tools" / "gen_data.py")

    if state.get("tree") != tree_fp or not (TREE / "src").is_dir():
        require_disposable_tree(state)
        log("prepare: decomp/, patches/ or prepare.py changed; recreating build/tree and build/gen")
        if run(VENV_PY, "tools/prepare.py", "all", check=False).returncode != 0:
            if (TREE / ".git" / "rebase-apply").exists():
                run("git", "am", "--abort", cwd=TREE, check=False)
            save({"tree": None, "tree_head": tree_head(), "gen": None})
            fail("tools/prepare.py all failed (above); build/tree is back at the unpatched decomp. To resolve a patch "
                 "conflict by hand: tools/prepare.py tree, fix it in build/tree, git am --continue, tools/prepare.py "
                 "export")
        state = {"tree": tree_fp, "tree_head": tree_head(), "gen": gen_fp()}
        save(state)
    elif state.get("gen") != gen_fp() or not (GEN / "include" / "port_abs.h").exists():
        log("prepare: port/ref, gen_data.py or build/tree changed; regenerating build/gen")
        run(VENV_PY, "tools/prepare.py", "gen")
        state["gen"] = gen_fp()
        save(state)


def build():
    emcmake = shutil.which("emcmake")
    if not emcmake:
        fail("emcmake is not on PATH; run `mise install`, then `mise run sync`")
    toolchain = Path(emcmake).resolve().parent / "cmake" / "Modules" / "Platform" / "Emscripten.cmake"
    cache = WEB / "CMakeCache.txt"
    configured = None
    if cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith("CMAKE_TOOLCHAIN_FILE:"):
                configured = Path(line.split("=", 1)[1]).resolve()
    if configured != toolchain.resolve() or not (WEB / "build.ninja").exists():
        if cache.exists():
            log(f"build: build/web was configured with {configured}; reconfiguring with {toolchain}")
            cache.unlink()
            shutil.rmtree(WEB / "CMakeFiles", ignore_errors=True)
        run(emcmake, "cmake", "-S", ".", "-B", WEB, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo")
    run("cmake", "--build", WEB)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-pull", action="store_true", help="build the checked-out source without fetching")
    args = ap.parse_args()
    repair_decomp_worktree()
    if not args.no_pull:
        before = digest(Path(__file__).resolve())
        pull()
        if digest(Path(__file__).resolve()) != before:
            log("tools/sync.py changed upstream; restarting it")
            argv = [sys.executable, str(Path(__file__).resolve()), "--no-pull"]
            if sys.platform == "win32":  # Windows execv exits this process at once, so mise would not wait
                sys.exit(subprocess.call(argv))
            os.execv(sys.executable, argv)
    sync_decomp()
    sync_venv()
    sync_prepare()
    build()
    log("build/web is up to date")


if __name__ == "__main__":
    main()
