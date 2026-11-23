# Can the sun actually travel, and does the lighting follow it?

Two questions were put to four references:

1. **Can the sun's azimuth be animated** — a real rising/setting arc across the sky — or is it
   welded to one bearing for the whole day?
2. **Does terrain and object lighting follow the runtime sun**, or is it locked to a fixed ~45°
   direction that only the *sky art* moves?

The point is to decide whether the `.dat` shadow pipeline can ship morning / noon / blizzlike /
evening presets that come with matching sun motion and matching lighting direction.

**This document is analysis only.** Nothing in `src/mod.c`, `shadow_dat.h`, `README.md` or the
build was touched. No commit, no push.

---

## Verdict

| Question | Answer | Confidence |
| --- | --- | --- |
| Sun azimuth animatable? | **Yes.** The azimuth lives in two writable static key tables in `WoW.exe`'s data section, and the evaluator reads them every frame. The vanilla content of those tables happens to be a single constant (225° for the light, 45° for the drawn disc) — that is *data*, not code. Overwrite the four or three value slots and the bearing moves. | high, from 1.12.1 disassembly (`benilla-re`) |
| Lighting follows the runtime sun? | **Yes.** Terrain and exterior objects are lit from a directional light whose direction is written every frame straight out of the sun-direction state. There is **no separate 45° lighting lock** in 1.12.1. The 45° you keep seeing is (a) the sun *disc*'s azimuth constant and (b) a fixed interior vector `(−0.30822, −0.30822, −0.9)` used only for interiors. | high |
| Baked terrain shadow (`MCSH`) follows the sun? | **No — it cannot.** `MCSH` is a pure bitmap: the client decodes 512 bits into a 64×64 (or 32×32) black/white texture and modulates the terrain by it. No light vector is involved. Its bearing exists only in the bytes someone authored, so a preset that changes the sun's bearing **must re-bake the shadow set**. | high |

So: a preset = *(authored `MCSH` set, sun key set)* shipped as a pair. The `.dat` half you already
have; the sun half is **7 value slots** overwritten in `WoW.exe`'s data section — the 4 light-azimuth
keys at `0xce8d90` plus the 3 disc-azimuth keys at `0xce8d4c`, which is all a new bearing needs. Add
the 4 polar keys at `0xce8db0` (8 slots total) when the preset also wants a different shadow length.

---

## Evidence base

The DLL hooks the 1.12.1 client (`WoW.exe`, build 5875), so that reference is the one that
settles the answer:

| Reference | Role |
| --- | --- |
| `I:\code\wow\benilla\benilla-re` | **primary.** The 1.12.1 client read at its bytes, cited by address, marked `verified`/`proof one`. Everything load-bearing below is from here. |
| `I:\code\wow\wowdecomp` | WotLK 3.3.5a client. Corroboration: same design, named symbols. |
| `I:\code\wow\wowdecomp_alpha` | Alpha 0.5.3 client. Same numeric tables as retail — shows the model is old and stable, and shows what 1.12.1 *kept* when retail gained a baked shadow map. |
| `I:\code\wow\misc\northlight` | Not a 1.12.1 mod: a D3D9 proxy for 3.3.5a. Design ideas worth borrowing, no 1.12.1 addresses. |

Where a claim is not from 1.12.1 itself, it is labelled.

---

## 1. The 1.12.1 sun is two separate things

`benilla-re → systems/lighting/celestial.md`. The day-night file keeps **two independent** angular
models, driven by two independent key tables:

| What | Direction model | Azimuth track | Polar track |
| --- | --- | --- | --- |
| the **light** (what shades) | `(cos a·sin p, sin a·sin p, cos p)` | `0xce8d90` `s_sunAzimuthTrack` | `0xce8db0` `s_sunPolarTrack` |
| the **sun disc** (what you see) | same formula, own tracks | `0xce8d4c` `s_sunDiscAzimuthTrack` | `0xce8d64` `s_sunDiscPolarTrack` |

Vanilla contents (angles are π × a constant, shown in degrees; times are clock times):

| track | VA | type | keys |
| --- | --- | --- | --- |
| light azimuth | `0xce8d90` | `float[8]` = 4 (time, value) | 00:00, 06:00, 12:00, 18:00 — all **225°** (1.25π) |
| light polar | `0xce8db0` | `float[8]` = 4 keys | 00:00 **127°**, 06:00 **110°**, 12:00 **127°**, 18:00 **110°** |
| disc azimuth | `0xce8d4c` | `float[6]` = 3 keys | 05:30, 12:00, 21:30 — all **45°** (0.25π) |
| disc polar | `0xce8d64` | `float[10]` = 5 keys | 05:30 **100°**, 11:55 **5°**, 12:00 **5°**, 12:05 **5°**, 21:30 **100°** |
| disc size | `0xce8cac` | `float[8]` = 4 keys | 06:00 2, 06:45 1, 20:15 1, 21:00 2 |

Compass, spelled out because it is not the intuitive one (`README.md:137-144`): on a north-up map
the client's azimuth runs **clockwise** from +X to +Y, so 0° = west, 90° = north, 180° = east,
270° = south, and +X itself is west. **45° = north-west**, **225° = south-east**.

### 1.1 It is one constant, but the constant is data

`0x6cf6c0(keys, count, phase)` in `ecx`, `edx` and the stack, `ret 4`, value in `st0`
(`celestial.md:12-29`):

* `phase` = the time of day, state `+0x04` in [0, 1) (`lighting/daynight.md:21`), clamped to [0, 1],
  NaN as 1;
* first key *i* with `phase ≤ time[i]`, and the key *j* before it, **cyclically**;
* `|time[i] − time[j]| < 0.001` → `value[j]`;
* otherwise `value[j] + (value[i] − value[j])·e/d`, computed at 53 bits.

The arrays at `0xce8d90`… are globals — writable statics, not `.rdata`. Their times and angles are
built from literals (`0x811510`–`0x811528` and multiples of π at `0x40490fdb`, `celestial.md:56-61`)
by static initializers `0x6ce4a0`/`0x6ce560` or at first use behind guard bits inside `0x6d3a10`
and `0x6d3b80`. `0x6d2210` calls `0x6d4500`, `0x6d2260`, `0x6d3a10` (`daynight.md:347`).

**Consequence:** writing the value slots changes the bearing of the light for the whole client —
terrain, objects, entities, the disc, the glare, all together, because they all read the same
tracks. Nothing in the code path derives 225° from anywhere else.

### 1.2 The two models are not the same vector, and that is a feature

The light's polar is not the disc polar's antipode:

| clock | disc polar | disc elevation | light polar | light's implied sun elevation |
| --- | --- | --- | --- | --- |
| 06:00 | 92.6° (interpolated between 05:30 100° and 11:55 5°) | **−2.6°** (just under the horizon) | 110° | **20°** |
| 12:00 | 5° | **85°** (near zenith) | 127° | **37°** |
| 18:00 | ~92.6° | **−2.6°** | 110° | **20°** |

Read the light polar as `90° + elevation`, because the vector is the direction **light travels**
(`celestial.md:65-67`: "p stays between 110° and 127°, so the direction always points down; it is
the direction light travels"). So at noon the client *draws* the sun at 85° but *lights* as if it
were at 37°. That flattening is deliberate: it keeps shadow lengths bounded while the disc is free
to go to the horizon. It is also what the bake reads — the README's preset heights (19.7° at
Morning, 37.3° at Noon, `README.md:124-127`) are the **light's** implied elevation, not the disc's.

### 1.3 What the azimuth being constant actually costs you

With azimuth constant at 45° and only the polar animating, the disc dips below the horizon **on the
same side** it rose from: it rises in the north-west and sets in the north-west. Vanilla has
exactly one low-sun shape — `README.md:126` "Evening *is* Morning" and `README.md:133-135` "it is
the *bearing* that never changes across a vanilla day — only the height does". That asymmetry is
the whole thing an arc preset buys you.

---

## 2. The chain: sun track → pixel

Everything below is 1.12.1, with addresses.

```
   time of day, light state +0x04  (state block from 0xce9b60; 0x6d48b0 returns its address)
                 │
       0x6cf6c0(keys, count, phase)          the one track evaluator
                 │
   ┌─────────────┴──────────────────────────────┐
   │ azimuth 0xce8d90, polar 0xce8db0           │ azimuth 0xce8d4c, polar 0xce8d64, size 0xce8cac
   ▼                                            ▼
 0x6d3b12–0x6d3b4f  sun direction          0x6d3fda / 0x6d3fed / 0x6d4067
 (cos a·sin p, sin a·sin p, cos p)          disc sits 12 units from the eye
 → light state +0x168, +0x16c, +0x170                │
   │                                                 ▼
   │                                          sky dome draws sunCenter.blp,
   │                                          moon02, stars, glares
   ▼
 0x66ff60 "World light commit" (lighting/fog.md:113-133)
   dims ambient +0x178 and diffuse +0x174 by the sun glare,
   sets the direction of [0xc89ef0] + 0x7c from +0x168   (store at 0x6700a3)
   writes +0x30 ← +0x178, +0x3c ← +0x174, +0x48 ← +0xcc  (0x6700a8, 0x6700f4, 0x67014e)
   │
   ▼
 0xc89ef0 = s_globalMapLight, "the light object the map creates for the sun"
   its light record at +0x7c: direction +0x24, ambient +0x30, colour +0x3c
   (absolute: direction 0xc89ef0+0xa0, day colours 0xc89ef0+0xac/+0xb0/+0xb4)
   │
   ├──────────────────────────► TERRAIN
   │                            0x684510 chunk draw pass (terrain/render.md:113-116):
   │                              lights picked once per chunk, at the centre of its box
   │                              (+0x5c) with radius +0x68 (23.570166 on flat ground);
   │                            0x6af9f0 adds the global exterior light block 0xc89ef0+0x7c,
   │                            then the chunk's own map lights, for the 4 nearest;
   │                            0x6847bb commits the device lights relative to the camera 0xc7cf20;
   │                            vertices are format 1, mask 0x09 = position + normal,
   │                            normals = MCNR (terrain/chunk.md:105-115) × (1/127) at +0x170;
   │                            render state 0xe (lighting) set around the layer draw
   │                            at 0x6bf3a9 / 0x6bf4fc (terrain/render.md:70),
   │                            disabled by layer flag bit 7.
   │
   ├──────────────────────────► OBJECTS (doodad defs)
   │                            0x6a8050 (terrain/lights.md:38-53):
   │                              +0xc bit 2 (lit as an interior) → global direction 0xca816c;
   │                              otherwise → the block's direction, ambient +0x30,
   │                              colour +0x3c times the def's sun scale +0xa4.
   │
   └──────────────────────────► ENTITIES
                                0x6a7300 (terrain/lights.md:12-36):
                                  +0xc bit 4 (lit by the sun) → block direction;
                                  otherwise → global 0xca816c,
                                  or with +0x90 bit 0x800: global + (sun − global)·(+0xa3/255),
                                  normalized (fsqrt / fdivr 1.0 at 0x6a74fa).
                                the sun scale +0xa4 ramps at 3.3333333 × dt (0x810808)
                                toward its target +0xf8: 2.5 lit, 0.5 in the chunk's shadow
                                (0x810a64), 1.0 interior (0x810a68) — mapentities.md:95,114
                                and the shadow bit comes from sampling the chunk's MCSH at
                                0x69b350 → sets bit 0x10 at 0x69e48d/0x69e49b.
                                the day ambient itself is [0xc89ef0]+0xac..+0xb4
                                (mapentities.md:133,149).
```

The `MCSH` branch of the same chain, for completeness:

```
   MCNK header +0x2c → MCSH sub-record → chunk field +0xf1c → address stored at +0x144
   gate: MCNK flags bit 0 (0x6af6ea test [eax],1) AND world flag 0x40 at 0xc7b2a4 (0x6af6ef)
        otherwise +0x144 and +0x148 are zeroed
   decode: 512 bytes of bits, least significant first
     0x6b0d00 / 0x6b0d30 / 0x6b0da0 under the dispatcher 0x6b0030 (terrain/chunk.md:164-181)
      clear bit → texel 0x0000, set bit → 0xffff: bit n is byte n>>3 masked by
      0x810bb4 (1 << k) and shifted by 0x810bd4 (k = n & 7), the texel being the word at
      0x86b590 it indexes (chunk.md:166-180); 64×64 when shadowLevel is 0, and on the
      0, 2, …, 60, 63 samples 32×32 otherwise, the texture itself 32 square only when
      the value is exactly 1, so a negative shadowLevel leaves a 64×64 texture holding
      the 32×32 walk (chunk.md:124-125, :169); MCNK flag bit 15 does not change the
      shadow walk, and the texture is +0x140 with the address kept at +0x144
   with a shader flag instead: one combined texture +0x13c packing
     texel = s<<12 | a1<<8 | a2<<4 | a3, shadow nibble 0xf for a clear bit, 0 for a set one
   (terrain/chunk.md:185-202; the same bytes feed the layer's alpha maps at +0x128 / count +0x138)
```

**No light vector enters that second block.** The bearing of a baked shadow exists only in which
bits were set. The client only ever *reads* them — which is exactly what the project exploits:
`README.md:475` mode `f` writes `flags |= 1`, `hdr+0x2c = arena buffer − hdr`, `hdr+0x30 = 512`,
so "the client's own gate, `CreateShadowTexture` and the decoders read live `.dat` bytes".

---

## 3. Where a fixed direction *does* appear — and where it does not

| Apparent "45° lock" | Reality |
| --- | --- |
| `0xce8d4c` disc azimuth = 45° at 3 keys | the drawn **sun's bearing**, a table entry. Animate it and the sun visibly travels. Nothing to do with shading. |
| `0xca816c` `s_globalLightDirection` (`ghidra/symbols/globals.tsv:4209`, `float[3]`, verified), stored by `0x6a72b0 InitTerrainLightDirection` = **(−0.30822, −0.30822, −0.9)** — the signature `c7 05 ?? ?? ?? ?? 03 cf 9d be` is the `mov` of that first float | the **interior / global map light**. Used only when an entity is not sun-lit (`0x6a7300`'s else-branch) or a def is interior-lit (`0x6a8050`'s `+0xc` bit 2), and as the starting point of the `+0xa3` blend. Its diagonal-in-xz shape *is* a 45°-looking vector, which is why it keeps being blamed for the look. It is written once, statically: `0x6a7413` only *loads* it into the scratch triple at `0xca8174`, and the blend and the normalization are "kept in the register" to `0x6a7518` (`lights.md:29-30`), so nothing writes the global at runtime. WotLK names the same triple `s_interiorSunDir` (`wowdecomp …/MapEntity.cpp:32`) — same constant, different label. |
| baked `MCSH` bearing ≈ 45° | authored data. Not a lock either: re-bake it. |

Nothing in the terrain or object lighting path has a hard-coded direction. The direction is always
either `[0xc89ef0]+0xa0` (runtime sun) or `0xca816c` (interior), chosen by `+0xc` bit 4 / `+0xc`
bit 2 / `+0x90` bit 0x800.

The strongest single proof that lighting tracks the runtime sun is the interior-light bake:
`terrain/lights.md:88-96` (`0x6a8410` for a def, `0x69e4c0` for an entity) raycasts down from the
object; when the hit face's flag byte says **sky-open** (selector bit 0, made at `0x6b9d34 and dl,1`),
then

> "the day light's colours (`+0x174`, `+0x178`) replace both words, `+0xa3` becomes `0xff` and
> `+0x90` bit 0x800 is set"

so the blend weight becomes 255/255 = 1, and the entity's light direction is
`global + (sun − global)·1` = **the runtime sun direction**, renormalized. Objects the client
decides are under open sky are lit by the sun it computed that frame.

---

## 4. How to animate it, and how to ship a preset

Three shapes: **A** writes the keys once per variant, **B** writes them every frame, **C** re-times
the existing keys so the client's own interpolation carries the motion. For this project's
per-variant `.dat` design, A is the fit for a static preset and C is the fit for a traveling sun;
B is only needed if you want a sun tied to something other than the client's own clock.

### 4.1 Shape A — overwrite the tables once per preset (static sun)

At load, when the `.dat`'s map/variant is chosen (`README.md:474`: `map 'Azeroth' variant
'shadows-Azeroth-morning'`), write the preset's key floats over `0xce8d90`, `0xce8db0`,
`0xce8d4c`, `0xce8d64`. The sun then sits where the preset says, for every hour — same look as the
`MCSH` set you shipped with it. No animation, no per-frame cost, no new code path in the client.
This is the low-risk rung.

### 4.2 Shape B — write per frame (real sun motion)

Hook the day-night update (the tracks are evaluated through `0x6cf6c0` each frame) or simply write
the value slots from your own clock each frame, then let the client's own evaluator interpolate.
Better and cheaper: **keep the client's tables but re-time the keys**, i.e. rewrite the *time* slots
too, so the client's own cyclic interpolation does the animation. Nothing per-frame, and the sun
still travels. See 4.4 (Shape C).

### 4.3 Patch timing — the guard bits

`0x6d3a10` and `0x6d3b80` build their tracks "at first use behind guard bits"; `0x6ce4a0`/`0x6ce560`
are static initializers (`celestial.md:60`). So the key floats may be zero (or placeholder) until
that first build runs. A one-shot patch must go **after** the day-night init, not at `Load`. The
robust one-shot site is the same place the project already runs after the client has a live chunk
and a live world (`src/mod.c`'s detour runs on chunk loads), or re-assert the write on every
`0x6cf6c0` call. A per-frame write is immune to this and needs no ordering argument.

### 4.4 Shape C — a real arc, by re-timing the existing keys

The key **count** is a parameter to `0x6cf6c0` (in `edx`), so growing a table means patching that
literal as well as writing — and `0xce8d90` (`float[8]`) is immediately followed by `0xce8db0`, so
there is no room to grow in place. Work with 4 light keys, 3 disc-azimuth keys, 5 disc-polar keys.

The disc model is `(cos a·sin p, sin a·sin p, cos p)` with `p` the polar from +Z, so
`p = 90° − elevation`. A sun that travels is a great-circle arc: its bearing sweeps exactly **180°**
from rise to set, and the noon bearing is midway between the two horizon bearings. So pick the
noon bearing first — the shipped `MCSH` presets are all at 45° (north-west), so keep noon at 45°:

| clock | sun bearing | light azimuth (bearing + 180°) |
| --- | --- | --- |
| dawn 05:30 | 315° (south-west) | 135° (light travels north-east… see note) |
| noon 12:00 | 45° (north-west) | **225°** — matches vanilla |
| dusk 20:30 | 135° (north-east) | 315° |

Note on the light azimuth: the light azimuth is the *travel* direction, so it is the antipode of
the sun's bearing. Dawn sun over the south-west ⇒ light travels toward the north-east (135°), and
the shadows run north-east. Dusk sun over the north-east ⇒ light travels south-west (315°). Noon
stays at vanilla's 225°.

Write the azimuth keys **monotonically** — the evaluator lerps in value space, so 315° → 495° (not
315° → 135°) or the sweep goes the way it came. `fsin`/`fcos` are periodic, so out-of-range angles
are fine: `0x6d3b25`–`0x6d3b4f` feed the raw float to `fsin`/`fcos` (`celestial.md:74-77`).

Suggested key sets, one arc preset ("Morning arc"). Values are π multiples in radians.

| track | time | value | radians |
| --- | --- | --- | --- |
| `0xce8d90` light azimuth (4) | 0.0000000 | 315° | `5.4977871` |
| | 0.2291667 (05:30) | 135° | `2.3561945` |
| | 0.5000000 (12:00) | 225° | `3.9269908` |
| | 0.8541667 (20:30) | 315° | `5.4977871` |
| `0xce8db0` light polar (4) | 0.0000000 | 110° | `1.9198623` |
| | 0.2291667 | 96° | `1.6755161` |
| | 0.5000000 | 127° | `2.2165682` |
| | 0.8541667 | 96° | `1.6755161` |
| `0xce8d4c` disc azimuth (3) | 0.2291667 | 315° | `5.4977871` |
| | 0.5000000 | 405° | `7.0685835` |
| | 0.8958333 (21:30) | 495° | `8.6393797` |
| `0xce8d64` disc polar (5) | 0.2291667 | 90° | `1.5707963` |
| | 0.4965278 (11:55) | 5° | `0.0872665` |
| | 0.5000000 | 5° | `0.0872665` |
| | 0.5034722 (12:05) | 5° | `0.0872665` |
| | 0.8958333 | 90° | `1.5707963` |
| `0xce8cac` disc size (4) | keep vanilla | | |

Two things to know about this table:

* **The night sweeps back.** Four keys with cyclic interpolation cannot hold a bearing still across
  midnight *and* sweep 180° across the day. The set above holds 315° from 20:30 to 24:00 and then
  unwinds during 00:00–05:30, which is as much of the night as 4 keys can cover. With 8 slots you
  cannot add a key without overrunning into `0xce8db0`; either patch the `count` literal in `edx` at
  the call site and place the extra keys in adjacent free globals, or accept the night unwind, or
  run one preset per variant (4.1/4.5) instead of animating across midnight.
* **96° is not a hard floor here.** Other engines clamp the light elevation (northlight:
  `celestial_context.h:145-153 MinLightElevation = .1045f` = sin 6°, justified as "a grazing light
  would stretch shadows to infinity"). In 1.12.1 the terrain shadow is *baked*, so a grazing light
  stretches no dynamic shadow — the clamp only protects a dynamic-shadow receiver, which 1.12.1
  does not have. The `96°` above is an aesthetic floor (very long shadows); vanilla's own floor is
  110° (20° elevation, shadows ~2.75× object height). Pick 100–110° if you want the vanilla feel.
* **Most of those bytes are already in the client.** `celestial.md:56-59` lists the literals the
   builders use: the times 0.2291667 (05:30) and 0.8958333 (21:30) at `0x811510` to `0x811528`, the
   offsets 0.0034722 (5 min) that make the 11:55/12:05 pair, and the angle constants 0.25, 0.75,
   0.6111111, 0.7055556 times π at `0x40490fdb`. Only 1.75π (315°) and the 2.25π/2.75π
   monotone forms are new numbers, and those are written as raw floats anyway. So the patch is
   byte-level, not arithmetic-level: it re-states what the client already knows. The adjacency is
   exact, `0xce8d90` + 0x20 = `0xce8db0`, `0xce8d4c` + 0x18 = `0xce8d64`, `0xce8d64` + 0x28 =
   `0xce8d8c`, and the next used global after that gap is `0xce8d90` again, which is where the
   "no room to grow in place" line above comes from.

### 4.4.1 What sv15 actually wrote, and where the sketch above changed

The sketch is the design record; sv15 wrote something narrower, and an in-game
visit is what made it narrow. Three things the sketch assumed were wrong:

* **the 180° sweep with new polar keys was not taken.** sv14 moved the light
  polar down to 80°, under 90°, and in game the ground came out mirrored and
  over-bright: past 90° is where the light direction stops pointing down, and a
  light direction pointing up makes the diffuse term of every up-facing normal
  zero, so the shading reads as reflected. The fix that holds is the simple one:
  **keep the polar keys stock's own, unchanged.** Then the day's heights, its
  rise at 06:10 and its set at 20:31 and its strength are stock's hour for hour,
  and only the azimuth moves — which is also the thing the player asked for, an
  evening that matches stock.
* **the sweep is a quarter turn, not a half one.** With stock's hours and
  heights, a sun that rises south-west (315° at 05:30) and sets north-west
  (405° at 21:30) passes **due west** at noon (360°), not north-west: a
  90° sweep across the day. That keeps vanilla's north-west at dusk, which is
  where the shipped `MCSH` presets are and where an evening has to match stock.
* **one arc, not one window per hour.** The four tracks are one global block
  that the day-night file builds once behind the guard bits at `0x6d3a10` and
  `0x6d3b80`; five windows holding five different arcs were never one sun
  crossing one sky, and the visits showed the disc, the ground and the objects
  disagreeing about where the light was. sv15 has two windows: the arc from
  00:00 to 20:00 and **stock written back** from 20:00 to 24:00 — writing the
  stock keys, not nothing, because writing nothing is what let a morning arc
  survive into the evening.

What it writes, `arc-swnw` (0° = west, 90° = north, clockwise; polar from the sky's
top), in the client's own key counts 4 / 4 / 3 / 5:

| track | time | value |
| --- | --- | --- |
| `0xce8d90` light azimuth (4) | 0.2291667 (05:30) | 495° |
| | 0.3645833 (08:45) | 517.5° |
| | 0.5000000 (12:00) | 540° |
| | 0.8958333 (21:30) | 585° |
| `0xce8db0` light polar (4) | 0.0 0.25 0.5 0.75 | 127° 110° 127° 110° — stock's own |
| `0xce8d4c` disc azimuth (3) | 0.2291667 0.5 0.8958333 | 315° 360° 405° |
| `0xce8d64` disc polar (5) | 0.2291667 0.4965278 0.5 0.5034722 0.8958333 | 100° 5° 5° 5° 100° — stock's own, at stock's own times |

The light azimuth is the disc azimuth plus 180° **at the disc's own key times**
(495° at 05:30, 540° at noon, 585° at 21:30) plus their midpoint at 08:45, and
stock's own light key times (00:00, 06:00, 12:00, 18:00) cannot hold that arc:
sampled there the disc gives 557°, 499°, 540° and 571°, so the first key is
*above* the second and the leg from midnight to six runs backwards 58° —
`0x6cf6c0` lerps in value space, so a descending pair unwinds the morning sweep
and the shading comes from the far side of the disc through the small hours, the
"180° off" symptom. Sampling the disc at its own three keys plus their midpoint
keeps the four values ascending, which is what makes the one sweep continuous.

Two corrections to earlier claims worth keeping, because they were both found by
reading the bytes rather than by believing them:

* sv14's stock table had the disc polar's noon trio at three keys all at 0.5;
  the client builds them at 11:55, 12:00 and 12:05 (`celestial.md:41`, the ±0.0034722
  offsets at `0x811510`), and a table with three keys at noon matches neither
  stock's own tracks nor an all-zero block, so `SunPin` refuses and the arc is
  never written on a client that has built its day-night file. sv15 carries the
  real times.
* the azimuth is left **unwrapped** (495°, not 135°) because `0x6cf6c0` lerps in
  value space and wraps only in time: a wrapped value is a backwards sweep and a
  gap is a jump at midnight. The client's `fsin`/`fcos` read either spelling the
  same.

### 4.5 Preset table — what to ship

A preset is a pair: the `.dat` shadow set (already the project's job) plus the sun key set.
The bake's bearing slider must equal the preset's sun bearing at the sampled hour, and its height
must equal `light polar − 90°`. `README.md:124-128` already lists the vanilla pairs; here they are
with the sun-key side and the new arc rows filled in.

| Preset | sun bearing | light polar | bake pair (bearing / height) | vanilla? |
| --- | --- | --- | --- | --- |
| Blizzlike — stock | 45° | 127° (midnight/noon), 110° (06/18) | `29 / 33` → 45.5°, 47.2° | yes, a no-op-looking calibration variant |
| Noon | 45° | 127° | `34 / 43` → 44.7°, 37.3° | yes |
| Morning | 45° | 110° | `42 / 63` → 45.3°, 19.7° | yes, the set the shipped `.dat` came from |
| Evening | 45° | 110° | `42 / 63` | identical to Morning in vanilla (`README.md:126`) |
| Night — white moon | 45° | 35° polar ⇒ the moon's own track | `24 / 26` → 45.4°, 55.2° | vanilla ships one static map for every hour (`README.md:128`) |
| **Arc, sampled 06:00** | 318° (south west) | 110° (20°) | re-bake at bearing ≈ 318°, height ≈ 20° | new |
| **Arc, sampled 12:00** | 360° (due west) | 127° (37°) | re-bake at bearing ≈ 360°, height ≈ 37° | new |
| **Arc, sampled 18:00** | 388° (north west) | 110° (20°) | re-bake at bearing ≈ 388°, height ≈ 20° | new |
| **Arc, sampled 21:00** | 403° (north west) | stock's own — this hour is `arc-stock`, stock written back | `42 / 63` unchanged | the vanilla evening |

The arc rows are the *sampled* hour: one arc crosses the sky, so one baked
`MCSH` set cannot follow it, and the honest pair is per hour you bake for. That
re-bake is the data half of the same row every other arc row names — Salmiak's
bake is at a fixed 45°, and re-baking is out of scope for this change, which is
why the sun test runs with stock shadow payloads.

The payoff worth selling: with the arc, Morning and Evening become genuinely different payloads
(shadows run NE vs SW) instead of the same bytes twice, which is exactly the difference `README.md:126`
says vanilla cannot make.

---

## 5. Risks and gotchas

| # | Risk | Note |
| --- | --- | --- |
| 1 | **Track floats are writable statics, not `.rdata`** | Good for patching, but it means a preset that patches them and a preset that does not can disagree with the same `.dat`. Keep them in sync per variant. |
| 2 | **Guard bits** | Values exist only after the first day-night build (`0x6d3a10`, `0x6d3b80`). Patch after init, or re-assert per frame. |
| 3 | **Bearing consistency** | Keep `light azimuth = disc azimuth + 180°`, otherwise the sun disc and the shading disagree visibly (an object lit from one side, its shadow cast from another). |
| 4 | **Monotone azimuth** | Lerp happens in value space; write a monotone sequence through the wrap. |
| 5 | **Key count** | 4 / 4 / 3 / 5 keys. `0x6ce4a0`-built tables are adjacent, so no in-place growth; patch the `count` argument in `edx` if you must add keys. |
| 6 | **Re-baking `MCSH`** | Mandatory if the bearing moves. The bake is external (Salmiak); the `MCSH` payload is the thing being swapped, so this is a data-parallel task, not a code task. `README.md:146-148` "Run one variant end to end at a time: a fresh whole-map bake drops the packed bits the previous walk left in memory" — the bake state is already per-variant discipline. |
| 7 | **Chunk coverage** | A morning set covers 487 of Azeroth's tiles (`README.md:85`); chunks the `.dat` does not cover keep stock `MCSH` (`README.md:810`) — with an animated azimuth, those uncovered chunks keep *stock* shadow bearings while the sun travels. Either cover the whole continent or expect a mixed field. |
| 8 | **x87 transcendentals** | `fsin`/`fcos` are approximated by CPU (`benilla-re docs/METHOD.md`); benign, but it means a difftest against the client must match on the same instruction, not on an exact math library. |
| 9 | **The `+0xa4` / `+0xf8` entity ramp** | Entity sun scale chases its target at 3.3333333 × dt, so an entity walked from sun into shadow takes ~0.6 s to settle. Do not expect an instant flip when the sun moves. |
| 10 | **Interior-lit surfaces never follow the sun** | Only the *sky-open* branch (selector bit 0) gets the runtime direction; surfaces under a group keep baked vertex-colour lighting from `0xca816c`. An arc preset changes exteriors only. |

---

## Appendix A. WotLK 3.3.5a (`wowdecomp`) — the same model, named

`DayNight::SetDirection` `0x007EEA90` (`WoW/Source/WorldClient/DayNight.cpp:500-510`) with
`0x007ED3B0` interpolation:

```cpp
const float s_azimuthTable[5]   = {3.926991f, 3.926991f, 3.926991f, 3.926991f, 3.926991f};
const float s_elevationTable[5] = {3.926991f /*unused idx0*/,
                                   2.2165682f /*midnight 00:00*/, 1.9198623f /*dawn 06:00*/,
                                   2.2165682f /*noon 12:00*/,     1.9198623f /*dusk 18:00*/};
const float s_theta = PI * 1.25f;   // sun azimuth, constant
```

Same numbers as 1.12.1's tables — same design, retail and WotLK. `sunMoonPath` is **scalar phase
only** in both: `DayNight::SetPlanets` `0x007EE756`/`0x007EE774`/`0x007EE84A` (`DayNight.cpp:485-493`)
positions bodies on the shared `m_dir`; `s_moonTheta = PI/4` and `s_moonElevation[5]` are one
constant azimuth per body, exactly as 1.12.1's `0xce8d4c`/`0xce8d0c` are one constant.

WotLK's terrain sun direction: `CMapRenderChunk::m_sunDir` at `+0x180` (`MapRenderChunk.cpp`), set
from `CWorld::UpdateDayNight` → `CMap::sunLight` → `CMapChunk::SelectLights()`, same chain shape as
1.12.1's `0xc89ef0+0xa0` → `0x6af9f0`. Objects: `DetailDoodad.cpp` `+0x00A0`, `M2Light.cpp::SetupSunlight`
`0x00835280`. Interiors: `s_interiorSunDir = (-0.30822, -0.30822, -0.9)` (`MapEntity.cpp:32`) —
the same triple 1.12.1 stores at `0xca816c`.

WotLK does have a **dynamic** direction-driven shadow system that 1.12.1 does not: `CShadowCache` /
`CShadowQuery`, while its baked `MCSH` remains direction-independent. That is the one structural
difference to be aware of when porting ideas, and it is the reason "the baked shadow and the sun
bearing" is a data problem in both versions and not a code problem in either.

## Appendix B. Alpha 0.5.3 (`wowdecomp_alpha`) — same model, older, and it *lacks* the baked map

`WoW/Common/DayNight.cpp:391-406` `SetDirection()` carries the identical numbers (theta = π·1.25
constant, phi = 0.70555556π / 0.6111111π), the same `InterpTable`, the same `SUNRISE`/`NOON`/`SUNSET`
constants. The alpha's azimuth really is one constant, and `sunMoonPath` (`DayNight.cpp:485-493`)
is scalar-only. So the "constant bearing" shape is not a retail decision — it has been this way
since alpha.

Chain, verified directly: `CWorld::UpdateDayNight` (`World.cpp:275-293`) writes
`CMap::sunLight->gxLight.m_dir = dnInfo->lightInfo.dir`; `CMapChunk::SelectLights()`
(`MapChunk.cpp:742`) puts that sun in Gx light slot 0; terrain vertices are P+N only
(`MapChunkRender.cpp:190-196`); the baked shadow is a separate overlay pass
(`MapChunkRender.cpp:391-399`) with `GxRs_MatDiffuse = CWorld::shadowColor`, `shadowModGxTex` plus
`shadowGxTexture`, world texgen `GEO_TO_TEX = -1/vertexList[1].y`; `CMap::QueryShadow`
(`MapQuery.cpp:97-126`) over `shadowBits[32]` with `OO_COORD_TO_SHADOW = 0.96f`. Objects:
`CMapStaticEntity::SelectLights()` (`MapDoodadDef.cpp:44-86`) — exterior-lit uses the sun dir with
`m_dirIntensity *= dirLightScale`, otherwise `gxLight.m_dir = interiorSunDir(-0.30822,-0.30822,-0.9)`;
`dirLightScaleAmount = 0.5f` (`MapEntity.cpp:20`), `dirLightScaleRate = 3.3333333f` (`:21`) — the
same 0.5 shadow value and the same 3.3333 ramp 1.12.1 has at `0x810a64` and `0x810808`.
`Flag_ExteriorLit = 0x0010` / `Flag_InteriorLit = 0x0008` (`Map.h:226-239`).

**One correction worth recording.** A first read of the alpha tree suggested there is "no retail
equivalent" of the alpha's runtime shadow lookup. There is: 1.12.1 bakes a per-chunk shadow map —
`MCNK` header `+0x2c` → `MCSH`, chunk field `+0xf1c`, address at `+0x144`, shadow texture `+0x140`,
combined packed texture `+0x13c` (`benilla-re terrain/chunk.md:55` and `:164-202`). The alpha has the
*lookup* (`QueryShadow` over `shadowBits`); retail replaced it with a *bitmap*. That is precisely
the step the project's `.dat` pipeline sits on.

## Appendix C. `northlight` — design ideas, and one hard negative

`I:\code\wow\misc\northlight` is a D3D9 proxy DLL (`frd9.dll`) for 3.3.5a with its own renderer. It
is not a 1.12.1 mod and none of its addresses apply. It is worth reading for policy.

**Hard negative:** northlight animates **elevation only**. `src/sky/celestial_time_warp.h:30`
`inline constexpr double kAzimuthRadians = kPi/4;` — one constant azimuth for both bodies, and
`:101` documents that sun and moon "share one azimuth, so they pass each other between sunset (20:30)
and moonrise (20:50)". So nobody has solved the azimuth arc there either; the "travel" it sells is
elevation plus a below-horizon Hermite dive (`kDiveDepth = 10`) plus disc-visibility weighting. Its
native keys reproduce the client's polar table (`0x7eecc0` keys, interpolated at `0x7ed3b0`) — the
WotLK counterparts of 1.12.1's `0xce8db0`/`0xce8d64`.

Ideas worth borrowing, in the order they would change this project:

1. **Low-pass only the light, never the discs.** `LightMotion`, 0.4 s, `world_renderer.h:332`
   applied at `:2746-2751`. If you animate azimuth, this is what keeps terrain shading, shadows and
   any volumetric term on one gently-changing direction while the discs keep exact clock positions.
2. **Quantize the direction for anything cached.** `world_math.h:62-71 quantizeDirection` (components
   rounded to 1/2048 then renormalized, "worst-case change below 0.03 degrees"),
   `quality_settings.h:142 ShadowDirectionSteps 256..2048`. Ported to this project: bucket the sun
   azimuth into N keys and keep one baked `MCSH` artifact per bucket, blending neighbours — the
   "cache stays valid while the body drifts within one lattice cell" idea, and it is the only sane
   way to pair a moving sun with *baked* shadows.
3. **Never invent a clock.** `celestial_disc.h:3` "this module never invents a clock, orbit or
   below-horizon source"; `README.md:41-45` "The sun light comes from the game's sky block". Drive
   from the client's own day clock (1.12.1: light state `+0x04`), do not synthesize one.
4. **The authored term is *subtracted* where shadowed, not replaced.** `world_effects.hlsl:430-441`
   `lerp(sun, moon, SunDirection.w)` and `float4(replacement - painted, …)`. Relevant if you ever
   composite rather than modulate.
5. **Baked-shadow strength as a band, not a swap.** `build_lighting.py:101-102` treats
   `LightIntBand` channel 8 as "Existing terrain shadow mask opacity, **NOT a new shadow map**",
   scaled by `(1.05 + .23*day)`; `sun_hue.h:12-16` maps band 8 → slot 2. If 1.12.1's `LightIntBand`
   layout exposes the same channel, a per-hour shadow *strength* comes free, without swapping
   payloads. (Verify against the vanilla layout before relying on it — 18 channels with 8 = shadow,
   0 = diffuse → slot 1, 1 = ambient → slot 0, 9 = sun, 10 = sunHalo is the 3.3.5 mapping.)
6. **Retime, don't replace.** `build_lighting.py:170-269`: "Only times move; the (relit) values stay,
   so the colour of every key is kept and only when it is reached changes", with hard invariants —
   values survive as a multiset, inserts copy the night value, times unique/increasing/in range,
   06:00–12:00 untouched, ≤ 16 keys. The same discipline is what a sun-key preset table should carry:
   move times, keep values, assert it.
7. **Keep blob shadows deliberately** (`README.md:32-34`, `renderer.cpp:1222`, `BlobShadowStrength`)
   — and "maps without a world cache get no extension shadows, so the game's own shadows and its
   round blob shadows are drawn as the game draws them" (`README.md:46-50`): a fallback that is
   honest about uncovered territory, the same problem as gotcha 7 above.
8. **Azimuth-keyed data loading.** `regional_shadow_range.h:41-56` restricts extra terrain loading
   to a corridor toward the shared azimuth (`client-config\shadow-range-profiles.ini:1-4`), and
   `world_effects.hlsl:68` `HorizonSun zw: horizontal sun direction × lobe strength`
   (`:1142-1146` `amplitude = sunWeight*cos(sun elevation)`) is the one place northlight carries a
   real horizontal sun vector — a directional vertex-colour/secondary-colour ramp reproduces it in
   fixed function.

## Appendix D. Side by side

| | Alpha 0.5.3 | 1.12.1 (this project's client) | WotLK 3.3.5a | northlight (3.3.5a renderer) |
| --- | --- | --- | --- | --- |
| sun azimuth | one constant π·1.25 | `0xce8d90`, 4 keys, all 225° | `s_azimuthTable` 5× 3.926991f, `s_theta` π·1.25 | `kAzimuthRadians = kPi/4`, constant |
| sun polar/elevation | 0.70555556π / 0.6111111π | `0xce8db0` 127°/110°/127°/110° | `s_elevationTable` 2.2165682/1.9198623 | native keys 100° low / 5° high, plus a dive below the horizon |
| animatable azimuth? | same data shape | **yes, tables are writable statics** | yes, same table | not attempted; constant by construction |
| terrain light direction | `CMapChunk` slot 0 = runtime sun | device light from `[0xc89ef0]+0xa0`, MCNR normals, state 0xe | `CMapRenderChunk::m_sunDir` `+0x180` | its own renderer's: constant-buffer sky light re-authored from the client's `LightIntBand` (`build_lighting.py`), driven by its own orbit |
| object light direction | sun or `interiorSunDir(-0.30822,-0.30822,-0.9)` | block direction or `0xca816c`, or the `+0xa3` blend | `DetailDoodad` `+0x00A0`, `M2Light`, `s_interiorSunDir` | global sky direction only, `authoredFill` keeps the residual |
| baked terrain shadow | none — runtime `QueryShadow` over `shadowBits[32]` | **`MCSH` bitmap, direction-independent**, 512 bits, gate `flags bit 0 && world flag 0x40` | `MCSH` bitmap **plus** `CShadowCache`/`CShadowQuery` | patched out (`patch_terrain_shadow.h`, `patch_terrain_shadow_coverage.h`) and replaced by its own maps |
| fixed 45°-looking vector | `interiorSunDir` | `0xca816c` = (−0.30822, −0.30822, −0.9) | `s_interiorSunDir` same triple | — (uses the sky direction everywhere) |

## Appendix E. What the answers change in the pipeline

* **Ship sun motion**: add 4 key sets (light azimuth, light polar, disc azimuth, disc polar) to the
  `.dat` or its sidecar, and have `src/mod.c` write them. Independent of the shadow payload, and it
  needs the guard-bit ordering argument of 4.3.
* **Morning/Evening become different products** once the azimuth moves: today they are the same
  bytes (`README.md:126`).
* **Every preset whose bearing is not 45° needs its own `MCSH` bake.** That is data work in the
  existing `whole-bake-diag.txt` → `shadow-variant MPQ export` → `.dat` flow
  (`README.md:16`), not new client code.
* **Uncovered chunks are the visible risk**, not the sun: stock `MCSH` under a traveling sun
  (`README.md:810`). Coverage, not the arc, is what decides whether a preset looks coherent.

## Appendix F. Open items this document does not settle

1. ~~Whether `0xca816c` is written anywhere besides `0x6a72b0`~~ — **settled: it is not.** The
   symbol is confirmed (`float[3]`, `s_globalLightDirection`, "Holds the direction of the global map
   light entities and doodads are lit from", verified/proof one, `ghidra/symbols/globals.tsv:4209`);
   the value is confirmed as the same `mov` immediate (`c7 05 ?? ?? ?? ?? 03 cf 9d be`, whose low
   little-endian dword `be 9d cf 03` decodes to −0.30822); and the entity consumer only *loads* it
   to the scratch triple `0xca8174` and keeps the blend and the renormalization in registers
   (`lights.md:29-30`). So the "interiors are fixed" line holds.
2. The exact `+0xa3` producer for entities that are not sky-open. The record layout pins its *slot*
   — `mapentities.md:12-13` puts the ambient word at `+0x9c` and the directional word at `+0xa0`
   with the intensity at `+0xa4`, so `+0xa3` is the fourth byte of the directional word, the
   colour's alpha. `lights.md:95` shows the interior bake writing 0xff there when sky-open, and
   `mapentities.md:132-134` shows the ambient word and target repacked from `[0xc89ef0]+0xac`
   (`0x670db0`, `0x69e280`); nothing documented shows what writes the directional word's alpha
   otherwise. This bounds how far a non-sky-open entity can drift toward the sun direction, not
   whether exteriors follow it.
3. Whether the bake's bearing slider accepts the full 0–360 compass or is clamped to the
   north-west quarter. `README.md:122-144` shows only ~45° pairs. The arc presets need 315° and
   135° — verify in the bake before shipping those rows.
