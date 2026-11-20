#!/usr/bin/env python3
"""Print the version the next release of HEAD gets (the release policy's single source of truth).

Releases are tags vX.Y.Z (tags not of that form are ignored); the latest is the highest one merged into HEAD.

  - no release yet                                        -> v0.1.0
  - a commit message since the latest release has #major  -> v(X+1).0.0
  - else a commit message since then has #minor           -> vX.(Y+1).0
  - otherwise                                             -> vX.Y.(Z+1)

Prints nothing when HEAD already carries a release tag (the release workflow then skips).
--previous prints the latest release tag instead (nothing if there is none).
"""
import argparse
import re
import subprocess

TAG_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")
MAJOR_RE = re.compile(r"#major\b")
MINOR_RE = re.compile(r"#minor\b")


def parse(tag):
    m = TAG_RE.match(tag)
    return tuple(map(int, m.groups())) if m else None


def latest(tags: list[str]) -> str | None:
    versions = [t for t in tags if parse(t)]
    return max(versions, key=parse) if versions else None


def next_version(tags: list[str], messages: list[str]) -> str:
    prev = latest(tags)
    if prev is None:
        return "v0.1.0"
    x, y, z = parse(prev)
    if any(MAJOR_RE.search(m) for m in messages):
        return f"v{x + 1}.0.0"
    if any(MINOR_RE.search(m) for m in messages):
        return f"v{x}.{y + 1}.0"
    return f"v{x}.{y}.{z + 1}"


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--previous", action="store_true", help="print the latest release tag merged into HEAD")
    args = ap.parse_args()

    tags = git("tag", "--merged", "HEAD", "--list", "v*").split()
    prev = latest(tags)
    if args.previous:
        if prev:
            print(prev)
        return
    if any(TAG_RE.match(t) for t in git("tag", "--points-at", "HEAD", "--list", "v*").split()):
        return
    messages = [m for m in git("log", "--format=%B%x00", f"{prev}..HEAD").split("\0") if m.strip()] if prev else []
    print(next_version(tags, messages))


if __name__ == "__main__":
    main()
