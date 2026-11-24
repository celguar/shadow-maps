# sv15: the sun travels one arc, the game's own clock picks where it applies

Same shadow pipeline as sv13 — the hook, the stubs, the arena, the writes and
the `.dat` are sv13's byte for byte — plus sv14's sun writes and two new things
of sv15's: a number in the mode file sets the client's own game clock's scale,
so one session can watch the arc cross the sky, and every preset that gets
written is read back out of the client's bytes and printed, so a sky report
names the live keys rather than the sky.

Nothing else in the pipeline changed: no new hook, no new file format, no new
export, and `verify_dll.py` still reports the same hook, the same three detour
tables and `Load` at RVA 0x27c0 — that RVA moved from sv14's 0x2720 because the
module grew, not because the contract did.

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
  which is midnight, which is the arc's window — the arc is written from the
  first chunk visit, and a visit whose clock had not arrived yet is corrected
  by the next one.

So, in game: **the hour you entered the world at names the sky.** The preset is
written at the first chunk visit and re-asserted at every visit after, and a
chunk visit that finds the clock in another window applies that window's preset,
once per preset in the log.

The only sun override is a sidecar file, `shadow-mod.sun`, next to the mode file
(next to `ShadowMaps.dll`, not next to the sources):

| `shadow-mod.sun` says | effect |
|---|---|
| (absent) | the clock decides — the concept, and what you should test first |
| `clock` | same, written out |
| `off` or `stock` | leave the client's own key tables alone, no arc, no write-back |
| `arc-senw` `arc-stock` | force that preset every visit, whatever the clock says — how one sky is watched without waiting for the hour |

A word that is neither is logged and the clock decides.

## The stock sun, and why it is not an arc

The client evaluates four static key tracks through one evaluator, `0x6cf6c0`
(keys, count, phase), and derives the sun's position as
`(cos a sin p, sin a sin p, cos p)` of the same phase. Stock's own keys, times
and values, from `0xce8d90` in the order the pin reads them:

| track | address | keys | stock keys, in degrees |
|---|---|---|---|
| light azimuth | `0xce8d90` | 4 | 00:00 06:00 12:00 18:00 — all 225° |
| light polar | `0xce8db0` | 4 | 00:00 127°, 06:00 110°, 12:00 127°, 18:00 110° |
| disc azimuth | `0xce8d4c` | 3 | 05:30 12:00 21:30 — all 45° |
| disc polar | `0xce8d64` | 5 | 05:30 100°, 11:55 5°, 12:00 5°, 12:05 5°, 21:30 100° |

The stock disc goes 100° polar at 05:30 to 5° at noon and back to 100° at 21:30
with its azimuth pinned at 45° on all three keys: a vertical wheel that rises
and sets on the same bearing. The stock light direction never moves in azimuth
at all — 225° on all four keys — and only rocks between 110° and 127° polar.
That is what makes the whole world's shading sit on one fixed bearing. The
lighting is runtime-driven (the chain is in `docs/sun-lighting-analysis.md`
with the evidence, and it is also why `MCCF`'s own `+0x2c` light is
initialized from these same tracks); the stock data simply has nothing to drive
it.

Read the two pairs as the client does, which is what makes an evening look the
way stock does:

* the **disc** polar is measured from the sky's top, so 5° is near zenith and
  100° is under the horizon: stock's disc is at +39° elevation at 08:50, +85° at
  noon, +0° at 20:30 and −10° at 21:50. Stock's sun is already below the horizon
  through the evening, and its disc's own size track `0xce8cac` (`06:00 2,
  06:45 1, 20:15 1, 21:00 2`) is why a night sun draws big when it draws at all.
* the **light** polar is read as `90° + elevation`, because that vector is the
  direction light *travels*: 110° is 20° above the horizon and 127° is 37° above
  it, so stock's light never once comes from below the horizon or from more than
  37° up. A polar of 80° is not "10° lower than a polar of 90°" — it is a light
  direction 10° above the horizon, which puts the diffuse term at zero for every
  up-facing normal and reads as mirrored, over-bright ground. That is the one
  thing the arc must not touch.
* the light **azimuth** is the travel direction, so the source bearing is that
  azimuth plus 180°: stock's 225° is light travelling south-east, light coming
  from north-west — the same side as its 45° disc. Compass words are map words
  here: down is south, left is west.

## What the arc writes instead

Every preset keeps the stock key counts, 4 / 4 / 3 / 5, because the count is the
`edx` to the evaluator and the blocks are adjacent — `0xce8d90 + 0x20` is
`0xce8db0`, `0xce8d4c + 0x18` is `0xce8d64`, and the key times live right after
the values at `kv + n` — so a preset that added a key would write over its
neighbour. Times and values change, shape does not.

sv14 got three things wrong, and an in-game visit showed each of them. What
sv15 writes is what that visit taught:

* **the stock disc polar keys are at 11:55, 12:00 and 12:05, not three keys at
  noon.** `celestial.md:41` and the builders at `0x811510` say the day-night
  file makes that pair by adding and subtracting 0.0034722 — five minutes. sv14
  pinned against a table whose noon times were all 0.5, so on a client that had
  already built its day-night file its own tracks matched neither the stock
  table nor the all-zero block: the pin refused and the arc was never written.
  sv15's stock table, and its presets, carry 11:55 and 12:05.
* **one arc, not five.** A sun crossing one sky cannot be at a different bearing
  in one window of the clock than in another, and the four tracks are one global
  block that the day-night file builds once behind the guard bits at `0x6d3a10`
  and `0x6d3b80`. sv14's five windows gave five different arcs that all kept
  living in every hour that did not overwrite them.
* **the evening writes the stock keys back, it does not write nothing.** Not
  writing is what let a morning arc survive into the evening.

The polar angles stay stock's, and the light azimuth stays the disc azimuth
plus 180°, at the disc's own key times. Those two rules are the whole point of
the arc: the bearing the ground and water are shaded from always agrees with the
bearing of the sun you can see — never 180° off it, never pointing up.

One key is one `DNKeyframe`: the TIME at `kv + 8k` and the VALUE at `kv + 8k + 4`
(`wowdecomp/WoW/Source/WorldClient/DayNight.h:32` says `m_time` at +0000 and
`m_value` at +0004, and that build's sun tables are the same shapes — 4 azimuth
keys at 0 0.25 0.5 0.75 all 3.926991, 4 polar keys 2.2165682 / 1.9198623 /
2.2165682 / 1.9198623, 3 disc azimuth keys at 0.2291667 0.5 0.8958333, 5 disc
polar keys at 0.2291667 0.4965278 0.5 0.5034722 0.8958333). sv14 wrote values
first and times second; the first in-game log proved that is the other client's
arrangement, because the four live first slots it printed were `0 0 13 13` deg —
which is exactly stock's first-key TIMES, `0.0 0.0 0.2292 0.2292`, in the slot the
evaluator calls the value of.

The clock windows are two, not five:

| preset | clock window | disc azimuth → light azimuth | disc polar → light polar |
|---|---|---|---|
| `arc-senw` | 00:00–20:00 | 225 135 45 @ 05:30 12:00 20:00 → 405 360 315 225 @ 05:30 08:45 12:00 20:00 | stock's own: 100 5 5 5 100 @ 05:30 11:55 12:00 12:05 21:30 → 127 110 127 110 @ 00:00 06:00 12:00 18:00 |
| `arc-stock` | 20:00–24:00 | stock's own tables, written back — 45° on all three keys → 225° on all four | stock's own, as above |

Bearing convention, pinned by an in-game sky check and not by the bytes: azimuth is
measured from +X toward +Y and reads **0° = north, 90° = west, 180° = south,
270° = east**, so it DECREASES as the sun walks east through south to west, which
is the clockwise-on-the-map sense a real sun has (down is south, left is west).
Stock's 45° disc and 225° light are both on the north west either way the azimuth
is turned, which is why the bytes alone cannot name the hand — the arc that
shipped first was, and its disc rose in the north east and set in the north west,
which said the other hand was the one in front of the bytes. The light-travel
azimuth is the antipode of the sun's bearing, so the source bearing is that
azimuth plus 180°. Polar is from the sky's top, so 100° is under the horizon.

What `arc-senw` looks like in game, off the evaluator — azimuths and elevations
from the same interpolation the client does, and the crossings are where the
disc polar passes 90°:

| clock | stock | `arc-senw` |
|---|---|---|
| horizon crossings | 06:10 rises, 20:30 sets | 06:10 rises, 20:30 sets — same, because the polar is stock's |
| 05:30, stock's sunrise hour | disc az 45° north west, elev −10° | disc az 225° south east, elev −10° |
| Kalimdor 08:50 | disc az 45° north west, elev +39° | disc az 179° south, elev +39° |
| Kalimdor noon | disc az 45° north west, elev +85° | disc az 135° south west, elev +85° |
| 16:00 | disc az 45° north west, elev +45° | disc az 90° west, elev +45° |
| 19:50 | disc az 45° north west, elev +7° | disc az 47° north west, elev +7° |
| EK 20:00, the handover | disc az 45° north west, elev +5° | disc az 45° north west, elev +5° — the arc is already stock's bearing there |
| EK 20:30 | disc az 45° north west, elev +0° | *(not this preset: 20:30 is `arc-stock`, which is stock)* |
| EK 21:50 | disc az 45° north west, elev −10° | *(not this preset: 21:50 is `arc-stock`, which is stock)* |

So the arc rises in the south east, on stock's own sunrise hour, passes due south
in the morning and due west in the afternoon, and is back on stock's own north
west bearing by 20:00 — half a turn walked across the south and west, the
clockwise-on-the-map sense a real sun has, instead of stock's sun, whose azimuth
never moves off the north west diagonal at all. It is dead on stock's own azimuth at 20:00,
which is why `arc-stock` taking over at 20:00 is not a jump, and the whole evening
— the set at 20:30, the light from the north west, the sunset on the horizon at
21:00 — is stock's own pixels, because the keys written there are stock's own.
The shading's bearing is the disc's bearing at every hour of that table, which is
what the readback line's last number, 0 deg apart, says.

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

The sv15 image is 161,792 bytes.

Ship it as sv13 ships it: `ShadowMaps.dll`, `ShadowMaps.def`, `ShadowMaps.dll.id`
in `dlls.txt`, and rename the shadow file to `shadowmaps.dat` **next to the
DLL**.

The mode file decides what runs, and that is how the arc is reached — the first
letter of `shadow-mod.mode` next to the DLL picks the rung, a `v` anywhere in it
turns the diagnostic lines on, and **a number anywhere in it sets the game
clock's scale to that many game minutes per real second**. A missing file means
`f` with quiet logging:

| put in `shadow-mod.mode` | what you get |
|---|---|
| (no file) | mode `f`, quiet: the arc runs **and** the `.dat` must be there, so a missing `.dat` means `Load` answers 1, the hook is not patched, and VanillaFixes shows its popup — no arc |
| `h v` | the test rung: hook, the audit of the **first** chunk only, **no `.dat` needed**, nothing allocated, no chunk header touched — shadows stay stock and the arc still runs, with the decision lines on |
| `h 5 v` | the same rung with the day at 5 game minutes a real second — a game day of 288 real seconds, under 5 minutes, an hour of sky every 12, so one session watches the arc cross the sky |
| `h 5 v q` | the same, and a `q` anywhere puts the **per-chunk** header dump back on: 2,000 lines of ~300 bytes, which is more than the newest 64 kB the log keeps, so this is what buries the sun lines — use it when a chunk is crashing, not when the sky is being read |
| `f v` | the full rung with the diagnostics on: the arc runs and the baked shadows draw |
| `d v` or `b v` | the silent rungs: the hook is in and the call happens, but the fixup returns before the sun, so **no arc** — their install line says so |

`n`, `a` and `c` never call the fixup at all, so they carry no arc either; the
install line names which rung you are on. **`h` and `f` are the two rungs that
carry the arc**, and `h` is the one to test on because it needs no `.dat`.

The log itself is bounded: `TrimLog` cuts a file past 512 kB to its newest 64
kB of whole lines at load, and the per-chunk header dump is capped at 2,000
lines. Those two together are why the sun lines were unreadable in a fast
session — the dump alone is ~600 kB, so it pushed out everything the log keeps.
The dump is now opt-in (a `q`), and the sun's own lines are capped: 4 giant
lines, 60 short ones, 4 keys-lost ones. So with `h 5 v` the whole day's evidence
fits and the file opens at a few tens of kB — the sun lines are near the top of
it, after install and the first-chunk audit, and everything under them is a
handful of clock and visit lines. Grep for `sun ` and the whole day is there.

The number is read as a run of digits: `9` is nine, `45` is forty-five, `2 7` is
seven because a gap ends a run and the last run wins, and a run is capped at 99.
**The number is the scale itself**, game minutes per real second, not a
multiplier of what is in the field: `0x643300` builds the clock with `+0x2c` =
`0x3c888889`, 1/60, and the server's time messages carry that same scale, so the
live float is 17 milli-game-minutes per real second, not 1000. A multiplier of
99 therefore asked for 1.65 game minutes a real second — an hour of sky in 36
real seconds, which is what the last visit watched the day do. `0x643820`
clamps the scale to `[1/60, 60]`, and this DLL keeps that clamp, so digits at or
above 60 all land on 60 game minutes a real second: one game hour a real second
and a whole game day in 24 real seconds. At 9 a day is 160 real seconds and an
hour of sky turns in 7. The scale is only re-written when the live float is not
already what this DLL wrote, which is what keeps the server re-sending its own
1/60 from compounding off: this is a test control, not a shipping one.

Then, in order:

1. Put `h v` in `shadow-mod.mode`, start the game, and look at the log,
   `shadow-mod.log.txt` next to the DLL. The line that matters is the applied
   line, and it says the clock, the preset and the arc's own numbers off the
   table it just wrote. For a world entered at 08:50 it reads (the clock digits
   vary with the client's own float, the words do not):

   ```
   sv15 sun arc: the client's clock at 0xce9b60+4 reads 08:50 (the float is 0.3681, half-minute 1060 of 2880), so arc-senw is the preset written into the client's own key tables at 0xce8d90 (light azimuth and polar), 0xce8d4c (disc azimuth) and 0xce8d64 (disc polar): the disc's azimuth runs DOWN from 225 deg to 45 deg, 180 deg over the day, so the sun walks south east through south and west to north west, the clockwise-on-the-map sense a real sun has, and its polar angle goes from 100 deg through 5 deg back to 100 deg, so the sun crosses the horizon at both ends instead of rocking on one bearing; the light azimuth is the disc azimuth plus 180, so the shading comes from the bearing the disc is in and the two never disagree about it, while both tracks keep stock's own polar angles, which is what keeps the light between +20 and +37 deg above the horizon all day; 0x6cf6c0 interpolates it every frame. re-asserted at every chunk visit
   ```

   One line per preset: the log does not repeat it while the clock stays in the
   same window.

2. Enter at an evening hour and check the line moves to the write-back instead.
   At 21:20:

   ```
   sv15 sun: the client's clock at 0xce9b60+4 reads 21:20 (the float is 0.8889, half-minute 2560 of 2880), which is arc-stock's window: the stock sun keys are WRITTEN back into the client's own tables at 0xce8d90 and 0xce8d4c, so an arc another continent or an earlier hour left cannot survive here. sv14 wrote nothing in this window and that was wrong twice over: the four tracks are one global block and the day-night file builds them once behind the guard bits at 0x6d3a10 and 0x6d3b80, so the last arc written keeps living in every hour that does not overwrite it -- and outside a preset's own key span 0x6cf6c0 takes the leg from its last key back to its first and interpolates ALONG it, so hours the preset never covered get an extrapolated sun whose light polar can come back above 90 deg, a light direction pointing above the horizon, which zeroes the diffuse term of every up-facing normal and reads as mirrored, reflected ground. the world keeps its stock sun here, and enter at another hour and the next line names the arc that gets written
   ```

   An evening that matches stock after that line — sun at or under the horizon,
   sunset visible, light from the north west — is the write-back working. An
   evening that is still the morning arc is not.

3. Put a number in the mode file and watch the sky instead of waiting for it:
   `h 5 v` puts the day at 5 game minutes a real second — a game day of 288 real
   seconds, just under 5 minutes, an hour of sky every 12 — and the clock the UI
   shows moves with it. The line to look for is

   ```
   sv15 clock: shadow-mod.mode had 5, so the game clock object's scale at 0xce8564, the game clock at 0xce8538's +0x2c, went from 17 to 5000 milli-game-minutes per real second. 5 is the mode file's number used as the scale itself rather than as a multiplier of what was there, because the server sends this scale as 0.01666667f, 1/60, which is also what 0x643300 builds the clock with: multiplying that live scale asked for 83 milli, an hour of sky in well under a minute but nowhere near a game minute a second. the float is what 0x643a70 multiplies the real seconds since the last minute by to get the day phase and what 0x6435a0 adds dt times of to the minute accumulator, so a whole game day is 288 real seconds and one hour of sky turns in 12 real seconds: the sun's phase, the clock the UI shows and the day count all move together. 0x643820 clamps the scale to [1/60, 60], 17 to 60000 milli, so digits at or above 60 land on 60, one game hour a real second and a day in 24 real seconds; the server re-sends its own scale in its time messages -- the 0x47 handler 0x6c5de0 and the 0x42 handler 0x6c5e80 both end in 0x643820 -- so the number is set again at every chunk visit
   ```

   Nothing here makes a clock: `0xce8564` is the scale float inside the client's
   own game-clock object at `0xce8538` (`benilla-re systems/gametime.md`), and
   the arc's phase, the day count and the clock the UI shows all read the same
   accumulator, so the sky and the clock cannot drift apart. The scale is only
   re-written when the float is not already what this DLL wrote, which is what
   keeps the server's time messages from compounding.

4. Force one preset without waiting for the clock: `arc-senw` in `shadow-mod.sun`
   in the same folder as the mode file, and `off` to leave the client's tables
   alone. The file is read once at install, so a fresh session is what applies a
   changed word — and a world entered at 23:00 is in the stock window by design,
   so the arc cannot be seen there at all until this file forces it. Forcing it
   there is the test of the write itself: the sky shows the arc or the client
   overwrote it, and the keys-lost line says which.

5. Every preset that is written gets a readback line after it, and that line is
   what makes a sky report decisive: it reads the four tracks back out of the
   client, replays the client's own evaluator rule over whatever those bytes
   hold at the client's own phase, and prints the disc's bearing and elevation
   and the bearing the shading comes from. An in-game sky that disagrees with
   this line is a disagreement about the live bytes, not about the tables the
   source says it wrote. Only the first four writes of a session get the giant
   line, with all 22 live keys in it; after that the same numbers come on the
   short line, because a fast day crosses a window boundary every few seconds
   and 200 giant lines is more than the log keeps:

   ```
   sv15 sun read back: what the client's own tracks hold now, and what its evaluator makes them give at the client's own phase. the phase at 0xce9b60+4 is 0.3681, half-minute 1060 of 2880, and arc-senw is the preset this visit wrote. the live keys, each key a time at +0 and a value at +4, printed as a track's times then its values, degrees as the radians at each address read back: light azimuth 0xce8d90 at 0.2292 0.3646 0.5000 0.8333 holds 405 360 315 225 deg; light polar 0xce8db0 at 0.0000 0.2500 0.5000 0.7500 holds 127 110 127 110 deg; disc azimuth 0xce8d4c at 0.2292 0.5000 0.8333 holds 225 135 45 deg; disc polar 0xce8d64 at 0.2292 0.4965 0.5000 0.5035 0.8958 holds 100 5 5 5 100 deg. interpolating those at the phase gives the disc at azimuth 179 deg and polar 51 deg and the light azimuth 359 deg and polar 118 deg, so the disc sits at bearing 179 deg, 39 deg above the horizon, and the shading comes from bearing 179 deg, 28 deg high: they are 0 deg apart. read the two bearings against the sky you are looking at: a disc bearing that matches makes the tables and the handedness right and the arc's own angles are what to change; a shading bearing 180 apart from the disc is the light pointing at the sun rather than away from it; and a disc polar that should keep it below the horizon at a night hour and does not is a key time belonging to another track. the azimuths are 0 = north, 90 = west, 180 = south, 270 = east, a frame an in-game sky check pinned and not the bytes, because stock's own 45 deg reads north west either way round
   ```

   The short line, which every write after the fourth gets:

   ```
   sv15 sun status: the client's own phase at 0xce9b60+4 is 0.3681, half-minute 1060 of 2880, and arc-senw was the key set this visit wrote into the live keys. what the client's evaluator makes those bytes give: the disc at azimuth 179 deg and polar 51 deg and the light at azimuth 359 deg and polar 118 deg, so the disc sits at bearing 179 deg, 39 deg above the horizon, and the shading comes from bearing 179 deg, 28 deg high: they are 0 deg apart. the live keys themselves are in the giant line, which this session already printed 4 times
   ```

   The last five numbers of either line are the ones to compare with the sky —
   the disc's bearing, its elevation, the shading's bearing, its elevation, and
   how far the two bearings are apart. The stock window gets the same line after
   its write-back, so an evening can be checked the same way.

6. The giant line is taken at the write, so it says the write **landed**. What
   says it **stayed** is the per-visit line, and it is the other half of the
   evidence: the same preset armed again, and its own first key read back out of
   the client. A visit that finds the key gone found something that replaced the
   write between visits, and the line names what replaced it. This kind of fault
   is not the same as a write that never happened, and the four live readings
   tell them apart — the stock first pairs are `0.0000 / 225 deg, 0.0000 / 127 deg,
0.2292 / 45 deg, 0.2292 / 100 deg`:

   ```
   sv15 sun keys lost: arc-senw was armed again and the client's own tables no longer hold what this DLL wrote there. each track's first key as the live bytes read it, a time at +0 and a value at +4, then what the preset's own table wants there: light azimuth 0xce8d90 holds 0.0000 and 225 deg and wants 0.2292 and 405; light polar 0xce8db0 holds 0.0000 and 127 deg wants 0.0000 and 127; disc azimuth 0xce8d4c holds 0.2292 and 45 deg wants 0.2292 and 225; disc polar 0xce8d64 holds 0.2292 and 100 deg wants 0.2292 and 100. the stock first keys are 0.0000 and 225 deg, 0.0000 and 127 deg, 0.2292 and 45 deg and 0.2292 and 100 deg, so a live four of exactly those pairs is the day-night file building its own tracks behind the guard bits at 0x6d3a10 and 0x6d3b80, and the next write here is what gets them back; any other pair is a table something else writes. this session prints this line 1 of 4 times
   ```

   One batch of four of these at one timestamp is the expected shape of a session:
   the DLL is loaded before the first frame, so the client builds its own tracks
   the first frame it is in a world and that is the rebuild this line names. The
   next visit writes the keys back and the visits after it print nothing. Four
   lines at one timestamp and none later is that rebuild, and the sun is on the
   arc from then on; lines that keep coming at later visits mean something
   replaces the write every visit, and that is what a stock sun in game means.

7. The rebuild above, seen from the other side: the client put its own stock
   keys back, and the line that names that event says so in words. It and the
   keys-lost line are the same event seen from two checks — the keys-lost line
   fires first, because it asks what the armed preset wants rather than waiting
   for the four to be exactly stock's:

   ```
   sv15 sun: the key tables at 0xce8d90 and 0xce8d4c hold the stock keys again under arc-senw, so the day-night file built its tracks after install: the keys are written again here, and the next chunk visit keeps them, so a stock sun in game after this line is that rebuild
   ```

   A stock sun after that line is the rebuild, not the arc failing. The line
   never prints under `arc-stock`, which writes the stock keys itself.

What is expected to look wrong, and why it is not this change: the baked `MCSH`
bearings still come from Salmiak's fixed 45° bake, so a chunk's own baked shadow
map disagrees with the sun once the sun has moved. That is the data half — the
same thing every arc row in `docs/sun-lighting-analysis.md` says — and re-baking
is out of scope here. Leave the shadow payloads stock for the sun test, which is
what mode `h` gives you without any extra work.

## Not done, deliberately

* No second hook, no per-frame patch site. A per-frame site means pinning a
  second prologue and this branch has not read one; the arc's motion comes from
  the client's own per-frame evaluation of the keys, and the clock's speed-up
  comes from the client's own scale float, so a second hook would buy nothing.
* Two presets, compiled in, and one arc. The five windows sv14 shipped were not
  one sun crossing one sky; a preset payload format still belongs in
  `shadow_dat.h`, not in the hook.
* The clock scale stays a test control, because it writes the client's own scale
  float, and a server that sends its own time messages sets that float again —
  this DLL re-asserts its number at every chunk visit rather than compounding on
  it.
* The azimuths are left unwrapped above 360° — 405°, not 45° — because `0x6cf6c0`
  lerps in **value** space and its cyclic search wraps in **time**, so a value
  that wraps back is a backwards sweep and a value that leaves a gap is a jump at
  midnight. Monotonic through the wrap is what makes the interpolation one
  continuous forward rotation. The client's `fsin`/`fcos` read either spelling
  the same.
* The arc's light azimuth carries four keys at the disc's own hours plus 08:45 —
  05:30, 08:45, 12:00, 20:00 — rather than stock's 00:00 06:00 12:00 18:00, so the
  two azimuth tracks are the same sweep shifted by 180° at the same hours and the
  readback says the shading bearing is the disc's bearing at every hour of the day.
  Keying the two tracks at different hours is what let each interpolate its own
  leg, and sv14 did exactly that: the shading came from a bearing 180° off the
  disc at some hours and a few degrees off it at others. The night leg — 20:00
  back to 05:30 — is the cyclic one and interpolates along itself, which is as
  much of the night as four keys can cover, and the disc is under the horizon
  through all of it, so it has nothing to disagree about.
