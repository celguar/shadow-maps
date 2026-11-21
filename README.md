# ShadowMaps.dll — Salmiak's baked shadows, drawn from a `.dat`

The 1.12.1 client draws a chunk's shadows from that chunk's `MCNK` shadow map
(`MCSH`). Salmiak bakes shadow maps and exports a shadow **variant**: a patch
archive whose `.adt` files carry new `MCSH` blocks. The client can already draw
those, but only if it re-reads the whole patched `.adt`. This mod skips that:
one small `.dat` file holds every chunk's 512-byte packed shadow map, and at
map load the DLL points the game at that payload instead of the bytes the `.adt`
would have carried. A shadow set is a 33 MB file you can swap, not a 340 MB
archive, and a bake never round-trips through a parsed `.adt`.

## Pipeline

```
Salmiak "Shadows" bake
   -> shadow-variant MPQ export        e.g. G:\Games\twmoa_1181\shadows-Azeroth-morning.mpq
   -> extract_shadowmaps.py            strips every MCSH body out of the archive
   -> shadowmaps.dat                   one 512-byte payload per chunk, global keys
   -> ShadowMaps.dll at map load       rewrites each chunk's MCNK header to point
                                       into the .dat's arena; the client draws it
```

Nothing here edits a `.adt`, an MPQ or a saved game. The client's own shadow
decoder runs unchanged; only the `ofsMCSH`/`sizeMCSH` pair in the `MCNK` header
and the `MCNK` flag bit are written before the client reads them.

## Files

| File | What it is |
| --- | --- |
| `src/mod.c` | the hook DLL: loads the `.dat`, installs the detour, rewrites `MCNK` headers |
| `shadow_dat.h` | the `.dat` layout, shared by the DLL and documented here |
| `shadowmaps.def` | the export list: `Load`, the entry point VanillaFixes calls |
| `CMakeLists.txt`, `CMakePresets.json` | the canonical build (mirrors ClassicAPI's project) |
| `build.py`, `build.ps1` | the no-CMake fallback: names every missing toolchain piece |
| `verify_dll.py` | confirms a built DLL carries what `src/mod.c` claims |
| `extract_shadowmaps.py` | shadow-variant archives → `shadowmaps.dat` |
| `probe_hooksite.py` | re-checks the hook sites against a real client `WoW.exe` |
| `tools-check/` | labelled stub headers, used **only** by `build.py --check` |

## The `.dat` file

All little-endian, packed at natural sizes, nothing aligned to anything else.
The file must be **exactly** `124 + entryCount*8 + payloadBytes` bytes: the
loader refuses a short file (truncated) *and* a long one (junk appended),
because either means the counts and the archive disagree and a shadow set would
be applied from a half-read table.

| Field | Bytes | Meaning |
| --- | --- | --- |
| `magic` | 4 | `SHDW` |
| `version` | 4 | `1` (`SHADOW_DAT_VERSION`) |
| `map` | 24 | map name, NUL-terminated inside the 24 (`Azeroth`) |
| `variant` | 40 | label for the log line, NUL-terminated inside the 40 |
| `entryCount` | 4 | chunks with a payload |
| `payloadUnique` | 4 | distinct payloads (`payloadBytes == payloadUnique*512`) |
| `tiles` | 4 | tiles this set covers |
| tile/chunk bounds | 8 × `int32` | `tx0,ty0,tx1,ty1` then `cx0,cy0,cx1,cy1` |
| `reserved` | 4 | `0` |
| entries | `entryCount` × 8 | `key` (`(gy<<10)\|gx`), `payloadOffset` — **strictly ascending** by key |
| payload area | `payloadUnique` × 512 | packed `MCSH` bodies, 64×64 texels, LSB-first |

The key mirrors Salmiak's own bake selection: a chunk's global coordinates are
`gx = 16*tileX + indexX`, `gy = 16*tileY + indexY`, both 0…1023, so `key =
(gy<<10)|gx` is a 20-bit chunk address — the same key Salmiak stores in
`_bakeChunks`, which is what makes a bake exportable, strippable and handable
to the client without a `.adt` in between. Chunks that share identical shadow
bytes share one payload (including one shared all-zero buffer, so a cleared
shadow really draws nothing); a chunk the bake never touched simply has **no**
entry, and the client keeps whatever stock shadow the `.adt` carries for it.

## Hook contract

`src/mod.c` refuses to patch unless the client's own bytes match the 1.12.1
build this contract was read from (`benilla-re`, `CMapChunk::Load`, build 5875):

| Name | Value | Why |
| --- | --- | --- |
| `ADDR_LOAD` | `0x6AF5F0` | `CMapChunk::Load(this, record, first)` — the one detour |
| resume | `0x6AF5F6` | the 6-byte prologue `55 8b ec 8b 45 08` means the 5-byte `E9` leaves a dead `0x08` |
| `ADDR_GATE` | `0x6AF6EA` | the shadow gate pinned as one run: `test [hdr],1 / je / test [0xc7b2a4],0x40 / je / mov ecx,[hdr+0x30] / mov edx,[this+0xf1c]` |
| `ADDR_MAP_NAME` | `0xC961A0` | `s_mapName`: only apply the payload on the map the `.dat` was extracted for |
| chunk fields | `+0x8c` `+0x90` | global column/row (`gx`, `gy`) |
| `hdr` | `record + 8` | the client's own `lea edx,[eax+8]` at `0x6AF970`: `MCNK` magic, then header fields |
| `MCNK` writes | `hdr+0x2c` `hdr+0x30` | `ofsMCSH` (relative to `hdr`, lands inside the arena), `sizeMCSH = 512` |
| detour | 36 emitted bytes | `STUB_BYTES 36`, patch slots `STUB_SLOT_FIXUP 12` and `STUB_SLOT_RESUME 30` |

`probe_hooksite.py` re-reads those pinned windows from a real `WoW.exe` and
disassembles them; it exits non-zero when a client is not the build this mod
was written for.

## Building

Requires CMake (3.10+) and an MSVC toolchain that can target 32-bit Windows.

The Windows SDK has to be installed too: `<windows.h>` is the SDK's, not the
compiler's, and so are the i686 import libraries (`kernel32.lib`, `ntdll.lib`)
— a compiler alone gives `C1083` at the first `#include` and `LNK1104` at every
Win32 call, and `build.py` names both by filename instead.

```
cmake --preset win32                      # the only supported configuration
cmake --build build --preset release
python verify_dll.py build/Release/ShadowMaps.dll
```

`CMakePresets.json`'s `win32` preset carries `-A Win32`; without it CMake
configures x64 and the result cannot load in the client, so `CMakeLists.txt`
stops that configure with a message rather than shipping a dead DLL.

Fallback, for a machine with MSVC but no CMake:

```
python build.py                    # finds the compiler, its include/lib trees, links out\ShadowMaps.dll
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
python build.py --check            # compiles src/mod.c only, no link: C errors, nothing about i686
```

`build.py` prefers the compiler's own folder that targets i686
(`VC\Tools\MSVC\*\bin\Hostx64\x86\cl.exe`): asked for a target it cannot emit,
the driver answers `D9002` "unknown parameter" and quietly produces an x64
image, so the folder name is checked before the link, not after.

Either path produces an image `verify_dll.py` accepts: `Load` exported
undecorated, i686 machine type, only operating-system DLLs imported (a `/MT`
build — the client's folder has no reason to carry vcruntime or ucrt DLLs), the
36-byte stub table present in the image, both patch slots on zeroed
`mov eax,imm32` immediates, and the stub disassembled back out of the linked
file.

## Install

1. Install VanillaFixes if it isn't already.
2. Copy `ShadowMaps.dll` into the game directory.
3. Add `ShadowMaps.dll` to `dlls.txt`.
4. Rename the chosen variant's `.dat` to `shadowmaps.dat` and put it **next to
   the DLL**, not next to the game.
5. Launch the game with `VanillaFixes.exe`.

Swapping shadow sets is one file: replace `shadowmaps.dat` and restart. The
`.dll` never changes between sets.

`Load` answers VanillaFixes with ClassicAPI's convention: **0 = installed**,
non-zero = refused. So the popup below is not a bug report, it is this mod
saying "I did not install", and the reason is always the first line of
`shadow-mod.log.txt`:

```
DLL G:\Games\twmoa_1181\ShadowMaps.dll was not loaded properly (load function failed)
```

The common cause is exactly the one that line names: the `.dat` still carries
its variant name.

## What this machine produces

`out\ShadowMaps.dll` 134,144 bytes and `build\Release\ShadowMaps.dll` 135,168
bytes, both i686, both exporting `Load`, both importing only `KERNEL32.dll`,
both accepted by `verify_dll.py` with zero complaints — i.e. the stub's stack
math is read from real linked code, not from a comment.

The test payload sits in the game folder as
`G:\Games\twmoa_1181\shadowmaps.dat`: 34,918,204 bytes (33.3 MiB), `SHDW` v1,
map `Azeroth`, variant `shadows-Azeroth-morning`,
114,328 entries, 66,413 unique payloads (34,003,456 payload bytes), 487 tiles in
`(22,22)..(44,60)`, chunk grid `(352,352)..(707,975)`, and `124 + entries*8 +
payloadBytes == filesize`.

Expected log lines, in `shadow-mod.log.txt` next to the DLL. The loader's own
lines carry the build stamp, so an old DLL is never read as a new one. A refusal
line is the whole story of a run that did not install, and VanillaFixes answers
any non-zero `Load` with its own generic `was not loaded properly (load
function failed)` popup — that popup is never the reason, this file is:

```
sv2 no shadowmaps.dat where the DLL is: the client keeps stock shadows; looked in 'G:\Games\twmoa_1181\shadowmaps.dat' (rename the variant .dat to that name, it belongs beside the DLL, not beside the game)
sv2 installed: map 'Azeroth' variant 'shadows-Azeroth-morning', entries 114328, payloads 66413 (32 MB arena), hook 0x6AF5F0 patched, resume 0x6AF5F6, detour at 0x........
fixup: first chunk matched: gy 352 gx 352 key 360800, shadow drawn from arena+........
fixup: 10000 chunks matched so far, of 114328 entries
```

…and every 10,000 matched chunks up to 60 lines, which is the in-game evidence
that the arena is what is being drawn and how far under 114,328 the tiles not
in this set leave it.

## Honest limits

- **No in-game test yet.** Everything above is verified from the built image and
  from the client's own bytes, not from a screenshot. The `MCNK` header writes
  follow the client's decode chain, but the first real confirmation is someone
  loading it and seeing the shadows.
- A shadow set only says "this chunk's shadow map is these 512 bytes". The
  client's own shadow path still decides the rest: the `MCNK` flag bit and the
  world-wide shadow flag gate it, and `shadowLevel` still picks the 64×64 vs
  32×32 mip.
- One `.dat` per map: the payload carries the map name and `fixup` compares it
  against the client's `s_mapName`, so a wrong-map `.dat` is ignored with one
  log line rather than applied somewhere it does not belong.
- A chunk whose `ofsMCSH` was 0 in the archive gets no entry at all, so
  stock-shadow chunks survive a morning set that covers only 487 of Azeroth's
  741 tiles.
- The 5-byte `E9` store is one store on a page the client is executing: a thread
  caught mid-`Load` could in principle see half a jump. The client loads no
  chunk that early, and the mod refuses to patch anything whose bytes do not
  match, which is the whole mitigation.
- The arena is allocated once at `.dat` load and every payload lives in it: no
  copy, no eviction, no per-chunk allocation. A full Azeroth variant is ~33 MB
  resident; the loader refuses payload areas over 1 GiB.
- `tools-check/` headers are stubs: `--check` against them catches C mistakes,
  and `build.py` only consults them when no real SDK exists, because a real
  header sees prototype shapes a stub cannot (UCRT's `fread` carries an
  element-size argument, and that is exactly the trap the stubs exist to keep
  visible).
