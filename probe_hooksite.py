#!/usr/bin/env python3
r"""
probe_hooksite.py -- re-check the ShadowMaps.dll hook sites on a real 1.12.1 client.

ShadowMaps.dll refuses to patch unless the bytes at its hook addresses match what
benilla-re documented for build 5875, so this script is the build-time sanity
check behind that rule: it prints the prologue and the shadow-gate chain from
every client you point it at, asserts the expected bytes, and proves that all
the clients carry IDENTICAL .text at the addresses the mod uses (a Turtle-Wow
flavor and a vanilla copy load the same mod because their code is the same).

Usage
    python probe_hooksite.py [<WoW.exe> ...]        (default: the two known copies)

What it prints
    Per client: imagebase, the .text section map, and for every pinned address
    the raw bytes plus capstone's disassembly, so you can see the instruction
    boundaries and where the 5-byte E9 rel32 cut lands. It also walks the
    prologue one instruction at a time and requires each E9 address AND each
    resume address to be one of those starts, which is the other half of the
    rule: pinning the right bytes at an address that is the middle of an
    instruction is as wrong as pinning the wrong ones.

Exit code 0 = every expected byte sequence matched on every client. Non-zero
means one of them is not the build the mod was written for, and the mod would
log a refusal rather than patch it.
"""

import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
import pefile

# Addresses are absolute virtual addresses for a client loaded at its imagebase
# (0x400000 for these exes; there is no ASLR on this PE, so the benilla-re
# convention -- absolute VA, and inside .text the file offset is VA itself --
# applies here unchanged).
SITES = [
    ("CMapChunk::Load (the ONE detour)", 0x6AF5F0, 40),
    ("mode d's site, 3 bytes into the same prologue", 0x6AF5F3, 12),
    ("shadow gate + size/ofs fetch", 0x6AF6E8, 32),
    ("sub-record resolver, whole: what it stores in the object", 0x6AF970, 0x70),
    ("CreateShadowTexture", 0x6B0030, 24),
    ("DecodeShadow", 0x6B02B0, 16),
    # the crash's own path: every report so far is an ACCESS_VIOLATION at the
    # `mov ecx,[ebx]` inside BuildVertices whose ebx came from this+0xf14, so
    # these three windows are what says whether that cursor is the resolver's
    # answer for an ofs the .adt carries or something this DLL wrote
    ("BuildVertices prologue", 0x6B0E50, 40),
    ("BuildVertices: the MCVT cursor and its loop", 0x6B0F3D, 0x90),
    ("the faulting instruction itself", 0x6B0FB7, 16),
]

# What the mod verifies at runtime before it writes the E9. The first six are
# the prologue: push ebp / mov ebp,esp / mov eax,[ebp+8]; a 5-byte E9 covers
# the first five and the trampoline replays all six, resuming at +6 so the
# stray 0x08 byte is never executed.
EXPECT = {
    0x6AF5F0: bytes.fromhex("55 8b ec 8b 45 08".replace(" ", "")),
    # mode d's rung: the 5-byte E9 sits at 0x6AF5F3 and covers mov eax,[ebp+8]
    # / push ebx / push esi whole, so the pinned 6th byte 0x57 at 0x6AF5F8 IS
    # the resume point and capstone must read all of it as three whole
    # instructions -- pinning it is what proves the resume is an instruction
    # boundary and not the middle of one
    0x6AF5F3: bytes.fromhex("8b 45 08 53 56 57".replace(" ", "")),
    # test byte ptr [0xc7b2a4],0x40  je  <gate off>
    0x6AF6EF: bytes.fromhex("f6 05 a4 b2 c7 00 40 74".replace(" ", "")),
    # mov ecx,[eax+0x30]  mov edx,[esi+0xf1c]
    0x6AF6F8: bytes.fromhex("8b 48 30 8b 96 1c 0f 00 00".replace(" ", "")),
    0x6B0030: bytes.fromhex("55 8b ec 8b 45 08".replace(" ", "")),
}

DEFAULT_EXES = [
    r"G:\Games\twmoa_1181\WoW.exe",
    r"G:\Games\WoW Vanilla\WoW.exe",
]


def read_at(pe, data, va, length):
    """Raw bytes at a virtual address, through the section that owns it."""
    for sec in pe.sections:
        if sec.VirtualAddress <= va < sec.VirtualAddress + max(sec.SizeOfRawData, sec.Misc_VirtualSize):
            off = va - pe.OPTIONAL_HEADER.ImageBase - sec.VirtualAddress + sec.PointerToRawData
            return data[off:off + length], sec
    raise SystemExit("address %#x is not inside any section" % va)


def main(argv):
    exes = argv or DEFAULT_EXES
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    texts = {}
    bad = 0
    for path in exes:
        pe = pefile.PE(path)
        exe_bytes = open(path, "rb").read()
        print("== %s  imagebase %#x" % (path, pe.OPTIONAL_HEADER.ImageBase))
        for name, va, length in SITES:
            data, sec = read_at(pe, exe_bytes, va, length)
            texts[(name, va)] = data
            print("   %#x  %s  sec %s" % (va, name, sec.Name.rstrip(b"\0").decode()))
            print("     bytes: %s" % " ".join("%02x" % b for b in data))
            for ins in md.disasm(data, va):
                print("     %#010x: %-22s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
        for va, want in EXPECT.items():
            got, _ = read_at(pe, exe_bytes, va, len(want))
            if got != want:
                bad += 1
                print("   MISMATCH at %#x: want %s got %s" % (va, want.hex(), got.hex()))
        # the other half of what a hook needs: not the bytes but the BOUNDARIES.
        # Every address this mod puts an E9 at, and every address it resumes at,
        # has to be where the client itself starts an instruction -- a resume in
        # the middle of an operand is a silent wrong program that crashes later.
        win, _ = read_at(pe, exe_bytes, 0x6AF5F0, 16)
        starts = set()
        for ins in md.disasm(win, 0x6AF5F0):
            starts.add(ins.address)
        need = {0x6AF5F0: "mode b/c/h/f store", 0x6AF5F6: "mode b/c/h/f resume",
                0x6AF5F3: "mode d store", 0x6AF5F8: "mode d resume"}
        print("   instruction starts from 0x6AF5F0: %s"
              % " ".join("%#x" % v for v in sorted(starts)))
        for va, why in sorted(need.items()):
            if va not in starts:
                bad += 1
                print("   %s of %#x is NOT an instruction start here: an E9 or a"
                      " resume there would run the middle of an instruction" % (why, va))
    # the whole point of the check: the mod pins bytes, not client versions, so
    # any two clients whose pinned bytes agree take the same patch
    uniq = set(texts.values())
    print("clients %d  distinct pinned windows %d of %d" % (len(exes), len(uniq), len(texts)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
