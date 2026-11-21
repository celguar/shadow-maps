#!/usr/bin/env python3
r"""
verify_dll.py -- confirm a built ShadowMaps.dll carries what mod.c claims.

mod.c says four things about the image it produces, and none of them is
visible from the source alone: it exports an undecorated "Load" (VanillaFixes
resolves the entry point with GetProcAddress(module, "Load")), it imports only
the operating system's own DLLs (built with /MT: a /MD build would demand
vcruntime140.dll next to the client, which the client's folder has no reason to
carry), it is an i686 PE (build 5875 is a 32-bit image, an x64 DLL simply
cannot load into it), and its detour is a 36-byte hand-written stub that has to
survive the compiler intact. A linker that mangled the export or a build that
dropped the stub would otherwise ship silently, so this reads the built image
the way the client's loader would.

The stub is NOT inline asm any more, so it is not in .text: stub_code is a
const array, which MSVC puts in .rdata, and BuildStub copies it into a private
executable page at install. So the search here covers every section's raw
bytes, not one named section, and what is matched is the byte table exactly as
mod.c writes it -- with the two imm32 slots still zero, since install patches
them with addresses only this process can know:

    9c                 pushfd                     fixup and resume slots are
    60                 pushad                        patched at install, so in
    83 c4 f8           add  esp, -8 (= sub esp,8)    the image they read 0
    8b 44 24 30        mov  eax, [esp+48]  = record (esp needs the SIB byte)
    50                 push eax
    51                 push ecx  (this)
    b8 <imm32>         mov  eax, <fixup_chunk>     slot STUB_SLOT_FIXUP  = 12
    ff d0              call eax
    83 c4 08           add  esp, 8
    61                 popad
    9d                 popfd
    55 8b ec           push ebp / mov ebp, esp     the 6 replayed prologue
    8b 45 08           mov  eax, [ebp+8] = record  bytes of 0x6AF5F0
    b8 <imm32>         mov  eax, <resume>         slot STUB_SLOT_RESUME = 30
    ff e0              jmp  eax                   -> 0x6AF5F6

(`add esp, -8` rather than `sub esp, 8`: x86-32 has no sub-with-imm8 form, and
mod.c says so; the disassembly below prints it as the unsigned 0xf8.)

The needle is PARSED out of mod.c rather than hardcoded, so the fingerprint
cannot drift from the source: if someone edits stub_code, this script follows.
It also cross-checks mod.c's own STUB_BYTES and slot defines against the
array, which is the same equality BuildStub asserts at runtime.

Usage
    python verify_dll.py [<ShadowMaps.dll> ...]   (default: out\ShadowMaps.dll)

Exit code 0 = every check passed. Anything else names what failed.
"""

import sys
import re
import os
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

MOD_CPP = r"I:\code\wow\misc\salmiak-shadow-mod\src\mod.c"
DEFAULT_DLLS = [r"I:\code\wow\misc\salmiak-shadow-mod\out\ShadowMaps.dll"]

# the DLLs a WoW 1.12.1 mod may legitimately import; anything else means the
# build pulled in a CRT or C++ runtime the client's folder has no reason to
# carry, and the DLL would fail to load on a stock machine.
OK_IMPORTS = {"ntdll.dll", "kernel32.dll", "kernel32.exe", "user32.dll",
              "advapi32.dll", "shell32.dll", "gdi.dll", "gdiplus.dll",
              "wow.exe"}

# fallback needle, used only if mod.c cannot be read; kept in sync by hand
FALLBACK_STUB = bytes.fromhex(
    "9c6083c4f88b4424305051b800000000ffd083c408619d558bec8b4508b800000000ffe0")
FALLBACK_SLOTS = (12, 30)


def parse_stub(source_text):
    """stub_code's bytes and the slot/length defines, straight from mod.c."""
    body = re.search(r"stub_code\[\]\s*=\s*\{(.*?)\}\s*;", source_text, re.S)
    if body is None:
        return None
    raw = re.sub(r"/\*.*?\*/", "", body.group(1), flags=re.S)
    data = bytes(int(t, 16) for t in re.findall(r"0[xX][0-9a-fA-F]+", raw))

    defines = {}
    for name in ("STUB_BYTES", "STUB_SLOT_FIXUP", "STUB_SLOT_RESUME"):
        hit = re.search(r"#define\s+%s\s+(\d+)u?" % name, source_text)
        if hit:
            defines[name] = int(hit.group(1))
    return data, defines


def main(argv):
    dlls = argv or DEFAULT_DLLS
    bad = 0
    md = Cs(CS_ARCH_X86, CS_MODE_32)

    try:
        with open(MOD_CPP, "rb") as fh:
            parsed = parse_stub(fh.read().decode("utf-8", "replace"))
        if parsed is None:
            print("mod.c has no stub_code table: falling back to the hardcoded needle")
            stub, slots = FALLBACK_STUB, FALLBACK_SLOTS
        else:
            stub, defs = parsed
            slots = (defs.get("STUB_SLOT_FIXUP", 11), defs.get("STUB_SLOT_RESUME", 29))
            want = defs.get("STUB_BYTES", len(stub))
            print("stub from mod.c: %d bytes, slots fixup=%d resume=%d"
                  % (len(stub), slots[0], slots[1]))
            if want != len(stub):
                bad += 1
                print("   STUB_BYTES says %d, the array has %d: BuildStub would "
                      "refuse to patch at runtime" % (want, len(stub)))
            for name, where in (("STUB_SLOT_FIXUP", slots[0]), ("STUB_SLOT_RESUME", slots[1])):
                if stub[where - 1] != 0xB8 or stub[where:where + 4] != b"\x00\x00\x00\x00":
                    bad += 1
                    print("   %s=%d does not point at a zeroed mov eax,imm32"
                          " immediate: install would write the address in the"
                          " wrong place" % (name, where))
    except OSError as exc:
        print("cannot read mod.c (%s): falling back to the hardcoded needle" % exc)
        stub, slots = FALLBACK_STUB, FALLBACK_SLOTS

    for path in dlls:
        if not os.path.isfile(path):
            bad += 1
            print("== %s: not there -- this machine cannot build the i686 DLL"
                  " (see README.md, toolchain section)" % path)
            continue
        pe = pefile.PE(path)
        blob = open(path, "rb").read()
        base = pe.OPTIONAL_HEADER.ImageBase
        print("== %s  imagebase %#x  machine %#x  (%d bytes)"
              % (path, base, pe.FILE_HEADER.Machine, len(blob)))
        if pe.FILE_HEADER.Machine != 0x14C:
            bad += 1
            print("   not i686 (want 0x14c): the 1.12.1 client is a 32-bit PE, "
                  "this DLL cannot be loaded by it")

        # ---- the export VanillaFixes looks up --------------------------------
        # pefile's DIRECTORY_ENTRY_EXPORT is the export DIRECTORY (an ExportDirData
        # holding .symbols), not a list: iterating it directly is a TypeError.
        want_load = None
        exp_dir = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
        for entry in (exp_dir.symbols if exp_dir is not None else []):
            name = (entry.name or b"").decode("ascii", "replace")
            if name in ("Load", "_Load"):
                want_load = (name, entry.ordinal, entry.address or 0)
        if want_load is None:
            bad += 1
            print("   no 'Load' export: VanillaFixes' InitAdditionalDLLs cannot "
                  "find this DLL, so nothing would ever install")
        else:
            print("   export Load at RVA %#x (ordinal %s)"
                  % (want_load[2], want_load[1] if want_load[1] is not None else "-"))

        # ---- imports: /MT means only the OS's own DLLs -----------------------
        for mod in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            name = mod.dll.decode("ascii", "errors").lower()
            flag = "" if name in OK_IMPORTS else "  <-- not an OS DLL"
            if flag:
                bad += 1
            print("   import %s (%d named)%s" % (name, len(mod.imports), flag))

        # ---- the detour: a const table, so in .rdata or wherever it landed ---
        found = 0
        for sec in pe.sections:
            label = sec.Name.rstrip(b"\0").decode("ascii", "errors")
            size = sec.SizeOfRawData
            raw = blob[sec.PointerToRawData:sec.PointerToRawData + size]
            at = raw.find(stub)
            if at < 0:
                continue
            found += 1
            va = base + sec.VirtualAddress + at
            print("   stub table found in %s at %#x (.text/.rdata offsets do not "
                  "matter: BuildStub copies this table into an executable page "
                  "of its own at install)" % (label, va))
            print("     bytes: %s" % " ".join("%02x" % b for b in raw[at:at + len(stub)]))
            if raw[at + slots[0] - 1] != 0xB8 or raw[at + slots[0]:at + slots[0] + 4] != b"\x00\x00\x00\x00":
                bad += 1
                print("     the fixup patch slot is not a zeroed mov eax,imm32")
            if raw[at + slots[1] - 1] != 0xB8 or raw[at + slots[1]:at + slots[1] + 4] != b"\x00\x00\x00\x00":
                bad += 1
                print("     the resume patch slot is not a zeroed mov eax,imm32")
            for ins in md.disasm(raw[at:at + len(stub)], va):
                print("     %#010x: %-22s %s %s"
                      % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
            last = md.disasm(raw[at + len(stub) - 2:at + len(stub)],
                             va + len(stub) - 2)
            tail = list(last)
            if not tail or tail[-1].mnemonic != "jmp":
                bad += 1
                print("     the table does not end in jmp eax: control would "
                      "fall through into whatever follows")
        if found == 0:
            bad += 1
            print("   the %d-byte stub table is NOT in any section of this image:"
                  " BuildStub's memcpy has no source to copy, so the detour is"
                  " dead (the compiler did not rewrite it, this means the"
                  " array was optimised out or the link failed)" % len(stub))
        elif found > 1:
            bad += 1
            print("   the stub table appears in %d sections: two copies means"
                  " BuildStub and the shipped bytes could disagree" % found)

    print("dlls %d" % len(dlls))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
