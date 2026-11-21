#!/usr/bin/env python3
"""Decode an in-game bug report (web/report.js) from a GitHub issue.

Reads FILE, or stdin when omitted, and finds the DK64R1.<base64url(deflate-raw(JSON))> blob anywhere in it:

  gh issue view 42 | python3 tools/report.py
  python3 tools/report.py issue.md --save dk64.eep

Prints the version, settings, the headless --resume arguments for the reporter's location, level progress and the
renderer probe of the spot they marked. --json prints the whole decoded report; --save writes their 2 KB EEPROM.
In the browser, F2 → Load report… boots straight into the same save and location.
"""
import argparse
import base64
import json
import re
import sys
import zlib

BLOB_RE = re.compile(r"DK64R1\.([A-Za-z0-9_-]+)")


def extract(text: str) -> dict:
    m = BLOB_RE.search(text)
    if not m:
        raise ValueError("no DK64R1 report data found")
    raw = m.group(1)
    data = base64.urlsafe_b64decode(raw + "=" * (-len(raw) % 4))
    obj = json.loads(zlib.decompress(data, -15))
    if not isinstance(obj, dict) or obj.get("v") != 1:
        raise ValueError("unsupported report version")
    return obj


def resume_arg(resume: dict) -> str:
    """The headless --resume value (port/src/main.c scans %d,%d,%i,%d,%d,%f,%f,%f,%d)."""
    r = resume
    return f"{r['slot']},{r['kong']},{r['map']},{r['exit']},{r['hasPos']},{r['x']},{r['y']},{r['z']},{r['yaw']}"


def describe(r: dict) -> str:
    version = r.get("version") or "source build"
    if r.get("revision") and r.get("version"):
        version += f" ({r['revision'][:7]})"
    lines = [f"version: {version}", f"build: {r.get('buildId') or '-'}"]
    if r.get("crash"):
        lines.append(f"crash: {r['crash']}")
    lines.append("settings: " + json.dumps(r.get("settings") or {}, separators=(",", ":")))
    if r.get("resume"):
        lines.append(f"resume: --resume {resume_arg(r['resume'])}  (headless starts with an empty save)")
    else:
        lines.append("resume: none")
    lines.append(f"save: {len(bytes.fromhex(r['save']))} bytes" if r.get("save") else "save: none")
    lines.append("progress: " + json.dumps(r.get("progress"), separators=(",", ":")))
    mark = r.get("mark")
    if mark:
        lines.append(f"mark: {mark['fx']:.3f}, {mark['fy']:.3f}")
        lines.append(mark.get("probe") or "(no probe)")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", help="issue text (default: stdin)")
    ap.add_argument("--json", action="store_true", help="print the whole decoded report")
    ap.add_argument("--save", metavar="OUT", help="write the reporter's EEPROM save to OUT")
    args = ap.parse_args()

    if args.file:
        with open(args.file, encoding="utf-8") as f:
            text = f.read()
    else:
        text = sys.stdin.read()
    try:
        r = extract(text)
    except ValueError as e:
        print(f"report.py: {e}", file=sys.stderr)
        sys.exit(1)

    print(json.dumps(r, indent=2) if args.json else describe(r))
    if args.save:
        if not r.get("save"):
            print("report.py: the report has no save file", file=sys.stderr)
            sys.exit(1)
        with open(args.save, "wb") as f:
            f.write(bytes.fromhex(r["save"]))
        print(f"wrote {args.save}", file=sys.stderr)


if __name__ == "__main__":
    main()
