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

## Which bake to run: the sun presets

A shadow set is only meaningful together with the sun that cast it, and Salmiak
names that sun for you: the **Sun preset** group in the Shadows panel
(`Salmiak/MainWindow.ShadowPanel.cs`) sits above the two sun sliders, and ticking
a preset writes its pair of degrees straight into the sliders the bake reads —
a shortcut, not a mode, and the sliders stay free to drag to anything else. The
bake then says the hour in its opening line (`Ray-baking 4096 chunk(s) at sun 42°
/ 63° = Morning 06:00`) and in the `start`, `landed` and `stopped` records of
`whole-bake-diag.txt`, which is what keeps a `shadows-Azeroth-morning` variant
attributable to the angles that produced it after both have been exported.

| Preset | Sliders | Reads back as | Why it is or is not worth shipping |
| --- | --- | --- | --- |
| Blizzlike - stock shadows | `29 / 33` | 45.5° of bearing, 47.2° of height | the nearest pair of whole degrees to the sun the stock 1.12.1 `MCSH` payloads were cast at, and the pair Salmiak's defaults still carry. **Shipping this is a no-op-looking variant**: it reproduces what the client's own `.adt`s already have, so it is the calibration run, not the payload to sell. |
| Morning 06:00 | `42 / 63` | 45.3°, 19.7° | the client's own polar track at 06:00, the longest shadows vanilla has. **This is the variant to ship** — it is what the `shadowmaps.dat` in the tables above was extracted from, and a long-shadow set is the one that visibly differs from stock. |
| Evening 18:00 | `42 / 63` | same pair | the client's polar track is symmetric about 12:00 and its azimuth is one constant all day, so vanilla has exactly one low-sun shape: Evening *is* Morning, and both ticks light at once. There is no separate evening set to bake. |
| Noon 12:00 | `34 / 43` | 44.7°, 37.3° | the polar's 127° key: same bearing, shorter shadows. |
| Night - moon 00:00 | `24 / 26` | 45.4°, 55.2° | the stock client ships **one** static shadow map sampled whatever hour it is, so night is not on the sun's tracks at all; this uses the white moon's own tracks (same bearing, a higher light). The low-sun pair is the wrong shape for it. |

The pairs are not rounded from the client's angles: each is the whole-degree pair
that re-solves nearest to its hour, per the bake's own ray
`(cos H sin R, sin H, cos H cos R)`. The hour-to-track reading comes from
`benilla-re` (`systems/lighting/celestial.md`), and it is the *bearing* that never
changes across a vanilla day — only the height does, so a morning variant and a
noon variant differ in shadow length, not direction.

Run one variant end to end at a time: a fresh whole-map bake drops the packed
bits the previous walk left in memory, so export (or discard) the morning set
before starting the noon one.

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
| `ADDR_LOAD` | `0x6AF5F0` | `CMapChunk::Load(this, record, first)` — where rungs `b` and `c` put the `E9`; `b` crashed from it and `c` came back clean |
| `ADDR_LOAD_D` | `0x6AF5F3` | the same function 3 bytes in, where rungs `d`, `h` and `f` put their `E9` — the shape a real run proved good, so it is the shape the working rungs ride |
| resume | `0x6AF5F6` / `0x6AF5F8` | `b`/`c` replay the 6-byte prologue `55 8b ec 8b 45 08` and resume after it at `0x6AF5F6`, so the 5-byte `E9` leaves a dead `0x08`; `d`/`h`/`f` cover `mov eax,[ebp+8]` / `push ebx` / `push esi` whole and resume on the `57` `push edi` at `0x6AF5F8` — both resume points entered the world clean |
| `ADDR_GATE` | `0x6AF6EA` | the shadow gate pinned as one run: `test [hdr],1 / je / test [0xc7b2a4],0x40 / je / mov ecx,[hdr+0x30] / mov edx,[this+0xf1c]` |
| `ADDR_MAP_NAME` | `0xC961A0` | `s_mapName`: only apply the payload on the map the `.dat` was extracted for |
| chunk fields | `+0x8c` `+0x90` | global column/row (`gx`, `gy`) |
| `hdr` | `record + 8` | the client's own `lea edx,[eax+8]` at `0x6AF970`: `MCNK` magic, then header fields |
| `MCNK` writes | `hdr+0x2c` `hdr+0x30` | `ofsMCSH` (relative to `hdr`, lands inside the arena), `sizeMCSH = 512` |
| stub `b` | 36 emitted bytes | `STUB_BYTES 36`, slots `STUB_SLOT_FIXUP 12` `STUB_SLOT_RESUME 30`; finds the record at `[esp+48]` after `pushad`/`pushfd` and an 8-byte scratch reservation |
| stub `c` | 12 emitted bytes | `STUB_C_BYTES 12`, one slot `STUB_C_SLOT_RESUME 7`: replays the prologue and returns through `push <resume>` / `ret`, calls nothing |
| stub `d`, also `h` and `f` | 29 emitted bytes | `STUB_D_BYTES 29`, slots `STUB_D_SLOT_FIXUP 7` `STUB_D_SLOT_RESUME 24`; takes the record from `[ebp+8]` and `this` from `ecx`, so no `esp` displacement is part of finding the arguments |

`probe_hooksite.py` re-reads those pinned windows from a real `WoW.exe` and
disassembles them; it exits non-zero when a client is not the build this mod
was written for. It also walks `CMapChunk::Load` one instruction at a time and
requires **every address the mod puts an `E9` at and every address it resumes at**
to be one of those starts: pinning the right bytes at an address that is the
middle of an operand is as wrong as pinning the wrong ones, and `0x6AF5F8` being
where the client itself starts a `push edi` is what makes rung `d` legal.

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
build — the client's folder has no reason to carry vcruntime or ucrt DLLs), and
**all three** detour tables present exactly once each — `b`'s 36 bytes, `c`'s 12
and `d`'s 29 — every patch slot on a zeroed `mov eax,imm32` or `push imm32`
immediate, each table disassembled back out of the linked file, and each calling
table's `add esp,N` matched against the epilogue `fixup_chunk` actually got from
this link (`b` 16 and `d` 8 with a plain `ret`; 8 and 0 with a `ret 8`; `c` calls
nothing so it has no add to match).

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

`out\ShadowMaps.dll` 138,752 bytes and `build\Release\ShadowMaps.dll` 139,776
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
[12 ms after this DLL was loaded] sv9 mode 'f' read from 'G:\Games\twmoa_1181\shadow-mod.mode' (no such file means f): n = inert, a = .dat plus arena, no hook, b = hook plus an empty call, c = the detour alone with no call at all, d = the same call from the client's own frame at 0x6AF5F3, h = hook plus audit, no payload, f = full
sv9 no shadowmaps.dat where the DLL is: the client keeps stock shadows; looked in 'G:\Games\twmoa_1181\shadowmaps.dat' (rename the variant .dat to that name, it belongs beside the DLL, not beside the game)
sv9 mode n: nothing read, allocated or patched, the client is untouched; shadows stay stock, the popup is expected
sv9 handler: first-chance vectored handler registered, so the next access violation logs the chunk that was being built and the address the client read (the client's own report is still written afterwards)
sv10 mode c installed: hook 0x6AF5F0 patched, resume 0x6AF5F6, detour at 0x........, the landing replays the prologue and returns without calling anything -- no .dat read, nothing allocated, no chunk header read or touched (shadows stay stock; this mode exists to test the store and the landing alone)
sv10 mode d installed: hook 0x6AF5F3 patched (the 5-byte E9 covers mov eax,[ebp+8] / push ebx / push esi), resume 0x6AF5F8 (push edi), detour at 0x........, fixup returns immediately -- no .dat read, nothing allocated, no chunk header read or touched (shadows stay stock; this mode exists to test the stub's argument lookup)
sv10 first fixup call: this 0x........ record 0x........ after 1 visits, mode d -- the hooking rungs print this once, mode c never does
sv10 mode b installed: hook 0x6AF5F0 patched, resume 0x6AF5F6, detour at 0x........, fixup returns immediately -- no .dat read, nothing allocated, no chunk header read or touched (shadows stay stock; this mode exists to test the stub alone)
sv10 mode h installed: hook 0x6AF5F3 patched (the 5-byte E9 covers mov eax,[ebp+8] / push ebx / push esi), resume 0x6AF5F8 (push edi) -- d's shape, the one the clean mode-d run proved, NOT b's [esp+48] frame, detour at 0x........, fixup audits the first chunk whole and dumps every chunk's ofss -- no .dat read, nothing allocated, no chunk header touched (shadows stay stock)
sv9 mode a installed footprint only: map 'Azeroth' variant 'shadows-Azeroth-morning', entries 114328, payloads 66413, arena 32 MB resident at 0x........0x........ NO hook, no fixup call, no chunk header touched, so the client runs its own code path whole: ...
sv10 mode f installed: map 'Azeroth' variant 'shadows-Azeroth-morning', entries 114328, payloads 66413, arena 32 MB at 0x........0x........, hook 0x6AF5F3 patched with resume 0x6AF5F8 -- d's shape, the one the clean mode-d run proved, NOT b's frame, detour at 0x........; matched chunks get flags |= 1, hdr+0x2c = arena buffer - hdr and hdr+0x30 = 512, so the client's own gate, CreateShadowTexture and the decoders read live .dat bytes
handler: ACCESS_VIOLATION c0000005 at 6b0fb7, read of 6cf97f32 -- the address the client dereferenced. last chunk seen: this 2b850008 record 2adbe132, fixup visits 1
handler: the object this chunk carries: hdr@f08 0x........, record@168 0x........, MCVT@f14 0x6cf97f32, MCNR@f18 0x........, ..., this+0xf14 == the address the client read
handler: the dying chunk's header: hdr 0x........ flags 0x8009 indexX 0x2 indexY 0xa, ofs MCVT 0x88 MCNR 0x2d4 MCLY 0x49c MCRF 0x4b4 MCAL 0x6c4 MCSH 0x4bc MCSE 0x1364 MCLQ 0x6cc, global col 0x1b2 row 0x20a
last chunk seen: this 0x........ record 0x........ hdr 0x........ flags 0x8009 indexX 0x2 indexY 0xa, ofs MCVT 0x88 MCNR 0x2d4 MCLY 0x49c MCRF 0x4b4 MCAL 0x6c4 MCSH 0x4bc MCSE 0x1364 MCLQ 0x6cc, global col 0x1b2 row 0x20a
audit first chunk: record 0x........ fourcc 'KNCM' size 0x1364, hdr 0x........ flags 0x8009 indexX 0x2 indexY 0xa
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

## Mode ladder (sv10)

Two real runs settled the last open question about the detour. sv9's `c` (the
store and landing at `0x6AF5F0`, a landing that **calls nothing**) and `d` (the
store at `0x6AF5F3`, the call, the record read from `[ebp+8]` on the client's own
live frame) **both entered the world clean, with no new crash report and stock
shadows** — stock being exactly what two rungs that write nothing must show. The
handler block never printed for either, so there was no second access violation.
Put together with the earlier `n`-clean and `a`-clean runs and the sv7/sv8
crashes, the list of what is proven is now: the 34 MB arena, the `.dat` read, the
5-byte store, the landing, the replayed prologue, and **both** resume points
(`0x6AF5F6` and `0x6AF5F8`); `verify_dll.py` re-checked all three detour tables
in the linked image, and `probe_hooksite.py` insists every store address and every
resume address is a real instruction start in the client. **The only piece of the
detour still known to be wrong is `b`'s `[esp+48]` argument lookup** through
`pushad`/`pushfd` and its 8-byte scratch — and sv9's own `h`/`f` runs never got
as far as the client: they printed `build defect: the detour byte table and its
slot offsets disagree, refusing to patch`, because `BuildStub`'s sanity check
expected a `0x68` push in front of every resume slot while `b`'s resume is the
operand of a `0xB8` `mov eax,imm32`. sv10 fixes that check to be shape-aware and
puts the rungs that do real work (`h`, `f`) on **`d`'s shape**, since that is the
shape a real run proved good; `b` keeps its own shape so that re-running it is
still the confirmation that the lookup was the whole difference.

Every hooking rung now also prints one `first fixup call: this .. record ..` line
the first time the fixup is entered — printed **before** the mode gates, so even a
silent rung prints it. Without that line a clean run means "the stub never called"
rather than "the call works", and mode `c`, whose landing calls nothing, is the
one rung that must not print it.

Seven rungs, the first letter of a `shadow-mod.mode` file **next to the DLL** (next
to `ShadowMaps.dll` and `shadowmaps.dat`, not next to the sources). A missing file
means `f`.

| Letter | What runs | What it separates |
| --- | --- | --- |
| `n` | nothing at all: the `.dat` is not opened, nothing is allocated, the client's bytes are not even read | a crash here means the fault has nothing to do with this mod. **Ran: clean** |
| `a` | the `.dat` is read and the whole arena is resident, **no hook, no call, no write** | a crash here while `n` was clean would mean the reservation itself breaks the client. **Ran: clean, 34 MB resident** |
| `c` | the store and the landing only: `0x6AF5F0` → stub replays the 6 prologue bytes and returns through `push <resume>` / `ret`, **calls nothing at all**, so no C code of this DLL runs on the client's loading path and eax/ecx/edx/esp/flags are exactly what the replay left them | store + landing + resume point, with nothing else in the picture. **Ran (sv9): clean** — entered the world, stock shadows, its three install lines and nothing else, and no `first fixup call` line because this landing calls nothing |
| `d` | the same call as `b`, found a different way: the `E9` sits at `0x6AF5F3`, its 5 bytes cover `mov eax,[ebp+8]` / `push ebx` / `push esi` whole, the resume is the next instruction boundary `0x6AF5F8` (`push edi`), and the stub takes the record from **`[ebp+8]`** and `this` from `ecx` — no `esp` displacement is part of finding them | `b`'s `[esp+48]` lookup, against everything else. **Ran (sv9): clean** — entered the world with stock shadows. Read `shadow-mod.log.txt` for its `first fixup call` line: that line is what makes this mean *the call happened with a pair of pointers* rather than *nothing happened* |
| `b` | the detour and a call per chunk that does **nothing at all** — no read, no log, no write, nothing allocated, and the only rung left on `b`'s own shape | the rung sv8's evidence is quoted from. **Ran (sv8): crashed** at the same `0x6B0FB7` reading the same `0x6CF97F32`, log ended at its two install lines. With `c` and `d` clean this isolates `b`'s `[esp+48]` lookup, and sv10 keeps this rung only so that re-running it confirms that |
| `h` | the detour, the first-chunk audit whole, and one compact header dump per chunk — no `.dat`, nothing allocated, no header touched, and riding **`d`'s shape** now | the audit and its `fopen`/`fprintf` inside the client's loading path. sv7 ran it on `b`'s shape and it crashed; sv9's run never installed at all (`build defect`); **sv10 has not run yet** — it must print the `first fixup call` line, the audit block and the per-chunk dumps, not just its install lines |
| `f` | everything: `.dat`, arena, hook and the matched-chunk writes, riding **`d`'s shape** | the only rung expected to draw shadows. sv7 crashed on `b`'s shape and never reached a `first chunk matched` line; sv9 refused at install; **sv10 has not run yet** — a clean `f` is the real confirmation and needs that `first chunk matched` line plus shadows on the ground |

`n` and `a` install nothing on purpose, so `Load` answers **4**, and VanillaFixes
answers any non-zero `Load` with its generic
`was not loaded properly (load function failed)` popup — for those two rungs the
popup is expected and means nothing; the log line is the whole story. `c`, `d`,
`b`, `h` and `f` hook, load successfully, and show no popup.

Two instruments, and the second one is why a crash report no longer has to be
guessed at. A **first-chance vectored handler** (`AddVectoredExceptionHandler`
with `CALL_FIRST = 1`, resolved by name off `kernel32.dll`) is registered once in
`Install`, for every mode past `n`, **before** the store is written — so a fault
inside the detour itself is already named by the time the store exists. It claims
nothing: it logs one block and answers `EXCEPTION_CONTINUE_SEARCH` (0, per
`EXCPT.H`), so WoW's own handler still writes its report afterwards.

```
handler: ACCESS_VIOLATION c0000005 at 6b0fb7, read of 6cf97f32 -- the address the
  client dereferenced. last chunk seen: this 2b850008 record 2adbe132, fixup visits N
handler: the object this chunk carries: hdr@f08 .., record@168 .., MCVT@f14 ..,
  MCNR@f18 .., MCSH@f1c .., MCLY@f20 .., MCAL@f24 .., MCRF@f28 .., MCLQ@f2c ..,
  MCSE@f30 ... this+0xf14 == the address the client read
handler: the dying chunk's header: hdr .. flags .. indexX .. indexY .., ofs MCVT ..
  MCNR .. MCLY .. MCRF .. MCAL .. MCSH .. MCSE .. MCLQ .., global col .. row ..
```

The first line is the answer to "which instruction faulted and which address did
it read", which is the whole question rung `c` asks — and `c` captures nothing, so
its run prints the second line of the pair instead, saying so. The other two lines
are the ones `d`, `b` and `h` print: the object's own fields, which is what the
client's resolver last wrote into `this+0xf08` and `this+0xf14..+0xf30`, and the
header of the chunk that was being built. `this+0xf14 == the address the client
read` is the interesting case: the poison cursor is already in the object before
`BuildVertices` reads it, so the question becomes where it got there — and the
header line says whether the `ofs` the resolver used is the `.adt`'s own. The
handler's own read is guarded (`__try` / `__except(1)`), so a bad `this` degrades
to one log line rather than a second crash.

The per-chunk line still backs it up:

```
last chunk seen: this 0x........ record 0x........ hdr 0x........ flags 0x........
  indexX 0x. indexY 0x., ofs MCVT 0x.. MCNR 0x... MCLY 0x... MCRF 0x... MCAL
  0x... MCSH 0x... MCSE 0x.... MCLQ 0x..., global col 0x... row 0x...
```

`Load` runs the fixup **before** its own body maps the header it was handed, so
the last line of these is the chunk whose `BuildVertices` died, and the eight ofss
printed are the values the resolver is about to copy into
`this+0xf14..+0xf30`. If the `.adt`'s own small ofs is printed there while the
crash's `ebx` is a big value, the object's copy was moved by something other than
this DLL; if a big value is printed there, the header the client holds is not the
header the `.adt` file has. The dump is capped at `AUDIT_LOG_MAX` 2000 chunks —
low enough that the whole first screen of loading prints, high enough that the log
stays readable.

Read the rungs in order and stop at the first one that changes the outcome.
Delete `shadow-mod.log.txt` between rungs: the log is appended and every line is
stamped with milliseconds **since this DLL was loaded**, so a fresh run's block
starts near 0 — but only a fresh file makes the blocks obvious. `GetTickCount`
counts milliseconds since the **system** booted, which is why an older log's raw
numbers look like hours and cannot be used to count client starts.

```
echo n > G:\Games\twmoa_1181\shadow-mod.mode      # ran: clean
del G:\Games\twmoa_1181\shadow-mod.log.txt        # between rungs
echo a > G:\Games\twmoa_1181\shadow-mod.mode      # ran: clean
echo c > G:\Games\twmoa_1181\shadow-mod.mode      # ran (sv9): clean, entered the world
echo d > G:\Games\twmoa_1181\shadow-mod.mode      # ran (sv9): clean, entered the world
echo h > G:\Games\twmoa_1181\shadow-mod.mode      # sv10: rides d's shape, run it next
echo f > G:\Games\twmoa_1181\shadow-mod.mode      # sv10: the shipping path
echo b > G:\Games\twmoa_1181\shadow-mod.mode      # ran (sv8): crashed; rerun to confirm the lookup
```

| What the run shows | What it means |
| --- | --- |
| `n` crashes | nothing here is at fault: look at the client and its patch state itself (which `.adt`/MPQ set it is really reading) |
| `n` clean, `a` crashes | the reservation is the cause: keep the payload bounded (expand-on-demand blocks, or map the `.dat`'s payload area) instead of one arena asked for before any chunk loads. **Did not happen: `a` ran clean with all 34 MB resident** |
| `a` clean, a hooking rung crashes and `c` crashes with it | the store, the landing or the resume point is wrong — `c` is the only detour shape that involves nothing else, so the handler block says whether the fault is still in the client's own `BuildVertices` read or inside the stub page. **Did not happen: `c` ran clean** |
| `c` clean, `d` clean | settled by the sv9 runs: the store, the landing, the replayed prologue and **both** resume points are good, so the only detour piece left known-bad is `b`'s `[esp+48]` argument lookup — and that is why `h` and `f` now ride `d`'s shape instead of `b`'s |
| `d` clean but its log has no `first fixup call` line | the detour is in and the client tolerated it, but the stub never reached the call, so `d` proved the store and nothing about the call: read the stub's `push`/`call` bytes again rather than the client |
| `h` clean with a `first fixup call` line, an audit block and per-chunk dumps | the audit and its `fopen`/`fprintf` survive inside the client's loading path, which is what `f` then adds nothing to. **Not yet run on sv10** |
| `h` clean, `f` crashes | the write is the cause, and the handler block plus the `first chunk matched` line say which field of which header. **Not yet reached** |
| `f` enters the world with a `first chunk matched` line and shadows on the ground | the contract holds end to end, and this is the first real confirmation. **Not yet reached** |
| `b` crashes with `c` and `d` clean | `b`'s `[esp+48]` displacement is not where its own `pushad`/`pushfd`/scratch put the record, so it was calling the fixup with a garbage argument pair all along. **What sv8 saw, and what sv10's rungs no longer ship** |
| no `mode` line at all in the log | `Load` was never called, so the loader, not the hook, stopped the mod |

## Honest limits

- **No shadow has ever been drawn, and no rung has ever printed a `first chunk
  matched` line.** Everything above is verified from the built image and from the
  client's own bytes, not from a screenshot. What *is* proven by real runs is the
  front half of the path — arena, `.dat`, store, landing, both resume points and
  the fixup call with its arguments read off the client's frame — and what is
  still unproven is the audit's file I/O on the loading path (`h`) and the header
  write (`f`), which sv9 never got to run at all. The first real confirmation is
  still someone loading it and seeing the shadows.
- The runs that installed left the client crashing inside `BuildVertices` —
   `ACCESS_VIOLATION at 0x6B0FB7`, the `mov ecx,[ebx]` that reads the MCVT
   floats, `ebx = this+0xf14` — with the same referenced address `0x6CF97F32` in
   every report while `this` moved. None of them had a `first chunk matched` line,
   so the chunk that died is one this DLL never wrote to. sv6's audit then settled
   the contract in the client's own memory: `record` really is an `MCNK` record
   (it prints `'KNCM'`, the client's byte-reversed spelling), `hdr = record + 8`,
   every table slot resolves as `hdr + value` with the fourcc 8 before it, and
   the fixed audit (it reads the fourcc at `hdr + ofs - 8`, the resolver's own
   `lea` math) printed all eight as their own fourcc — `'TVCM'` for MCVT at
   `0x88`, `'HSCM'` for MCSH at `0x4bc`, `'ESCM'` for MCSE at `0x1364`, `'QLCM'`
   for MCLQ at `0x6cc` — with the record's own `size 0x1364`, which is exactly
   where the next `MCNK` sits. So the basis, `FIELD_MCSH_OFS 0x2c` and the whole
   sub-record table are settled in the client's memory, byte for byte — for the
   first chunk.
- **The ladder runs are real evidence, and nothing past `a` has drawn a shadow.**
   The sv7 four-rung build: `n` clean, `a` clean with the whole 34 MB arena
   resident and no hook, `h` and `f` crashed entering the world at `0x6B0FB7`
   reading `0x6CF97F32`. The sv8 build added the rung whose fixup returns before
   touching anything, and **it crashed the same way**, with the log ending at its
   two install lines — so no call's work, no allocation and no write is needed for
   the fault, only the hook. Its crash report is also intact: the client's frame is
   where the report says it is, with the return address and both arguments, and
   `this+0xf14` already holds the unmapped `0x6CF97F32` while `ECX` moves. So the
   poison cursor is in the **object** before `BuildVertices` reads it, and the stub's
   own ESP math is not what broke the frame. The sv9 build then ran `c` and `d`, and
   **both entered the world clean** — which is what moved `h` and `f` onto `d`'s
   shape — and its `h`/`f` runs printed a build defect instead of installing, which
   sv10's shape-aware slot check fixes. Nothing about the shadow payload has ever
   been drawn, and no rung has ever reached a `first chunk matched` line.
- The crash dumps the client writes next to its reports (`Errors\*.dmp`, WoW's own
   `MDMP` container: magic, timestamp, then `(type, size, file-offset)` directory
   entries and a memory list of `(address, high, size, file-offset)` blocks) do
   **not** help here. They hold only small windows around the registers — a few
   hundred bytes each — and neither the `CMapChunk` object nor the address it read
   is inside one, so `this+0xf08` and `this+0xf14..+0xf30` cannot be recovered
   after the fact. That is exactly why the first-chance handler exists: it reads
   those fields while the object is still alive.
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
