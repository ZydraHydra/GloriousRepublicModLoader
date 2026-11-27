#!/usr/bin/env python3
"""Automated .rdata string XREF & MSVC x64 RTTI anchor harvester for SOVIET64.exe."""

from __future__ import annotations

import argparse
import bisect
import json
import re
import struct
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

try:
    import pefile  # type: ignore
    import capstone  # type: ignore
    from capstone import x86  # type: ignore
except ImportError as exc:
    missing = getattr(exc, "name", "pefile/capstone")
    print(
        f"Error: Missing Python dependency '{missing}'.\n"
        "Use the project venv:\n"
        "  .venv\\Scripts\\python scripts\\harvest_anchors.py ...",
        file=sys.stderr,
    )
    sys.exit(2)


DEFAULT_STRINGS = [
    "header.bin",
    "buildings.bin",
    "vehicles.bin",
    "citizens.bin",
    "COST_RUB",
    "COST_USD",
    "media_soviet",
    "script.ini",
]

DEFAULT_RTTI_KEYWORDS = [
    "Building",
    "Vehicle",
    "Citizen",
    "Construction",
    "Resource",
    "Camera",
]

CATEGORY_ANCHORS: Dict[str, Dict[str, List[str]]] = {
    "LogisticsAndRouting": {
        "strings": ["fuel_station", "repair", "distribution", "line_", "waypoint"],
        "rtti": ["Distribution", "Line", "Waypoint"],
    },
    "RailAndTerrain": {
        "strings": ["semaphore", "block", "train", "heightmap"],
        "rtti": ["Rail", "Semaphore", "Terrain"],
    },
    "UtilitiesAndClimate": {
        "strings": ["substation", "heating", "boiler", "temperature", "snow"],
        "rtti": ["Electric", "Heating", "Weather"],
    },
    "ProgressionAndRules": {
        "strings": ["realistic", "research", "university", "space", "rocket"],
        "rtti": ["Research", "Difficulty"],
    },
    "WorkshopAndVFS": {
        "strings": ["workshop", "784150", "workshopconfig.ini"],
        "rtti": ["Workshop"],
    },
}


class PeAnalyzer:
    def __init__(self, exe_path: Path) -> None:
        self.exe_path = exe_path
        self.raw = exe_path.read_bytes()
        self.pe = pefile.PE(data=self.raw, fast_load=False)
        self.image_base: int = self.pe.OPTIONAL_HEADER.ImageBase
        self.size_of_image: int = self.pe.OPTIONAL_HEADER.SizeOfImage

        self.sections: List[Tuple[str, int, int, bytes]] = []
        self.text_va = 0
        self.text_bytes = b""
        self.rdata_va = 0
        self.rdata_bytes = b""

        for sec in self.pe.sections:
            name = sec.Name.rstrip(b"\x00").decode("ascii", errors="ignore")
            va = sec.VirtualAddress
            vsz = sec.Misc_VirtualSize or sec.SizeOfRawData
            off = sec.PointerToRawData
            rsz = min(vsz, sec.SizeOfRawData, len(self.raw) - off)
            data = self.raw[off : off + rsz]
            self.sections.append((name, va, rsz, data))
            if name == ".text":
                self.text_va = va
                self.text_bytes = data
            elif name == ".rdata":
                self.rdata_va = va
                self.rdata_bytes = data

        if not self.text_bytes:
            raise RuntimeError(f"Missing .text section in {exe_path}")
        if not self.rdata_bytes:
            raise RuntimeError(f"Missing .rdata section in {exe_path}")

        self.pdata_begins: List[int] = []
        self.pdata_ends: List[int] = []
        self._load_pdata()

    def _load_pdata(self) -> None:
        exc_dir = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]
        ]
        if not exc_dir.VirtualAddress or not exc_dir.Size:
            return
        raw_off = self.rva_to_raw(exc_dir.VirtualAddress)
        if raw_off is None:
            return
        count = min(exc_dir.Size // 12, (len(self.raw) - raw_off) // 12)
        entries: List[Tuple[int, int]] = []
        for i in range(count):
            b, e, _ = struct.unpack_from("<III", self.raw, raw_off + i * 12)
            if 0 < b < e <= self.size_of_image:
                entries.append((b, e))
        entries.sort(key=lambda x: x[0])
        self.pdata_begins = [x[0] for x in entries]
        self.pdata_ends = [x[1] for x in entries]

    def rva_to_raw(self, rva: int) -> Optional[int]:
        for _, va, rsz, _ in self.sections:
            if va <= rva < va + rsz:
                for sec in self.pe.sections:
                    if sec.VirtualAddress == va:
                        return sec.PointerToRawData + (rva - va)
        return None

    def is_text_rva(self, rva: int) -> bool:
        return self.text_va <= rva < self.text_va + len(self.text_bytes)

    def is_rdata_rva(self, rva: int) -> bool:
        return self.rdata_va <= rva < self.rdata_va + len(self.rdata_bytes)

    def lookup_function_bounds(self, rva: int) -> Optional[Tuple[int, int]]:
        idx = bisect.bisect_right(self.pdata_begins, rva) - 1
        if idx < 0:
            return None
        b = self.pdata_begins[idx]
        e = self.pdata_ends[idx]
        if b <= rva < e:
            return (b, e)
        return None


def scan_string_xrefs(
    pe: PeAnalyzer,
    targets: List[str],
    filter_kw: Optional[str],
    max_xrefs_per_str: int = 8,
) -> List[Dict[str, Any]]:
    # 1. Locate exact/substring matches in .rdata (ASCII & UTF-16LE)
    str_hits: Dict[int, str] = {}
    for s in targets:
        if filter_kw and filter_kw.lower() not in s.lower():
            continue
        needle = s.encode("ascii")
        pos = 0
        while True:
            idx = pe.rdata_bytes.find(needle, pos)
            if idx == -1:
                break
            # Prefer start of null-terminated string
            start = idx
            while start > 0 and 32 <= pe.rdata_bytes[start - 1] < 127 and (idx - start) < 64:
                start -= 1
            end = idx + len(needle)
            while end < len(pe.rdata_bytes) and 32 <= pe.rdata_bytes[end] < 127 and (end - start) < 120:
                end += 1
            if end < len(pe.rdata_bytes) and pe.rdata_bytes[end] == 0:
                text_val = pe.rdata_bytes[start:end].decode("ascii", errors="replace")
                str_hits[pe.rdata_va + start] = text_val
            pos = idx + 1

    if not str_hits:
        return []

    # 2. Scan .text for RIP-relative LEA/MOV [rip+disp32] pointing to any str_hit RVA
    #    Opcodes: [REX] 8D /r (ModRM & 0xC7 == 0x05)
    tbytes = pe.text_bytes
    tlen = len(tbytes)
    xrefs_by_str: Dict[int, List[int]] = {rva: [] for rva in str_hits}

    i = 0
    while i + 6 <= tlen:
        b0 = tbytes[i]
        q = i + 1 if 0x40 <= b0 <= 0x4F else i
        if q + 6 <= tlen and tbytes[q] == 0x8D and (tbytes[q + 1] & 0xC7) == 0x05:
            disp = struct.unpack_from("<i", tbytes, q + 2)[0]
            next_rva = pe.text_va + q + 6
            target_rva = next_rva + disp
            if target_rva in xrefs_by_str:
                xrefs_by_str[target_rva].append(pe.text_va + i)
                i = q + 6
                continue
        i += 1

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    results: List[Dict[str, Any]] = []
    for str_rva, str_val in sorted(str_hits.items()):
        sites = xrefs_by_str.get(str_rva, [])
        if not sites:
            continue
        func_entries: List[Dict[str, Any]] = []
        seen_funcs = set()
        for site_rva in sites[:max_xrefs_per_str]:
            bounds = pe.lookup_function_bounds(site_rva)
            if bounds:
                func_rva, func_end = bounds
            else:
                func_rva, func_end = site_rva, min(site_rva + 0x100, pe.text_va + tlen)
            if func_rva in seen_funcs:
                continue
            seen_funcs.add(func_rva)

            # Disassemble function to collect candidate RIP-relative global singletons
            off0 = func_rva - pe.text_va
            span = min(func_end - func_rva, 0x600)
            singletons: List[Dict[str, str]] = []
            seen_targets = set()
            for insn in md.disasm(tbytes[off0 : off0 + span], func_rva):
                for op in insn.operands:
                    if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP and insn.disp_size == 4:
                        tgt = insn.address + insn.size + op.mem.disp
                        if not pe.is_text_rva(tgt) and not pe.is_rdata_rva(tgt) and 0 < tgt < pe.size_of_image:
                            if tgt not in seen_targets:
                                seen_targets.add(tgt)
                                singletons.append(
                                    {
                                        "insn_rva": f"0x{insn.address:X}",
                                        "mnemonic": f"{insn.mnemonic} {insn.op_str}",
                                        "global_rva": f"0x{tgt:X}",
                                    }
                                )
            func_entries.append(
                {
                    "xref_rva": f"0x{site_rva:X}",
                    "func_rva": f"0x{func_rva:X}",
                    "func_size": f"0x{func_end - func_rva:X}",
                    "candidate_globals": singletons[:12],
                }
            )

        results.append(
            {
                "string": str_val,
                "string_rva": f"0x{str_rva:X}",
                "xref_count": len(sites),
                "functions": func_entries,
            }
        )
    return results


def scan_rtti_vtables(
    pe: PeAnalyzer,
    keywords: List[str],
    filter_kw: Optional[str],
) -> List[Dict[str, Any]]:
    # 1. Find MSVC RTTITypeDescriptor names: ".?AV<Name>@@" in .data/.rdata
    active_kws = [filter_kw.lower()] if filter_kw else [k.lower() for k in keywords]
    type_descs: Dict[int, str] = {}

    for _, sec_va, _, sec_bytes in pe.sections:
        for m in re.finditer(rb"\.\?AV[A-Za-z0-9_:@<>]+@@\x00", sec_bytes):
            name = m.group(0)[:-1].decode("ascii", errors="replace")
            lo = name.lower()
            if not any(kw in lo for kw in active_kws):
                continue
            # TypeDescriptor layout (x64): [0x00] pVFTable (8B), [0x08] spare (8B), [0x10] name[]
            name_rva = sec_va + m.start()
            td_rva = name_rva - 0x10
            if td_rva > 0:
                type_descs[td_rva] = name

    if not type_descs:
        return []

    # 2. Scan .rdata for RTTICompleteObjectLocator (signature == 1, 36 bytes on x64):
    #    +0x00 signature (1), +0x04 offset, +0x08 cdOffset, +0x0C pTypeDescriptor (RVA),
    #    +0x10 pClassDescriptor (RVA), +0x14 pSelf (RVA)
    rdata = pe.rdata_bytes
    rva0 = pe.rdata_va
    col_by_va: Dict[int, Tuple[int, str]] = {}

    for off in range(0, len(rdata) - 24, 4):
        sig, _, _, p_td, _, p_self = struct.unpack_from("<IIIIII", rdata, off)
        col_rva = rva0 + off
        if sig == 1 and p_self == col_rva and p_td in type_descs:
            col_va = pe.image_base + col_rva
            col_by_va[col_va] = (col_rva, type_descs[p_td])

    # 3. Scan .rdata for vtable[-1] == col_va (8-byte aligned pointers)
    results: List[Dict[str, Any]] = []
    for off in range(0, len(rdata) - 16, 8):
        ptr = struct.unpack_from("<Q", rdata, off)[0]
        if ptr in col_by_va:
            col_rva, cls_name = col_by_va[ptr]
            vtable_rva = rva0 + off + 8
            # Count consecutive .text pointers in the vtable
            m_count = 0
            v_off = off + 8
            while v_off + 8 <= len(rdata) and m_count < 256:
                fn_va = struct.unpack_from("<Q", rdata, v_off)[0]
                fn_rva = fn_va - pe.image_base
                if not pe.is_text_rva(fn_rva):
                    break
                m_count += 1
                v_off += 8
            if m_count > 0:
                first_fn = struct.unpack_from("<Q", rdata, off + 8)[0] - pe.image_base
                results.append(
                    {
                        "class_name": cls_name,
                        "vtable_rva": f"0x{vtable_rva:X}",
                        "col_rva": f"0x{col_rva:X}",
                        "method_count": m_count,
                        "first_method_rva": f"0x{first_fn:X}",
                    }
                )
    results.sort(key=lambda r: r["class_name"])
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description="GRML .rdata String XREF & MSVC RTTI Anchor Harvester")
    parser.add_argument("--exe", required=True, type=Path, help="Path to SOVIET64.exe")
    parser.add_argument("--strings", action="store_true", help="Scan .rdata engine strings & .text XREFs")
    parser.add_argument("--rtti", action="store_true", help="Enumerate MSVC x64 RTTI vtables")
    parser.add_argument(
        "--category",
        type=str,
        default=None,
        help=f"Domain category filter ({', '.join(CATEGORY_ANCHORS.keys())}, or 'all')",
    )
    parser.add_argument("--filter", type=str, default=None, help="Case-insensitive keyword filter")
    parser.add_argument("--json", action="store_true", help="Emit JSON output")
    args = parser.parse_args()

    if not args.strings and not args.rtti:
        args.strings = True
        args.rtti = True

    if not args.exe.exists():
        print(f"Error: Executable not found: {args.exe}", file=sys.stderr)
        return 2

    string_targets = list(DEFAULT_STRINGS)
    rtti_keywords = list(DEFAULT_RTTI_KEYWORDS)

    if args.category:
        if args.category.lower() == "all":
            string_targets = []
            rtti_keywords = []
            for cat in CATEGORY_ANCHORS.values():
                string_targets.extend(cat["strings"])
                rtti_keywords.extend(cat["rtti"])
            string_targets.extend(DEFAULT_STRINGS)
            rtti_keywords.extend(DEFAULT_RTTI_KEYWORDS)
        else:
            matched_cat = next(
                (k for k in CATEGORY_ANCHORS if k.lower() == args.category.lower()),
                None,
            )
            if not matched_cat:
                valid = ", ".join(list(CATEGORY_ANCHORS.keys()) + ["all"])
                print(f"Error: Unknown category '{args.category}'. Valid: {valid}", file=sys.stderr)
                return 2
            string_targets = list(CATEGORY_ANCHORS[matched_cat]["strings"])
            rtti_keywords = list(CATEGORY_ANCHORS[matched_cat]["rtti"])

    pe = PeAnalyzer(args.exe)
    payload: Dict[str, Any] = {"exe": str(args.exe)}
    if args.category:
        payload["category"] = args.category

    if args.strings:
        targets = list(string_targets)
        if args.filter and all(args.filter.lower() not in t.lower() for t in targets):
            targets.append(args.filter)
        payload["string_xrefs"] = scan_string_xrefs(pe, targets, args.filter)

    if args.rtti:
        payload["rtti_vtables"] = scan_rtti_vtables(pe, rtti_keywords, args.filter)

    if args.json:
        print(json.dumps(payload, indent=2))
        return 0

    if "string_xrefs" in payload:
        print(f"=== String XREFs ({len(payload['string_xrefs'])} matches) ===")
        for item in payload["string_xrefs"]:
            print(f"\n[{item['string_rva']}] \"{item['string']}\" (xrefs={item['xref_count']})")
            for fn in item["functions"]:
                print(f"  func @ {fn['func_rva']} (size {fn['func_size']}, xref @ {fn['xref_rva']})")
                for g in fn["candidate_globals"]:
                    print(f"    -> {g['global_rva']} via {g['insn_rva']}: {g['mnemonic']}")

    if "rtti_vtables" in payload:
        print(f"\n=== MSVC RTTI Vtables ({len(payload['rtti_vtables'])} matches) ===")
        for vt in payload["rtti_vtables"]:
            print(
                f"  {vt['class_name']:<48} vtable={vt['vtable_rva']:<10} "
                f"methods={vt['method_count']:<4} first_fn={vt['first_method_rva']}"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
