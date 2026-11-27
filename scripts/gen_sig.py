#!/usr/bin/env python3
"""Automated x86-64 AOB signature generator & batch verifier for grml_offsets.ini."""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

try:
    import pefile  # type: ignore
    import capstone  # type: ignore
    from capstone import x86  # type: ignore
except ImportError as exc:
    missing = getattr(exc, "name", "pefile/capstone")
    print(
        f"Error: Missing Python dependency '{missing}'.\n"
        "Install dependencies with:\n"
        "  pip install -r scripts/requirements.txt\n"
        "or use the project venv:\n"
        "  .venv\\Scripts\\python scripts\\gen_sig.py ...",
        file=sys.stderr,
    )
    sys.exit(2)


RESERVED_SECTIONS = {"signatures", "builds", "general"}
DEFAULT_OFFSETS_PATH = Path(__file__).resolve().parent.parent / "config" / "grml_offsets.ini"


class PeContext:
    def __init__(self, exe_path: Path) -> None:
        self.exe_path = exe_path
        self.raw_bytes = exe_path.read_bytes()
        self.sha256 = hashlib.sha256(self.raw_bytes).hexdigest().lower()
        self.pe = pefile.PE(data=self.raw_bytes, fast_load=False)
        self.image_base: int = self.pe.OPTIONAL_HEADER.ImageBase
        self.timestamp: int = self.pe.FILE_HEADER.TimeDateStamp
        self.size_of_image: int = self.pe.OPTIONAL_HEADER.SizeOfImage
        self.entry_point: int = self.pe.OPTIONAL_HEADER.AddressOfEntryPoint

        text_sec = None
        for sec in self.pe.sections:
            name = sec.Name.rstrip(b"\x00").decode("ascii", errors="ignore")
            if name == ".text":
                text_sec = sec
                break
        if text_sec is None:
            raise RuntimeError(f"No .text section found in {exe_path}")

        self.text_va: int = text_sec.VirtualAddress
        vsize = text_sec.Misc_VirtualSize or text_sec.SizeOfRawData
        raw_off = text_sec.PointerToRawData
        raw_sz = min(vsize, text_sec.SizeOfRawData, len(self.raw_bytes) - raw_off)
        self.text_bytes: bytes = self.raw_bytes[raw_off : raw_off + raw_sz]
        self.file_version: str = self._read_pe_version()

    def _read_pe_version(self) -> str:
        if hasattr(self.pe, "VS_FIXEDFILEINFO") and self.pe.VS_FIXEDFILEINFO:
            ffi = self.pe.VS_FIXEDFILEINFO[0]
            ms = ffi.FileVersionMS
            ls = ffi.FileVersionLS
            ver = f"{(ms >> 16) & 0xFFFF}.{ms & 0xFFFF}.{(ls >> 16) & 0xFFFF}.{ls & 0xFFFF}"
            if ver != "0.0.0.0":
                return ver
        return ""

    def detect_build_version(self, builds_map: Dict[str, str]) -> str:
        if self.sha256 in builds_map:
            return builds_map[self.sha256]
        if self.file_version:
            return self.file_version
        return "unknown"


def parse_ini_sections(ini_text: str) -> Tuple[List[str], Dict[str, Dict[str, str]]]:
    order: List[str] = []
    sections: Dict[str, Dict[str, str]] = {}
    current = ""
    for raw_line in ini_text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith(";") or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]") and len(line) >= 2:
            current = line[1:-1].strip()
            if current not in sections:
                order.append(current)
                sections[current] = {}
            continue
        if "=" in line and current:
            k, v = line.split("=", 1)
            sections[current][k.strip()] = v.strip()
    return order, sections


def is_declarative_section(name: str, kv: Dict[str, str]) -> bool:
    lo = name.lower()
    if lo in RESERVED_SECTIONS or lo.startswith("profile."):
        return False
    return bool(kv.get("pattern", "").strip())


def pattern_to_regex(pattern_str: str) -> re.Pattern[bytes]:
    parts: List[bytes] = []
    for tok in pattern_str.strip().split():
        if tok in ("?", "??"):
            parts.append(b".")
        else:
            val = int(tok, 16)
            parts.append(re.escape(bytes([val])))
    return re.compile(b"".join(parts), re.DOTALL)


def scan_text_pattern(text_bytes: bytes, pattern_str: str) -> List[int]:
    rx = pattern_to_regex(pattern_str)
    return [m.start() for m in rx.finditer(text_bytes)]


def parse_int_auto(s: str, default: int = 0) -> int:
    s = s.strip()
    if not s:
        return default
    return int(s, 0)


def resolve_target(
    pe_ctx: PeContext,
    match_text_off: int,
    kv: Dict[str, str],
) -> Tuple[int, Optional[int], str]:
    match_rva = pe_ctx.text_va + match_text_off
    mode = kv.get("resolve", "function_start").strip().lower()
    op_off = parse_int_auto(kv.get("op_offset", "0"), 0)
    instr_len = parse_int_auto(kv.get("instr_len", "0"), 0)
    extra = parse_int_auto(kv.get("extra_offset", "0"), 0)

    if mode == "function_start":
        return match_rva, match_rva + extra, mode

    if mode in ("rip_rel32", "call_rel32"):
        if op_off < 0 or match_text_off + op_off + 4 > len(pe_ctx.text_bytes) or instr_len <= 0:
            return match_rva, None, mode
        disp = struct.unpack_from("<i", pe_ctx.text_bytes, match_text_off + op_off)[0]
        return match_rva, match_rva + instr_len + disp + extra, mode

    if mode == "disp32":
        if op_off < 0 or match_text_off + op_off + 4 > len(pe_ctx.text_bytes):
            return match_rva, None, mode
        disp = struct.unpack_from("<I", pe_ctx.text_bytes, match_text_off + op_off)[0]
        return match_rva, disp + extra, mode

    return match_rva, None, mode


def is_rel32_branch(insn: capstone.CsInsn) -> Optional[Tuple[int, int]]:
    raw = bytes(insn.bytes)
    if len(raw) >= 5 and raw[0] in (0xE8, 0xE9):
        return (len(raw) - 4, 4)
    if len(raw) >= 6 and raw[0] == 0x0F and (0x80 <= raw[1] <= 0x8F):
        return (len(raw) - 4, 4)
    return None


def wildcard_instruction_tokens(
    insn: capstone.CsInsn,
    insn_rel_off: int,
    mode: str,
    recorded_op_off: Optional[int],
    recorded_instr_len: Optional[int],
) -> Tuple[List[str], Optional[int], Optional[int]]:
    raw = bytes(insn.bytes)
    tokens = [f"{b:02X}" for b in raw]
    wild = [False] * len(raw)

    # 1. Relative CALL / JMP / Jcc rel32
    rel_branch = is_rel32_branch(insn)
    if rel_branch is not None:
        off, sz = rel_branch
        for i in range(off, min(off + sz, len(wild))):
            wild[i] = True
        if mode == "call_rel32" and recorded_op_off is None:
            recorded_op_off = insn_rel_off + off
            recorded_instr_len = insn_rel_off + insn.size

    # 2. RIP-relative disp32 & large struct disp32 (abs >= 0x80)
    has_rip_mem = False
    has_large_disp32 = False
    for op in insn.operands:
        if op.type == x86.X86_OP_MEM:
            if op.mem.base == x86.X86_REG_RIP:
                has_rip_mem = True
            elif insn.disp_size == 4 and abs(op.mem.disp) >= 0x80:
                has_large_disp32 = True

    if insn.disp_size == 4 and insn.disp_offset >= 0 and (has_rip_mem or has_large_disp32):
        d_off = insn.disp_offset
        for i in range(d_off, min(d_off + 4, len(wild))):
            wild[i] = True
        if has_rip_mem and mode == "rip_rel32" and recorded_op_off is None:
            recorded_op_off = insn_rel_off + d_off
            recorded_instr_len = insn_rel_off + insn.size
        if has_large_disp32 and mode == "disp32" and recorded_op_off is None:
            recorded_op_off = insn_rel_off + d_off

    # 3. Fallback for disp32 mode targeting an imm32 (e.g. imul rax, r8, 0x380)
    if mode == "disp32" and recorded_op_off is None and insn.imm_size == 4 and insn.imm_offset >= 0:
        i_off = insn.imm_offset
        for i in range(i_off, min(i_off + 4, len(wild))):
            wild[i] = True
        recorded_op_off = insn_rel_off + i_off

    for idx, is_w in enumerate(wild):
        if is_w:
            tokens[idx] = "??"

    return tokens, recorded_op_off, recorded_instr_len


def generate_signature(
    pe_ctx: PeContext,
    rva: int,
    name: str,
    mode: str,
    anchor: Optional[str],
    verified_build: str,
) -> Dict[str, str]:
    if rva < pe_ctx.text_va or rva >= pe_ctx.text_va + len(pe_ctx.text_bytes):
        raise ValueError(
            f"RVA 0x{rva:X} is outside .text [0x{pe_ctx.text_va:X}, 0x{pe_ctx.text_va + len(pe_ctx.text_bytes):X})"
        )

    start_off = rva - pe_ctx.text_va
    window = pe_ctx.text_bytes[start_off : start_off + 256]

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    tokens: List[str] = []
    op_offset: Optional[int] = None
    instr_len: Optional[int] = None
    unique_found = False
    extra_remaining = 0

    for insn in md.disasm(window, rva):
        rel_off = insn.address - rva
        insn_tokens, op_offset, instr_len = wildcard_instruction_tokens(
            insn, rel_off, mode, op_offset, instr_len
        )
        tokens.extend(insn_tokens)

        if not unique_found:
            pat_str = " ".join(tokens)
            matches = scan_text_pattern(pe_ctx.text_bytes, pat_str)
            if len(matches) == 1:
                unique_found = True
                extra_remaining = 2
            elif len(matches) == 0:
                raise RuntimeError(f"Pattern unexpectedly matched 0 times at RVA 0x{rva:X}: {pat_str}")
        else:
            extra_remaining -= 1
            if extra_remaining <= 0:
                break

    pat_str = " ".join(tokens)
    final_matches = scan_text_pattern(pe_ctx.text_bytes, pat_str)
    if len(final_matches) != 1:
        raise RuntimeError(
            f"Failed to isolate a unique signature from RVA 0x{rva:X} (matches={len(final_matches)})"
        )

    if mode in ("rip_rel32", "call_rel32") and (op_offset is None or instr_len is None):
        raise RuntimeError(f"Mode '{mode}' requested, but no matching operand found starting at RVA 0x{rva:X}")
    if mode == "disp32" and op_offset is None:
        raise RuntimeError(f"Mode 'disp32' requested, but no disp32/imm32 operand found starting at RVA 0x{rva:X}")

    result: Dict[str, str] = {
        "name": name,
        "pattern": pat_str,
        "resolve": mode,
    }
    if op_offset is not None:
        result["op_offset"] = str(op_offset)
    if instr_len is not None and mode in ("rip_rel32", "call_rel32"):
        result["instr_len"] = str(instr_len)
    result["extra_offset"] = "0"
    if anchor:
        result["anchor_xref"] = anchor
    if verified_build and verified_build != "unknown":
        result["verified_build"] = verified_build
    return result


def format_ini_section(entry: Dict[str, str]) -> str:
    lines = [f"[{entry['name']}]"]
    for key in ("pattern", "resolve", "op_offset", "instr_len", "extra_offset", "anchor_xref", "verified_build"):
        if key in entry and entry[key] != "":
            lines.append(f"{key} = {entry[key]}")
    return "\n".join(lines) + "\n"


def upsert_ini_section(ini_path: Path, entry: Dict[str, str]) -> None:
    text = ini_path.read_text(encoding="utf-8-sig") if ini_path.exists() else ""
    new_block = format_ini_section(entry).rstrip("\n")
    sec_name = entry["name"]
    pattern = re.compile(
        rf"(?ms)^\[{re.escape(sec_name)}\]\r?\n.*?(?=^\[|\Z)"
    )
    if pattern.search(text):
        updated = pattern.sub(lambda _: new_block + "\n\n", text, count=1).rstrip() + "\n"
    else:
        sep = "\n\n" if text and not text.endswith("\n\n") else ("\n" if text and not text.endswith("\n") else "")
        updated = text + sep + new_block + "\n"
    ini_path.write_text(updated, encoding="utf-8", newline="\n")


def run_verify_all(pe_ctx: PeContext, offsets_path: Path) -> int:
    if not offsets_path.exists():
        print(f"Error: Offsets file not found: {offsets_path}", file=sys.stderr)
        return 2

    ini_text = offsets_path.read_text(encoding="utf-8-sig")
    order, sections = parse_ini_sections(ini_text)
    builds = {k.lower(): v for k, v in sections.get("builds", {}).items()}
    detected_ver = pe_ctx.detect_build_version(builds)

    print(
        f"Verifying {offsets_path} against {pe_ctx.exe_path.name} "
        f"(build={detected_ver}, TS=0x{pe_ctx.timestamp:08X}, SizeOfImage=0x{pe_ctx.size_of_image:X}, EP=0x{pe_ctx.entry_point:X})"
    )
    print("-" * 88)

    failures = 0
    checked = 0
    for sec_name in order:
        kv = sections[sec_name]
        if not is_declarative_section(sec_name, kv):
            continue
        checked += 1
        pat = kv["pattern"]
        required = kv.get("required", "1").strip().lower() not in ("0", "false", "no")
        matches = scan_text_pattern(pe_ctx.text_bytes, pat)

        if len(matches) == 1:
            match_rva, target, mode = resolve_target(pe_ctx, matches[0], kv)
            if target is None:
                print(f"  [FAIL]     {sec_name:<24} @ 0x{match_rva:08X} -> invalid {mode} metadata")
                if required:
                    failures += 1
            elif mode == "disp32":
                print(f"  [OK]       {sec_name:<24} @ 0x{match_rva:08X} -> disp32 0x{target:X}")
            else:
                print(f"  [OK]       {sec_name:<24} @ 0x{match_rva:08X} -> Resolved 0x{target:08X} ({mode})")
        elif len(matches) == 0:
            print(f"  [MISSING]  {sec_name:<24} (0 matches)")
            if required:
                failures += 1
        else:
            print(f"  [AMBIGUOUS]{sec_name:<24} ({len(matches)} matches)")
            if required:
                failures += 1

    print("-" * 88)
    print(f"Checked {checked} declarative offset(s); failures={failures}")
    return 1 if failures > 0 else 0


def main() -> int:
    parser = argparse.ArgumentParser(description="GRML automated x86-64 AOB signature generator & verifier")
    parser.add_argument("--exe", required=True, type=Path, help="Path to SOVIET64.exe")
    parser.add_argument("--offsets", type=Path, default=DEFAULT_OFFSETS_PATH, help="Path to grml_offsets.ini")
    parser.add_argument("--verify-all", action="store_true", help="Batch-verify all entries in grml_offsets.ini")
    parser.add_argument("--rva", type=str, help="Target RVA (hex, e.g. 0x123456)")
    parser.add_argument("--name", type=str, help="INI section name to generate")
    parser.add_argument(
        "--mode",
        choices=["function_start", "rip_rel32", "call_rel32", "disp32"],
        default="function_start",
        help="Resolution mode",
    )
    parser.add_argument("--anchor", type=str, default=None, help="Optional anchor_xref string/RTTI name")
    parser.add_argument("--write", action="store_true", help="Upsert generated section into grml_offsets.ini")
    args = parser.parse_args()

    if not args.exe.exists():
        print(f"Error: Executable not found: {args.exe}", file=sys.stderr)
        return 2

    pe_ctx = PeContext(args.exe)

    if args.verify_all:
        return run_verify_all(pe_ctx, args.offsets)

    if not args.rva or not args.name:
        parser.error("Generator mode requires --rva and --name (or use --verify-all)")

    rva = int(args.rva, 16) if args.rva.lower().startswith("0x") else int(args.rva, 0)
    builds: Dict[str, str] = {}
    if args.offsets.exists():
        _, secs = parse_ini_sections(args.offsets.read_text(encoding="utf-8-sig"))
        builds = {k.lower(): v for k, v in secs.get("builds", {}).items()}
    verified_build = pe_ctx.detect_build_version(builds)

    entry = generate_signature(pe_ctx, rva, args.name, args.mode, args.anchor, verified_build)
    block = format_ini_section(entry)
    print(block)

    if args.write:
        upsert_ini_section(args.offsets, entry)
        print(f"Updated [{args.name}] in {args.offsets}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
