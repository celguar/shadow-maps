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
| `shadow-mod.mode` | **not in this repo**: the one-letter mode file that lives next to the shipped DLL, see the mode ladder; a missing file means `f` |
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

The write is also checked against the client's own resolver at run time. `0x6AF970`
turns eight header ofss into addresses with one `lea esi,[ofs+record+8]` each,
and `BuildVertices` (`0x6B0E50`) then walks 145 floats off the MCVT one — its
inner loop at `0x6B0FB7` is `mov ecx,[ebx]` with `ebx = this+0xf14` advanced 4
a vertex. So the seven ofss this file does not write (`+0x14 +0x18 +0x1c +0x20
+0x24 +0x58 +0x60`) are sampled before and after the mutation: if any of them
moved, `FIELD_MCSH_OFS` is not `0x2c` on this client, and the run would die in
that vertex loop with an `ACCESS_VIOLATION` on what is really an arena
displacement. In that case the ofss are put back, one log line names `hdr`, the
ofs slot that moved, its value before and after, the arena and the displacement
that was written — enough to recompute the field — and mutation stops for the
rest of the run, so the session finishes instead of crashing.

`extract_shadowmaps.py` already proves the same basis from the file side: before
it writes one `.dat` entry it insists on `b"MCSH"` at `hdr + ofsMCSH - 8`, so in
a stock `.adt` the header's value addresses the 512-byte body with magic+size in
front of it, `sizeMCSH` is 520 there, and `hdr + ofs` is the same address the
client computes. What is therefore *not* settled by reading the file is what the
client has in memory, and `AuditHeader` prints exactly that: for the first chunk
to enter `Load`, and for the first chunk with a payload, it logs the record's own
magic and size, `hdr`, `hdr`'s flags and `indexX`/`indexY`, and for each of the
eight slots the value, the address `hdr + value` resolves to and the fourcc found
there. The MCVT line names the address whose read faulted at `0x6B0FB7`, so the
line says whether the value in `hdr+0x14` is the `.adt`'s own — a mapping the
client did not get — or something this DLL moved. A slot of 0 means the `.adt`
carries no such sub-record, and a value too big to be inside one record is
printed, never dereferenced.

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
its variant name. Allocation refusals name the shape they asked for and the
operating system's own `GetLastError`, so a bad parameter is read, not guessed.

`VirtualAlloc` and `VirtualProtect` do **not** take a bit-or of access bits here:
`<winnt.h>` gives a level family — `0x1` readable, `0x2` write-combine (written
but never read back), `0x4` readable/writable/executable, `0x40` =
`PAGE_EXECUTE_READWRITE`, the same level over every mapping of the page — and
every OR of two of them (`0x3`, `0x7`, `0xF`) is refused outright with
`GetLastError 0x57` "the parameter specified is invalid". So the arena asks
`0x4` and the detour page and the write dance ask `0x40`, which is the value
minhook (ClassicAPI's own submodule) passes for every trampoline page. Verified
by allocating one page per level in a throwaway i686 process and writing to it:
only `0x4` and `0x40` came back with the bytes they were given.

The stub's `add esp, 16` after the fixup call is the other half of the same kind
of trap. MSVC gives a `static` C function a **plain `ret`** — opcode `c3`, no
`imm16` — so the caller, not the callee, must drop the two pushed argument dwords
(`this`, `record`) plus the 8-byte scratch the stub reserves before them: 16 in
total. Asking for 8 instead is silent at compile time and an access violation
inside the client at run time: `popad` then runs 8 bytes low, every register
comes back from the wrong slot, and `ecx` — `CMapChunk::Load`'s `this`, which the
client stores at `this+0x168` at `0x6AF5FF` right after resuming — holds whatever
the client's old `ebx` was. `verify_dll.py` reads `fixup_chunk`'s epilogue out of
the linked image and requires the stub's `add` to match whichever `ret` form this
build chose, so an off-by-eight is a build failure, not an in-game crash.

## What this machine produces

`out\ShadowMaps.dll` 138,240 bytes and `build\Release\ShadowMaps.dll` 139,264
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
[12 ms after this DLL was loaded] sv7 mode 'f' read from 'G:\Games\twmoa_1181\shadow-mod.mode' (no such file means f): n = inert, a = .dat plus arena, no hook, h = hook plus audit, no payload, f = full
sv7 no shadowmaps.dat where the DLL is: the client keeps stock shadows; looked in 'G:\Games\twmoa_1181\shadowmaps.dat' (rename the variant .dat to that name, it belongs beside the DLL, not beside the game)
sv7 mode n: nothing read, allocated or patched, the client is untouched; shadows stay stock, the popup is expected
sv7 mode h installed: hook 0x6AF5F0 patched, resume 0x6AF5F6, detour at 0x........, fixup audits the first chunk only -- no .dat read, nothing allocated, no chunk header touched (shadows stay stock; this mode exists to test the stub)
sv7 mode a installed footprint only: map 'Azeroth' variant 'shadows-Azeroth-morning', entries 114328, payloads 66413, arena 32406 KB resident at 0x..........0x........ NO hook, no fixup call, no chunk header touched, so the client runs its own code path whole: ...
sv7 mode f installed: map 'Azeroth' variant 'shadows-Azeroth-morning', entries 114328, payloads 66413, arena 32406 KB at 0x..........0x........, hook 0x6AF5F0 patched, resume 0x6AF5F6, detour at 0x........; matched chunks get flags |= 1, hdr+0x2c = arena buffer - hdr and hdr+0x30 = 512, so the client's own gate, CreateShadowTexture and the decoders read live .dat bytes
audit first chunk: record 0x........ fourcc 'KNCM' size 0x........, hdr 0x........ flags 0x........ indexX 0x.. indexY 0x..
audit first chunk: MCVT at hdr+0x14 is 0x........ -> address 0x........, fourcc at -8 'MCVT'
audit first chunk: MCSH at hdr+0x2c is 0x........ -> address 0x........, fourcc at -8 'MCSH'
fixup: first chunk with no payload: gy 519 gx 439 key 531895 (the bake left it without a shadow map, or its tile is not in this set)
fixup: first chunk matched: gy 352 gx 352 key 360800, shadow drawn from arena+0, this 0x........ record 0x........ hdr 0x........ arena 0x........, hdr flags 0x........ size 0x200 ofs 0x........, mcvtof 0x........ ... mclof 0x........
fixup: 10000 chunks matched so far, of 114328 entries
```

Every line is prefixed by the milliseconds since **this DLL was loaded** and
carries the build stamp and, for the install lines, the mode letter (the prefix
is written once above for readability — every line carries it), so a log
never has to be read as "which build and which rung made this". The record's
fourcc prints as the client holds it in memory — byte-reversed against ASCII, so
an `MCNK` record reads `'KNCM'` here, the same reversed spelling `extract_shadowmaps.py`
uses on disk — and a table slot's fourcc is read 8 before the address its value
resolves to, which is the resolver's own `lea esi,[ofs+record+8]` math.

…and every 10,000 matched chunks up to 60 lines, which is the in-game evidence
that the arena is what is being drawn and how far under 114,328 the tiles not
in this set leave it. The first-chunk line names the chunk the client really
loaded first and comes in both flavours: this set covers 487 of Azeroth's 741
tiles, so a chunk outside it (`gy 519 gx 439` is tile `27,32`) legitimately logs
"no payload", and only the `first chunk matched` line proves a payload was drawn.
That line also dumps the header the write went into — `this`, `record`, `hdr`,
the arena and all eight sub-record ofss — so the addresses a crash report names
can be matched against it. The `audit` lines are the same facts for a chunk that
was never written to, which is what the current build is about.

## Mode ladder (sv7)

The crash is still only pinned to "the client reads an ADT buffer that is not
there". What sv6's audit settled is the contract itself — in the client's own
memory, `hdr = record + 8`, the fourcc of a sub-record sits 8 before the address
its table slot resolves to, and `FIELD_MCSH_OFS 0x2c` is the right slot — so the
open questions are about **our footprint** and **our stub**, not about the
shadow write. One binary answers them in four rungs, picked by the first letter
of a `shadow-mod.mode` file **next to the DLL** (i.e. next to `ShadowMaps.dll`
and `shadowmaps.dat`, not next to the sources). A missing file means `f`.

| Letter | What runs | What it proves |
| --- | --- | --- |
| `n` | nothing at all: the `.dat` is not opened, nothing is allocated, the client's bytes are not even read | a run that still crashes at `0x6B0FB7` means the fault has nothing to do with this mod — it is the client and its patch state on its own |
| `a` | the `.dat` is read and the whole arena is resident, **no hook, no call, no write** | a run that crashes here while `n` was clean means the reservation itself is what breaks the client, and the arena has to become bounded rather than one 34 MB commit |
| `h` | the detour and the first-chunk audit only — no `.dat`, nothing allocated, no header touched | a run that crashes here while `n` was clean means the stub's frame math or the detour landing is at fault, not the payload and not the write |
| `f` | everything: `.dat`, arena, hook, and the matched-chunk writes | the only rung expected to draw shadows |

`n` and `a` install nothing on purpose, so `Load` answers **4**, and VanillaFixes
answers any non-zero `Load` with its generic
`was not loaded properly (load function failed)` popup — for these two rungs the
popup is expected and means nothing; the log line is the whole story.

Read the rungs in order and stop at the first one that changes the outcome.
Delete `shadow-mod.log.txt` between rungs: the log is appended and every line is
stamped with milliseconds **since this DLL was loaded**, so a fresh run's block
starts near 0 — but only a fresh file makes the blocks obvious. `GetTickCount`
counts milliseconds since the **system** booted, which is why an older log's raw
numbers look like hours and cannot be used to count client starts.

```
echo n > G:\Games\twmoa_1181\shadow-mod.mode      # rung 1
del G:\Games\twmoa_1181\shadow-mod.log.txt        # between rungs
echo a > G:\Games\twmoa_1181\shadow-mod.mode      # rung 2
echo h > G:\Games\twmoa_1181\shadow-mod.mode      # rung 3
echo f > G:\Games\twmoa_1181\shadow-mod.mode      # rung 4, the shipping path
```

| What the run shows | What it means |
| --- | --- |
| `n` crashes | nothing here is at fault: look at the client and its patch state itself (which `.adt`/MPQ set it is really reading) |
| `n` clean, `a` crashes | the reservation is the cause: keep the payload bounded (expand-on-demand blocks, or map the `.dat`'s payload area) instead of one arena asked for before any chunk loads |
| `n` clean, `a` clean, `h` crashes | the stub is the cause: the frame math or the store, and the audit lines say whether the crash is already in the client's own read of the header |
| `n`/`a`/`h` clean, `f` crashes | the write is the cause, and the `audit` lines plus the `first chunk matched` dump say which field of which header |
| `f` enters the world with a `first chunk matched` line and shadows on the ground | the contract holds end to end, and this is the first real confirmation |
| no `mode` line at all in the log | `Load` was never called, so the loader, not the hook, stopped the mod |

## Honest limits

- **No in-game test yet.** Everything above is verified from the built image and
  from the client's own bytes, not from a screenshot. The `MCNK` header writes
  follow the client's decode chain, but the first real confirmation is someone
  loading it and seeing the shadows.
- The runs that installed (sv4 three times, sv6 once) all left the client
   crashing inside `BuildVertices` — `ACCESS_VIOLATION at 0x6B0FB7`, the `mov
   ecx,[ebx]` that reads the MCVT floats, `ebx = this+0xf14` — and in every
   report the same referenced address `0x6CF97F32` while `this` moved. None of
   them had a `first chunk matched` line, so the chunk that died is one this
   DLL never wrote to. sv6's audit then ran in one of those runs and settled the
   contract in the client's own memory: `record` really is an `MCNK` record
   (it prints `'KNCM'`, the client's byte-reversed spelling), `hdr = record + 8`,
   every table slot resolves as `hdr + value` with the fourcc 8 before it — the
   audit read it at the slot address, which is why its fourcc column printed the
   junk 8 bytes late; that column is now fixed, so the next audit says `'MCSH'`
   and `'MCLQ'` where they belong — and the `MCNK size` matched the next
   `MCNK` at `hdr + size`. So the basis and `FIELD_MCSH_OFS 0x2c` are settled,
   and the fault is still the same stable `0x6CF97F32` on a chunk with no payload
   even in sv6's 4 KB-footprint build. What is **not** settled is whether the
   fault has anything to do with this mod at all, and the four rungs above are
   what settles it. Until one of those outcomes is real, the shadow payload has
   still never been seen by the client.
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
  resident; the loader refuses payload areas over 1 GiB. Mode `a` is the one
  rung that asks for the whole arena and then stops — no hook, no write — because
  the reservation itself is the thing that rung is testing.
- `tools-check/` headers are stubs: `--check` against them catches C mistakes,
  and `build.py` only consults them when no real SDK exists, because a real
  header sees prototype shapes a stub cannot (UCRT's `fread` carries an
  element-size argument, and that is exactly the trap the stubs exist to keep
  visible).
