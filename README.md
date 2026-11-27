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
   -> shadow-variant MPQ export        e.g. D:\games\wow-1.12.1\shadows-Azeroth-morning.mpq
   -> extract_shadowmaps.py            strips every MCSH body out of the archive
   -> shadowmaps.dat                   one 512-byte payload per chunk, global keys,
                                       every map the archives carried inside one file
   -> ShadowMaps.dll at map load       rewrites each chunk's MCNK header to point
                                       into the .dat's arena; the client draws it
```

Nothing here edits a `.adt`, an MPQ or a saved game. The client's own shadow
decoder runs unchanged; only the `ofsMCSH`/`sizeMCSH` pair in the `MCNK` header
and the `MCNK` flag bit are written before the client reads them.

## Reading an archive that Salmiak did not write

`extract_shadowmaps.py` reads any MPQ that carries `MCSH` blocks, not only Salmiak's
own exports, and this machine's own files are the proof: `Data\patch-3.mpq` and
`Data\patch-6.mpq` (Blizzard's writers, 16,470 and 3,903 files) and `patch-S.mpq`
(219 MB, 350 files, written by another tool) all extract, and re-extracting
`shadows-Azeroth-morning.mpq` after these reader changes still gives the same set as
before it — 487 tiles, 114,328 chunks, 66,413 payloads after dedupe, now written as
35,375,544 bytes because every entry carries its map row (the same set was 34,918,204
bytes under the old one-map entry shape). What a foreign writer does differently is
named by libmpq's flag set (`cmangos/classic/dep/libmpq/libmpq/mpq-internal.h`):

- a hash slot whose block index is `0xFFFFFFFE` is a **deleted** entry, not a used one:
  stock tools rewrite an archive by deleting, and counting those as used makes a table
  of 352 files look like thousands of them and makes the table-decryption sanity check
  refuse the whole archive. This is what `patch-S.mpq` needed first.
- `LIBMPQ_FLAG_ENCRYPTED` (bit 16): the file's sector-offset table and every one of its
  sectors are WEP-encrypted. The seed is recovered the libmpq way — the table's first
  decrypted dword is its own byte size, and the next must sit at most one sector ahead —
  then the table decrypts with `seed - 1` and sector `s` with `seed + s`. `patch-S.mpq`'s
  `(listfile)` is one of these, so without this its names never resolve at all.
- `LIBMPQ_FLAG_SINGLE` (bit 24): the file is one run of bytes and its table is the two
  entries of its own, not a table on disk.
- `LIBMPQ_FLAG_EXTRA` (bit 26): the table carries one more entry, which the sector walk
  never reaches.
- the `.adt`s in these archives open with a small `MVER`/`MHDR` prefix and keep a
  `WOT`-style body (`MCIN`/`MTEX`/`MMDX`/`MWMO`/`MDDF`/`MODF`/`MH2O`) ahead of their
  256 `MCNK`s; the walk follows the file's own top-level chunk list, so those come and go
  without any special case here.
- fourccs are stored as dwords, so an `MCNK` is spelled `KNCM` on disk — which is also
  what Salmiak's own exports and Blizzard's patches do. A writer that stored the plain
  ASCII spelling is read the same way: `fourcc()` matches the raw bytes against
  `KNOWN_MAGICS` before it reverses them, and the `MCSH` header test accepts both
  spellings, so neither convention silently yields zero chunks.
- a hash entry whose block index is out of range, and a `(listfile)` name that resolves
  to nothing, are reported (`N of M listfile names have no hash entry`) instead of being
  guessed at; a tile whose bytes cannot be read is skipped and named
  (`unreadable <name>: <why>`) so one odd file in one archive does not end the run.

One archive can also carry **several maps**: `patch-S.mpq` holds tiles of 12 of them
(Azeroth 174 tiles, Kalimdor 125, and nine smaller ones). One `.dat` now carries them all
in one file — a map table after the header says which rows belong to which map, and every
entry names its row — so the extractor groups tiles by the map folder in the name,
case-insensitively (`World\Maps\Azeroth` and `World\maps\azeroth` are one map, which is how
`Data\patch-3.mpq` and `Data\patch-6.mpq` merge into one 178-tile set), and writes
**one `.dat` whatever the archives carry**: `-o` is the file, and no per-map name is made
from it. A chunk key is only unique *inside* a map, so the DLL picks the run whose name
matches the client's `s_mapName` (`0xC961A0`) and binary-searches just that run; a chunk
outside every run keeps the stock shadow. `--map NAME` narrows the file to that map and
**repeats**, so `--map Azeroth --map Kalimdor` ships both in one file. The DLL still only
ever reads the fixed name `shadowmaps.dat` beside it, so the whole multi-map file is what
gets renamed to that.

The three archives above were extracted side by side, with the numbers each one printed:

| archives | files | tiles | `MCSH` chunks | payloads after dedupe | `.dat` bytes |
| --- | --- | --- | --- | --- | --- |
| `shadows-Azeroth-morning.mpq` (Salmiak) | 490 | 487 | 114,328 | 66,413 | 35,375,544 |
| `patch-S.mpq` (another tool), all 12 of its maps in one file | 350 | 309 | 76,547 | 40,779 | 21,797,872 |
| `Data\patch-3.mpq` + `Data\patch-6.mpq`, `--map Azeroth --map Kalimdor` | 20,373 | 353 | 31,474 | 18,926 | 10,067,980 |

The same archives cut to one map each, under the old one-map entry shape, gave 11,248,644
bytes (`patch-S.mpq`, Azeroth's 38,257 chunks) and 5,099,676 bytes (the stock pair, Azeroth's
15,684 chunks); the legacy files cut this way still sit in the game folder — `shadowmaps.patchS-legacy.dat`
at 11,248,644 and `shadowmaps-Kalimdor.dat` at 7,818,452 — and the sv11 and sv12 loaders read both
unchanged, which is the point of `mapCount 0`. What the loader actually opens today is the multi-map
file under the fixed name, so `shadowmaps.dat` is 21,797,872 bytes there (see "What this machine
produces").

The stock pair overlaps too, so the run gate is doing real work on archives this machine
did not write and did not bake: its one file has **716 chunk keys shared between Azeroth's
and Kalimdor's runs**, the two key ranges being `390736..982538` and `311856..818927`. Payload
interning crossed the run boundary as well — Azeroth's 9,715 and Kalimdor's 9,249 distinct
bodies are 18,964 apart and the file stores 18,926, so 38 bodies are used by both maps and
stored once, which is safe because what decides whether a body is used at all is the entry's
map row, not the blob.

The stock pair has far fewer chunks than tiles × 256 because most of their chunks carry
no `MCSH` at all: the stock 1.12.1 ADTs in a patch MPQ are not shadow-carrying everywhere.
Those chunks get no entry, which is the documented behaviour — the DLL leaves the client's
own shadow state alone for them rather than forcing the flag on with nothing to draw.

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
| Blizzlike - stock shadows | `29 / 33` | 45.5° of bearing (north-west), 47.2° of height | the nearest pair of whole degrees to the sun the stock 1.12.1 `MCSH` payloads were cast at, and the pair Salmiak's defaults still carry. **Shipping this is a no-op-looking variant**: it reproduces what the client's own `.adt`s already have, so it is the calibration run, not the payload to sell. |
| Morning 06:00 | `42 / 63` | 45.3°, 19.7° — sun north-west, shadows south-east | the client's own polar track at 06:00, the longest shadows vanilla has. **This is the variant to ship** — it is what the `shadowmaps.dat` in the tables above was extracted from, and a long-shadow set is the one that visibly differs from stock. |
| Evening 18:00 | `42 / 63` | same pair | the client's polar track is symmetric about 12:00 and its azimuth is one constant all day, so vanilla has exactly one low-sun shape: Evening *is* Morning, and both ticks light at once. There is no separate evening set to bake. |
| Noon 12:00 | `34 / 43` | 44.7°, 37.3° | the polar's 127° key: same bearing, shorter shadows. |
| Night - moon 00:00 | `24 / 26` | 45.4°, 55.2° | the stock client ships **one** static shadow map sampled whatever hour it is, so night is not on the sun's tracks at all; this uses the white moon's own tracks (same bearing, a higher light). The low-sun pair is the wrong shape for it. |

The pairs are not rounded from the client's angles: each is the whole-degree pair
that re-solves nearest to its hour, per the bake's own ray
`(cos H sin R, sin H, cos H cos R)`. The hour-to-track reading comes from
`benilla-re` (`systems/lighting/celestial.md`), and it is the *bearing* that never
changes across a vanilla day — only the height does, so a morning variant and a
noon variant differ in shadow length, not direction.

Which way that bearing points, spelled out once because the compass here is not the
one a map reader assumes: on a north-up map the client's azimuth runs **clockwise**
from +X to +Y, so 0° is west, 90° north, 180° east, 270° south and +X itself is west.
A bearing of ~45° is the north-west quarter, the client's 225° of travel is toward the
south-east, and every preset above therefore puts the sun over the north-west with
shadows running to the south-east — the same way the stock 1.12.1 maps already draw
them, which is how the `29 / 33` default was recognised by eye against the client's own
terrain. The pairs are not mirrored against that look; only the words around them are.

Run one variant end to end at a time: a fresh whole-map bake drops the packed
bits the previous walk left in memory, so export (or discard) the morning set
before starting the noon one.

### The six-set plan: what ships

Shipping one low-sun variant was the old shape of this project. It is not the
shape now, and the reason is the one the sun fix produced: the DLL walks the
client's sun along a real east→south→west arc, so the sun's *bearing* changes
through the day, and a shadow map is baked at exactly one bearing. `MCSH` bodies
are direction-independent — the fixup only repoints a chunk at a payload — so
every bearing change is a re-bake. Six bearings a day is what the user ships, and
one `.dat` carries all six per map, matched to the client's own day clock by the
version-2 record's `h0` word (see the table above).

| Set | Baked at | Its window opens | What the sun is doing there |
| --- | --- | --- | --- |
| 1 | `06:30` | `05:50` | the arc's dawn crossing: the sun is low in the east, this is the first set the day uses |
| 2 | `09:00` | `07:45` | mid-morning |
| 3 | `11:30` | `10:15` | late morning |
| 4 | `15:30` | `13:30` | early afternoon |
| 5 | `19:30` | `17:30` | the last set before the arc hands over |
| 6 | `21:30` | `21:30` | the arc's dusk crossing, so this set owns sunset, the whole night and the pre-dawn hours until the next `05:50` — the set that must read like stock |

Set 6 opens at its own stamp rather than at a midpoint because that is where the
DLL's arc stops moving the sun, and set 1 opens at `05:50`, the arc's dawn
crossing, because before it the light is still stock's and stock shadows are the
ones that match the ground. The four openings in between are midpoints of
neighbouring stamps — a guess for the hours between two bakes, and what `--open`
is for.

The set stamped `21:30` is the one the user bakes at the stock angles for the
whole continent, not the patch's stock shadows: that is why the plan says six
presets rather than five-plus-whatever-the-client-already-has, and it is what
lets stock hours carry custom-zone shadows too.

Honest arena numbers for that file — measured, not extrapolated, from the stand-in
six-set test `.dat` in the game folder (see the section below): 589,793 entries
over 333,072 distinct payloads, so **162.6 MB of arena** plus 4.5 MB of key
arrays, in a 177,610,696-byte file, all six sets of one Azeroth. A real six-set
Azeroth plus a real six-set Kalimdor is the same order of magnitude — call it
~400 MB, not the ~192 MB an earlier round of numbers implied, which was one
continent's EK-side figure. The DLL logs the footprint before it asks for the
arena and refuses over `0x30000000` (768 MiB) of payloads rather than half-fitting
one.

That plan needs the sv17 refill to be visible at a first login, which is where
the client has already built the chunks around the character while its clock
still read midnight: the set that owns the real hour is picked by a clock that
does not know the hour yet, and a resident chunk is never built again. The pass
described in the refill section re-picks exactly those chunks the moment the
clock leaves midnight, so the ground the player stands on comes back with the
set the sky is in. A chunk that loads *after* the crossing needs none of it: its
own load sees the real clock.

## Files

| File | What it is |
| --- | --- |
| `src/mod.c` | the hook DLL: loads the `.dat`, installs the detour, rewrites `MCNK` headers |
| `shadow_dat.h` | the `.dat` layout, shared by the DLL and documented here |
| `shadowmaps.def` | the export list: `Load`, the entry point VanillaFixes calls |
| `shadow-mod.mode` | **not in this repo**: the mode file that lives next to the shipped DLL — see the mode ladder and the Logging section. Its first rung letter (`n`/`a`/`b`/`c`/`d`/`h`/`f`) picks the rung and a `v` anywhere in it turns the diagnostic lines on; a missing file means `f` with quiet logging, which is what ships |
| `CMakeLists.txt`, `CMakePresets.json` | the canonical build (mirrors ClassicAPI's project) |
| `build.py`, `build.ps1` | the no-CMake fallback: names every missing toolchain piece |
| `verify_dll.py` | confirms a built DLL carries what `src/mod.c` claims |
| `extract_shadowmaps.py` | shadow-variant archives → one `shadowmaps.dat`, several maps inside it, foreign archives included |
| `probe_hooksite.py` | re-checks the hook sites against a real client `WoW.exe` |
| `tools-check/` | labelled stub headers, used **only** by `build.py --check` |

## The `.dat` file

All little-endian, packed at natural sizes, nothing aligned to anything else.
The file must be **exactly** as long as its own header says, and the loader
refuses a short file (truncated) *and* a long one (junk appended), because
either means the counts and the archive disagree and a shadow set would be
applied from a half-read table. Two lengths are legal at each version, and the
header's last word picks which:

```
mapCount == 0                 124 + entryCount*8   + payloadBytes   (legacy, one map)
mapCount 1..64   version 1    124 + mapCount*28 + entryCount*12 + payloadBytes
mapCount 1..256  version 2    124 + mapCount*32 + entryCount*12 + payloadBytes
```

| Field | Bytes | Meaning |
| --- | --- | --- |
| `magic` | 4 | `SHDW` |
| `version` | 4 | `1` or `2` (`SHADOW_DAT_VERSION` is the newest one this DLL reads); 2 says a map record carries a clock |
| `map` | 24 | map name, NUL-terminated inside the 24 (`Azeroth`), or `MULTI` when the file carries several maps — a six-set continent is still ONE name here |
| `variant` | 40 | label for the log line, NUL-terminated inside the 40 |
| `entryCount` | 4 | chunks with a payload, all maps and sets together |
| `payloadUnique` | 4 | distinct payloads (`payloadBytes == payloadUnique*512`) |
| `tiles` | 4 | tiles this set covers, all maps together (sets of one map share their tiles) |
| tile/chunk bounds | 8 × `int32` | `tx0,ty0,tx1,ty1` then `cx0,cy0,cx1,cy1`, over every map |
| `mapCount` | 4 | `0` for the single-map file the format had before, `1..64` for a file that carries a map table at version 1, `1..256` at version 2 — it counts RECORDS, so a map with six shadow sets costs six |
| map records | `mapCount` × 28 (v1) or × 32 (v2) | `char name[24]` NUL-terminated inside the 24, then that record's `entryCount`; version 2 adds one dword after the count, `h0`, where this record's run of entries starts in the day, half-minutes from midnight (`06:30` is 780, `21:30` is 2580) |
| entries | `entryCount` × 8 or × 12 | `key`, `payloadOffset`, then the entry's row in the map table — the third word is there only when `mapCount` is not 0 |
| payload area | `payloadUnique` × 512 | packed `MCSH` bodies, 64×64 texels, LSB-first |

The key mirrors Salmiak's own bake selection: a chunk's global coordinates are
`gx = 16*tileX + indexX`, `gy = 16*tileY + indexY`, both 0…1023, so `key =
(gy<<10)|gx` is a 20-bit chunk address — the same key Salmiak stores in
`_bakeChunks`, which is what makes a bake exportable, strippable and handable
to the client without a `.adt` in between. Chunks that share identical shadow
bytes share one payload (including one shared all-zero buffer, so a cleared
shadow really draws nothing); a chunk the bake never touched simply has **no**
entry, and the client keeps whatever stock shadow the `.adt` carries for it.

A key is unique only **inside a map**: tile 14,20 chunk 3,5 of Azeroth and of
Kalimdor are the same 20-bit number. So the map table and the third entry word
exist to say which run an entry belongs to, and the per-chunk gate picks the run
it binary-searches by comparing the client's `s_mapName` against the run's name
(`CiEqName`, case-insensitive) — never a `==`, and the client's name is re-read
per chunk, so one session that visits two maps still works. Entries are grouped
by map in table order with keys strictly ascending inside a group, which is what
makes a run one contiguous span of the arrays. The same key in two runs is
legitimate; a repeated key inside one run is the same chunk listed twice and is
refused. The payload pool stays interned **across** maps, so an identical body
used by Azeroth and by Kalimdor is stored once — safe, because what decides
whether a payload is used at all is the entry's map index, not the blob.

This is not hypothetical on this machine: `patch-S.mpq` carries tiles of twelve
maps, and its one `.dat` (21,797,872 bytes, `124 + 12*28 + 76547*12 + 40779*512`)
has **7,927 chunk keys that appear in more than one map's run**, 38,257 of them
Azeroth's and 27,723 Kalimdor's, with the two key ranges overlapping. Searching
the whole file instead of one run would answer those chunks with another map's
shadow. With `mapCount 1` the table still gets a row, and the header's `map`
word keeps the real map name so an older reader that ignores the table still
gates on the right map; with several maps the header says `MULTI`, so an older
reader refuses outright instead of drawing one map's shadows onto another's.

Version 2 is the same table with one more word per row, so **a row is a shadow SET
of a map, not a map**: six rows all named `Azeroth` is one continent's six presets.
The extra dword, `h0`, is where that row's entries take over in the day, in the
same half-minutes-from-midnight the DLL's `SunHalfMinute` reads the client's clock
in. The contract the loader enforces is that a name's rows are **adjacent** in the
table — the gate walks them as one group and refuses a name that appears in two
places — and **strictly ascending in `h0`** inside the group, because the pick is
"the row whose `h0` is the greatest at or below the clock", wrapping to the biggest
`h0` when the clock precedes all of them. That wrap is what makes a set stamped at
the arc's dusk crossing own the night: its window is `21:30 → 05:50` across
midnight. `h0 == 0xFFFFFFFF` (`SHADOW_DAT_H0_ALL`) is only legal on a one-row map,
where there is nothing to pick between and the clock is never read. The rows are
written in that order by `extract_shadowmaps.py`, which prints the `windows:` line
it settled on so a set paired with the wrong archive is visible rather than silent.

**An entry's `payloadOffset` addresses the payload area, which starts after BOTH
tables** — the map-record table, then the entry table — so byte 0 of the pool is file
offset `124 + mapCount*28 + entryCount*stride`. That term is not cosmetic, and finding
out what it costs is what caught a bug: sv11 checked the file's size against a rule
that counted the record table, then filled the arena with a seek that did not skip it.
With twelve maps the whole pool sat 336 bytes early, so a chunk's 64×64 map was the
tail of one payload over the head of the next — 336 bytes is 42 texel rows, and the
ground came back as shadows of about the right shapes in the wrong place, looking
mirrored and rotated. The one-map cut of the same archive drew correctly under the
same loader, which is what made the format a suspect rather than the bake: `mapCount
0` means `rec_bytes 0` and the two offsets agree. sv12 restored the term in the one
`fseek` that reads the payload area, and the disagreement is measurable against the
archive that made the file: read the pool where the size rule puts it, and all 76,547
entries of the twelve-run file carry exactly the `MCSH` body `patch-S.mpq` holds for
their chunk, run by run (Azeroth 38,257 of 38,257, EmeraldSanctum 171 of 171); read
it where sv11 did and none of the 76,547 match. Nothing about a legacy file changes —
its `rec_bytes` is 0, so both reads are the same read.

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

The clients it probes are not spelled out in the script: pass them on the
command line, or keep them in the git-ignored `probe_hooksite.local.txt` beside it
(one `WoW.exe` per line), or set `SHADOW_PROBE_EXES` as a `;` separated list.
Which copies a machine has says nothing about the contract above, and the folder
paths quoted elsewhere in this README are one machine's layout, written to be read
rather than copied.

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

The two paths are not equally quiet about the same source, and that is a flag
difference, not a code difference: `build.py` passes `/D_CRT_SECURE_NO_WARNINGS`
and compiles with `/W3`, so it prints nothing about this file's `fopen` calls,
while the CMake preset carries no warning-level or deprecation flags at all and
answers one `C4996` line per `fopen` in the source — four of them now, three in
sv12. `ClassicAPI`'s own `CMakeLists.txt` sets neither flag either, so the list
here stays the honest one and the notes are read as what they are: the deprecation
nag, on calls whose behaviour `verify_dll.py` and the runs both confirm.

## Install

1. Install VanillaFixes if it isn't already.
2. Copy `ShadowMaps.dll` into the game directory.
3. Add `ShadowMaps.dll` to `dlls.txt`.
4. Rename the file to `shadowmaps.dat` and put it **next to the DLL**,
   not next to the game. One file carries every map the archives had —
   the loader picks the run whose name matches the client's map — so one
   rename ships the whole set, and a legacy single-map file works the same
   way.
5. Launch the game with `VanillaFixes.exe`.

There is no mode file to write and no logging decision to make: with only the two files
above, the mod installs the full rung and `shadow-mod.log.txt` gets one line per launch —
see the Logging section for what that line is and for what a `v` in a `shadow-mod.mode`
adds. Swapping shadow sets is one file: replace `shadowmaps.dat` and restart. The
`.dll` never changes between sets.

`Load` answers VanillaFixes with ClassicAPI's convention: **0 = installed**,
non-zero = refused. So the popup below is not a bug report, it is this mod
saying "I did not install", and the reason is always the first line of
`shadow-mod.log.txt`:

```
DLL D:\games\wow-1.12.1\ShadowMaps.dll was not loaded properly (load function failed)
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

## Re-filling chunks that are already resident (sv17, sv18's fix)

The selection machinery has one thing on top of sv16 that a first login makes
visible. The client builds the chunks around the character before the server's
time packet arrives, so during those loads the day clock reads midnight
(benilla-re `systems/gametime.md`: the clock object at `0xce8538` starts at
midnight and the same `0x6435a0` advance that later moves it has not run yet
with a day count to carry). A `.dat` with several sets per map is then picked by
that unknown clock, and the rule wraps to the record with the biggest opening,
i.e. the set stamped at sunset. Nothing in vanilla re-runs
`CMapChunk::Load 0x6af5f0` for a chunk that is already resident — the map pass
only re-creates chunks it *destroyed* (`0x6984f0`, `0x66fc80`, `0x66fd50`
recomputes windows from the camera, never a standing player's chunks) — so the
record that owns the real hour would otherwise never be drawn at that login.

The refill is what sv17 adds: one pass over the chunks this run gave a shadow to,
as soon as the client's own clock stops reading midnight. The primitive is the
client's own, from `systems/terrain/chunk.md`: `CreateShadowTexture 0x6b0030`
stores the MCSH address it was handed at `chunk+0x144`, `AcquireChunkTexture
0x6aed20` builds the texture at `chunk+0x140` through the fill callback
`0x6b1180`, and `DecodeShadow 0x6b02b0` re-reads `+0x144` when it fills — so
re-pointing `+0x144` and asking the device to re-fill the texture gives a
standing chunk new shadows **without destroying it and without touching its
`.adt` header**, which is the whole reason the sv3..sv8 and sv10-b header
rewrites died. Asking is `GxTexUpdateRect 0x58ac70` with four zero rectangles:
the minimap's `0x4ecef8` calls it that way and means the whole texture, while
the chunk layer's own `0x6b001e` passes the tile size twice; whole is the form
that needs no sub-record bounds. On a shader client the chunk's one texture is
at `+0x13c` (shadow packed with the layer alphas, `0x6b00a0`/`0x6b02f0`/
`0x6b0350`, read from the same `+0x144`), and the client picks between the two
on bit 12 of `0xC89F44`/`0xC9A31C` at its `0x6af71f` — the refill asks the same
way, so it re-fills the texture the chunk is actually drawing.

What is kept per installed chunk is four dwords — the chunk, the entry key its
payload came from, the map's **first** record and the record the pick chose —
up to `REFILL_MAX 8192` of them, and the refill re-picks with
`PickRunIdx(first_record)` so the load and the refill cannot disagree about
which set is current. `g_refill_dropped` counts installs past the cap, because a
big city block can exceed 8192 chunks and the refill line names what was never
offered rather than leave it unstated.

sv18 is that test's repair, and it is why sv17's refill line said `0 of 2359`.
`fixup_chunk` builds the key as `(row << 10) | column` from `+0x90` and `+0x8c`
— the log's own fixup line printed `gy 811 gx 448 key 830912`, and
`830912 = (811 << 10) | 448` — while sv17 decoded it the other way round, `key >>
10` against `+0x8c` and `key & 0x3ff` against `+0x90`, i.e. row against column.
Only a chunk whose column equals its row passed, so every recorded chunk was
counted as stopped answering. sv18 takes the column out of the low 10 bits and
the row off the top, splits the one counter into the two halves it was always
testing — `dead` for a `+0x144` that is not in this arena, `unown` for a chunk
whose own column and row no longer make the recorded key — and prints one line
each naming the first chunk that failed and the addresses it compared, so a run
that fills nothing says *which* test failed instead of *that* one did.

sv19 is the pass's one remaining bogus test, and it is why sv18's refill line
said `2359 have no tile in the record the clock picked` while naming nothing
wrong. sv18's line already proves the two liveness tests passed — `dead` and
`unown` are 0 of 2359 — so the chunks were found and re-picked; the `absent`
counter has two sources, and the .dat the game was reading rules out the
bisection one: measured against `shadowmaps.dat` itself, the key the log named,
830912 (row 811 column 448), is present in the key runs of **all six** records,
whose runs are 361040..998927 over rows 352..975 and columns 352..707. So the
search found it, and the test behind it refused it: that test read the dword at
`payload + FIELD_MCSH_SIZE` and demanded `0x30`, but 0x30 is where the *MCNK
header* keeps `sizeMCSH`, and a payload is the bare 512 shadow bytes with no tag
and no size of its own — measured, 0 of a 60-payload sample of each of the six
runs has `0x30` at payload offset `0x30`, the bytes there being shadow data (and
0 in the shared all-zero payload). The load does no such test, so sv19 drops it:
an entry's payload offset was validated at install already, and a load and a
refill have to agree about what a chunk answers. `absent` now means exactly one
thing, the picked record having no entry for the tile.

sv19 also names the two things the pass could otherwise get wrong silently: a
`notex` counter, plus one line for the first chunk that hits it, for a chunk
that was handed its new payload but had no texture at the field the shader's
state picked — nothing was marked dirty for it, so its new shadow only shows
when the client next builds that chunk's texture — and one line before the pass
naming which texture field it marks dirty and what the two shader switches read
at the moment it ran. A pass that re-fills everything and changes nothing on the
ground is one of those two, not a crash, and this is what says which.

sv20 is that choice, repaired, and sv19's own line is what named the reason. The
run printed `refilled 2288` and `notex 2288`: every payload moved into `chunk+0x144`
and none of the 2288 got a dirty mark, because the pass asked for `chunk+0x140`
and found 0 there — while the notex line itself printed the chunk's texture,
`0x2f7e9008`, at `chunk+0x13c`. The test that chose `+0x140` was wrong: `0x6af71f`
inside `CMapChunk::Load` takes the combined texture under **either shader flag**,
and both flags are the client's `uint8_t` globals that **hold 1** when a terrain
shader set is in use / when the terrain pixel shaders are in use — not a bit of
the dword starting at them. Both read `0x1` in this client, so a chunk has ONE
shadow-carrying texture, the combined one at `+0x13c` that packs the shadow with
the alphas of layers 1..3 — and `0x6b02f0` fills it from `+0x144` together with
the four alpha pointers, the same field this refill re-points.

So sv20 reads the two bytes, and per chunk asks **both** fields and marks
whichever one actually holds a texture: a chunk whose texture sits at the field
the bytes did not pick goes to a `mismatch` counter and one line naming the field
it marked, and `notex` counts only a chunk with 0 at *both* fields. The fallback
is what makes asking for the right texture independent of the switches — exactly
one of the two fields ever holds one — and it also covers a player who changed
graphics between a chunk's load and this pass.

A recorded chunk is only written to when it still answers as the chunk this run
installed: its `+0x144` must still be inside `[g_arena, g_arena + arena bytes)`
— only this DLL put an address there — and its own global column and row at
`+0x8c`/`+0x90`, the same pair the load made the key from, must still make the
recorded key. A chunk the client destroyed and freed, or a heap block reused by
another object, fails one of those and is left alone. (The area-table
dereference, `area+0x278` with the grid's 16×16 indices, is *not* used: the
chunk's own `+0x8c`/`+0x90` are its absolute map indices, and no area pointer
is in hand at the hook.)

Two triggers reach that pass, and an exchange makes it one pass per session
whichever gets there first. The chunk hook's half (`RefillKeep`, called from
`fixup_chunk` next to the arc) fires when chunks keep loading: it is one
volatile read per chunk after the pass. The other half is a thread of this DLL
started after the install, which waits up to twenty real minutes for the clock
to leave midnight and then asks — that is what covers a first-login player who
stands where the game booted him and never loads another chunk.

Honest about what a re-fill does and does not do:

* it lands between the client's own frames, on a thread of this DLL: the writes
  are one pointer per chunk plus the client's dirty-rect call, and a texture the
  device is mid-upload on is refilled when the device next refills it.
* a tile the picked record has no entry for keeps the shadow the load gave it,
  which is the old record's shadow, not the client's stock one: a refill can
  only hand a chunk a new shadow, it cannot take one back. A chunk loaded
  *after* the crossing is the one that would draw stock there.
* a chunk this run never installed is not touched: its shadows are the
  client's, and the two liveness tests also catch a chunk that stopped being
  the chunk this run installed — sv18's run is what proved both tests work:
  they counted 0 and 0 out of 2359, so `chunk+0x144` really is an address in
  this run's arena after a matched load.
* a session whose server time really *is* midnight cannot be told from a login
  whose clock is still unknown, so those chunks keep the record the load
  picked, the record that owns the night — the watchdog's twenty-minute bound is
  what makes that visible rather than silent, and raising it watches a night
  session cross into the day.

sv21 is what sv20's crash said about the mark itself. The run got further and
died inside the client: `ERROR #132 (0x85100084)`, `0023:00594B9E`, referenced
memory `0x00000140`, `0xC0000005`. `0x594b9e` is client code — the DLL is not in
that address's neighbourhood at all — and its bytes are `c6 06 01`,
`mov byte ptr [esi], 1` inside `0x594b90`, where `esi = [ebp+8]` is the value
`0x58acd0` pushed, which is the `ecx` that `GxTexUpdateRect`'s own tail passed
down, which is the `ecx` at the C call site — the one a cdecl call never sets.
`0x140` is `FIELD_SHADOW_TEX`, the constant the refill's own address arithmetic
left in that register: the store went into an address that is not mapped.
So the calling convention was never the problem — the texture register was.

sv21 says the client's own shape, in machine code on the detour's execute page,
because this compiler cannot say it in C: MSVC rejects `__thiscall` and
`__fastcall` on a function-pointer typedef, so no declaration of
`GxTexUpdateRect (0x58ac70)` can name "texture in `ecx`, rect word at +4 in
`edx`". All three of the client's own call sites — `CreateShadowTexture`'s push-1
at `0x6b007c`, `CombinedChunkTexture`'s at `0x6b00ce`, `CMapChunk::UpdateShadow`'s
(0x6aff80) at `0x6b0011`, call at `0x6b001e` — run
`push 1 / push <size> / push <size> / push 0 / xor edx,edx / mov ecx,<texture>`,
and the `size` there is `0x20` only when the client's `shadowLevel` option
(`0xc63284`) reads exactly 1, else `0x40` — the same comparison that decided the
texture's side, so the refill reads that dword and compares the same way instead
of assuming 64. The last word is the flag whose test at `0x594c23` (`test eax,eax
/ je 0x594c2f`) decides whether the device is told at all: the four zeros sv17..sv20
pushed answer *no* to it, and their `{0,0,0,0}` rect takes the `cmp ebx,ecx / jge
0x594c08` branch that only resets the texture's own rectangle words — so that form
could never have changed the ground, on top of faulting.

Two build gates were needed to make the thunk's bytes true, and both found real
defects rather than confirming a guess. First the encoding: `[esp+4]` cannot be
written in three bytes — `rm=100` with a displacing `mod` is `[ebp+disp8]`, the
frame pointer, and the only way to say `esp` as the base is the SIB byte `0x24`
then the `disp8`. sv21's first draft omitted it, and capstone decoded `8b 4c 04`
as `mov ecx,[esp+eax-0x75]`: a different instruction, which pushed a garbage
texture and *returned quietly* — a wrong answer that looks like a pass. So
`verify_dll.py` now disassembles the table from the linked image and requires
`mov ecx,[esp+4]`, `mov eax,[esp+8]`, four pushes of `1`, size, size, `0`,
`xor edx,edx`, one `call eax`, a terminating `ret`, and the zeroed `mov`
imm32 at slot 17 with `0xB8` before it. Second, the table was not in the image
at all: `/O2` turns a `memcpy` of 24 constant bytes out of a `const` array into
immediate stores and never takes the array's address, so there was nothing to
copy and the byte-for-byte gate said "the 24-byte thunk table is NOT in any
section". The copy length is a `volatile` local for exactly that reason, which
makes the copy a real `memcpy` out of the table in `.rdata` — the shipped bytes
*are* mod.c's table. `BuildStub` re-reads the page (the detour's read-back
already says the same about its own jmp) and refuses the whole install if the
page does not hold the table with `0x58ac70` in its imm32.

sv22 is the two things sv21's confirmed refill then reported wrong in game, and
both were found by reading which field of the chunk the client reads.

**Issue 2 — the entity and grass shadows followed the old set while the ground's
texture followed the new one.** The client keeps the shadow map's address in TWO
places, and they are read by different code. `chunk+0x144`
(`FIELD_SHADOW_OFS`) is what `CreateShadowTexture (0x6b0030)` stores and what the
shadow texture is filled from — sv21 re-pointed that one, which is why the ground
redrew. `chunk+0xf1c` (`FIELD_PTR_MCSH`) is what `CMapChunk::Load` writes through
the resolver as `hdr + ofsMCSH`, and it is what the shadow *tests* dereference:
`IsInChunkShadow (0x69b350)`, dereferenced at `0x69b466`, which the doodad,
entity and light code asks, and the grass detail's own test at `0x6c013b`. So an
NPC standing where the old set's shadow was kept getting the shadow effect and
one standing in the new one got nothing: the picture and the effect were reading
two different records. sv22 writes both fields, always the same address, which is
what makes the effect and the picture about the same bytes.

**Issue 1 — a sparse preset (the 21:30 one, baked only for `patch-S.mpq`'s
places) was never drawn where it should be.** Two reasons, both fixed. A chunk
with no entry used to `return` from `fixup_chunk` before being recorded, so a
later, fuller record could never reach it — sv22 records the chunk with
`REFILL_NONE` and the stock address it read. And a tile the picked record has no
entry for used to keep "the shadow the load gave it", which for a resident chunk
is the *previous* record's shadow; sv22 hands back the client's own stock `MCSH`,
the address `hdr + ofsMCSH` that `fixup_chunk` read for every chunk this `.dat`
names, so a sparse preset costs nothing extra in the file and no copy of the
night's shadows is kept — the client already dereferences that same address
through `chunk+0xf1c` while the chunk is alive.

**The refill now follows the day instead of happening once.** sv17 stopped at one
pass per session, so the day's own crossings were never drawn: the log said set
4 of 6 was picked while the ground stayed on the record the midnight load chose.
`g_set_applied[group]` is the record a map group's chunks were last given, and the
chunk hook asks `RefillKeep(group, which)` — one array read and one integer
compare per chunk — so a pass runs at the day's crossing and only at a crossing,
at most once a day per record. `AnyGroupDue` is the same question from the
watchdog thread, for the session that stands where it booted and has no further
chunk to be called on.

## What this machine produces

The sv22 build, i.e. what this branch's HEAD is: `out\ShadowMaps.dll`
**172,032 bytes**, i686, exports `Load` at RVA `0x2cd0`, accepted by `verify_dll.py`
with zero complaints, and its log banner reads `sv22` (82 times, with no `sv21`,
`sv20` or `sv19` in the image). It is installed in the game folder at
`G:\Games\twmoa_1181\ShadowMaps.dll`, byte-identical, sha256
`0fc13c5e40dc51603f0474e662a3832e3689ded703ab6258519eb92fbef30194`. The sv21 build
was 171,520 bytes with `Load` at `0x2cc0` — sv22's refill code moves the export by
16 bytes — and sv18's was 169,472 bytes there, and sv16's was 165,888 with `Load` at
was 165,888 with `Load` at `0x2c90`: sv22 changes the refill's writes, its trigger
and the load's record of a chunk it left stock, so an sv21 log line and an sv22 one
about the same `.dat` differ in what the refill found, not in what it accepted. The
paragraphs below are the sv13 round's record of the game folder and keep their own
numbers. The sv18 image next to it
was 169,472 bytes with `Load` at the same RVA `0x2cc0`, and sv16's was 165,888
with `Load` at `0x2c90`: sv18 changes one comparison and two log lines, so an
sv17 log line and an sv18 one about the same `.dat` differ only in what the
refill's liveness test accepted. The paragraphs below are the sv13 round's record
of the game folder and keep their own numbers. The sv17 image next to it
was 168,960 bytes with `Load` at the same RVA `0x2cc0`, and sv16's was 165,888
with `Load` at `0x2c90`: sv18 changes one comparison and two log lines, so an
sv17 log line and an sv18 one about the same `.dat` differ only in what the
refill's liveness test accepted. The paragraphs below are the sv13 round's record
of the game folder and keep their own numbers.

`out\ShadowMaps.dll` 146,944 bytes and `build\Release\ShadowMaps.dll` 147,968
bytes, both i686, both exporting `Load` (at RVA `0x2530` and `0x2760`), both
importing only `KERNEL32.dll`, both accepted by `verify_dll.py` with zero
complaints — i.e. the stub's stack math is read from real linked code, not from
a comment.

What sits in the game folder today is the shipping **pair**: `ShadowMaps.dll` (the
sv13 image, 147,968 bytes) and `shadowmaps.dat` — now the
multi-map file: 21,797,872 bytes, `SHDW` v1, `mapCount 12`, header map word `MULTI`,
variant `patch-S`, 76,547 entries, 40,779 payloads (20,878,848 payload bytes), 309 tiles,
and `124 + 12*28 + 76547*12 + 20878848 == filesize`, its 12 runs being Azeroth 38,257,
Kalimdor 27,723, Sunnyglade 2,145, Giln 2,048, netherstormbg 2,048, Karazahn40 1,280,
Grove 1,121, Winterveil 1,024, Hateforge 281, development 256, Moomoo 193 and
EmeraldSanctum 171. Whatever map the client is in picks one of those runs by name —
the sv10 run's log line said `Azeroth`, whose run is this file's first with 38,257
entries — so the same shadows that legacy cut drew must come back through the table,
and the run gate is what decides it. Two other files sit beside them and are never read,
because the loader only ever opens the fixed name: `shadowmaps.patchS-legacy.dat`
(11,248,644 bytes, the Azeroth-only cut renamed aside so the legacy path can be
re-tested by renaming back) and `shadowmaps-Kalimdor.dat` (7,818,452 bytes, Kalimdor's
125 tiles and 27,723 entries). Both legacy files report `mapCount 0` and pass the
8-byte rule, which is the point of `mapCount 0`: swapping between the two shapes is one
file rename, no rebuild.

Beside them sits the six-set test file, `shadowmaps-6sets-test.dat` (177,610,696 bytes,
`SHDW` **v2**, header map word `Azeroth`, variant `6sets-standin`, `mapCount 6` records of
32 bytes, 589,793 entries, 333,072 payloads = 170,532,864 payload bytes = **162.6 MB of
arena**, 498 tiles, and `124 + 6*32 + 589793*12 + 170532864 == filesize`). Its six runs are
one per full-size Azeroth bake in the folder — `shadows-Azeroth-morning` stamped 06:30,
`-noon` 09:00, `-day` 11:30, `-dusk` 15:30, `-blizzlike` 19:30 and `patch-S` 21:30 — at
windows 05:50, 07:45, 10:15, 13:30, 17:30, 21:30. **Say what is guessed in it:** those five
bakes were run at Salmiak's old wrong angles, so their bearings do not agree with the arc's
sun at those hours; the file is a stand-in whose purpose is to exercise the *selection*
machinery — six sets of one map, a name that appears six times, a per-chunk pick against
the clock, 162 MB of arena — not to look right. The `patch-S` 21:30 run is only 174 tiles
and 38,257 entries, so at night hours most of Azeroth has no entry and falls back to the
client's own `MCSH`; that is the known gap the user's own whole-continent 21:30 bake closes.
Build the file with `--set` once per archive in the same order, so an off-by-one there would
put noon's shadows in morning's window, and the script prints the `windows:` line it settled
on so a wrong pairing is visible rather than silent.

What that file tests on an sv18 image, once it is renamed to the fixed name next to the
DLL and `f 5 v` is in the mode file: one install line naming 6 records and max sets 6, the
`.dat windows` line with the six openings, one refill line at first login, and
one switch line per window crossing as the day is sped up. The refill line is the sv18
evidence — its "no longer an address in this arena" and "no longer make the key they were
recorded under" counts are what say the pass found the chunks rather than found none, and
its two first-chunk lines are what name the test that did fail, with the addresses it
compared, when it does.

There is **no `shadow-mod.mode` in the folder**, which is the whole point of the sv13 image
sitting there: the state being tested is the state a user gets, and a launch from it writes
one line. Beside it are `shadow-mod.log.txt` (1,575,875 bytes, what sv10 and sv12 left, and
what the next load's `TrimLogFile` rewrites as one banner line plus its newest 64 KB) and
`shadow-mod.log.sv12.txt`, the whole file copied aside so none of those runs' evidence is
lost. Writing `f v` into `shadow-mod.mode` brings back the whole trail the block below shows.

The log walkthrough below was made with the **morning** set's legacy `.dat`
(34,918,204 bytes, `mapCount 0`, map `Azeroth`, variant `shadows-Azeroth-morning`,
114,328 entries, 66,413 payloads, 487 tiles in `(22,22)..(44,60)`, chunk grid
`(352,352)..(707,975)`), so the counts in those lines are that file's, and the
folder's `patch-S` file has 38,257 entries instead.

Expected log lines, in `shadow-mod.log.txt` next to the DLL. The loader's own
lines carry the build stamp, so an old DLL is never read as a new one. A refusal
line is the whole story of a run that did not install, and VanillaFixes answers
any non-zero `Load` with its own generic `was not loaded properly (load
function failed)` popup — that popup is never the reason, this file is. The block
below is sv10's real runs, which had no levels; it is what an sv13 run prints once
its mode file carries a `v` (or runs a rung other than `f`), and it is more than an
sv13 launch from the shipping state prints — that one writes its install line and nothing
else unless something went wrong, and the install line says which of the two levels ran,
so a one-line log is never mistaken for a broken one; see the Logging section:

```
[12 ms after this DLL was loaded] sv10 mode 'h' read from 'D:\games\wow-1.12.1\shadow-mod.mode' (no such file means f): n = inert, a = .dat plus arena, no hook, b = hook plus an empty call, c = the detour alone with no call at all, d = the same call from the client's own frame at 0x6AF5F3, h = hook plus audit, no payload, f = full
sv10 no shadowmaps.dat where the DLL is: the client keeps stock shadows; looked in 'D:\games\wow-1.12.1\shadowmaps.dat' (rename the variant .dat to that name, it belongs beside the DLL, not beside the game)
sv10 mode n: nothing read, allocated or patched, the client is untouched; shadows stay stock, the popup is expected
sv10 handler: first-chance vectored handler registered, so the next access violation logs the chunk that was being built and the address the client read (the client's own report is still written afterwards)
sv10 mode c installed: hook 0x6AF5F0 patched, resume 0x6AF5F6, detour at 0x........, the landing replays the prologue and returns without calling anything -- no .dat read, nothing allocated, no chunk header read or touched (shadows stay stock; this mode exists to test the store and the landing alone)
sv10 mode d installed: hook 0x6AF5F3 patched (the 5-byte E9 covers mov eax,[ebp+8] / push ebx / push esi), resume 0x6AF5F8 (push edi), detour at 0x........, fixup returns immediately -- no .dat read, nothing allocated, no chunk header read or touched (shadows stay stock; this mode exists to test the stub's argument lookup)
sv10 first fixup call: this 0x........ record 0x........ after 1 visits, mode d -- the hooking rungs print this once, mode c never does
sv10 mode b installed: hook 0x6AF5F0 patched, resume 0x6AF5F6, detour at 0x........, fixup returns immediately -- no .dat read, nothing allocated, no chunk header read or touched (shadows stay stock; this mode exists to test the stub alone)
sv10 mode h installed: hook 0x6AF5F3 patched, resume 0x6AF5F8 (push edi) -- d's shape, the one the clean mode-d run proved, NOT b's [esp+48] frame, detour at 0x........, fixup audits the first chunk whole and dumps every chunk's ofss -- no .dat read, nothing allocated, no chunk header touched (shadows stay stock)
sv10 first fixup call: this 0x2cd20008 record 0x2c3dca46 after 1 visits, mode h -- the hooking rungs print this once, mode c never does
sv10 audit first chunk: record 0x2c3dca46 fourcc 'KNCM' size 0x254c, hdr 0x2c3dca4e flags 0x8009 indexX 0 indexY 0x5
sv10 audit first chunk: MCSH at hdr+0x2c is 0x4ec -> address 0x2c3dcf3a, fourcc at -8 'HSCM'
sv10 mode a installed footprint only: map 'Azeroth' variant 'shadows-Azeroth-morning', entries 114328, payloads 66413, arena 32 MB resident at 0x........0x........ NO hook, no fixup call, no chunk header touched, so the client runs its own code path whole: ...
sv10 mode f installed: map 'Azeroth' variant 'shadows-Azeroth-morning', entries 114328, payloads 66413, arena 32 MB at 0x........0x........, hook 0x6AF5F3 patched with resume 0x6AF5F8 -- d's shape, the one the clean mode-d run proved, NOT b's frame, detour at 0x........; matched chunks get flags |= 1, hdr+0x2c = arena buffer - hdr and hdr+0x30 = 512, so the client's own gate, CreateShadowTexture and the decoders read live .dat bytes
handler: ACCESS_VIOLATION 0xc0000005 at 0x6b0fb7, read of 0x6cf97f32 -- the address the client
dereferenced. last chunk seen: this 0x2bac0008 record 0x2b17ca46, fixup visits 1
handler: the object this chunk carries: hdr@0xf08 0x6af5fe, record@0x168 0x6af5f6, MCVT@0xf14 0x6cf97f32, MCNR@0xf18 0x8d6af5ff, MCSH@0xf1c 0x51f3fe50, MCLY@0xf20 0x1af65e4e, MCAL@0xf24 0x4c58117, MCRF@0xf28 0x89d7448b, MCLQ@0xf2c 0xe93980fe, MCSE@0xf30 0xcef64cfe. this+0xf14 == the address the client read
handler: the dying chunk's header: hdr 0x2b17ca4e flags 0x8009 indexX 0 indexY 0x5, ofs MCVT 0x88 MCNR 0x2d4 MCLY 0x49c MCRF 0x4e4 MCAL 0x6f4 MCSH 0x4ec MCSE 0x254c MCLQ 0x1efc, global col 0x1c0 row 0x205
last chunk seen: this 0x........ record 0x........ hdr 0x........ flags 0x8009 indexX 0x2 indexY 0xa, ofs MCVT 0x88 MCNR 0x2d4 MCLY 0x49c MCRF 0x4b4 MCAL 0x6c4 MCSH 0x4bc MCSE 0x1364 MCLQ 0x6cc, global col 0x1b2 row 0x20a
audit first chunk: record 0x........ fourcc 'KNCM' size 0x1364, hdr 0x........ flags 0x8009 indexX 0x2 indexY 0xa
audit first chunk: MCVT at hdr+0x14 is 0x........ -> address 0x........, fourcc at -8 'TVCM'
audit first chunk: MCSH at hdr+0x2c is 0x........ -> address 0x........, fourcc at -8 'HSCM'
fixup: first chunk with no payload: gy 560 gx 496 key 573936 (the bake left it without a shadow map, or its tile is not in this set)
fixup: first chunk matched: gy 517 gx 448 key 529856, shadow drawn from arena+0, this 0x2dac0008
 record 0x2d17ca46 hdr 0x2d17ca4e arena 0x6ac0000, hdr flags 0x8009 size 0x200 ofs 0xd99435b2,
 mcvtof 0x88 mcnrof 0x2d4 mclyof 0x49c mcrof 0x4e4 mcalof 0x6f4 mcseof 0x254c mclof 0x1efc
fixup: 10000 chunks matched so far, of 114328 entries   (never printed in the sv10 run: that run walked one starting area, so it matched under 10000 chunks)
sv12 shadowmaps.dat answers for 12 map(s): Azeroth 38257 entries Kalimdor 27723 entries Sunnyglade 2145 entries Giln 2048 entries netherstormbg 2048 entries Karazahn40 1280 entries Grove 1121 entries Winterveil 1024 entries Hateforge 281 entries development 256 entries Moomoo 193 entries EmeraldSanctum 171 entries
fixup: client map 'Outland' is not one the payload names (Azeroth Kalimdor): the payload is ignored on this map
```

The last two lines are the multi-map file's shape, written in the words the loader
uses rather than quoted from a run: no run has printed them yet. The install line is
one per install and names **every run the file answers for**, with entry counts — the
line above is the twelve-run file that sits in the game folder today, so that is the
line the next mode-`f` launch prints, and a two-map file prints the same line with two
names. The `fixup` line is one per session and appears only when the client's `s_mapName`
matches none of the names (its bracketed list is the run names joined by single spaces,
counts left out), so the sv10 runs, whose file named the map the client was in, never
printed it.

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
tiles, so a chunk whose tile is outside it logs "no payload" right away, and so
does a chunk inside a covered tile that the bake left without a shadow map — the
sv10 run logged that second kind, `gy 560 gx 496` being tile `31,35`. Only the
`first chunk matched` line proves a payload was drawn, and that line is what the
sv10 mode-`f` run printed.
That line also dumps the header the write went into — `this`, `record`, `hdr`,
the arena and all eight sub-record ofss — so the addresses a crash report names
can be matched against it. The `audit` lines are the same facts for a chunk that
was never written to, and the sv10 mode-`h` run printed them whole: all eight
slots resolved on their own fourcc, beside a record size that is exactly where
the next `MCNK` sits.

## Logging (sv13)

Two levels, one file, one writer, and the quiet one is what ships: **no `shadow-mod.mode`
file at all** means mode `f` with quiet logging, and a launch that worked writes exactly
**one** line — its install line, naming the `.dat`'s runs, the arena, the hook and the fact
that logging is quiet. `shadow-mod.mode` is not part of the shipping pair above, it is the
debug switch, and a `v` in it is the half that turns the rest of the log on.

Always written, whatever the mode:

- one line per thing that **went wrong**: every refusal (client build mismatch, no or
  malformed or wrong-size `.dat`, out of memory), the mode-`n` line, a client map the payload
  has no run for, and the field contract moving under a shadow write — the things a player
  would otherwise see nothing about, since all they notice is stock shadows.
- the exception instrument's capture of a launch that was already dying: its `ACCESS_VIOLATION`
  line and the two object/header dumps. They only ever fire on a run that crashed anyway, so
  keeping them on the always level costs a clean launch nothing, and a crash stays diagnosable
  at any level.

Diagnostic level, turned on by a `v` **anywhere** in the mode file (`f v` = the shipping rung
with the whole trail sv12 printed) and turned on by itself for every rung but `f`, since
walking the ladder means the log is what you are reading: the long mode legend, the `b`/`c`/`d`/`h`
install lines, the handler-registration lines, the first-chunk audit block, the per-chunk
`last chunk seen` dump and its cap line, `first fixup call`, `first chunk matched`, the
no-payload line and the progress lines.

`TrimLogFile` runs once per process, before the mode file is read: past `LOG_MAX_BYTES` 512 KB
the file is rewritten as one banner line — naming both sizes and how many bytes of the old
file it kept — plus its newest `LOG_KEEP_BYTES` 64 KB, cut at the next newline rather than
mid-line. Under the limit the file is left whole, so a previous launch's lines are still there
to compare a new run against. That is what stops a session of verbose launches from leaving a
log the size of the session: the 1.5 MB the sv10 and sv12 runs left is cut on the next load.

## Mode ladder (sv10 ladder, sv13 build)

Five rungs of the ladder have been run on the real client — `c` and `d` on the sv9
build, `h`, `f` and `b` on this one — and the shipping path works.

The ladder did not move between sv10, sv11, sv12 and sv13: same seven rungs, same stub shapes,
same 5-byte store and resume points, same `MCNK` header writes, so every fact in the table
below is sv10's real runs and reads the same from an sv11, sv12 or sv13 log — the stamp
slot just says `sv13`. What sv11 changed is only the `.dat`'s chunk addressing above, what
sv12 changed is only where the payload-area read seeks, and what sv13 changed is only which
level each line is written at: the rungs, their writes and their bytes are untouched, so a
ladder rung prints the same lines it printed in sv10 as long as its mode file carries a `v`.
`verify_dll.py` accepts both sv13 images with the same three detour tables at the same sizes
(`b` 36, `c` 12, `d` 29), the same `add esp` pairing and the same plain-`ret` `fixup_chunk`
epilogue. What an sv11 run of this file showed in game: shadows came on the ground, but every
chunk's was the mix that paragraph above describes — mirrored-and-rotated shapes in about the
right places — so the hook, the header write and the run gate all ran and only the arena's fill
was wrong. An sv12 run of the same file has been done, and it answered what that left open: the
twelve-run set entered the world and drew its shadows, and the ground matched the legacy cut it
replaced. What has not been re-run since is the ladder on an sv13 image, which is what the `v`
in the commands below is for.
An sv12 run of the same file has not been done yet.

sv9's `c` (the store and landing at `0x6AF5F0`, a landing that **calls
nothing**) and `d` (the store at `0x6AF5F3`, the call, the record read from
`[ebp+8]` on the client's own live frame) entered the world clean with stock
shadows; sv10's `h` entered the world and printed its `first fixup call` line,
the whole first-chunk audit block and 2000 per-chunk header dumps; and sv10's
`f` — **loaded the world and drew the baked set** — the morning shadows were on
the ground, and the log carries `fixup: first chunk matched: gy 517 gx 448 key
529856, shadow drawn from arena+0`. So the `.dat`, the 34 MB arena, the 5-byte
store, the landing, the replayed prologue, both resume points, the audit's file
I/O inside `Load` and the `MCNK` header writes are all proven by real runs.

The one rung that still crashes is `b`, and its own run says why, 
not its argument lookup. Its `first fixup call` line printed a valid pair (`this
0x2bac0008 record 0x2b17ca46`), and the handler's block named the rest: `Load`
came back at `0x6AF5F6` holding **the resume address in `eax`** where it wants
the record, because `stub_code` ends in `mov eax,<resume> / jmp eax` — and the
`mov eax,[ebp+8]` one line above it is exactly what that clobbers. `Load` then
files the resume address as the chunk's record (`0x6AF5FF mov [esi+0x168],eax` — the
dump reads `record@0x168 = 0x6af5f6`, and `0x6AF5F0 + 6 = 0x6AF5F6`), and the
resolver's `lea edx,[eax+8]` makes `hdr@0xf08 = 0x6af5fe` — a dword inside
`CMapChunk::Load`'s own code. So the eight sub-record ofss are read out of `.text`,
and the one the crash reports keep naming is MCVT: the dword at `0x6AF5FE+0x14 =
0x6AF612` is `0x6C8E8934`, hence `this+0xf14 = 0x6AF5FE + 0x6C8E8934 = 0x6CF97F32`,
which is the exact address every report since sv3 has named, read by `mov ecx,[ebx]`
at `0x6B0FB7`. Every cursor the handler printed is `hdr + (that .text dword)`, and
the dying chunk's real header (MCVT `0x88`, MCSH `0x4ec`, MCSE `0x254c`, global col
`0x1c0` row `0x205`) was printed intact beside it. `c`, `d`, `h` and `f` return
through `push <resume> / ret`, which leaves `eax` = record; `b` does not, and that
is the whole difference. `StubShape` is where that rule lives.

Every hooking rung now also prints one `first fixup call: this .. record ..` line
the first time the fixup is entered — printed **before** the mode gates, so even a
silent rung prints it. Without that line a clean run means "the stub never called"
rather than "the call works", and mode `c`, whose landing calls nothing, is the
one rung that must not print it.

Seven rungs, the first letter of a `shadow-mod.mode` file **next to the DLL** (next
to `ShadowMaps.dll` and `shadowmaps.dat`, not next to the sources). A missing file
means `f` with quiet logging — the shipped state — and a `v` anywhere in the file turns
the diagnostic lines on, which every rung but `f` does by itself.

| Letter | What runs | What it separates |
| --- | --- | --- |
| `n` | nothing at all: the `.dat` is not opened, nothing is allocated, the client's bytes are not even read | a crash here means the fault has nothing to do with this mod. **Ran: clean** |
| `a` | the `.dat` is read and the whole arena is resident, **no hook, no call, no write** | a crash here while `n` was clean would mean the reservation itself breaks the client. **Ran: clean, 34 MB resident** |
| `c` | the store and the landing only: `0x6AF5F0` → stub replays the 6 prologue bytes and returns through `push <resume>` / `ret`, **calls nothing at all**, so no C code of this DLL runs on the client's loading path and eax/ecx/edx/esp/flags are exactly what the replay left them | store + landing + resume point, with nothing else in the picture. **Ran (sv9): clean** — entered the world, stock shadows, its three install lines and nothing else, and no `first fixup call` line because this landing calls nothing |
| `d` | the same call as `b`, found a different way: the `E9` sits at `0x6AF5F3`, its 5 bytes cover `mov eax,[ebp+8]` / `push ebx` / `push esi` whole, the resume is the next instruction boundary `0x6AF5F8` (`push edi`), and the stub takes the record from **`[ebp+8]`** and `this` from `ecx` — no `esp` displacement is part of finding them | `b`'s `[esp+48]` lookup, against everything else. **Ran (sv9): clean** — entered the world with stock shadows. Read `shadow-mod.log.txt` for its `first fixup call` line: that line is what makes this mean *the call happened with a pair of pointers* rather than *nothing happened* |
| `b` | the detour and a call per chunk that does **nothing at all** — no read, no log, no write, nothing allocated, and the only rung left on `b`'s own shape | the shape that breaks the return rule above, kept as the evidence for it. **Ran (sv8) and
again (sv10): crashed** both times at `0x6B0FB7` reading `0x6CF97F32`, and the sv10 run
named why: its `first fixup call` line printed a valid `this 0x2bac0008 record
0x2b17ca46`, so the `[esp+48]` lookup was right, while the handler dumped `record@0x168
= 0x6af5f6` and `hdr@0xf08 = 0x6af5fe` — the resume address filed as the chunk's
record. Nothing that ships rides this shape |
| `h` | the detour, the first-chunk audit whole, and one compact header dump per chunk — no `.dat`, nothing allocated, no header touched, and riding **`d`'s shape** now | the audit and its `fopen`/`fprintf` inside the client's loading path. **Ran (sv10): clean** — entered the world, printed the `first fixup call` line, the whole
first-chunk audit block with all eight slots resolving on their own fourcc, and 2000
per-chunk header dumps. The audit and its `fopen`/`fprintf` survive inside `Load` |
| `f` | everything: `.dat`, arena, hook and the matched-chunk writes, riding **`d`'s shape** | the only rung expected to draw shadows. **Ran (sv10): drew the set.** The world loaded, the morning shadows showed on the ground,
and the log carries `fixup: first chunk matched: gy 517 gx 448 key 529856, shadow drawn
from arena+0` followed by `fixup: first chunk with no payload: gy 560 gx 496 key 573936` —
a chunk in a tile this set does not cover. The contract holds end to end |

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
Delete `shadow-mod.log.txt` between rungs to make each run's block obvious: the log
is appended and every line is stamped with milliseconds **since this DLL was loaded**,
so a fresh run's block starts near 0. Deleting is no longer *required* — sv13's
`TrimLogFile` cut the 1.5 MB these runs left to its newest 64 KB on the next load —
but it is still the clearest way to read one run at a time. The `v` in the commands
below is sv13's: it puts the diagnostics on for a mode-`f` run, and every other rung
turns them on by itself, so these lines printed in sv9 and sv10 without it. `GetTickCount`
counts milliseconds since the **system** booted, which is why an older log's raw
numbers look like hours and cannot be used to count client starts.

```
echo n  > D:\games\wow-1.12.1\shadow-mod.mode     # ran: clean
echo a v > D:\games\wow-1.12.1\shadow-mod.mode    # ran: clean
echo c v > D:\games\wow-1.12.1\shadow-mod.mode    # ran (sv9): clean, entered the world
echo d v > D:\games\wow-1.12.1\shadow-mod.mode    # ran (sv9): clean, entered the world
echo h v > D:\games\wow-1.12.1\shadow-mod.mode    # ran (sv10): clean, printed the audit
echo f v > D:\games\wow-1.12.1\shadow-mod.mode    # ran (sv10): drew the baked set
echo b v > D:\games\wow-1.12.1\shadow-mod.mode    # ran (sv8, sv10): crashed by its jmp eax
```

| What the run shows | What it means |
| --- | --- |
| `n` crashes | nothing here is at fault: look at the client and its patch state itself (which `.adt`/MPQ set it is really reading) |
| `n` clean, `a` crashes | the reservation is the cause: keep the payload bounded (expand-on-demand blocks, or map the `.dat`'s payload area) instead of one arena asked for before any chunk loads. **Did not happen: `a` ran clean with all 34 MB resident** |
| `a` clean, a hooking rung crashes and `c` crashes with it | the store, the landing or the resume point is wrong — `c` is the only detour shape that involves nothing else, so the handler block says whether the fault is still in the client's own `BuildVertices` read or inside the stub page. **Did not happen: `c` ran clean** |
| `c` clean, `d` clean | settled by the sv9 runs: the store, the landing, the replayed prologue and
**both** resume points are good. The one detour shape still known bad is `b`'s, and it is bad at
its **return** — `mov eax,<resume> / jmp eax` puts the resume address in `eax`, which is the
record argument — not at its argument lookup |
| `d` clean but its log has no `first fixup call` line | the detour is in and the client tolerated it, but the stub never reached the call, so `d` proved the store and nothing about the call: read the stub's `push`/`call` bytes again rather than the client |
| `h` clean with a `first fixup call` line, an audit block and per-chunk dumps | the audit
and its `fopen`/`fprintf` survive inside the client's loading path, which is what `f` then
adds nothing to. **Ran (sv10): exactly that** |
| `h` clean, `f` crashes | the write is the cause, and the handler block plus the `first chunk
matched` line say which field of which header. **Did not happen: `f` ran clean** |
| `f` enters the world with a `first chunk matched` line and shadows on the ground | the contract
holds end to end, and this is the first real confirmation. **Ran (sv10): all of it, with the
morning set visible on the ground** |
| `b` crashes with `c`/`d`/`h`/`f` clean | `b`'s landing hands the resume address to `Load`
in `eax`, the record argument, so the client resolves its `MCNK` header inside its own code
and every sub-record ofs is `.text`. **What sv8 and sv10 both saw, and the reason nothing
that ships rides this shape** |
| no `mode` line at all in the log | `Load` was never called, so the loader, not the hook, stopped the mod |

## Honest limits

- **The shipping path is confirmed in game twice: sv10's mode `f` loaded the world and drew
  the baked morning set** (legacy single-map `.dat`, 114,328 entries), with `fixup: first chunk
  matched: gy 517 gx 448 key 529856, shadow drawn from arena+0` in the log and the shadows visible
  on the ground — and the **sv12 image on the twelve-run multi-map file loaded the world and drew
  its shadows, matching the legacy cut it replaced**, so the map table, the run gate and the 12-byte
  entries are proven in game too. What has *not* been checked in either run is every chunk: each
  walked the chunks around one spawn point, so neither matched all of its file's entries, neither
  reached a `10000 chunks matched so far` line, and both hit the 2000-chunk per-chunk cap. A whole-
  continent walk is still only implied by the coverage the `.dat` states (487 tiles in the morning
  set, chunk grid `(352,352)..(707,975)`; 309 tiles in the folder's `patch-S` file)
- **The sv13 image's own in-game run has not been done yet** — it is the same hook, the same stub
  shapes, the same writes and the same `.dat` as the sv12 image that did run, plus one level switch
  in the writer and one log trim at load, so what is unproven about it is those two, not the contract
- **The crash every earlier build produced is now fully explained, and it was never in the
  `.adt`.** Every hooked run died at `0x6B0FB7`, the `mov ecx,[ebx]` that reads the MCVT
  floats, reading `0x6CF97F32` while `this` moved. The sv10 mode-`b` handler dump names the
  reason: `record@0x168 = 0x6af5f6` and `hdr@0xf08 = 0x6af5fe`, which are that rung's own
  resume address `0x6AF5F0 + 6` and its `+8`, so `Load` had been resumed with `eax` = resume
  address instead of the record — `stub_code` ends in `mov eax,<resume> / jmp eax`. It
  then filed that value as the chunk's record and resolved its header table inside its own
  code, so the ofss are `.text`: the dword at `0x6AF5FE+0x14 = 0x6AF612` is `0x6C8E8934`,
  hence `this+0xf14 = 0x6AF5FE + 0x6C8E8934 = 0x6CF97F32`, the address every report names. All
  eight cursors the handler printed reproduce off the client's `.text`. The audit of the same
  chunk printed the real table beside it (MCVT `0x88`, MCSH `0x4ec`, MCSE `0x254c`, global col
  `0x1c0` row `0x205`), so the contract, `FIELD_MCSH_OFS 0x2c` and the whole sub-record table
  are settled in the client's memory byte for byte
- **The ladder runs are real evidence, and the last one drew the set.** The sv7 four-rung build:
  `n` clean, `a` clean with the whole 34 MB arena resident and no hook, `h` and `f` crashed at
  `0x6B0FB7` reading `0x6CF97F32`. The sv8 build added the rung whose fixup returns before
  touching anything, and **it crashed the same way**, log ending at its install lines — so
  nothing the call does is needed for the fault, only the hook. Its report is intact: the
  frame is where the report says, and `this+0xf14` already holds the unmapped value while
  `ECX` moves. The sv9 build ran `c` and `d`, **both clean**, which moved `h` and `f` onto
  `d`'s shape; its `h`/`f` runs printed `build defect` instead of installing, which sv10's
  shape-aware slot check fixes. The sv10 build then ran `h` clean with a full audit and `f`
  **with the baked set drawn on the ground**, and re-ran `b`, which crashed and named its own
  return form as the reason. Nothing about the shadow payload is unexplained any more
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
- One `.dat` can carry several maps, but a chunk is only looked up inside the run whose
  map name matches the client's `s_mapName`: a `.dat` cut for other maps is ignored with
  one log line naming every run it answers for, rather than applied somewhere it does not
  belong. A multi-map file and a legacy single-map file (`mapCount` 0, 8-byte entries) are
  both read by the same loader, so swapping between them is one file rename.
- **What the multi-map shape has shown in game, so far:** the sv11 build put this
  twelve-run `.dat` on the ground as mirrored-and-rotated shadows — right shapes,
  wrong place — and the reason is one read: the payload area was not seeked past the
  map-record table, so every entry sat 336 bytes off (see the `.dat` section). The
  legacy cut of the same archive drew right under the same build, because its
  `rec_bytes` is 0. The sv12 build put the term back; its run of this file has not
  happened yet, so the twelve-run file being drawn correctly is still only measured
  against `patch-S.mpq`'s own `MCSH` bodies — all 76,547 entries, run by run — and
  not yet seen in game.
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
