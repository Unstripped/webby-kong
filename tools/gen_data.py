#!/usr/bin/env python3
"""Generate port sources from the decomp and the committed reference manifest (port/ref).

The decomp defines its static data in C; what is left as splat assembly is data only handwritten asm reads
(registered in decomp/handwritten_asm.json) and asm .bss. That data is re-emitted from the asm directives:
its readers are the port's C rewrites of that asm (port/src/asm), which read it at the directive widths.

All outputs go under build/gen/:
  include/ovl/<ovl>.h      overlay rename header (force-included into overlay TUs)
  include/port_abs.h       PORT_ABS_<name> constants
  include/port_ovl_fn.h    OVL_FN(type, addr) -> per-overlay thunk port_ovlfn_<addr>
  include/inc/<seg>/*.inc.c  the decomp's ROM-extracted data initialisers, in N64 byte order in memory
  data_<seg>.c             asm-only data as native C objects
  gen_layout_check.c       portGenLayoutCheck()
  rom_layout.c             gPortSegRom
  rom_fill.c               gPortRomFillSegs: placeholders filled from the user's ROM at boot (port/src/romfill.c)
  ovl_entry.c              portOverlayEntry(), overlay function dispatchers and OVL_FN thunks
  report.txt               everything reported; any error makes gen exit non-zero

The wasm carries no ROM bytes: data whose bytes come from the ROM extraction (asm data, data_inc includes, shadow
snapshots) is emitted as placeholders. Metadata of the ROM extraction (ELF symbols, splat's auto symbols, asm label
sizes and directive widths) comes from port/ref, which --ref regenerates from the reference decomp build
(decomp/{asm,assets,bin,build}; `tools/prepare.py ref`, after a decomp bump).
"""
import argparse
import bisect
import hashlib
import os
import re
import shutil
import struct
import sys
import tempfile
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DECOMP = ROOT / "decomp"
TREE = ROOT / "build" / "tree"
GEN = ROOT / "build" / "gen"
REF = ROOT / "port" / "ref"

OVERLAYS = ["multiplayer", "minecart", "bonus", "race", "critter", "boss", "arcade", "jetpac", "menu"]
SEGMENTS = ["boot", "global"] + OVERLAYS
ELF_NAMES = {"boot": "dk64.us", "global": "global", **{o: o for o in OVERLAYS}}
SEG_SYM_RE = re.compile(r"_(ROM_START|ROM_END|VRAM|VRAM_END|BSS_START|BSS_END|LOAD_START|LOAD_END)$")
OVL_VRAM = (0x80024000, 0x80050000)
DATA_IN_TEXT = {  # global .text ranges of handwritten asm holding data; extra names come from symbol_addrs.us.txt
    "asm/global/1F9A0.s": (0x8061ACA0, 0x8061ACA0 + 0x204 * 4),
    "asm/global/16B60.s": (0x80611F0C, 0x80612790),
}
RAW_FILES = {  # RSP DMEM images (6710/14CF0 osTask data): byte copies into DMEM, so kept in N64 byte order
    "asm/global/data/165290.rodata.s",
}
SEG_INDEX = {"global": 1, **{o: i + 2 for i, o in enumerate(OVERLAYS)}}  # gPortSegRom rows (gen_rom_layout order)


def stream_file(seg):
    """decomp/bin/segments/<file>: the segment's inflated stream (text gzip member + data gzip member)."""
    return "global.bin" if seg == "global" else f"ovl{SEG_INDEX[seg]:02d}.bin"


class Report:
    def __init__(self):
        self.lines = []
        self.errors = 0

    def note(self, msg):
        self.lines.append(msg)

    def error(self, msg):
        self.errors += 1
        self.lines.append("ERROR: " + msg)


REPORT = Report()


def write_if_changed(path: Path, text: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text() == text:
        return
    path.write_text(text)


def decomp_fingerprint():
    """SHA-1 of the decomp inputs port/ref is derived from; needs no git (Docker build contexts have no .git)."""
    files = {p for sub in ("src", "include") for p in (DECOMP / sub).rglob("*")
             if p.is_file() and p.suffix in (".c", ".h")}
    for pat in ("*.yaml", "symbol_addrs*.txt", "undefined_syms.us.txt", "data_inc.us.txt", "handwritten_asm.json"):
        files.update(p for p in DECOMP.glob(pat) if p.is_file())
    h = hashlib.sha1()
    for rel, p in sorted((p.relative_to(DECOMP).as_posix(), p) for p in files):
        h.update(rel.encode() + b"\0" + p.read_bytes() + b"\0")
    return h.hexdigest()


# ----------------------------------------------------------------------------- ELF symbols

class ElfSyms:
    """Symbols of one segment ELF: read from the reference build's ELF (--ref) or port/ref/symbols.<seg>.txt."""

    def __init__(self, seg):
        self.seg = seg
        self.path = DECOMP / "build" / f"{ELF_NAMES[seg]}.elf"
        self.defined = {}      # name -> (addr, size, type)
        self.absolute = {}     # name -> addr

    @classmethod
    def from_elf(cls, seg):
        from elftools.elf.elffile import ELFFile
        self = cls(seg)
        with open(self.path, "rb") as fh:
            elf = ELFFile(fh)
            for s in elf.get_section_by_name(".symtab").iter_symbols():
                if not s.name or s["st_info"]["type"] in ("STT_SECTION", "STT_FILE"):
                    continue
                shndx = s["st_shndx"]
                if shndx == "SHN_UNDEF":
                    continue
                if shndx == "SHN_ABS":
                    self.absolute[s.name] = s["st_value"]
                    continue
                self.defined[s.name] = (s["st_value"], s["st_size"], s["st_info"]["type"])
        return self

    @classmethod
    def from_ref(cls, seg):
        self = cls(seg)
        path = REF / f"symbols.{seg}.txt"
        if not path.is_file():
            REPORT.error(f"port/ref/symbols.{seg}.txt missing: run tools/prepare.py ref")
            return self
        for line in path.read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            name, addr, size, typ = line.split()
            if typ == "ABS":
                self.absolute[name] = int(addr, 16)
            else:
                self.defined[name] = (int(addr, 16), int(size, 16), typ)
        return self


def load_elves(ref=True):
    make = ElfSyms.from_ref if ref else ElfSyms.from_elf
    return {seg: make(seg) for seg in SEGMENTS}


def overlay_clashes(elves):
    out = {}
    for ovl in OVERLAYS:
        others = set()
        for seg, e in elves.items():
            if seg != ovl:
                others.update(e.defined)
        out[ovl] = sorted(n for n in elves[ovl].defined if n in others and not SEG_SYM_RE.search(n))
    return out


def asm_name(name, seg_of_def, clashes):
    if seg_of_def in OVERLAYS and name in clashes[seg_of_def]:
        return f"{seg_of_def}__{name}"
    return name


def gen_overlay_headers(clashes):
    for ovl, clash in clashes.items():
        lines = [
            f"/* generated by tools/gen_data.py: overlay {ovl} */",
            f"#ifndef PORT_OVL_{ovl.upper()}_H",
            f"#define PORT_OVL_{ovl.upper()}_H",
            f"#define PORT_OVERLAY_{ovl.upper()} 1",
        ]
        lines += [f"#define {n} {ovl}__{n}" for n in clash]
        lines.append("#endif")
        write_if_changed(GEN / "include" / "ovl" / f"{ovl}.h", "\n".join(lines) + "\n")


def parse_undefined_syms():
    """undefined_syms.us.txt plus splat's undefined_syms_auto.*.txt (addresses the decomp links by value; copied
    from decomp/build to port/ref by --ref)."""
    out = {}
    paths = [DECOMP / "undefined_syms.us.txt"] + sorted(REF.glob("undefined_syms_auto.*.txt"))
    for path in paths:
        text = re.sub(r"/\*.*?\*/", "", path.read_text(), flags=re.S)
        for m in re.finditer(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;", text, re.M):
            out.setdefault(m.group(1), int(m.group(2), 0))
    return out


def abs_symbols(elves):
    syms = dict(parse_undefined_syms())
    for e in elves.values():
        for n, v in e.defined.items():
            if SEG_SYM_RE.search(n):
                syms.setdefault(n, v[0])
        for n, v in e.absolute.items():
            if SEG_SYM_RE.search(n):
                syms.setdefault(n, v)
    return syms


def gen_port_abs(abs_syms):
    lines = ["/* generated by tools/gen_data.py */", "#ifndef PORT_ABS_H", "#define PORT_ABS_H"]
    lines += [f"#define PORT_ABS_{n} 0x{v:08X}u" for n, v in sorted(abs_syms.items())]
    lines.append("#endif")
    write_if_changed(GEN / "include" / "port_abs.h", "\n".join(lines) + "\n")


# ----------------------------------------------------------------------------- game sources

def game_flags():
    inc = [ROOT / "port/include/override", ROOT / "port/include", GEN / "include", GEN / "include/inc", TREE / "include",
           TREE / "include/PR", TREE / "include/libultra", TREE / "include/compiler/ido"]
    return ["-target", "wasm32-unknown-emscripten", "-std=gnu89", "-fsigned-char",
            "-D_LANGUAGE_C", "-D_MIPS_SZLONG=32", "-DF3DEX_GBI_2", "-D_FINALROM", "-DNDEBUG",
            "-DBUILD_VERSION=VERSION_J", "-DTARGET_PORT=1", "-nostdinc",
            "-Wno-int-conversion", "-Wno-incompatible-pointer-types", "-Wno-deprecated-non-prototype",
            "-Wno-pointer-sign", "-Wno-unknown-pragmas",
            "-include", str(ROOT / "port/include/port/prelude.h")] + [f"-I{p}" for p in inc]


BOOT_SOURCES = ["1CC0.c", "30E0.c", "3630.c", "3D20.c", "6690.c", "libultra/io/vitbl.c",
                "libultra/libc/ll.c", "libultra/libc/llcvt.c"]


def game_sources():
    """Game TUs the port compiles (CMakeLists.txt GAME_*_SRC)."""
    src = TREE / "src"
    files = sorted((src / "global").glob("*.c"))
    files += [src / "boot" / f for f in BOOT_SOURCES]
    for sub in ("gu", "mgu", "vimodes"):
        files += sorted((src / "boot/libultra" / sub).glob("*.c"))
    for o in OVERLAYS:
        files += sorted((src / o).glob("*.c"))
    return files


def seg_of_source(path: Path):
    rel = path.relative_to(TREE / "src").parts[0]
    return rel if rel in OVERLAYS else ("boot" if rel == "boot" else "global")


OVL_FN_RE = re.compile(r"\bOVL_FN\s*\(\s*([^,()]*\(\s*\*\s*\)\s*\([^()]*\))\s*,\s*(0x[0-9A-Fa-f]+)\s*\)")
FN_PTR_TYPE_RE = re.compile(r"^(.*?)\s*\(\s*\*\s*\)\s*\((.*)\)$")


def ovl_fn_uses():
    """OVL_FN(type, addr) uses in non-overlay game TUs -> {addr token: (ret, (arg types...))}."""
    out = {}
    for path in game_sources():
        if seg_of_source(path) in OVERLAYS:
            continue
        for m in OVL_FN_RE.finditer(path.read_text(errors="replace")):
            t = FN_PTR_TYPE_RE.match(m.group(1).strip())
            args = tuple(a.strip() for a in t.group(2).split(",")) if t.group(2).strip() else ("void",)
            sig = (t.group(1).strip(), args)
            tok = m.group(2)
            if out.setdefault(tok, sig) != sig:
                REPORT.error(f"OVL_FN {tok}: used as {out[tok]} and {sig}")
    return out


def gen_ovl_fn_header(uses):
    lines = ["/* generated by tools/gen_data.py */", "#ifndef PORT_OVL_FN_H", "#define PORT_OVL_FN_H",
             "/* overlay functions stored in non-overlay data call the active overlay's function (ovl_entry.c) */",
             "#define OVL_FN(type, addr) ((type)port_ovlfn_##addr)"]
    lines += [f"extern {ret} port_ovlfn_{tok}({', '.join(args)});" for tok, (ret, args) in sorted(uses.items())]
    lines.append("#endif")
    write_if_changed(GEN / "include" / "port_ovl_fn.h", "\n".join(lines) + "\n")


def data_inc_rows():
    """data_inc.us.txt -> [(seg, name, vram, size, width, file)]."""
    rows = []
    for line in (DECOMP / "data_inc.us.txt").read_text().splitlines():
        line = line.split("#", 1)[0].split()
        if line:
            rows.append((line[0], line[1], int(line[2], 0), int(line[3], 0), int(line[4], 0), line[5]))
    return rows


INC_PLACEHOLDER = ("/* generated by tools/gen_data.py: placeholder filled from the ROM at boot (build/gen/rom_fill.c); "
                   "non-zero so the object stays in .data */\n1\n")


def gen_data_inc(fills, clashes):
    """include/inc/<seg>/<name>.inc.c for the rows of data_inc.us.txt, the ROM asset bytes the decomp extracts to
    assets/inc: placeholders, filled at boot with the ROM bytes in N64 byte order (textures the RDP reads that way)."""
    compiled = {p.relative_to(TREE) for p in game_sources()}
    n = 0
    for seg, name, vram, size, width, file in data_inc_rows():
        write_if_changed(GEN / "include" / "inc" / seg / f"{name}.inc.c", INC_PLACEHOLDER)
        if Path(file) in compiled:
            fills.add(seg, asm_name(name, seg, clashes), 0, vram, size, 1)
        else:
            REPORT.note(f"data_inc: {name} ({file}) not compiled")
        n += 1
    REPORT.note(f"data_inc: {n} includes")


# ----------------------------------------------------------------------------- C declaration index

def lower_scalar(t):
    import clang.cindex as ci
    K = ci.TypeKind
    t = t.get_canonical()
    k = t.kind
    if k == K.VOID:
        return "void"
    if k in (K.POINTER, K.BLOCKPOINTER, K.CONSTANTARRAY, K.INCOMPLETEARRAY):
        return "void *"
    if k == K.FLOAT:
        return "float"
    if k == K.DOUBLE:
        return "double"
    if k in (K.LONGLONG, K.ULONGLONG):
        return "long long"
    if k in (K.CHAR_S, K.SCHAR, K.CHAR_U, K.UCHAR, K.BOOL, K.SHORT, K.USHORT, K.INT, K.UINT, K.LONG, K.ULONG, K.ENUM):
        return "int"
    return None


def lower_function(t):
    import clang.cindex as ci
    t = t.get_canonical()
    ret = lower_scalar(t.get_result())
    if ret is None:
        return None
    if t.kind == ci.TypeKind.FUNCTIONNOPROTO:
        return (ret, ("void",), False)
    args = []
    for a in t.argument_types():
        la = lower_scalar(a)
        if la is None:
            return None
        args.append(la)
    return (ret, tuple(args) or ("void",), t.is_function_variadic())


def index_tu(path_str):
    """Parse one TU; return its file-scope variables and functions."""
    import clang.cindex as ci
    path = Path(path_str)
    errors = []
    args = game_flags()
    seg = seg_of_source(path)
    if seg in OVERLAYS:
        args = args + ["-include", str(GEN / "include" / "ovl" / f"{seg}.h")]
    idx = ci.Index.create()
    tu = idx.parse(str(path), args=args)  # bodies needed: is_definition() is False when they are skipped
    errs = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
    if errs:
        errors.append(f"libclang {path.name}: {errs[0]}")
    vars_ = []   # (name, defined, static)
    funcs = []   # (name, defined, static, sig)
    for c in tu.cursor.get_children():
        if c.kind == ci.CursorKind.VAR_DECL:
            static = c.storage_class == ci.StorageClass.STATIC
            extern = c.storage_class == ci.StorageClass.EXTERN
            vars_.append((c.spelling, not extern, static))
        elif c.kind == ci.CursorKind.FUNCTION_DECL:
            static = c.storage_class == ci.StorageClass.STATIC
            funcs.append((c.spelling, c.is_definition(), static, lower_function(c.type)))
    return seg, vars_, funcs, errors


class Index:
    def __init__(self):
        self.c_defined = {}          # name -> (seg, static)
        self.func = {}               # name -> sig (definition preferred)
        self.func_defined = {}       # name -> (seg, static)
        self.declared = set()        # every file-scope variable name C declares

    def build(self):
        files = [str(p) for p in game_sources()]
        with ProcessPoolExecutor(max_workers=os.cpu_count()) as ex:
            results = list(ex.map(index_tu, files, chunksize=4))
        for seg, vars_, funcs, errors in results:
            for e in errors:
                REPORT.error(e)
            for name, defined, static in vars_:
                self.declared.add(name)
                if defined:
                    self.c_defined.setdefault(name, (seg, static))
            for name, defined, static, sig in funcs:
                if defined:
                    self.func_defined[name] = (seg, static)
                    if sig is not None:
                        self.func[name] = sig
                elif sig is not None and name not in self.func:
                    self.func[name] = sig
        return self


# ----------------------------------------------------------------------------- asm data parsing

class Label:
    def __init__(self, name, vram, seg, file, kind):
        self.name = name
        self.vram = vram
        self.seg = seg
        self.file = file
        self.kind = kind       # data / rodata / bss
        self.data = bytearray()
        self.widths = {}       # offset -> directive width (1/2/4/8)
        self.aliases = []      # other names for this exact address
        self.size = 0

    def put(self, off, b):
        end = off + len(b)
        if len(self.data) < end:
            self.data.extend(b"\0" * (end - len(self.data)))
        self.data[off:end] = b


LINE_RE = re.compile(r"^\s*/\*\s*([0-9A-Fa-f ]+?)\s*\*/\s*\.(\w+)\s*(.*?)\s*(?:/\*.*\*/)?\s*$")
LABEL_RE = re.compile(r"^\s*(?:dlabel|glabel)\s+([A-Za-z_.$][\w.$]*)")
SECTION_RE = re.compile(r"^\s*\.section\s+\.?(\w+)")


def unescape(s):
    out = bytearray()
    i = 0
    while i < len(s):
        ch = s[i]
        if ch == "\\":
            i += 1
            e = s[i]
            if e in "01234567":
                j = i
                while j < len(s) and j < i + 3 and s[j] in "01234567":
                    j += 1
                out.append(int(s[i:j], 8) & 0xFF)
                i = j
                continue
            if e == "x":
                j = i + 1
                while j < len(s) and s[j] in "0123456789abcdefABCDEF":
                    j += 1
                out.append(int(s[i + 1:j], 16) & 0xFF)
                i = j
                continue
            out.append({"n": 10, "t": 9, "r": 13, "\\": 92, '"': 34, "'": 39, "a": 7, "b": 8, "f": 12, "v": 11}[e])
            i += 1
            continue
        out.extend(ch.encode("latin-1"))
        i += 1
    return bytes(out)


def parse_int(tok):
    tok = tok.strip()
    neg = tok.startswith("-")
    v = int(tok.lstrip("-"), 0)
    return -v if neg else v


def parse_asm_file(path: Path, seg):
    rel = str(path.relative_to(DECOMP))
    kind_default = "data"
    for k in ("rodata", "bss", "data"):
        if path.name.endswith(f".{k}.s"):
            kind_default = k
            break
    labels = []
    cur = None
    kind = kind_default
    for raw in path.read_text().splitlines():
        m = SECTION_RE.match(raw)
        if m:
            kind = {"rodata": "rodata", "bss": "bss", "late_rodata": "rodata"}.get(m.group(1), "data")
            continue
        m = LABEL_RE.match(raw)
        if m:
            cur = Label(m.group(1), None, seg, rel, kind)
            labels.append(cur)
            continue
        m = LINE_RE.match(raw)
        if not m or cur is None:
            continue
        toks = m.group(1).split()
        op, arg = m.group(2), m.group(3)
        if len(toks) == 1:
            vram, blob = int(toks[0], 16), None
        else:
            vram = int(toks[1], 16)
            blob = bytes.fromhex(toks[2]) if len(toks) > 2 else None
        if cur.vram is None:
            cur.vram = vram
        off = vram - cur.vram
        if op == "word":
            a = arg.strip()
            if not re.match(r"^-?(0x[0-9A-Fa-f]+|\d+)$", a):
                # symbol words would need relocating; handwritten-asm data has none
                REPORT.error(f"{rel}: {cur.name}+0x{off:X}: .word {a} needs a relocation")
                continue
            cur.put(off, struct.pack(">I", parse_int(a) & 0xFFFFFFFF))
            cur.widths[off] = 4
        elif op in ("short", "half", "hword"):
            cur.put(off, struct.pack(">H", parse_int(arg) & 0xFFFF))
            cur.widths[off] = 2
        elif op == "byte":
            cur.put(off, bytes([parse_int(arg) & 0xFF]))
            cur.widths[off] = 1
        elif op == "float":
            cur.put(off, blob if blob and len(blob) == 4 else struct.pack(">f", float(arg)))
            cur.widths[off] = 4
        elif op == "double":
            cur.put(off, blob if blob and len(blob) == 8 else struct.pack(">d", float(arg)))
            cur.widths[off] = 8
        elif op == "space":
            cur.put(off, b"\0" * parse_int(arg))
        elif op in ("asciz", "ascii"):
            s = unescape(arg.strip()[1:-1])
            if op == "asciz":
                s += b"\0"
            cur.put(off, s)
        else:
            REPORT.error(f"{rel}: unknown directive .{op}")
    labels = [l for l in labels if l.vram is not None]
    for a, b in zip(labels, labels[1:]):
        if b.vram < a.vram + len(a.data):
            REPORT.error(f"{rel}: labels {a.name}/{b.name} overlap")
        a.size = b.vram - a.vram
    if labels:
        labels[-1].size = len(labels[-1].data)
    for l in labels:
        if len(l.data) < l.size:
            l.data.extend(b"\0" * (l.size - len(l.data)))
    return labels


def symbol_addrs():
    out = {}
    for line in (DECOMP / "symbol_addrs.us.txt").read_text().splitlines():
        m = re.match(r"^\s*([A-Za-z_]\w*)\s*=\s*(0x[0-9A-Fa-f]+)\s*;(.*)$", line)
        if m and "type:func" not in m.group(3):
            out.setdefault(int(m.group(2), 16), m.group(1))
    return out


def split_labels(labels, lo, hi, names):
    """Split labels at `names` (address -> name) inside [lo, hi)."""
    out = []
    for l in labels:
        cuts = sorted(a for a in names if l.vram < a < l.vram + l.size and lo <= a < hi)
        if not cuts:
            if l.vram in names and names[l.vram] != l.name:
                REPORT.note(f"{l.name} also named {names[l.vram]}")
            out.append(l)
            continue
        bounds = [l.vram] + cuts + [l.vram + l.size]
        for s, e in zip(bounds, bounds[1:]):
            n = l.name if s == l.vram else names[s]
            nl = Label(n, s, l.seg, l.file, l.kind)
            off = s - l.vram
            nl.data = bytearray(l.data[off:e - l.vram])
            nl.size = e - s
            nl.widths = {o - off: v for o, v in l.widths.items() if off <= o < e - l.vram}
            out.append(nl)
    return out


def asm_data_files():
    asm = DECOMP / "asm"
    files = {seg: [] for seg in SEGMENTS}
    files["boot"] += sorted((asm / "data").glob("*.s"))  # not asm/data/libultra: exceptasm, replaced by the port
    files["global"] += sorted((asm / "global/data").glob("*.s"))
    files["global"] += [DECOMP / f for f in DATA_IN_TEXT]
    for o in OVERLAYS:
        files[o] += sorted((asm / o / "data").glob("*.s"))
    return files


def parse_asm_labels():
    """seg -> the asm data labels of decomp/asm (--ref), split at symbol_addrs.us.txt names inside DATA_IN_TEXT."""
    names = symbol_addrs()
    per_seg = {}
    for seg, fl in asm_data_files().items():
        out = []
        for f in fl:
            labels = parse_asm_file(f, seg)
            rel = str(f.relative_to(DECOMP))
            if rel in DATA_IN_TEXT:
                lo, hi = DATA_IN_TEXT[rel]
                labels = split_labels([l for l in labels if lo <= l.vram < hi], lo, hi, names)
                for l in labels:
                    l.kind = "rodata"
            out += labels
        per_seg[seg] = out
    return per_seg


def leaves(label):
    """-> [(off, size)]: directive widths; bytes without a directive stay single bytes."""
    if label.file in RAW_FILES:
        return [(o, 1) for o in range(label.size)]
    out = []
    pos = 0
    while pos < label.size:
        w = label.widths.get(pos)
        if w and pos + w <= label.size:
            out.append((pos, w))
            pos += w
        else:
            out.append((pos, 1))
            pos += 1
    return out


def fill_runs(label):
    """-> [(off, size, width)]: runs of consecutive same-width leaves; the boot fill swaps each run at its width
    (asm data is read natively at its directive widths by port/src/asm)."""
    lv = leaves(label)
    out = []
    i = 0
    while i < len(lv):
        off, size = lv[i]
        j = i
        while j < len(lv) and lv[j] == (off + (j - i) * size, size):
            j += 1
        out.append((off, (j - i) * size, size))
        i = j
    return out


def ref_asm_data():
    """port/ref/asm_data.txt -> ({seg: [Label]} without data, {seg: [(vram, size, width)]} ROM byte runs)."""
    labels = {seg: [] for seg in SEGMENTS}
    runs = defaultdict(list)
    path = REF / "asm_data.txt"
    if not path.is_file():
        REPORT.error("port/ref/asm_data.txt missing: run tools/prepare.py ref")
        return labels, runs
    for line in path.read_text().splitlines():
        f = line.split()
        if not f or f[0].startswith("#"):
            continue
        if f[0] == "label":
            _, seg, file, name, vram, size, kind = f
            l = Label(name, int(vram, 16), seg, file, kind)
            l.size = int(size, 16)
            labels[seg].append(l)
        elif f[0] == "fill":
            _, seg, vram, size, width = f
            runs[seg].append((int(vram, 16), int(size, 16), int(width)))
        else:
            REPORT.error(f"port/ref/asm_data.txt: bad line {line!r}")
    return labels, runs


def collect_labels(index, elves):
    """-> ({seg: [Label]}, {seg: [(vram, size, width)]}): the asm labels of port/ref, split at C-declared interior
    names, plus shadow snapshots; and the asm ROM byte runs."""
    labels, runs = ref_asm_data()
    per_seg = {}
    for seg in SEGMENTS:
        out = []
        for l in labels[seg]:
            if l.name in index.c_defined:
                REPORT.error(f"{l.name}: defined in C and in {l.file}")
                continue
            out.append(l)
        out = split_interior(out, seg, index, elves)
        out.extend(shadow_labels(seg))
        per_seg[seg] = out
    return per_seg, runs


def elf_bytes(elves, seg, vram, size):
    from elftools.elf.elffile import ELFFile
    with open(elves[seg].path, "rb") as fh:
        elf = ELFFile(fh)
        for sec in elf.iter_sections():
            a = sec["sh_addr"]
            if sec["sh_type"] == "SHT_PROGBITS" and a <= vram and vram + size <= a + sec["sh_size"]:
                return sec.data()[vram - a:vram - a + size]
    return None


def shadow_entries():
    """port/shadow_data.txt -> [(name, vram, size, seg)]."""
    path = ROOT / "port" / "shadow_data.txt"
    out = []
    if not path.exists():
        return out
    abs_syms = parse_undefined_syms()
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        name, size = line.split()
        vram = abs_syms.get(name)
        if vram is None:
            REPORT.error(f"shadow_data.txt: {name} has no address")
            continue
        out.append((name, vram, int(size, 0), "boot" if vram < OVL_VRAM[0] else "global"))
    return out


def shadow_labels(seg):
    """port/shadow_data.txt: `NAME SIZE` emits NAME as a read-only snapshot of the original big-endian bytes at its
    address (filled from the ROM at boot). For aliases into C objects whose native layout differs (e.g. byte reads
    inside a function-pointer table) and which the game never writes."""
    out = []
    for name, vram, size, lseg in shadow_entries():
        if lseg != seg:
            continue
        l = Label(name, vram, seg, f"shadow:{name}", "rodata")
        l.size = size
        REPORT.note(f"shadow {name}: 0x{size:X} original bytes")
        out.append(l)
    return out


def split_interior(labels, seg, index, elves):
    """C-declared names placed inside a generated label become labels of their own (contiguous, so the layout
    is unchanged); names placed exactly at a label's start become aliases of it. Candidates are the segment
    ELF's defined symbols plus absolute symbols the decomp links by value."""
    names = {l.name for l in labels}
    cuts = defaultdict(dict)
    starts = sorted((l.vram, i) for i, l in enumerate(labels))
    keys = [s[0] for s in starts]
    cands = {}
    for n, (addr, size, typ) in elves[seg].defined.items():
        if typ != "STT_FUNC":
            cands[n] = addr
    if seg in ("boot", "global"):
        for n, v in parse_undefined_syms().items():
            if 0x80000000 <= v < 0x80800000 and not (OVL_VRAM[0] <= v < OVL_VRAM[1]):
                if (seg == "boot") == (v < OVL_VRAM[0]):
                    cands.setdefault(n, v)
    else:
        # names the overlay's own link resolved by value inside its VRAM
        path = REF / f"undefined_syms_auto.{seg}.txt"
        if path.exists():
            for m in re.finditer(r"^\s*([A-Za-z_]\w*)\s*=\s*(0x[0-9A-Fa-f]+)\s*;", path.read_text(), re.M):
                v = int(m.group(2), 16)
                if OVL_VRAM[0] <= v < OVL_VRAM[1]:
                    cands.setdefault(m.group(1), v)
    for n, addr in cands.items():
        if n in names or n in index.c_defined or n not in index.declared:
            continue
        i = bisect.bisect_right(keys, addr) - 1
        if i < 0:
            continue
        l = labels[starts[i][1]]
        if l.vram == addr:
            l.aliases.append(n)
            REPORT.note(f"alias {n} = {l.name}")
        elif l.vram < addr < l.vram + l.size:
            cuts[starts[i][1]][addr] = n
    out = []
    for i, l in enumerate(labels):
        if i not in cuts:
            out.append(l)
            continue
        for a, n in sorted(cuts[i].items()):
            REPORT.note(f"interior label {n} split out of {l.name}+0x{a - l.vram:X}")
        out.extend(split_labels([l], l.vram, l.vram + l.size, cuts[i]))
    return out


# ----------------------------------------------------------------------------- emission

def c_ident(name):
    return "g_" + name


class Emitter:
    """Emits labels as u8 placeholders at their N64 alignment; their bytes come from the ROM fill (RomFill)."""

    def __init__(self, seg, clashes):
        self.seg = seg
        self.clashes = clashes

    def asm(self, name):
        return asm_name(name, self.seg, self.clashes)

    def emit_label(self, label, out):
        n = label.size
        align = 1
        while align < 16 and label.vram % (align * 2) == 0:
            align *= 2
        out.append(f"/* {label.name} @ 0x{label.vram:08X} size 0x{n:X} */")
        out.append(f'__attribute__((used, aligned({align}))) u8 {c_ident(label.name)}[{max(n, 1)}] '
                   f'__asm__("{self.asm(label.name)}") = {{0}};')
        for a in label.aliases:
            out.append(f'extern u8 alias_{a}[] __asm__("{a}") __attribute__((alias("{self.asm(label.name)}")));')

    def emit(self, labels):
        by_file = defaultdict(list)
        for l in labels:
            by_file[l.file].append(l)
        body = []
        for file, ls in by_file.items():
            ls.sort(key=lambda l: l.vram)
            sect_kind = "bss" if all(l.kind == "bss" for l in ls) else "data"
            tag = re.sub(r"\W", "_", file)
            body.append(f"\n/* ---- {file} ---- */")
            body.append(f'#pragma clang attribute push (__attribute__((section(".{sect_kind}.dkgen_{tag}"))), apply_to = variable(is_global))')
            lead = ls[0].vram % 16
            if lead:
                body.append(f"__attribute__((used, aligned(16))) u8 lead_{tag}[{lead}] = {{0}};")
            for l in ls:
                if sect_kind == "bss" and l.kind != "bss":
                    REPORT.error(f"{file}: mixed bss and data")
                self.emit_label(l, body)
            body.append("#pragma clang attribute pop")
        head = [f"/* generated by tools/gen_data.py: segment {self.seg}; do not edit */", '#include "port/port.h"']
        return "\n".join(head + body) + "\n"


class RomFill:
    """Objects whose bytes come from the ROM: emitted as placeholders, filled at boot (port/src/romfill.c) from the
    segment's inflated stream (text gzip member + data gzip member)."""

    def __init__(self, abs_syms):
        self.abs_syms = abs_syms
        self.pieces = defaultdict(list)  # seg -> [(stream offset, symbol, offset in symbol, size, width)]

    def add(self, seg, sym, sym_off, vram, size, width):
        if seg not in SEG_INDEX:
            REPORT.error(f"rom fill: {sym} in {seg} holds ROM bytes; only global and overlays are filled")
            return
        self.pieces[seg].append((vram - self.abs_syms[f"{seg}_VRAM"], sym, sym_off, size, width))

    def write(self):
        externs = {}
        body = []
        segs = []
        for seg in SEG_INDEX:
            ps = sorted(self.pieces.get(seg, []))
            if not ps:
                continue
            body.append(f"static const PortRomFill sFill_{seg}[] = {{")
            for off, sym, sym_off, size, width in ps:
                ident = "rf_" + re.sub(r"\W", "_", sym)
                externs.setdefault(ident, f'extern u8 {ident}[] __asm__("{sym}");')
                body.append(f"    {{ {ident} + 0x{sym_off:X}, 0x{off:X}u, 0x{size:X}u, {width}u }},")
            body.append("};")
            stream = self.abs_syms[f"{seg}_BSS_START"] - self.abs_syms[f"{seg}_VRAM"]
            segs.append(f"    {{ {SEG_INDEX[seg]}u, 0x{stream:X}u, sFill_{seg}, {len(ps)}u }}, /* {seg} */")
        lines = ["/* generated by tools/gen_data.py: objects filled from the ROM at boot (port/src/romfill.c) */",
                 '#include "port_internal.h"', ""]
        lines += list(externs.values()) + [""] + body
        lines += ["const PortRomFillSeg gPortRomFillSegs[] = {"] + (segs or ["    { 0 },"]) + ["};",
                  f"const u32 gPortRomFillSegCount = {len(segs)}u;"]
        write_if_changed(GEN / "rom_fill.c", "\n".join(lines) + "\n")
        REPORT.note(f"rom fill: {sum(len(p) for p in self.pieces.values())} pieces in {len(segs)} segments")


def add_label_fills(fills, seg, labels, runs, clashes):
    """Shadow snapshots, and the asm ROM byte runs cut at the labels they span (labels are split by split_interior
    after port/ref recorded the runs)."""
    asm = sorted((l for l in labels if not l.file.startswith("shadow:")), key=lambda l: l.vram)
    for l in labels:
        if l.file.startswith("shadow:"):
            fills.add(seg, asm_name(l.name, seg, clashes), 0, l.vram, l.size, 1)
    starts = [l.vram for l in asm]
    for v, n, w in runs:
        covered = 0
        i = max(bisect.bisect_right(starts, v) - 1, 0)
        while i < len(asm) and asm[i].vram < v + n:
            l = asm[i]
            s, e = max(v, l.vram), min(v + n, l.vram + l.size)
            if s < e:
                if (s - v) % w or (e - s) % w:
                    REPORT.error(f"rom fill: {l.name} splits a {w}-byte run at 0x{s:08X}")
                fills.add(seg, asm_name(l.name, seg, clashes), s - l.vram, s, e - s, w)
                covered += e - s
            i += 1
        if covered != n:
            REPORT.error(f"rom fill: run 0x{v:08X}+0x{n:X} not covered by labels")


def gen_layout_check(per_seg, clashes):
    lines = ["/* generated by tools/gen_data.py */", '#include "port/port.h"',
             "#include <stdio.h>", "#include <stdlib.h>", ""]
    decls = set()
    checks = []
    total = 0
    for seg, labels in per_seg.items():
        by_file = defaultdict(list)
        for l in labels:
            by_file[l.file].append(l)
        for file, ls in by_file.items():
            ls.sort(key=lambda l: l.vram)
            total += len(ls)
            for a, b in zip(ls, ls[1:]):
                an, bn = asm_name(a.name, seg, clashes), asm_name(b.name, seg, clashes)
                decls.add(f'extern u8 c_{an}[] __asm__("{an}");')
                decls.add(f'extern u8 c_{bn}[] __asm__("{bn}");')
                checks.append(f'    if ((u32)c_{bn} - (u32)c_{an} != 0x{b.vram - a.vram:X}u) fail("{a.name}/{b.name}");')
    lines += sorted(decls)
    lines += ["", "static void fail(const char *what) {",
              '    printf("PORT: generated data layout broken at %s\\n", what);', "    abort();", "}", "",
              "void portGenLayoutCheck(void) {"]
    lines += checks
    lines += [f'    printf("PORT: layout ok %d labels\\n", {total});', "}"]
    write_if_changed(GEN / "gen_layout_check.c", "\n".join(lines) + "\n")


def gen_rom_layout(abs_syms):
    order = ["boot", "global", "multiplayer", "minecart", "bonus", "race", "critter", "boss", "arcade",
             "jetpac", "menu", "seg11", "seg12", "seg13", "seg14", "seg15", "seg16", "seg17"]
    rows = []
    for n in order:
        s, e = abs_syms.get(f"{n}_ROM_START"), abs_syms.get(f"{n}_ROM_END")
        if s is None or e is None:
            REPORT.error(f"rom layout: missing {n}_ROM_START/END")
            s = e = 0
        rows.append(f"    {{ 0x{s:08X}u, 0x{e:08X}u }}, /* {n} */")
    text = "\n".join(["/* generated by tools/gen_data.py (order of src/boot/main.c SEG()) */",
                      '#include "port/port.h"', "", "const u32 gPortSegRom[18][2] = {"] + rows + ["};"]) + "\n"
    write_if_changed(GEN / "rom_layout.c", text)


OVL_IDS = {"menu": 2, "multiplayer": 3, "minecart": 4, "race": 5, "critter": 6, "boss": 7, "bonus": 8,
           "arcade": 9, "jetpac": 10}


def overlay_functions_at(elves, index, addr):
    """[(overlay, C name)] of the C-defined overlay functions at `addr`."""
    out = []
    for ovl in OVERLAYS:
        for n, (a, size, typ) in elves[ovl].defined.items():
            if a == addr and typ == "STT_FUNC" and n in index.func_defined:
                out.append((ovl, n))
    return out


def gen_ovl_entry(elves, clashes, index, ovl_fns):
    entries = {}
    for ovl in ("arcade", "jetpac"):
        for n, (addr, size, typ) in elves[ovl].defined.items():
            if addr == 0x80024000 and typ == "STT_FUNC":
                entries[ovl] = asm_name(n, ovl, clashes)
    lines = ["/* generated by tools/gen_data.py */", '#include "port/port.h"', "#include <stdio.h>",
             "#include <stdlib.h>", "", "extern int gPortCurrentOverlay;"]
    for ovl, fn in entries.items():
        lines.append(f'extern void ovl_{ovl}_entry(void) __asm__("{fn}");')
    lines += ["", "void portOverlayEntry(void) {", "    switch (gPortCurrentOverlay) {"]
    for ovl in entries:
        lines.append(f"    case {OVL_IDS[ovl]}: ovl_{ovl}_entry(); return;")
    lines += ['    default: printf("PORT: overlay entry with overlay %d\\n", gPortCurrentOverlay); abort();',
              "    }", "}"]
    # Non-overlay code calls overlay functions by their VRAM (func_800XXXXX declared, never defined in C):
    # dispatch to whichever overlay is active.
    for name in sorted(index.func):
        m = re.match(r"^func_(800[2-4][0-9A-F]{4})$", name)
        if not m or name in index.func_defined:
            continue
        addr = int(m.group(1), 16)
        if addr == 0x80024000:
            continue
        targets = overlay_functions_at(elves, index, addr)
        if not targets:
            REPORT.error(f"overlay dispatcher {name}: no overlay defines a function at 0x{addr:08X}")
            continue
        sigs = {index.func.get(n) for _, n in targets}
        if len(sigs) != 1 or None in sigs:
            REPORT.error(f"overlay dispatcher {name}: targets disagree on their signature {sigs}")
            continue
        ret, args, var = sigs.pop()
        if index.func.get(name) != (ret, args, var):
            REPORT.note(f"overlay dispatcher {name}: callers declare {index.func.get(name)}, target is {(ret, args, var)}")
        params = [] if args == ("void",) else [f"{t} a{i}" for i, t in enumerate(args)]
        call_args = ", ".join(f"a{i}" for i in range(len(params)))
        lines.append("")
        for ovl, n in targets:
            lines.append(f'extern {ret} ovl_{ovl}_{name}({", ".join(args)}) __asm__("{asm_name(n, ovl, clashes)}");')
        lines.append(f"{ret} {name}({', '.join(params) or 'void'}) {{")
        lines.append("    switch (gPortCurrentOverlay) {")
        for ovl, _ in targets:
            ret_kw = "return " if ret != "void" else ""
            tail = "" if ret != "void" else " return;"
            lines.append(f"    case {OVL_IDS[ovl]}: {ret_kw}ovl_{ovl}_{name}({call_args});{tail}")
        lines.append(f'    default: printf("PORT: {name} called with overlay %d\\n", gPortCurrentOverlay); abort();')
        lines += ["    }", "}"]
        REPORT.note(f"overlay dispatcher {name}: {', '.join(o for o, _ in targets)}")
    # OVL_FN(type, addr) thunks: called through the cast type; each overlay's function at addr gets the thunk's
    # arguments it declares (0 past the thunk's own, which were stale registers on the N64)
    for tok, (ret, args) in sorted(ovl_fns.items()):
        addr = int(tok, 16)
        name = f"port_ovlfn_{tok}"
        targets = overlay_functions_at(elves, index, addr)
        if not targets:
            REPORT.error(f"OVL_FN {tok}: no overlay defines a function there")
            continue
        params = [] if args == ("void",) else [f"{t} a{i}" for i, t in enumerate(args)]
        lines.append("")
        for ovl, n in targets:
            if index.func.get(n) is None:
                REPORT.error(f"OVL_FN {tok}: {ovl} {n} has no lowerable prototype")
                continue
            tret, targs, _ = index.func[n]
            lines.append(f'extern {tret} ovlfn_{ovl}_{tok}({", ".join(targs)}) __asm__("{asm_name(n, ovl, clashes)}");')
        lines.append(f"{ret} {name}({', '.join(params) or 'void'}) {{")
        lines.append("    switch (gPortCurrentOverlay) {")
        for ovl, n in targets:
            if index.func.get(n) is None:
                continue
            tret, targs, _ = index.func[n]
            nargs = 0 if targs == ("void",) else len(targs)
            call = f"ovlfn_{ovl}_{tok}({', '.join(f'a{i}' if i < len(params) else '0' for i in range(nargs))})"
            if ret == "void":
                lines.append(f"    case {OVL_IDS[ovl]}: {call}; return;")
            elif tret == "void":
                lines.append(f"    case {OVL_IDS[ovl]}: {call}; return 0;")
            else:
                lines.append(f"    case {OVL_IDS[ovl]}: return ({ret}){call};")
        lines.append(f'    default: printf("PORT: overlay function {tok} called with overlay %d\\n", '
                     f'gPortCurrentOverlay); abort();')
        lines += ["    }", "}"]
        REPORT.note(f"OVL_FN {tok}: {', '.join(f'{o} {n}' for o, n in targets)}")
    write_if_changed(GEN / "ovl_entry.c", "\n".join(lines) + "\n")


def find_container(objects, starts, addr, seg):
    segs = [seg, "global", "boot"] if seg in OVERLAYS else ["global", "boot"]
    for s in segs:
        objs = objects[s]
        i = bisect.bisect_right(starts[s], addr) - 1
        # walk back over objects starting at or before addr; the innermost (latest start) containing wins
        while i >= 0 and addr - objs[i][0] < 0x100000:
            start, end, n = objs[i]
            if start <= addr < end:
                return n, addr - start
            i -= 1
    return None


def report_unresolved(index, per_seg, elves, abs_syms):
    """C-declared variables defined by neither C nor generated data: interior labels of C objects
    (fix by patch: `.set NAME, PARENT+OFF` at the native offset) or absolute symbols (PORT_ABS_X)."""
    objects = defaultdict(list)
    for seg, e in elves.items():
        for n, (addr, size, typ) in e.defined.items():
            if typ == "STT_OBJECT" or (typ == "STT_NOTYPE" and not SEG_SYM_RE.search(n)):
                objects[seg].append((addr, addr + max(size, 1), n))
    for seg in objects:
        objects[seg].sort()
    starts = {seg: [o[0] for o in objs] for seg, objs in objects.items()}
    generated = {l.name for ls in per_seg.values() for l in ls} | {a for ls in per_seg.values() for l in ls for a in l.aliases}
    for name in sorted(index.declared - set(index.c_defined) - generated):
        for seg in SEGMENTS:
            if name in elves[seg].defined:
                c = find_container(objects, starts, elves[seg].defined[name][0], seg)
                where = f"interior of {c[0]}+0x{c[1]:X}" if c and c[0] != name else "no containing object"
                REPORT.note(f"UNRESOLVED {name}: defined in {seg}, {where}")
                break
        else:
            if name in abs_syms:
                v = abs_syms[name]
                c = None
                if 0x80000000 <= v < 0x80800000:
                    for seg in SEGMENTS:
                        c = c or find_container(objects, starts, v, seg)
                where = f", interior of {c[0]}+0x{c[1]:X}" if c else ""
                REPORT.note(f"UNRESOLVED {name}: absolute 0x{v:08X} (PORT_ABS_{name}){where}")


def run_full(elves, clashes, abs_syms, ovl_fns, fills):
    index = Index().build()
    per_seg, runs = collect_labels(index, elves)
    for seg in SEGMENTS:
        add_label_fills(fills, seg, per_seg[seg], runs.get(seg, []), clashes)
    for seg in SEGMENTS:
        write_if_changed(GEN / f"data_{seg}.c", Emitter(seg, clashes).emit(per_seg[seg]))
    gen_layout_check(per_seg, clashes)
    gen_rom_layout(abs_syms)
    gen_ovl_entry(elves, clashes, index, ovl_fns)
    fills.write()
    n = sum(len(v) for v in per_seg.values())
    REPORT.note(f"total labels: {n}")
    report_unresolved(index, per_seg, elves, abs_syms)
    return n


# ----------------------------------------------------------------------------- reference manifest (port/ref)

def check_ref_fingerprint():
    path = REF / "decomp_fingerprint.txt"
    if not path.is_file():
        REPORT.error("port/ref/decomp_fingerprint.txt missing: run tools/prepare.py ref with the reference build")
        return
    if [l for l in path.read_text().splitlines() if l and not l.startswith("#")] != [decomp_fingerprint()]:
        REPORT.error("port/ref was generated from a different decomp (decomp/ changed): "
                     "run tools/prepare.py ref with the reference build")


def gen_ref():
    """--ref: port/ref from the reference decomp build (ELFs, splat's auto symbols, asm), with every byte run the
    boot fill copies checked against the segments' inflated ROM streams (decomp/bin/segments). Written to a staging
    directory that replaces port/ref only when no check failed, so a failed run never stamps a new fingerprint."""
    global REF
    final = REF
    final.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".ref-", dir=final.parent))
    errors = REPORT.errors
    REF = stage
    try:
        write_ref()
    except BaseException:
        shutil.rmtree(stage)
        raise
    finally:
        REF = final
    if REPORT.errors > errors:
        shutil.rmtree(stage)
        REPORT.note(f"ref: {final.relative_to(ROOT)} left unchanged")
        return
    stage.chmod(0o755)
    if final.exists():
        shutil.rmtree(final)
    stage.rename(final)


def write_ref():
    """gen_ref's body: writes the manifest into REF (the staging directory)."""
    elves = load_elves(ref=False)
    for seg, e in elves.items():
        lines = [f"# {seg} ELF symbols of the reference decomp build (tools/gen_data.py --ref): name addr size type; "
                 "ABS = absolute; ELF symbol-table order (gen keeps the ELF's choice among names at one address)"]
        lines += [f"{n} 0x{a:08X} 0x{s:X} {t}" for n, (a, s, t) in e.defined.items()]
        lines += [f"{n} 0x{a:08X} 0x0 ABS" for n, a in e.absolute.items() if SEG_SYM_RE.search(n)]
        write_if_changed(REF / f"symbols.{seg}.txt", "\n".join(lines) + "\n")
    for p in sorted((DECOMP / "build").glob("undefined_syms_auto.*.txt")):
        (REF / p.name).write_bytes(p.read_bytes())
    abs_syms = abs_symbols(elves)

    lines = ["# asm-only data labels and the ROM byte runs filled at boot (tools/gen_data.py --ref): "
             "label seg file name vram size kind / fill seg vram size width"]
    runs = []
    for seg, labels in parse_asm_labels().items():
        for l in labels:
            lines.append(f"label {seg} {l.file} {l.name} 0x{l.vram:08X} 0x{l.size:X} {l.kind}")
            if l.kind != "bss" and any(l.data):
                runs += [(seg, l.vram + off, l.data[off:off + n], w) for off, n, w in fill_runs(l)]
    lines += [f"fill {seg} 0x{v:08X} 0x{len(b):X} {w}" for seg, v, b, w in runs]
    write_if_changed(REF / "asm_data.txt", "\n".join(lines) + "\n")

    streams = {}
    for seg in SEG_INDEX:
        path = DECOMP / "bin" / "segments" / stream_file(seg)
        base = abs_syms[f"{seg}_VRAM"]
        n = abs_syms[f"{seg}_BSS_START"] - base
        if not path.is_file():
            REPORT.error(f"ref: {path.relative_to(ROOT)} missing (tools/dk make setup in decomp/)")
            continue
        data = path.read_bytes()
        if len(data) != n:
            REPORT.error(f"ref: {seg} stream is 0x{len(data):X} bytes, expected 0x{n:X}")
            continue
        streams[seg] = (base, data)

    def check(what, seg, vram, blob):
        if seg not in SEG_INDEX:
            REPORT.error(f"ref: {what} is in {seg}; only global and overlays are filled from the ROM")
        elif seg in streams:
            base, data = streams[seg]
            off = vram - base
            if off < 0 or data[off:off + len(blob)] != bytes(blob):
                REPORT.error(f"ref: {what} at 0x{vram:08X} differs from the ROM stream")

    for seg, v, b, w in runs:
        check("asm fill run", seg, v, b)
    n_inc = 0
    for seg, name, vram, size, width, file in data_inc_rows():
        if seg not in SEG_INDEX:
            REPORT.note(f"ref: data_inc {name} ({seg}) not checked: not a filled segment")
            continue
        src = DECOMP / "assets" / "inc" / seg / f"{name}.inc.c"
        if not src.is_file():
            REPORT.error(f"data_inc: {src.relative_to(ROOT)} missing (run `tools/dk make split` in decomp/)")
            continue
        blob = b"".join(int(h, 16).to_bytes(width, "big") for h in re.findall(r"0x([0-9A-Fa-f]+)", src.read_text()))
        if len(blob) != size:
            REPORT.error(f"ref: data_inc {name} has 0x{len(blob):X} bytes, expected 0x{size:X}")
        check(f"data_inc {name}", seg, vram, blob)
        n_inc += 1
    for name, vram, size, seg in shadow_entries():
        data = elf_bytes(elves, seg, vram, size)
        if data is None:
            REPORT.error(f"shadow_data.txt: {name} not inside a {seg} section")
            continue
        check(f"shadow {name}", seg, vram, data)
    (REF / "decomp_fingerprint.txt").write_text(
        "# decomp_fingerprint() of the decomp the files beside this were generated from\n" + decomp_fingerprint() + "\n")
    REPORT.note(f"ref: {len(runs)} asm runs, {n_inc} data_inc rows and the shadow entries match the ROM streams")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--headers", action="store_true", help="only the include/ headers")
    ap.add_argument("--ref", action="store_true",
                    help="regenerate port/ref from the reference decomp build (decomp/{asm,assets,bin,build})")
    args = ap.parse_args()
    GEN.mkdir(parents=True, exist_ok=True)
    if args.ref:
        gen_ref()
        msg = "gen: port/ref left unchanged" if REPORT.errors else "gen: reference manifest written to port/ref"
    else:
        check_ref_fingerprint()
        elves = load_elves()
        clashes = overlay_clashes(elves)
        abs_syms = abs_symbols(elves)
        fills = RomFill(abs_syms)
        gen_overlay_headers(clashes)
        gen_port_abs(abs_syms)
        ovl_fns = ovl_fn_uses()
        gen_ovl_fn_header(ovl_fns)
        gen_data_inc(fills, clashes)
        if args.headers:
            msg = "gen: headers written"
        else:
            msg = f"gen: {run_full(elves, clashes, abs_syms, ovl_fns, fills)} labels"
    (GEN / "report.txt").write_text("\n".join(REPORT.lines) + "\n")
    print(f"{msg}, {REPORT.errors} errors (build/gen/report.txt)")
    return 1 if REPORT.errors else 0


if __name__ == "__main__":
    sys.exit(main())
