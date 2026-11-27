# sv15: the sun travels one arc, the game's own clock picks where it applies

Same shadow pipeline as sv13 — the hook, the stubs, the arena, the writes and
the `.dat` are sv13's byte for byte — plus sv14's sun writes and two new things
of sv15's: a number in the mode file sets the client's own game clock's scale,
so one session can watch the arc cross the sky, and every preset that gets
written is read back out of the client's bytes and printed, so a sky report
names the live keys rather than the sky.

Everything below is sv15's, and it is still true of the sv16 build except for
the `.dat`: sv16 grows the format so ONE file carries several shadow presets per
map, matched to the same clock that picks the sun's window. That part is the
section at the end, and sv17's section after it is the one pass that re-picks
chunks that are already standing — it changes nothing else.

Nothing else in the pipeline changed: no new hook, no new file format, no new
export, and `verify_dll.py` still reports the same hook, the same three detour
tables and `Load` at RVA `0x27c0` — that RVA moved from sv14's `0x2720` because
the module grew, not because the contract did. (sv17's image has it at `0x2cc0`
for the same reason: growth.)

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
plus 180°, at the disc's own key times — plus one light key 20 minutes past the
disc's first, 05:50, which is what keeps the morning shading within a few degrees
of the sun instead of tens. Those two rules are the whole point of
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
| `arc-senw` | 00:00–21:30 | 225 135 45 @ 05:30 12:00 21:30 → 225 405 315 225 @ 05:30 05:50 12:00 21:30 | stock's own: 100 5 5 5 100 @ 05:30 11:55 12:00 12:05 21:30 → 127 110 127 110 @ 00:00 06:00 12:00 18:00 |
| `arc-stock` | 21:30–24:00 | stock's own tables, written back — 45° on all three keys → 225° on all four | stock's own, as above |

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
| 05:30, stock's sunrise hour | disc az 45° north west, elev −10° | disc az 225° south east, elev −10°; the shading is still at the night's 45° and swings onto the disc by 05:50 |
| Kalimdor 08:50 | disc az 45° north west, elev +39° | disc az 179° south, elev +39°; shading from 181°, 2° apart |
| Kalimdor noon | disc az 45° north west, elev +85° | disc az 135° south west, elev +85°; shading from 135°, 0° apart |
| 16:00 | disc az 45° north west, elev +45° | disc az 97° west, elev +45°; shading from 97°, 0° apart |
| 19:55 | disc az 45° north west, elev +6° | disc az 60° north west, elev +6°; shading from 60°, 0° apart |
| EK 20:00 | disc az 45° north west, elev +5° | disc az 59° north west, elev +5° — **still walking west**, shading from 59° |
| EK 20:30, the set | disc az 45° north west, elev +0° | disc az 54°, elev +0° — on the horizon and still moving west |
| EK 21:00 | disc az 45° north west, elev −5° | disc az 50°, elev −5°, still moving west |
| 21:30, stock's sunset key | disc az 45° north west, elev −10° | disc az 45° north west, elev −10° — the arc is dead on stock's bearing here, and `arc-stock` writes the same keys on |

So the arc rises in the south east, on stock's own sunrise hour, passes due south
in the morning and due west in the afternoon, and is back on stock's own north west bearing at
21:30 — half a turn walked across the south and west, the clockwise-on-the-map sense a real sun
has, instead of stock's sun, whose azimuth never moves off the north west diagonal at all. It is
dead on stock's own azimuth at 21:30, stock's own sunset key, which is why `arc-stock` taking
over there is not a jump, and the whole evening — the set at 20:30, the light from the north west,
the sunset on the horizon at 21:00 — is stock's own light, because the keys written there are
stock's own.

The reversal the first arc showed in game is what these key times are set by. The earlier arc's
last azimuth key sat at 20:00, and `0x6cf6c0` is cyclic, not clamped: past the last key it
interpolates along the leg that closes the day, toward the first key, so the disc turned back from
north west through west, south west and south while it was still above the horizon and the shading
followed it. Run the shipped keys through the client's own rule instead, and the disc's azimuth
descends all the way — 225° at 05:30, 135° at noon, 59° at 20:00, 54° at the set, 50° at 21:00,
45° at 21:30 — and the shading's bearing is the disc's to 0° over the afternoon and 4.6° at the
worst over the day (the gap sits at 05:50, the light's own second key, the one the disc does not
have). That is what the readback line's last number says.

Through the night, 21:30 back to 05:30, the shading does not follow the disc and cannot, because
the disc sits 10° under the horizon there: `arc-senw` holds stock's own 225° light azimuth at its
first and its last key both, so the leg that closes the day is flat and the shading stays north
west the whole way, which is stock's own night. The disc's own azimuth, which three keys cannot
also make flat, unwinds from 45° at 21:30 to 101° at midnight to 157° at 02:30 to 225° at 05:30
under that horizon; the one consumer of it that draws is the **sun glare**, which takes the disc's
position (`benilla-re systems/lighting/celestial.md:85-86`), so what to look for at 02:00 is a
glare low on the horizon travelling the north west quadrant while the ground stays put.

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

The lines quoted under the steps below are transcribed from the session that ran the **first**
arc — the one whose last azimuth key sat at 20:00. The words around the numbers are what the code
writes and have not changed, and so is the order of the keys, but the arc's own key values and
times in them are that arc's, not the shipped one's: read the shipped table above
(`225 405 315 225` at `05:30 05:50 12:00 21:30` for the light azimuth, `225 135 45` at
`05:30 12:00 21:30` for the disc azimuth), and expect the readback's last number to be 0° over the
afternoon and 2° at 08:50 rather than 0° at every hour, since the light's second key now sits 20
minutes past the disc's first rather than at 08:45. The window boundaries in the quoted text are
the earlier arc's too: `arc-stock` now begins at 21:30, so a world entered at 21:20 gets the arc
and one entered at 22:00 gets the write-back.

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
* The arc's light azimuth carries four keys at the disc's own hours plus one 20 minutes past the
  disc's first — 05:30, 05:50, 12:00, 21:30 — rather than stock's 00:00 06:00 12:00 18:00, so the
  two azimuth tracks are the same sweep shifted by 180° at the same hours and the readback says the
  shading's bearing is the disc's to 0° over the afternoon and 4.6° at its worst, at 05:50. The one
  key the light has and the disc does not is what buys: keying it at 08:45 instead, where the first
  arc did, sat the shading 40° behind the disc by 05:50 and 61° by 06:30 while the sun was climbing.
  Keying the two tracks at different hours is what let each interpolate its own leg, and sv14 did
  exactly that: the shading came from a bearing 180° off the disc at some hours and a few degrees
  off it at others. The night leg — 21:30 back to 05:30 — is the cyclic one and is **flat** here,
  because the light's first and last azimuth keys hold the same 225°: the shading stays north west
  through all of it, which is stock's own night. The disc's own azimuth is the track that cannot be
  made flat with three keys and unwinds from 45° through 101° at midnight to 225° under that horizon;
  the glare is the only thing that reads it.

## sv16: one `.dat` carries six shadow sets of one map, and the clock picks

The sun half of sv15 is finished and the user confirmed it in game: the arc
crosses the sky, the EK 21:00 sunset matches stock, the ground is not over-bright
and the shading's bearing agrees with the sun's. What that leaves is the data
half, and the reason the data half is six sets rather than one is written out
once, because it is the whole point of the format change:

* `MCSH` bodies are **direction-independent**. The fixup repoints a chunk's
  `MCNK` header at a payload; it never rotates one. So a shadow map baked at
  bearing A cannot be made to read as bearing B at runtime — a bearing change is
  a re-bake.
* the arc moves the sun's bearing through the day: 225 at 05:30, 135 at noon,
  45 at 21:30. One baked set therefore agrees with the sun at one moment and
  disagrees with it everywhere else.
* so a day needs one baked set per bearing, and the thing that decides which one
  is live is the same clock that decides the sun's window — which is why the
  `.dat`'s map table grows a clock word instead of the DLL growing a second
  mechanism.

Six presets is what the user bakes: `06:30`, `09:00`, `11:30`, `15:30`, `19:30`
and the stock one stamped `21:30`. Six, not five, because the stock-hours set is
baked for the whole continent rather than left to the client's own `MCSH`: the
custom zones the user bakes are not in the client's `.adt`s, so stock hours need
a baked set too or those zones lose their shadows at night.

### The format, and the pick

Version 2's map record is 32 bytes instead of 28: `char name[24]`, `entryCount`,
and `h0` — where this record's entries take over in the day, half-minutes from
midnight, the same unit `SunHalfMinute` reads the client's clock in (`SUN_HMIN_DAY`
is 2880, so `06:30` is 780 and `21:30` is 2580). A map with several sets is several
records sharing one name, adjacent in the table and strictly ascending in `h0`; the
loader refuses a name that appears in two places, refuses two windows of one name
that overlap, and refuses a window outside a day.

The pick, `PickRunIdx` at the chunk's fixup, right after the name gate:

```
the clock is not read for a one-record map at all
otherwise  the record of the matched name whose h0 is the greatest at or below the clock
           wrapping to the biggest h0 when the clock precedes all of them
```

The wrap is not cosmetic: it is what gives a set stamped at 21:30 the window
`21:30 -> 05:50` across midnight, i.e. what keeps stock shadows through sunset, the
whole night and the pre-dawn hours, and switches to the first baked set at the arc's
own dawn crossing instead. That is the user's rule — "after sunset till dawn it
should use stock preset. Only when closer to sunrise it should pick 6 30 preset" —
implemented as one window that crosses midnight rather than as a special case.

Windows the loader sees are therefore not windows the bake was aimed at: the
bake's stamp says where the sun was when those shadows were cast, the record's
`h0` says where the day starts using them. Both matter and only the second is
machine-checked, so `extract_shadowmaps.py` prints both, as a `windows:` line,
because a set paired with the wrong archive is a silently wrong day and one
printed line is not.

Day tiling the six sets ship in, and the same set of numbers in both roles:

| bake stamp | record `h0` | half-minutes |
|---|---|---|
| `06:30` | `05:50` | 700 |
| `09:00` | `07:45` | 930 |
| `11:30` | `10:15` | 1230 |
| `15:30` | `13:30` | 1620 |
| `19:30` | `17:30` | 2100 |
| `21:30` | `21:30` | 2580 |

The first `h0` is the arc's dawn crossing and the last is the arc's sunset handover
key, both from the shipped arc, not from the stamps; the four in between are midpoints
of neighbouring stamps, which is a guess for the hours between two bakes, and `--open`
is the flag that replaces a guess.

### What was built, and what is guessed in it

A real multi-preset `.dat` now exists in the game folder:
`shadowmaps-6sets-test.dat`, 177,610,696 bytes, version 2, header map word `Azeroth`
(not `MULTI` — one map name, six records), variant `6sets-standin`, `mapCount 6`,
589,793 entries, 333,072 distinct payloads = 170,532,864 payload bytes, and
`124 + 6*32 + 589793*12 + 170532864 == filesize`. It was built out of the full-size
Azeroth bakes that already exist, one archive per `--set`:

```
shadows-Azeroth-morning  06:30      shadows-Azeroth-dusk      15:30
shadows-Azeroth-noon     09:00      shadows-Azeroth-blizzlike 19:30
shadows-Azeroth-day      11:30      patch-S                   21:30
```

**Say what is wrong in it, because it is the point of the file:** those five bakes
were run at Salmiak's old angles, so their bearings do not agree with the arc's sun
at those hours. What this file proves is the *selection* machinery — six sets under
one name, a per-chunk pick against the client's clock, a 162.6 MB arena, a version-2
table the loader accepts — not the look. The `patch-S` set stamped 21:30 is only 174
tiles and 38,257 entries, so at night hours most of Azeroth has no entry at all and
falls back to the client's own `MCSH`; that is the known gap the user's own
whole-continent 21:30 bake closes.

Measured footprint, printed by the loader before it asks for the arena: **162.6 MB
of payload arena plus 4.5 MB of key arrays** for one six-set Azeroth. A six-set
Azeroth plus a six-set Kalimdor is the same order of magnitude — call it ~400 MB,
not the ~192 MB an earlier round of numbers implied, which was one continent's EK-side
figure and never both of them. The payload cap went to `0x30000000` (768 MiB) with an
honest refusal line, so a file too big for a 32-bit client refuses with its MB
counted rather than half-fitting and drawing from a short table.

### What sv16 does not do

* No per-chunk re-pick of the set. The pick happens at the chunk's fixup, so a
  preset changes at the next chunk visit after a window crossing, not mid-chunk.
  Re-picking inside one chunk would mean re-searching per chunk header for a
  difference no one can see; the latch is deliberate.
* No compression of the payload pool. Six sets of one continent is 162 MB of arena
  today, and interning identical bodies already saved 43% of the entries; anything
  more belongs in the file, not in a client that has to decompress per chunk.
* The sun sidecar `shadow-mod.sun` forces sun presets, not shadow sets. A
  shadow-set override would be a second thing to keep in sync with the clock, and
  the clock drives both, so it does not have one.

## sv17: chunks that are already standing get the set the clock picked

sv16's rule — a chunk is picked at its own fixup, so a window crossing changes
the set at the next chunk that loads — says nothing about chunks that are
already loaded, and a first login is exactly that case.

The evidence, in the client's bytes:

* The clock is not known while the world boots. `systems/gametime.md`: the
  clock object is `0xce8538` (0x70 bytes), its minute at `[0xce8538]` and hour
  at `[0xce853c]`, and the day fraction the sun reads is `(minute + 60·hour) ·
  (1/1440)` through `0x6c5f60`/`0x642be0`. Its handlers — `0x6c5de0`, `0x6c5e80`
  (`ReceiveNewTimeSpeed`, the opcode `0x42` packet that carries the time),
  `0x6c6010`, `0x6c6080`, `0x6c6120` — are registered by
  `ClientInitializeGameTime 0x6c5d70` through `0x5ab650`, and the per-frame
  advance is `0x6c5d50` from the client's tick handler `0x401ec0`. An earlier
  round of this work read the clock as `-1`/`-1` during every initial chunk
  load, i.e. the day fraction is 0: at those loads the pick is made by a clock
  that reads midnight, and sv16's wrap rule lands on the record with the
  biggest `h0`, the one stamped at sunset.
* Nothing rebuilds a resident chunk. `systems/terrain/chunk.md` and
  `systems/mapupdate.md`: the map pass re-creates chunks it *destroyed*
  (`0x6984f0`, windows `0x6c1f70`, windows computed by `0x672730`), a forced
  reload is `CWorld::UnloadAreas 0x66fc80(position)` or the rebuild at
  `0x6985d0`, and `CWorld::BeginFrame 0x66fd50` recomputes windows from the
  camera position only. A player standing where the game booted him loads no
  further chunk, so `CMapChunk::Load 0x6af5f0` — the one hook, and the one
  place sv16 picks a set — never runs for his chunks again.
* A resident chunk's shadows can still be replaced. `CreateShadowTexture
  0x6b0030` stores the MCSH address it was handed at `chunk+0x144`; `AcquireChunkTexture
  0x6aed20` builds the texture at `chunk+0x140` whose fill callback `0x6b1180`
  runs `DecodeShadow 0x6b02b0`, which reads `+0x144` again; the combined-shader
  texture at `chunk+0x13c` (`0x6b00a0` → `0x6b02f0` → `0x6b0350`) reads the same
  `+0x144`. So `+0x144` is the one place a chunk's shadow bytes are named, and
  both textures read from it.
* Asking for a re-fill is `GxTexUpdateRect 0x58ac70`. Its full-body signature in
  `bundle/signature.tsv` (line 16419) cleans `ret 0x10`, so four stack dwords,
  and reads only `[ebp+8]`, `[ebp+0xc]`, `[ebp+0x10]`, `[ebp+0x14]`: the
  documented sixth word is never read. The chunk layer's call at `0x6b001e`
  passes `0,0,size,size`; the minimap's at `0x4ecef8` passes four zeros, which
  is the whole texture. Four zeros is what the refill asks.
* `fixup_chunk` is handed `(this, record)` and the chunk's own absolute map
  indices are at `+0x8c` (column) and `+0x90` (row) — `systems/terrain/chunk.md`
  says an area's chunk object lives at `area + 0x278 + index·4` and the chunk's
  `+0x8c`/`+0x90` are the area's base plus the grid index, so the chunk's own
  pair is enough: the area-table deref is not needed, and no area pointer is in
  hand at the hook. `LinkNeighbours 0x6c29c0` reads index −16, +1, +16, −1 with
  the edge arms at 240 + column, −15, +15 and −1, i.e. column and row both
  0..15: a 16×16 grid.

What sv17 does with that:

* `fixup_chunk` keeps four dwords per installed chunk — the chunk, the entry
  key, the map's FIRST record and the record the pick chose — up to
  `REFILL_MAX 8192`, and counts installs past the cap in `g_refill_dropped`.
  The first record is kept because the refill re-picks with
  `PickRunIdx(first_record)`: the load's pick and the refill's cannot disagree
  about which record owns the moment.
* `RefillShadowTextures` re-picks every recorded chunk, bisects its key inside
  the record the clock picked, checks the payload is still an MCSH of
  `FIELD_MCSH_SIZE`, writes the new address at `chunk+0x144` and asks
  `GxTexUpdateRect` with four zeros for the texture the chunk is drawing —
  `+0x13c` when bit 12 of `0xC89F44` or of `0xC9A31C` is set, which is the
  client's own gate at `0x6af71f`, else `+0x140`.
* A chunk is written to only when it still answers as the chunk this run
  installed: `+0x144` inside `[g_arena, g_arena + g_arena_bytes)` — only this
  DLL put an address there — and `+0x8c`/`+0x90` still making the recorded key.
  A destroyed and freed chunk, or a recycled heap block, fails one of those.
* Two triggers, one pass. `RefillKeep` runs from `fixup_chunk`, next to the arc
  and with the same reach, and is one volatile read per chunk once the pass
  happened; the pass fires when the clock stops reading midnight. The other
  half is `RefillWatchdog`, a thread of this DLL started from
  `EnsureInitialized` after the install, which waits up to twenty real minutes
  for the clock to leave midnight and then asks — that is the half that covers
  the player who stands still. `InterlockedCompareExchange` on `g_refill_taken`
  is what keeps the pass to once per session whichever got there first.
* `g_arena_bytes` is set at the install next to `g_arena`, and the pass logs one
  line naming how many chunks were re-filled, how many now draw a different
  set than they were built with, how many have no tile in the picked record,
  how many stopped answering and were left alone, and how many installs were
  past the cap.

What sv17 does not do, and says so:

* The refill cannot take a shadow back. A tile the picked record has no entry
  for keeps the payload the load gave it, which is the previous record's shadow,
  not the client's stock `MCSH`. Only a chunk loaded after the crossing draws
  stock there. The line counts those chunks as `absent`.
* The pass can land between the client's own frames — the watchdog is not on the
  client's thread. The writes are one pointer per chunk plus the client's own
  dirty-rect call, and the chunk currently inside `Load` is never in the record
  list yet, because the record is appended at the end of `fixup_chunk` and the
  refill runs at the start of it.
* A session whose server time really is midnight is indistinguishable from a
  login whose clock is still unknown, so those chunks keep the record the load
  picked. The watchdog's twenty-minute bound is what makes that a visible
  absence rather than a silent one.
* Chunks the client destroyed and rebuilt on its own, and chunks this run never
  installed, are left alone: their shadows are the client's to set.

## sv18: sv17's refill ran and filled nothing, and what its test really compared

The sv17 run in the game folder printed both halves of the pass and they disagree:

```
sv17 shadows: map 'Azeroth' picked set 6 of 6, the window opened at 21:30
  (h0 2580 of 2880 half-minutes), with the client's own clock read as 00:00
fixup: first chunk matched: gy 811 gx 448 key 830912, shadow drawn from arena+26066944
sv17 refill: the clock reads 2391 of 2880 half-minutes ...
sv17 refill: 0 of the 2359 chunks the map load left standing were re-filled at
  this clock: ... 2359 were left alone because the chunk stopped answering as the
  one this DLL installed
```

So the install, the pick, the record list and both triggers are fine — 2359 chunks
were recorded and the watchdog reached the pass at 18453 ms with the clock read as
19:55 — and the pass's liveness test is what rejected all of them. The test and the
load do not agree about the key:

* `fixup_chunk` reads `gx` at `chunk+0x8c` and `gy` at `chunk+0x90` and builds
  `key = (gy << 10) | gx`. The log's own fixup line is the arithmetic:
  `830912 = (811 << 10) | 448`, with `gy 811` the row and `gx 448` the column.
* sv17's test decoded it the other way round: `key >> 10` compared against
  `+0x8c` and `key & 0x3ff` compared against `+0x90`, i.e. row against column and
  column against row. For that first chunk it asks whether `811 == 448`, so it
  answers no, and it answers no for every chunk whose column differs from its row
  — which is every chunk off the diagonal.

sv18's change is that decode plus a way to tell the two halves apart:

* The column comes out of the low 10 bits and the row off the top, the same pair
  `fixup_chunk` read them.
* The one "stopped answering" counter is split into `dead`, a `+0x144` that is
  outside `[g_arena, g_arena + g_arena_bytes)`, and `unown`, a chunk whose own
  `+0x8c`/`+0x90` no longer make the key it was recorded under, and the summary
  line names both separately.
* Each half also gets one line for the first chunk it rejected, printing the chunk,
  the two field values, the recorded key and its decode, and for `dead` the value at
  `+0x144` against the arena's bounds. A refill that fills nothing now says which
  test failed and with what numbers, rather than only that one of them did.

What sv18 still does not do, and the next test has to answer:

* Whether `+0x144` really holds the arena address after a matched load — the fixup
  line proves the load wrote `arena + 26066944` relative to `hdr` into `hdr+0x2c`,
  and `CreateShadowTexture 0x6b0030` is read as storing what it was handed at
  `+0x144`, but nothing has printed that field from a live chunk yet. If the
  `dead` count is the whole 2359 and the `first dead chunk` line prints a `+0x144`
  outside the arena, then that read is not where the chunk's shadow bytes are named
  and the arena half of the test has to go.
* The pass is one burst: every re-fill asks `GxTexUpdateRect` in the same call, so
  2359 textures are marked dirty at once rather than spread over frames. A visible
  hitch at that moment is the pass, not the arc.
* One pass per session, still. The chunks standing at this run's login are moved to
  the record the clock picked when the clock stopped reading midnight; a record that
  becomes current later in the day reaches only the chunks that load after it.

## sv19: sv18's refill found every chunk and then refused all of them for a reason that is not true

sv18 in the game folder answers the question sv18 left open, in one line:

```
sv18 refill: 0 of the 2359 chunks the map load left standing were re-filled at
  this clock: 0 of them now draw a shadow of a different record ..., 2359 have no
  tile in the record the clock picked and keep the shadow the load gave them, 0
  were left alone because their shadow is no longer an address in this arena, 0
  because their own column and row no longer make the key they were recorded under,
  0 were already drawn from the record the clock picked ..., 0 past the cap
```

* `dead` and `unown` are both **0**, so both liveness tests passed for all 2359
  chunks. That settles sv18's open question: after a matched load, `chunk+0x144`
  really does hold an address inside this run's arena — `CreateShadowTexture
  0x6b0030` stores what it was handed, and the load's `hdr+0x2c` displacement is
  the same arena address the fixup line printed (`arena + 26066944`).
* `same` is 0 as expected: the load ran under set 6 (h0 2580, the night's
  window) and the clock at 20:50 picked set 5 (h0 2100, 17:30), so every chunk
  wanted a new record.
* All 2359 went into the one remaining counter, and there are two places that
  counter is bumped: the record's bisection missing the key, and the payload
  sanity test behind it. The .dat the game was reading says the bisection cannot
  have missed: measured against `shadowmaps.dat` itself (134,014,920 bytes,
  `SHDW` v2, one map name `Azeroth`, 6 records of 114234/110857/109970/109865/
  109865/114234 entries at h0 700/930/1230/1620/2100/2580, 669025 entries,
  246067 payloads), the key the log named — 830912, row 811 column 448 — is
  present in **all six** records' key runs, whose keys run 361040..998927 over
  rows 352..975 and columns 352..707. A slice that contains the key, searched by
  the bisection the load also uses, cannot answer "no tile".

So the payload test is what refused all 2359, and it is a test of something that
does not exist. It read the dword at `hdr + FIELD_MCSH_SIZE` and demanded it be
`0x30`, with `hdr` taken as `g_arena + g_offs[a]` — a **payload**. But `FIELD_MCSH_SIZE`
(0x30) is where the **MCNK header** keeps `sizeMCSH`, and the load puts 512 there
(`hdr` in `fixup_chunk` is `record + 8`, the ADT's MCNK header, not a payload). A
payload is the bare 512 shadow bytes the client reads at `hdr + ofsMCSH`: no tag,
no size, nothing to check. Measured on that file, 0 of a 60-payload sample of
each of the six records has `0x30` at payload offset `0x30` — the bytes there are
shadow data, and in the shared all-zero payload they are 0. So the test answered
no for essentially every chunk, and answered it while counting the chunk as
"having no tile", which is why the line said nothing was wrong with any chunk.

sv19's change is narrow and stays inside the pass:

* The payload test is gone. Nothing replaces it: an entry's payload offset was
  already validated when the `.dat` was read — a multiple of 512 and with the
  whole payload inside the payload area, both refused at install otherwise — and
  the load does no content test either. A load and a refill have to agree about
  what a chunk answers, and this is the one place they did not.
* A chunk whose picked record has no entry still goes to `absent`, and that
  counter now means exactly one thing: the record the clock picked has no shadow
  for this tile.
* `notex` is a new counter, and one line for the first chunk that hits it: the
  payload was handed to `chunk+0x144` but `chunk+0x140` (or `+0x13c`) read 0, so
  nothing was marked dirty and the new shadow only shows when the client next
  builds that chunk's texture. It is named apart because it is the one way the
  pass can report "re-filled 2359" and the ground still not change — the same
  reason the shader's field choice is printed once with the two switch values it
  read, so a run says which texture the refill asked for rather than leaving that
  to be inferred from the picture.

What sv19 still leaves, in order of what the next run will tell:

* Whether `GxTexUpdateRect 0x58ac70` on the whole texture really does make the
  client re-fill from the new `+0x144`: if it does, the night's chunks visibly
  move to the 17:30 set at that moment; if it does not, `refilled` is 2359 and
  the ground does not move, and the answer is in the fill callback's read of
  `+0x144` rather than in the pick.
* One pass per session, still. The chunks standing at this run's login are moved
  when the clock stops reading midnight; a record that becomes current later in
  the day reaches only the chunks that load after it.
* One burst: 2359 `GxTexUpdateRect` calls in one call, so a hitch at that
  moment is the pass, not the arc.

## sv20: sv19's pass re-filled 2288 chunks and marked none of them, and its own line said why

The sv19 run answers sv19's open question two lines deep. The summary says the
payload hand-over worked, and the line before it says what was handed to what
field:

```
sv19 refill: the clock reads 2606 of 2880 half-minutes (the day at 9050 of 10000)
  and the .dat carries 6 records in total: every chunk the map load left standing
  is picked again for the record that owns this moment
sv19 refill: the textures it marks dirty are read from chunk+0x140, the other
  candidate is chunk+0x13c, because the shader switch at 0xc89f44 reads 0x1 and
  the one at 0xc9a31c reads 0x1 and bit 12 of of neither is set
sv19 refill first chunk with no texture to mark: this 0x33440008 took its payload
  at arena+0 and its 0x140 reads 0, its other candidate 0x13c reads 0x2f7e9008 …
sv19 refill: 2288 of the 2327 chunks the map load left standing were re-filled at
  this clock: 2288 of them now draw a shadow of a different record …, 39 have no
  tile in the record the clock picked …, 0 dead, 0 unown, 0 same, 2288 took a
  payload but had no texture at the field the shader's state picked …
```

So the pick, the arena, the identity test and the bisection are all fine — 2288
payloads were re-pointed into `chunk+0x144` and the log even printed the client's
own texture pointer, `0x2f7e9008`, sitting at `chunk+0x13c`. What was missing is
the dirty mark: the pass asked for `chunk+0x140`, found 0 there, and counted all
2288 as `notex`.

The test that picked `+0x140` was wrong, and the reference says so plainly:

* `systems/terrain/chunk.md`, `CMapChunk::Load`: "the shadow map when flags bit
  0 is set and world flag `0x40` is on, **the combined texture under either
  shader flag**" — the gate at `0x6af71f`.
* The two globals are bytes, not bit-flags of a dword: `0xc89f44` is `s_terrainShaderFlagA`,
  "Holds 1 when a terrain shader set is in use, which selects the combined chunk
  texture"; `0xc9a31c` is `s_terrainShaderFlag`, "Holds 1 when the terrain pixel
  shaders are in use: combined chunk textures and `_s` textures". Reading the
  dword at either address and asking for bit 12 was invented; the test is the
  byte being 1, and the run printed both bytes as `0x1`.
* `systems/terrain/chunk.md`, "Combined chunk texture": with either flag set a
  chunk gets **one** texture at `+0x13c`, texel `s << 12 | a1 << 8 | a2 << 4 | a3`,
  and `0x6b02f0` "gathers four alpha pointers … and passes them **with +0x144** to
  `0x6b0350`". So the combined texture is filled from the same `+0x144` this refill
  re-points — marking it is exactly what makes the new shadows show, and it is the
  texture this client draws.

sv20's change is inside the pass and has three parts:

* The choice reads the two **bytes** and picks `+0x13c` when either holds 1 —
  here both do, so the pass asks for the combined texture.
* Per chunk, both fields are read and the refill marks **whichever one holds a
  texture**; a chunk whose texture sits at the field the bytes did not pick bumps
  a `mismatch` counter and gets one line, naming the field it actually marked and
  what the bytes read. That fallback is what makes asking the right field not
  depend on reading the switches at all — exactly one of the two fields ever
  holds a chunk texture — and it also covers a player who changed graphics
  between a chunk's load and this pass.
* `notex` now counts only a chunk with **0 at both** fields, which is the one
  remaining way a re-filled pass can leave the ground alone, and its one-shot
  line says both fields' offsets instead of a candidate's value.

What the next run has to show, in order:

* The choice line should now print `chunk+0x13c` first and end with "one of them
  holds 1". If it does, the byte test is confirmed against this client.
* `notex` should be 0. If `mismatch` is 0, the bytes and the chunks agreed and the
  choice alone was enough; if it is 2288, the fallback did the work and says so.
* If `refilled` is 2288, `notex` is 0 and the ground still does not move at that
  moment, the remaining suspect is `GxTexUpdateRect 0x58ac70` and the fill
  callback's read of `+0x144` — sv19 has already ruled the pick out.
* One file note, honest: the `.dat` this run read opens its night record at 21:50
  (h0 2620), not the 21:30 (h0 2580) of the six-set plan, so its tiling came from
  one of the user's own re-stamps rather than the plan's numbers.

## sv21: the mark reached the client and the client faulted in its own code

The sv20 run got all the way through the pass and died inside the client. The
game's report:

```
ERROR #132 (0x85100084) Fatal Exception
Program: G:\Games\twmoa_1181\WoW.exe
0023:00594B9E, referenced memory 0x00000140, could not be written
0xC0000005 (ACCESS_VIOLATION)
```

`0x594b9e` is client code, not the DLL's (the DLL sits at `0x14000000`), and
reading the image says exactly what it is. `0x594b9e` is the third byte of
`c6 06 01` at `0x594b90 + 0xe` — `mov byte ptr [esi], 1` — with `esi = [ebp+8]`
of `0x594b90`. That `0x594b90` was entered by `0x58acd0`, which pushed its own
`ecx`, which `GxTexUpdateRect (0x58ac70)`'s tail handed unchanged from its own
`ecx`, which is the `ecx` at the C call site. The cdecl call the refill used
never set `ecx`, and the value left there was the refill's own address
arithmetic: `FIELD_SHADOW_TEX = 0x140`, which is the "referenced memory" the
report names. So the calling convention and any stack imbalance were never the
problem — the missing texture register was, and the store went to a page that
does not exist.

The client's own call sites, all three of them, say what the shape is. Their
push-1 bytes are at `0x6b007c` in `CreateShadowTexture`, `0x6b00ce` in
`CombinedChunkTexture` and `0x6b0011` in `CMapChunk::UpdateShadow` (0x6aff80,
whose call is at `0x6b001e`), and all three run the same six words:

```
push 1 / push <size> / push <size> / push 0 / xor edx,edx / mov ecx,<texture>
```

Reading the callee back is what makes those words mean the same thing. `0x58ac70`
keeps `[ebp+8]` at rect+0, `edx` at rect+4, `[ebp+0x10]` at rect+8 and
`[ebp+0xc]` at rect+0xc of a local rect at `[ebp-0x10]`, pushes arg 4, and hands
that rect in `edx` plus the unchanged `ecx` to `0x58acd0`, which pushes the flag,
the rect and the texture, loads the device at `0xc0ed38` into `ecx` as `this` and
calls `0x594b90` — whose `ret 0xc` and `0x58acd0`'s `ret 4` keep the stack
balanced, with `GxTexUpdateRect`'s own `ret 0x10` removing the four args at the
top. So `[ebp+8]` there *is* the texture, which is why the first thing it does is
write a byte into it (`0x594b9e: mov byte ptr [esi], 1` — that byte is the
dirty flag), `[ebp+0xc]` is the rect, and `[ebp+0x10]` is arg 4, whose test at
`0x594c23` decides whether the device is told at all: `0x594c27` reads the
device's vtable and does `push <texture> / call dword ptr [edx]`, its slot-0
method, and `je 0x594c2f` skips that whole block. Inside that method the rect is
read at `0x594ba1` (`cmp ebx,ecx / jge 0x594c08` on rect+0 and rect+8) and the
clamped words are stored at `+4`, `+8`, `+0xc`, `+0x10` of the texture through
the `0x5977f0` helper at `0x594be9`.

That last line also retires an idea sv17..sv20 leaned on. The four-zeros form
is real in the client — the minimap at `0x4ecee8` calls `0x58ac70` as
`push 0 ×4 / xor edx,edx / call`, with the texture in `ecx` and gated by a
one-shot `byte [0xbc8308]` — but it answers *no* to the flag test, so the
device is never told, and its `{0,0,0,0}` rect takes the branch that only
resets the texture's own rectangle words. Four zeros can never be the refill's
call. `0x4ecee8` is the only four-zeros call site in the client's shadow path.

The `size` in those three sites is not a constant either: `CombinedChunkTexture`
compares the client's `shadowLevel` option, `0xc63284`, as its very first
instruction, and the texture's side and the two sizes they push are `0x20` when
that dword is exactly 1 and `0x40` otherwise (the setter accepts `atoi` ≤ 1).
So the refill reads the same dword and compares the same way; a client on the
game's default answers 0 and gets 0x40, and a 64-wide rectangle handed to a
client that built 32×32 textures names texels that do not exist.

Why the shape is machine code and not C: MSVC rejects `__thiscall` and
`__fastcall` on a function-pointer typedef, so no declaration can name "texture
in `ecx`, the rect's second word in `edx`". The thunk is 24 bytes emitted onto
the detour's own `PAGE_EXECUTE_READWRITE` page at `STUB_PAGE_THUNK = 0x80`,
past the longest detour table this source has emitted (`stub_sv20` and `stub_sv16`
are 0x40 bytes; `BuildStub` checks the pair against the rung it really emitted,
so a longer detour in a future source is refused rather than overlapped), and
C calls it as a plain cdecl pair `mark_rect(tex, tile)`:

```
8b 4c 24 04   mov  ecx, [esp+4]   = the texture C passed
8b 44 24 08   mov  eax, [esp+8]   = the tile size C passed
6a 01         push 1              = arg4, the flag: 0x594c23 tests it
50            push eax            = arg3, stored at rect+8
50            push eax            = arg2, stored at rect+0xc
6a 00         push 0              = arg1, stored at rect+0
33 d2         xor  edx, edx       = the rect's word at +4
b8 <imm32>    mov  eax, <0x58ac70>   slot GX_RECT_THUNK_SLOT_TARGET = 17
ff d0         call eax
c3            ret                 = back to C, which cleans its 2 args
```

The stack discipline is small and checkable: at the thunk's entry `[esp]` is the
return to C and `[esp+4]/[esp+8]` are C's two args; the four pushes are exactly
what `GxTexUpdateRect`'s own `ret 0x10` removes; the thunk's `ret` returns to C,
whose `add esp, 8` cleans its own pair. Nothing else is promised — cdecl already
says `eax`, `ecx`, `edx` may change, and the callee keeps `ebx`, `esi`, `edi`,
`ebp`. Confirmed in the linked image: the call site is `push [tile] / push esi
/ call dword ptr [esp+0x60] / add esp, 8`.

Two gates had to be added, and each one caught a real defect instead of
confirming a guess.

* **The encoding.** `[esp+4]` cannot be written in three bytes. `rm=100` with a
  displacing `mod` is `[ebp+disp8]` — the *frame* pointer — and the only way to
  make `esp` the base is the SIB byte `0x24` (no index, base `esp`) and then the
  `disp8`. sv21's first draft wrote `8b 4c 04`, which capstone decodes as
  `mov ecx, [esp+eax-0x75]`: a different instruction, a garbage texture, and —
  because the callee's first test is `test ecx,ecx` and its early-out is a quiet
  `ret 0x10` — a thunk that *returned quietly*, which is a wrong answer that
  looks like a pass. So `verify_dll.py` disassembles the table out of the linked
  image and requires `mov ecx,[esp+4]`, `mov eax,[esp+8]`, four pushes whose
  values include `1` and `0`, one `call eax`, a terminating `ret`, and a zeroed
  `mov` imm32 with `0xB8` in front at slot 17.
* **The table was not in the image at all.** `GX_RECT_THUNK_BYTES` is 24
  constant and `gx_rect_thunk` is `const`, so `/O2` turns
  `memcpy(page + 0x80, gx_rect_thunk, 24)` into immediate stores and never takes
  the array's address: the array has no home in `.rdata`, and the byte-for-byte
  gate printed "the 24-byte thunk table is NOT in any section of this image". The
  copy length is a `volatile` local for exactly that reason, which makes the copy
  a real `memcpy` out of the table — the shipped bytes *are* mod.c's table — and
  `PatchHook` re-reads the page after the install: the table's prefix bytes, the
  client address in its imm32, the `ff d0` and the trailing `c3`, all must be
  where `BuildStub` says. A thunk that is not on the page means the refill would
  call whatever else is there.

What the next run has to show, in order:

* The refill's new line should print the tile it marked with and what
  `0xc63284` read: a client on defaults answers `0` and `64 by 64`.
* No `0x594b9e` report. If the marks are now accepted, `refilled` keeps its
  sv20 number (2288 of 2327 for that `.dat`) and the ground changes at the
  record boundary the clock crossed.
* If the pass still reports marks and the ground does not move, the next thing to
  read is the fill callback (`0x6b1150`/`0x6b1180`/`0x6b11b0` through `0x6aed20`
  with `push 2 / push 2`) and whether it re-reads `+0x144` on the *refill* path
  or only on a fresh build — that is the one edge sv19's and sv20's evidence did
  not settle.
* One file note still owed: the user's real six-set `.dat` opens its night
  record at 21:50, not the plan's 21:30, so the tiling of that file came from one
  of the user's own re-stamps.
