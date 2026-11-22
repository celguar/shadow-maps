# sv14: the sun travels an arc, and the game's own clock picks it

Same shadow pipeline as sv13 — the hook, the stubs, the arena, the writes and
the `.dat` are sv13's byte for byte — plus one new thing: the DLL writes the
client's own sun key tables, so the sun the client evaluates (the direction
terrain and objects are shaded from, and the sun disc drawn in the sky) travels
a real arc instead of the stock one, and **which arc gets written is the
client's in-game time of day**.

Nothing else in the pipeline changed: no new hook, no new file format, no new
export, and `verify_dll.py` still reports the same hook, the same three detour
tables and `Load` at RVA 0x2720.

## How it is driven

The DLL never makes a clock of its own. It reads the client's:

* the day-night state block at `0xce9b60` holds the game clock word, the time of
  day in `[0,1)` and the day count at `+0`, `+4` and `+8`, and the client stamps
  that time every rendered world frame (`benilla-re systems/lighting/daynight.md#light-state`);
* the DLL converts it the way the client's own light-table build does —
  `half-minute = round(tod * 2880)`, 2880 being the half-minutes in a day, so
  120 half-minutes to an hour — and picks the one preset whose window of
  half-minutes holds that number (`benilla-re systems/lighting/celestial.md` for the tracks and
  the evaluator `0x6cf6c0`);
* the read happens at the per-chunk fixup, the one place this DLL already runs
  on every loaded chunk, so the choice is made where the world frames have
  already started stamping the block. A block that was never stamped reads 0,
  which is midnight, which is `arc-high`'s window — an arc is written from the
  first chunk visit, and the visit whose clock had not arrived yet is corrected
  by the next one.

So, in game: **the hour you entered the world at names the arc.** The preset is
written at the first chunk visit and re-asserted at every visit after, and a
chunk visit that finds the clock in another window applies that window's preset,
once per preset in the log.

The only override is a sidecar file, `shadow-mod.sun`, next to the mode file
(next to `ShadowMaps.dll`, not next to the sources):

| `shadow-mod.sun` says | effect |
|---|---|
| (absent) | the clock decides — the concept, and what you should test first |
| `clock` | same, written out |
| `off` or `stock` | leave the stock key tables alone, no arc |
| `arc-high` `arc-long` `arc-low` `arc-west` `arc-stock` | force that preset every visit, whatever the clock says — how one arc is watched without waiting for the hour |

A word that is neither is logged and the clock decides.

## The stock sun, and why it is not an arc

The client evaluates four static key tracks through one evaluator, `0x6cf6c0`
(keys, count, phase), and derives the sun's position as
`(cos a sin p, sin a sin p, cos p)` of the same phase:

| track | address | keys | stock keys, in degrees |
|---|---|---|---|
| light azimuth | `0xce8d90` | 4 | 00:00 06:00 12:00 18:00 — all 225° |
| light polar | `0xce8db0` | 4 | 127 110 127 110 |
| disc azimuth | `0xce8d4c` | 3 | 05:30 12:00 21:30 — all 45° |
| disc polar | `0xce8d64` | 5 | 100 5 5 5 100 |

The stock disc goes 100° polar at 05:30 to 5° at noon and back to 100° at 21:30
with its azimuth pinned at 45° on all three keys: a vertical wheel that rises
and sets on the same bearing. The stock light direction never moves in azimuth
at all — 225° on all four keys — and only rocks between 110° and 127° polar.
That is what makes the whole world's shading sit on one fixed bearing. The
lighting is runtime-driven (the chain is in `docs/sun-lighting-analysis.md`
with the evidence, and it is also why `MCCF`'s own `+0x2c` light is
initialized from these same tracks); the stock data simply has nothing to drive
it.

## What sv14 writes instead

Every preset keeps the stock key counts, 4 / 4 / 3 / 5, because the count is the
`edx` to the evaluator and the blocks are adjacent — `0xce8d90 + 0x20` is
`0xce8db0`, `0xce8d4c + 0x18` is `0xce8d64`, and the key times live right after
the values at `kv + n` — so a preset that added a key would write over its
neighbour. Times and values change, shape does not. Two rules, both read off the
evaluator:

* the azimuth runs monotonically through the wrap and completes exactly 360°
  over the cycle. `0x6cf6c0` lerps in **value** space and its cyclic search
  wraps in **time**, so a value that wraps back is a backwards sweep and a value
  that leaves a gap is a jump at midnight. Monotonic through the wrap is what
  makes the interpolation one continuous forward rotation. This is why the
  azimuths below are left unwrapped (495°, not 135°): wrapping is what reverses
  the sweep, and the client's sin/cos read either spelling the same.
* the light azimuth is the disc azimuth plus 180°, and the light polar is the
  disc polar subtracted from 180°, at the same key times. So the direction the
  shading comes from and the sun you can see are one sun all day. (The stock
  pair is not antipodal — 225° against 45° in azimuth but 127°/110° against
  100°/5°/5°/5°/100° in polar. Being antipodal is the deliberate change.)

The clock windows are five 600-half-minute (five-hour) windows:

| preset | clock window | disc azimuth → light azimuth | disc polar → light polar |
|---|---|---|---|
| `arc-high` | 00:00–05:00 | 315 405 495 @ 05:30 12:00 21:30 → 495 585 675 675 | 100 30 5 30 100 @ 05:30 08:45 12:00 15:15 21:30 → 80 150 175 80 |
| `arc-long` | 05:00–10:00 | 270 405 540 @ 03:00 12:00 23:00 → 450 585 720 720 | 100 30 5 30 100 @ 03:00 07:30 12:00 16:30 23:00 → 80 150 175 80 |
| `arc-low` | 10:00–15:00 | 225 315 405 @ 08:00 12:00 16:00 → 405 495 585 585 | 100 60 35 60 100 @ 08:00 10:00 12:00 14:00 16:00 → 80 120 145 80 |
| `arc-west` | 15:00–20:00 | 180 270 360 @ 06:00 12:00 18:00 → 360 450 540 540 | 100 40 10 40 100 @ 06:00 09:00 12:00 15:00 18:00 → 80 140 170 80 |
| `arc-stock` | 20:00–24:00 | the stock tables above — **nothing is written** | |

Bearing convention: azimuth is clockwise from +X to +Y, 0° = west, 90° = north,
180° = east, 270° = south, and the light-travel azimuth is the antipode of the
sun's bearing. Polar is from the sky's top, so 100° is under the horizon.

What each arc looks like in game. The bearings are mod 360 of the preset's own
keys and the direction names follow the same eight points as the convention
above — west, northwest, north, northeast, east, southeast, south, southwest —
and the horizon crossing is where the polar angle passes 90°, interpolated the
way the client interpolates it, which is why a crossing sits a little past a
key that is exactly on a cardinal point (`arc-west`'s keys are 180° and 360°,
its crossings read 188° and 353°):

| preset | rise | noon | set |
|---|---|---|---|
| `arc-high` | ~05:58, azimuth 321°, southwest of west | azimuth 405° → 45°, northwest, polar 5°, nearly overhead | ~20:36, azimuth 487° → 127°, northeast of north |
| `arc-long` | ~03:38, azimuth 280°, just west of south | azimuth 405° → 45°, northwest, polar 5° | ~22:04, azimuth 529° → 169°, east — an eighteen-and-a-half-hour day |
| `arc-low` | ~08:30, azimuth 236°, southeast of south | azimuth 315°, southwest, polar 35°, a short low day | ~15:30, azimuth 394° → 34°, north-northwest |
| `arc-west` | ~06:30, azimuth 188°, east | azimuth 270°, due south, polar 10° | ~17:30, azimuth 353°, west — the textbook arc |

`arc-west` is the one to look at first: it is the cleanest arc — rises due east,
due south and high at noon, sets due west — so if any arc is going to read as
"the sun moved", this is it.

Before anything is written the four tracks are pinned: each must hold the stock
keys this build was read from, within the degree a float rounds to, or be an
all-zero block whose builder has not run (`0x6d3a10` and `0x6d3b80` build these
at first use behind the guard bits at `0x6d3b44` and `0x6d3ba4`, and that is
after this DLL was loaded). A track holding neither is not the shape the sun
state reads, and the sun is left alone with one log line while the shadows keep
working.

## Testing in game

Build and check (the worktree's own front end):

```
python build.py --check .          # compiles src/mod.c only: C errors, nothing about i686
python build.py .                  # links out\ShadowMaps.dll
python verify_dll.py out\ShadowMaps.dll
```

The sv14 image is 152,064 bytes.

Ship it as sv13 ships it: `ShadowMaps.dll`, `ShadowMaps.def`, `ShadowMaps.dll.id`
in `dlls.txt`, and rename the shadow file to `shadowmaps.dat` **next to the
DLL**.

The mode file decides what runs, and that is how the arc is reached — the first
letter of `shadow-mod.mode` next to the DLL picks the rung, and a `v` anywhere
in it turns the diagnostic lines on. A missing file means `f` with quiet
logging:

| put in `shadow-mod.mode` | what you get |
|---|---|
| (no file) | mode `f`, quiet: the arc runs **and** the `.dat` must be there, so a missing `.dat` means `Load` answers 1, the hook is not patched, and VanillaFixes shows its popup — no arc |
| `h v` | the test rung: hook, the per-chunk audit, **no `.dat` needed**, nothing allocated, no chunk header touched — shadows stay stock and the arc still runs, with the diagnostic lines on |
| `f v` | the full rung with the diagnostics on: the arc runs and the baked shadows draw |
| `d v` or `b v` | the silent rungs: the hook is in and the call happens, but the fixup returns before the sun, so **no arc** — their install line says so |

`n`, `a` and `c` never call the fixup at all, so they carry no arc either; the
install line names which rung you are on. **`h` and `f` are the two rungs that
carry the arc**, and `h` is the one to test on because it needs no `.dat`.

Then, in order:

1. Put `h v` in `shadow-mod.mode`, start the game, and look at the log,
   `shadow-mod.log.txt` next to the DLL. The line that matters is the applied
   line, and it says the clock, the preset and the arc's own numbers off the
   table it just wrote. For a world entered at 10:50 it reads (the clock digits
   vary with the client's own float, the words do not):

   ```
   sv14 sun arc: the client's clock at 0xce9b60+4 reads 10:50 (the float is 0.4514, half-minute 1300 of 2880), so arc-low is the preset written into the client's own key tables at 0xce8d90 (light azimuth and polar), 0xce8d4c (disc azimuth) and 0xce8d64 (disc polar): the disc's azimuth runs from 225 deg to 405 deg, 180 deg over the day, and its polar angle goes from 100 deg through 35 deg back to 100 deg, so the sun crosses the horizon at both ends instead of rocking on one bearing; the light tracks are the disc's antipode, so the shading follows the sun you can see, and 0x6cf6c0 interpolates it every frame. re-asserted at every chunk visit
   ```

   One line per preset: the log does not repeat it while the clock stays in the
   same window.

2. Enter at a different hour and check the line's clock and preset move
   together: 04:00 → `arc-high`, 05:10 → `arc-long`, 10:50 → `arc-low`, 16:50
   → `arc-west`, 21:50 → `arc-stock`, and 21:50 instead prints

   ```
   sv14 sun: the client's clock at 0xce9b60+4 reads 21:50 (the float is 0.9097, half-minute 2620 of 2880), which is arc-stock's window: that preset is the stock sun, so nothing is written and the world keeps its stock sun. enter at another hour and the same line names the arc that gets written
   ```

3. Watch the world, not only the log: the sun disc rises and sets at different
   compass bearings, its glare sweeps the horizon instead of sliding along one
   line, and terrain normals light from the direction the disc is actually in —
   faces that were lit at the stock fixed bearing change through the day, and
   the disc and the shading agree because the disc azimuth is the light
   azimuth's antipode.
4. Force one arc without waiting for the clock: `arc-west` in `shadow-mod.sun`,
   same folder as the mode file. That is the fastest way to see the cleanest
   arc, and `off` in that file is the way back to the stock sun — it logs
   `sv14 sun: shadow-mod.sun said off, so the stock sun key tables are left
   alone and the world keeps its fixed 45 deg bearing` and touches nothing.
5. If the day-night file ever rebuilds its tracks after install, the line to
   look for is

   ```
   sv14 sun: the key tables at 0xce8d90 and 0xce8d4c hold the stock keys again under arc-low, so the day-night file built its tracks after install: the keys are written again here, and the next chunk visit keeps them, so a stock sun in game after this line is that rebuild
   ```

   A stock sun after that line is the rebuild, not the arc failing.

What is expected to look wrong, and why it is not this change: the baked `MCSH`
bearings still come from Salmiak's fixed 45° bake, so a chunk's own baked shadow
map disagrees with the sun once the sun has moved. That is the data half — the
same thing every arc row in `docs/sun-lighting-analysis.md` says — and it is not
in this concept. Leave the shadow payloads stock for the sun test, which is what
mode `h` gives you without any extra work.

## Not done, deliberately

* No second hook, no per-frame patch site. A per-frame site means pinning a
  second prologue and this branch has not read one; the arc's motion comes from
  the client's own per-frame evaluation of the keys, so a second hook would buy
  only the preset switch mid-session — and the preset switch happens on the next
  chunk visit, which is every chunk the world loads.
* Five presets, compiled in, one per clock window. No `.sun` payload format yet:
  the preset shape is what such a payload would carry, and that format belongs in
  `shadow_dat.h`, not in the hook.
* The azimuths are left unwrapped above 360° because that is what keeps the
  sweep forward. The client's sin/cos do not care; if anything else in the
  client ever compares these floats with an angle difference, that is a thing to
  test, not a claim made here.
