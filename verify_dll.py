#!/usr/bin/env python3
r"""
verify_dll.py -- confirm a built ShadowMaps.dll carries what mod.c claims.

mod.c says four things about the image it produces, and none of them is
visible from the source alone: it exports an undecorated "Load" (VanillaFixes
resolves the entry point with GetProcAddress(module, "Load")), it imports only
the operating system's own DLLs (built with /MT: a /MD build would demand
vcruntime140.dll next to the client, which the client's folder has no reason to
carry), it is an i686 PE (build 5875 is a 32-bit image, an x64 DLL simply
cannot load into it), and its detours are three hand-written byte tables that
have to survive the compiler intact -- sv9's ladder needs all three, so a build
that lost one is a build whose ladder cannot run and that ships silently. This
reads the built image the way the client's loader would.

The stubs are NOT inline asm any more, so they are not in .text: the detour
shapes are const arrays, which MSVC puts in .rdata, and BuildStub copies one of
them into a private executable page at install, choosing which by the mode
letter. sv9 has three shapes, and this script parses all three out of mod.c and
requires each to be present exactly once in the image:

    stub_code   mode b/h/f, 36 bytes, slots fixup=12 resume=30
    stub_code_c mode c,     12 bytes, resume slot 7, calls nothing
    stub_code_d mode d,     29 bytes, slots fixup=7 resume=24

stub_code, the one that calls the fixup through the client's own arguments:

    9c                 pushfd                     fixup and resume slots are
    60                 pushad                        patched at install, so in
    83 c4 f8           add  esp, -8 (= sub esp,8)    the image they read 0
    8b 44 24 30        mov  eax, [esp+48]  = record (esp needs the SIB byte)
    50                 push eax
    51                 push ecx  (this)
    b8 <imm32>         mov  eax, <fixup_chunk>     slot STUB_SLOT_FIXUP  = 12
    ff d0              call eax
    83 c4 10           add  esp, 16  (8 arg bytes the callee left, 8 scratch)
    61                 popad
    9d                 popfd
    55 8b ec           push ebp / mov ebp, esp     the 6 replayed prologue
    8b 45 08           mov  eax, [ebp+8] = record  bytes of 0x6AF5F0
    b8 <imm32>         mov  eax, <resume>         slot STUB_SLOT_RESUME = 30
    ff e0              jmp  eax                   -> 0x6AF5F6

(`add esp, -8` rather than `sub esp, 8`: x86-32 has no sub-with-imm8 form, and
mod.c says so; the disassembly below prints it as the unsigned 0xf8.)

stub_code_c, mode c, the detour with no call at all:

    55 8b ec           push ebp / mov ebp, esp     the 6 replayed prologue
    8b 45 08           mov  eax, [ebp+8] = record  bytes of 0x6AF5F0
    68 <imm32>         push <resume>              slot STUB_C_SLOT_RESUME = 7
    c3                 ret                        -> 0x6AF5F6

stub_code_d, mode d, the store at 0x6AF5F3 and the arguments off the client's
own frame pointer, so no esp displacement is involved in finding them:

    60                 pushad
    9c                 pushfd
    ff 75 08           push dword [ebp+8] = record
    51                 push ecx  (this)
    b8 <imm32>         mov  eax, <fixup_chunk>     slot STUB_D_SLOT_FIXUP  = 7
    ff d0              call eax
    83 c4 08           add  esp, 8   (the 2 arg dwords: no scratch reserved)
    9d                 popfd
    61                 popad
    8b 45 08 53 56     mov eax,[ebp+8] / push ebx / push esi  the 5 covered bytes
    68 <imm32>         push <resume>              slot STUB_D_SLOT_RESUME = 24
    c3                 ret                        -> 0x6AF5F8

A fourth table is not a rung's detour but the refill's client call, and it is
machine code for the same reason -- GxTexUpdateRect (0x58ac70) keeps its texture
in ecx and the rectangle's second word in edx, and this compiler refuses both
__thiscall and __fastcall on a function-pointer typedef, so C cannot say the
calling shape at all. sv20 found that out by crashing: the report's 0x594b9e is
`mov byte ptr [esi], 1` with esi = [ebp+8] of 0x594b90 = the ecx 0x58acd0 was
entered with = the ecx the cdecl call never set, and the "write of 0x140" is
FIELD_SHADOW_TEX read back as an address. gx_rect_thunk, 24 bytes, is copied onto
the SAME execute page as the detour, at STUB_PAGE_THUNK, and its one imm32 slot
carries 0x58ac70 at install:

    gx_rect_thunk  24 bytes, target slot 17, page offset 0x40

     8b 4c 24 04      mov  ecx, [esp+4]   = the texture C passed
     8b 44 24 08      mov  eax, [esp+8]   = the tile size C passed
     6a 01            push 1              = arg4, the flag: 0x594c23 tests it
     50               push eax            = arg3, stored at rect+8
     50               push eax            = arg2, stored at rect+0xc
     6a 00            push 0              = arg1, stored at rect+0
     33 d2            xor  edx, edx       = the rect's word at +4, from a
                                            register not the stack
     b8 <imm32>       mov  eax, <0x58ac70>  slot GX_RECT_THUNK_SLOT_TARGET = 15
     ff d0            call eax
     c3               ret                 = back to C, which cleans its 2 args

The callee's own `ret 0x10` removes the four pushed words, so esp comes back
where C expects it. The four zeros sv17..sv20 pushed instead cannot work at all:
the last word is the flag whose test at 0x594c23 skips the device upload, and a
{0,0,0,0} rect takes the `cmp ebx,ecx / jge 0x594c08` branch that only resets the
texture's rectangle words. The page offset must also clear the longest detour
table, which is checked against every rung here and against code_bytes in BuildStub.

One check cannot come from the source at all: the stubs' `add esp,N` after the
call. MSVC gives a static C function a PLAIN ret (opcode c3) and makes the caller
drop the arguments, but the optimiser is free to choose `ret imm16` instead, and
either way the stub must undo exactly what THIS build chose -- an eight-byte
mistake is silent at compile time and an ACCESS_VIOLATION inside the client at
runtime, because popad then restores every register from the wrong slot. So the
sibling map file locates fixup_chunk in the image, its epilogue is disassembled,
and each calling stub's add is required to be the matching amount for ITS OWN
argument bytes: b pushes 2 arg dwords and reserves 8 scratch bytes, so its add is
16 minus what the callee already dropped -- 16 with a plain ret, 8 with a `ret 8`
-- and d pushes the same 2 arg dwords and reserves nothing, so its add is 8 minus
the same amount, 8 with a plain ret and 0 with a `ret 8`. The two tables must
therefore disagree by exactly the 8-byte scratch reservation, and not by more or
less. mode c calls nothing and has no add, so nothing is checked for it.

Usage
    python verify_dll.py [<ShadowMaps.dll> ...]   (default: out\ShadowMaps.dll)

Exit code 0 = every check passed. Anything else names what failed.
"""

import sys
import re
import os
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

# Everything this script reads lives inside the project, so the folder it starts
# from is the folder holding this file: build.py's default out\ShadowMaps.dll is
# beside it, and mod.c is under src\. Deriving the root instead of spelling an
# absolute path keeps the script true on any machine -- the one that built the
# DLL and any that only reads a copy -- and says nothing about any of them. A
# copy carried off the project root alone misses MOD_CPP, and the reader below
# says so and falls back to the hardcoded needles.
ROOT = os.path.dirname(os.path.abspath(__file__))
MOD_CPP = os.path.join(ROOT, "src", "mod.c")
DEFAULT_DLLS = [os.path.join(ROOT, "out", "ShadowMaps.dll")]

# the DLLs a WoW 1.12.1 mod may legitimately import; anything else means the
# build pulled in a CRT or C++ runtime the client's folder has no reason to
# carry, and the DLL would fail to load on a stock machine.
OK_IMPORTS = {"ntdll.dll", "kernel32.dll", "kernel32.exe", "user32.dll",
              "advapi32.dll", "shell32.dll", "gdi.dll", "gdiplus.dll",
              "wow.exe"}

# fallback needles, used only if mod.c cannot be read; kept in sync by hand.
FALLBACK_STUBS = {
    "b": ("9c6083c4f88b4424305051b800000000ffd083c410619d558bec"
          "8b4508b800000000ffe0", 36, 12, 30),
    "c": ("558bec8b45086800000000c3", 12, 0, 7),
    "d": ("609cff750851b800000000ffd083c4089d618b45085356"
          "6800000000c3", 29, 7, 24),
}

# what each rung is expected to push for the fixup (its add esp,N must be this
# minus whatever the callee's own `ret imm16` already dropped) and how its last
# instruction returns to the client: b jumps, c and d use push/ret.
STUB_SHAPE = {  # rung: (table name, define prefix, arg bytes, tail mnemonic)
    "b": ("stub_code", "STUB_", 16, "jmp"),
    "c": ("stub_code_c", "STUB_C_", None, "ret"),
    "d": ("stub_code_d", "STUB_D_", 8, "ret"),
}


def parse_stubs(source_text):
    """Every detour byte table in mod.c, with its own slot/length defines."""
    tables = []
    for rung in ("b", "c", "d"):
        name, pre, pushed, tail = STUB_SHAPE[rung]
        body = re.search(r"\b%s\[\]\s*=\s*\{(.*?)\}\s*;" % name, source_text, re.S)
        if body is None:
            continue
        raw = re.sub(r"/\*.*?\*/", "", body.group(1), flags=re.S)
        data = bytes(int(t, 16) for t in re.findall(r"0[xX][0-9a-fA-F]+", raw))
        slot_fixup = None if rung == "c" else (12 if rung == "b" else 7)
        slot_resume = 30 if rung == "b" else 7
        want = len(data)
        hit = re.search(r"#define\s+%sBYTES\s+(\d+)u?" % pre, source_text)
        if hit:
            want = int(hit.group(1))
        if slot_fixup is not None:
            hit = re.search(r"#define\s+%sSLOT_FIXUP\s+(\d+)u?" % pre, source_text)
            if hit:
                slot_fixup = int(hit.group(1))
        hit = re.search(r"#define\s+%sSLOT_RESUME\s+(\d+)u?" % pre, source_text)
        if hit:
            slot_resume = int(hit.group(1))
        tables.append({"rung": rung, "name": name, "bytes": data, "want": want,
                       "fixup": slot_fixup, "resume": slot_resume,
                       "pushed": pushed, "tail": tail})
    return tables or None


def parse_thunk(source_text):
    """The refill's client-call thunk in mod.c, with its slot and page defines."""
    body = re.search(r"\bgx_rect_thunk\[\]\s*=\s*\{(.*?)\}\s*;", source_text, re.S)
    if body is None:
        return None
    raw = re.sub(r"/\*.*?\*/", "", body.group(1), flags=re.S)
    data = bytes(int(t, 16) for t in re.findall(r"0[xX][0-9a-fA-F]+", raw))
    want = len(data)
    hit = re.search(r"#define\s+GX_RECT_THUNK_BYTES\s+(\d+)u?", source_text)
    if hit:
        want = int(hit.group(1))
    slot = None
    hit = re.search(r"#define\s+GX_RECT_THUNK_SLOT_TARGET\s+(\d+)u?", source_text)
    if hit:
        slot = int(hit.group(1))
    page = None
    hit = re.search(r"#define\s+STUB_PAGE_THUNK\s+(0[xX][0-9a-fA-F]+|\d+)u?",
                    source_text)
    if hit:
        page = int(hit.group(1), 0)
    return {"bytes": data, "want": want, "slot": slot, "page": page}


def callee_cleanup(path, blob, pe, base, md):
    """Bytes the callee itself removes for the fixup call, read from the image.

    The map beside the DLL names fixup_chunk; its epilogue's `ret` form is what
    the stub's add esp,N must match. Returns 0 (plain ret: caller cleans), the
    imm16 the linker chose (callee cleans), or None when the function cannot be
    located in this build."""
    map_path = os.path.join(os.path.dirname(path), "ShadowMaps.map")
    if not os.path.isfile(map_path):
        return None
    raw = open(map_path, "rb").read()
    va = None
    for enc in ("cp1252", "utf-16", "utf-8"):
        try:
            txt = raw.decode(enc, "replace")
            break
        except Exception:
            continue
    for line in txt.splitlines():
        hit = re.search(r"([0-9a-fA-F]{8})\s+f\s+mod\.obj", line)
        if hit and "fixup_chunk" in line:
            va = int(hit.group(1), 16)
    if va is None:
        return None

    def rd(v, n):
        for sec in pe.sections:
            size = max(sec.Misc_VirtualSize, sec.SizeOfRawData)
            if sec.VirtualAddress <= v - base < sec.VirtualAddress + size:
                off = v - base - sec.VirtualAddress + sec.PointerToRawData
                return blob[off:off + n]
        return b""

    body = rd(va, 0x2000)
    for ins in md.disasm(body, va):
        if ins.mnemonic == "ret":
            if not ins.op_str:
                return 0
            return int(ins.op_str, 0)
    return None


def main(argv):
    dlls = argv or DEFAULT_DLLS
    bad = 0
    md = Cs(CS_ARCH_X86, CS_MODE_32)

    thunk = None
    try:
        with open(MOD_CPP, "rb") as fh:
            source_text = fh.read().decode("utf-8", "replace")
        tables = parse_stubs(source_text)
        thunk = parse_thunk(source_text)
        if tables is None:
            print("mod.c has no stub_code tables: falling back to the hardcoded"
                  " needles")
            tables = []
            for rung in ("b", "c", "d"):
                hexs, want, fx, rs = FALLBACK_STUBS[rung]
                name, pre, pushed, tail = STUB_SHAPE[rung]
                tables.append({"rung": rung, "name": name, "bytes": bytes.fromhex(hexs),
                               "want": want, "fixup": fx, "resume": rs,
                               "pushed": pushed, "tail": tail})
        else:
            have = [t["rung"] for t in tables]
            for rung in ("b", "c", "d"):
                if rung not in have:
                    bad += 1
                    print("   mod.c has no stub_code table for rung %s: sv9's ladder"
                          " cannot run that rung, so the ladder in README.md would"
                          " be a lie" % rung)
        for t in tables:
            print("stub %s from mod.c: %d bytes (want %d), slots fixup=%s resume=%d,"
                  " pushes %s arg bytes, tail %s"
                  % (t["rung"], len(t["bytes"]), t["want"],
                     ("%d" % t["fixup"]) if t["fixup"] is not None else "none",
                     t["resume"],
                     ("%d" % t["pushed"]) if t["pushed"] is not None else "no call",
                     t["tail"]))
            if t["want"] != len(t["bytes"]):
                bad += 1
                print("   %sBYTES says %d, the array has %d: BuildStub would"
                      " refuse to patch that rung at runtime"
                      % (STUB_SHAPE[t["rung"]][1], t["want"], len(t["bytes"])))
            for label, where in (("SLOT_FIXUP", t["fixup"]), ("SLOT_RESUME", t["resume"])):
                if where is None:
                    continue
                want_pre = 0xB8 if t["bytes"][where - 1] == 0xB8 else 0x68
                if t["bytes"][where - 1] not in (0xB8, 0x68) or t["bytes"][where:where + 4] != b"\x00\x00\x00\x00":
                    bad += 1
                    print("   %s%s=%d does not point at a zeroed imm32 operand:"
                          " install would write the address into the middle of an"
                          " instruction" % (STUB_SHAPE[t["rung"]][1], label, where))
                elif where + 4 > len(t["bytes"]):
                    bad += 1
                    print("   %s%s=%d is past the end of the table"
                          % (STUB_SHAPE[t["rung"]][1], label, where))
        if thunk is None:
            bad += 1
            print("   mod.c has no gx_rect_thunk table: the refill cannot call"
                  " GxTexUpdateRect with its texture in ecx, which is exactly"
                  " what faulted in sv20 at 0x594b9e")
        else:
            print("thunk gx_rect_thunk from mod.c: %d bytes (want %d), target slot %s,"
                  " page offset %#x"
                  % (len(thunk["bytes"]), thunk["want"],
                     ("%d" % thunk["slot"]) if thunk["slot"] is not None else "none",
                     thunk["page"] if thunk["page"] is not None else 0))
            if thunk["want"] != len(thunk["bytes"]):
                bad += 1
                print("   GX_RECT_THUNK_BYTES says %d, the array has %d: BuildStub"
                      " refuses to patch and the refill is left alone"
                      % (thunk["want"], len(thunk["bytes"])))
            where = thunk["slot"]
            if (where is None or where + 4 > len(thunk["bytes"])
                    or thunk["bytes"][where - 1] != 0xB8
                    or thunk["bytes"][where:where + 4] != b"\x00\x00\x00\x00"):
                bad += 1
                print("   GX_RECT_THUNK_SLOT_TARGET does not point at a zeroed"
                      " mov imm32: install would write the client address into"
                      " the middle of an instruction")
            if thunk["page"] is None:
                bad += 1
                print("   STUB_PAGE_THUNK is not defined: where the thunk lands"
                      " on the execute page is unchecked")
            else:
                for t in tables:
                    if thunk["page"] < len(t["bytes"]):
                        bad += 1
                        print("   STUB_PAGE_THUNK=%#x is inside the %s detour table"
                              " of %d bytes: the client's jmp and the thunk"
                              " would run over each other"
                              % (thunk["page"], t["name"], len(t["bytes"])))
                if thunk["page"] + len(thunk["bytes"]) > 0x1000:
                    bad += 1
                    print("   the thunk at %#x runs past the 0x1000 page"
                          % thunk["page"])
    except OSError as exc:
        print("cannot read mod.c (%s): falling back to the hardcoded needles"
              " and leaving the thunk unchecked" % exc)
        thunk = None
        tables = []
        for rung in ("b", "c", "d"):
            hexs, want, fx, rs = FALLBACK_STUBS[rung]
            name, pre, pushed, tail = STUB_SHAPE[rung]
            tables.append({"rung": rung, "name": name, "bytes": bytes.fromhex(hexs),
                           "want": want, "fixup": fx, "resume": rs,
                           "pushed": pushed, "tail": tail})

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

        # ---- the detour's esp math: what the callee actually did -------------
        cleaned = callee_cleanup(path, blob, pe, base, md)
        if cleaned is None:
            print("   fixup_chunk not in a sibling map (a copy in the game folder"
                  " has none): the epilogue form is unchecked here, the two"
                  " build outputs are the ones that decide")
        else:
            if cleaned:
                print("   fixup_chunk epilogue: ret %#x (the callee already drops"
                      " %d bytes)" % (cleaned, cleaned))
            else:
                print("   fixup_chunk epilogue: plain ret, the caller cleans")
        for t in tables:
            if t["pushed"] is None:
                continue  # mode c calls nothing: there is no add to match
            want = None if cleaned is None else (t["pushed"] - cleaned)
            call_at = t["bytes"].find(b"\xff\xd0")
            add_at = t["bytes"].find(b"\x83\xc4", call_at + 2) if call_at >= 0 else -1
            got = (t["bytes"][add_at + 2]
                   if add_at >= 0 and add_at + 3 <= len(t["bytes"]) else None)
            print("   stub %s calls the fixup with %d pushed bytes, then adds esp,%s"
                  % (t["rung"], t["pushed"],
                     "%#x" % got if got is not None else "nothing"))
            if want is not None and got != want:
                bad += 1
                print("     want esp,%#x (%d pushed minus the %d the epilogue"
                      " already dropped): off by %d, popad restores every"
                      " register from the wrong slot and the client faults on the"
                      " first chunk"
                      % (want, t["pushed"], cleaned,
                         abs(want - (got if got is not None else 0))))

        # ---- the detours: one const table per rung, in .rdata or wherever ----
        for t in tables:
            stub, fx, rs = t["bytes"], t["fixup"], t["resume"]
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
                print("   stub %s (%d bytes) found in %s at %#x (.text/.rdata"
                      " offsets do not matter: BuildStub copies this table into"
                      " an executable page of its own at install)"
                      % (t["rung"], len(stub), label, va))
                print("     bytes: %s" % " ".join("%02x" % b for b in raw[at:at + len(stub)]))
                for label2, where in (("fixup", fx), ("resume", rs)):
                    if where is None:
                        continue
                    if raw[at + where - 1] not in (0xB8, 0x68) or raw[at + where:at + where + 4] != b"\x00\x00\x00\x00":
                        bad += 1
                        print("     the %s patch slot of stub %s is not a zeroed"
                              " imm32 operand" % (label2, t["rung"]))
                ins_list = list(md.disasm(raw[at:at + len(stub)], va))
                for ins in ins_list:
                    print("     %#010x: %-22s %s %s"
                          % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
                if not ins_list or ins_list[-1].mnemonic != t["tail"]:
                    bad += 1
                    print("     stub %s does not end in %s: control would fall"
                          " through into whatever follows the table"
                          % (t["rung"], t["tail"]))
            if found == 0:
                bad += 1
                print("   the %d-byte stub %s table is NOT in any section of this"
                      " image: BuildStub's memcpy has no source to copy for that"
                      " rung, so its detour is dead (the array was optimised out"
                      " or the link failed)" % (len(stub), t["rung"]))
            elif found > 1:
                bad += 1
                print("   the stub %s table appears in %d sections: two copies"
                      " means BuildStub and the shipped bytes could disagree"
                      % (t["rung"], found))

        # ---- the refill's client-call thunk -------------------------------
        if thunk is None:
            print("   no thunk table parsed from mod.c: its bytes are unchecked"
                  " here, and a refill that reaches GxTexUpdateRect through a"
                  " plain C pointer faults inside the client, as sv20 did")
        else:
            found = 0
            for sec in pe.sections:
                label = sec.Name.rstrip(b"\0").decode("ascii", "errors")
                size = sec.SizeOfRawData
                raw = blob[sec.PointerToRawData:sec.PointerToRawData + size]
                at = raw.find(thunk["bytes"])
                if at < 0:
                    continue
                found += 1
                va = base + sec.VirtualAddress + at
                print("   thunk gx_rect_thunk (%d bytes) found in %s at %#x"
                      % (len(thunk["bytes"]), label, va))
                ins_list = list(md.disasm(raw[at:at + len(thunk["bytes"])], va))
                for ins in ins_list:
                    print("     %#010x: %-22s %s %s"
                          % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
                if not ins_list:
                    bad += 1
                    print("     the thunk disassembles to nothing: it is not code")
                    continue
                if ins_list[-1].mnemonic != "ret":
                    bad += 1
                    print("     the thunk does not end in ret: control would fall"
                          " through into whatever follows it on the execute page")
                pushes = [i.op_str for i in ins_list if i.mnemonic == "push"]
                calls = [i for i in ins_list if i.mnemonic == "call"]
                def takes(mn, reg, word):
                    return [i for i in ins_list
                            if i.mnemonic == mn
                            and i.op_str.replace(" ", "").startswith(reg + ",")
                            and word in i.op_str.replace(" ", "")]

                ecx = takes("mov", "ecx", "esp+4")
                size = takes("mov", "eax", "esp+8")
                if len(pushes) != 4 or "1" not in pushes or "0" not in pushes:
                    bad += 1
                    print("     the thunk pushes %s instead of 1, the size twice"
                          " and 0: the callee's rect and its device flag"
                          " would not be the client's own"
                          % ("[" + ",".join("'%s'" % p for p in pushes) + "]"))
                if len(calls) != 1 or calls[0].op_str != "eax":
                    bad += 1
                    print("     the thunk does not call eax once: what reaches"
                          " 0x58ac70 is not what install patched")
                if not ecx:
                    bad += 1
                    print("     nothing puts the texture in ecx: that is the"
                          " register whose absence wrote 0x140 in sv20")
                if not size:
                    bad += 1
                    print("     nothing reads the size C passed off the stack:"
                          " the rectangle would be a constant, not the tile"
                          " shadowLevel decided")
                where = thunk["slot"]
                if where is not None and (raw[at + where - 1] != 0xB8
                                          or raw[at + where:at + where + 4]
                                          != b"\x00\x00\x00\x00"):
                    bad += 1
                    print("     the thunk's patch slot in this image is not a"
                          " zeroed mov imm32: install would overwrite an"
                          " instruction operand")
            if found == 0:
                bad += 1
                print("   the %d-byte thunk table is NOT in any section of this"
                      " image: BuildStub's memcpy has no source to copy, so no"
                      " refill can ask the device to refill a texture"
                      % len(thunk["bytes"]))
            elif found > 1:
                bad += 1
                print("   the thunk table appears in %d sections: two copies"
                      " means BuildStub and the shipped bytes could disagree"
                      % found)

    print("dlls %d" % len(dlls))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
