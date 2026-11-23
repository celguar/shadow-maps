/* ShadowMaps.dll -- Salmiak's baked shadows, drawn by the 1.12.1 client itself.
 *
 * What this is. Salmiak bakes shadow maps and can export them as a shadow
 * variant archive whose .adt files carry new MCSH blocks. The client already
 * draws those MCSH bytes, but only by re-reading whole patched terrain files.
 * This DLL skips the terrain half entirely: it reads a shadowmaps.dat sitting
 * next to itself -- every chunk's packed 512-byte shadow map, deduplicated --
 * and at chunk-load time it hands the game the LIVE arena bytes for that
 * chunk. No patched .adt, no archive swap, no shadow ever round-trips through
 * a parsed .adt. See README.md for the pipeline and the install steps.
 *
 * The ONE hook. E9 rel32 at CMapChunk::Load (0x6AF5F0, 5 bytes). probe_hooksite
 * .py re-checked on both client copies (vanilla and the Turtle-Wow flavor,
 * byte-identical here): the prologue is 6 bytes -- push ebp / mov ebp,esp /
 * mov eax,[ebp+8] -- so the trampoline replays all six and resumes at 0x6AF5F6
 * (push ebx); the stray 0x08 byte left behind by the 5-byte jmp is never
 * executed. Everything the shadow needs is read from the in-memory MCNK
 * header DURING Load (0x6AF6EA test [hdr],1 -> 0x6AF6EF test world flag
 * 0x40 -> 0x6AF6F8 mov ecx,[hdr+0x30] -> 0x6AF6FB mov edx,[this+0xf1c]), the
 * ofs itself was copied from hdr+0x2c into this+0xf1c, CreateShadowTexture
 * (0x6B0030) stores that raw ofs at this+0x144, and the game's own sub-record
 * resolver (0x6AF970) turns ofss into addresses as hdr + ofs with no range
 * check. So the hook mutates the header BEFORE the original body runs --
 * flags |= 1, ofs = (arena buffer - hdr), size = 512 -- and the game's own
 * gate, CreateShadowTexture, DecodeShadow, the combined-texture shader path
 * and IsInChunkShadow all consume the arena. A chunk the .dat has no entry
 * for is left untouched, and the payload map name is checked against the
 * client's own s_mapName (0xC961A0) at every chunk, so a payload for the
 * wrong map is ignored rather than half-applied. One .dat can carry several
 * maps now, so that check picks WHICH run of the entry table gets searched:
 * a chunk key is only unique inside one map, and the run is what keeps
 * Azeroth's shadows off Kalimdor's chunks of the same number.
 *
 * What sv10 settled, and what sv11 adds on top of it. sv10 put the working rungs
 * on d's stub shape (the record read from [ebp+8] on the client's own frame, the
 * 8-byte scratch before the two pushed arguments and the matching `add esp, 16`),
 * made the slot check stop assuming every resume is a `push` operand -- b's resume
 * is the operand of a `mov`, which was the disagreement that refused h and f --
 * and gave every hooking rung one "first fixup call" line naming the two pointers
 * it really got, so a clean d run now means "the call happened". Re-running b
 * against those is what confirms the [esp+48] lookup was the whole difference,
 * byte for byte: the crash's EIP and EBX match what filing the resume address as
 * the chunk's record produces, so the poison cursor is the client's own resolver
 * reading a code address as an `MCNK` header, and no [esp+48] arithmetic is ever
 * wrong. sv11 does not touch the hook, the stubs or the writes at all: it changes
 * only how one .dat addresses chunks, because a chunk key is unique only INSIDE a
 * map. tile 14,20 chunk 3,5 of Azeroth and of Kalimdor are the same 20-bit number,
 * and `patch-S.mpq`'s one file has twelve maps with 7927 keys that appear in more
 * than one of them -- searching the whole entry table would answer those chunks
 * with another map's shadow. So the header's reserved word became mapCount: 0 is
 * the single-map file the format had, with 8-byte entries and one run named by the
 * header's map field (every .dat written so far still loads unchanged), and 1..64
 * puts a table of 28-byte name+count records after the header and gives every
 * entry a third word, its row in that table. Install reads the table once, refuses
 * a table whose counts do not add up to entryCount, whose entries are not grouped
 * in table order, whose keys are not strictly ascending inside a run, or whose
 * index words do not name a record; the per-chunk gate then compares the client's
 * map name against each run's and binary-searches only that run -- the name is
 * re-read per chunk, so a session that changes map keeps working without an
 * install-time map-name dependency. extract_shadowmaps.py writes the new shape
 * always, one file for however many maps the archives carry, so the shipping
 * command is one command and the swap is still one file rename.
 *
 * What sv12 fixes on top of sv11: sv11's multi-map file drew in game, but not
 * the map it carried -- every chunk showed a mix that read as mirrored and
 * rotated, while the legacy single-map cut of the same archive drew right. The
 * .dat was never the suspect: measured against patch-S.mpq's own MCSH bodies,
 * all 76547 entries point at the payload they should ONLY when the payload area
 * is read from 124 + mapCount*28 + entryCount*12, and reading it from 124 +
 * entryCount*12 -- which is what sv11's loader did -- matches 0 of 76547. The
 * record table was skipped when checking the file's size but not when filling
 * the arena, so every arena offset sat 336 bytes (42 of the 64 texel rows)
 * short of its own payload and each chunk drew the tail of one map's shadow
 * over the head of the next. A legacy file has mapCount 0, so rec_bytes is 0
 * and both reads agree: that is why nothing older than the multi-map shape
 * ever showed this. One seek changed.
 *
 * What sv13 adds: the diagnostics behind a letter, so the log stops growing.
 *   Shipping is the default, and it is the state users get: no shadow-mod.mode
 *   file at all means mode f with quiet logging, and a run that worked writes
 *   ONE line, its install line naming the .dat, the arena, the hook and the
 *   logging level. Lines that say something went wrong stay on that level in
 *   every mode: a refusal (wrong client build, missing or malformed .dat, out
 *   of memory), the field contract moving under the shadow write, a client map
 *   the payload has no run for, and the exception instrument's capture of a
 *   launch that was already dying -- those are the things a player would
 *   otherwise see nothing about. The audit block, the per-chunk header dump,
 *   the first-match and progress lines and the long mode legend move to the
 *   diagnostic level, which a v anywhere in the mode file turns on ("f v" is
 *   the full run with sv12's whole trail), and which every rung but f turns on
 *   by itself, since walking the ladder means the log is what you are reading.
 *   TrimLogFile at load cuts the file to its newest 64 KB once it is past 512
 *   KB, on a line boundary rather than mid-line, so a verbose run cannot leave
 *   a file the size of the session and the previous launch's lines are still
 *   there to compare a new run against.
 *
 * What sv14 adds: the sun travels an arc, and which arc is the clock's own.
 *  Everything above stays -- the hook, the stubs, the writes, the arena and
 *  the .dat are sv13's byte for byte -- and one thing is added: the DLL also
 *  writes the client's own sun key tables, the four tracks the day-night file
 *  builds at 0xce8d90 (the lighting sun's azimuth), 0xce8db0 (its polar angle),
 *  0xce8d4c (the sun disc's azimuth) and 0xce8d64 (its polar angle), and that
 *  its own evaluator 0x6cf6c0 then reads every frame (benilla-re lighting/
 *  celestial.md#static-tracks; docs/sun-lighting-analysis.md, on the main
 *  checkout, is the chain that shows terrain and object shading follow these
 *  runtime values rather than a separate constant). The stock tables cannot
 *  make an arc: the lighting azimuth is 1.25pi on all four keys and the disc
 *  azimuth is 0.25pi on all three, so the sun rocks between two polar angles
 *  on one fixed bearing -- a vertical wheel, it rises and sets on the same
 *  bearing -- and the world's shading never turns. sv14 writes key sets whose
 *  azimuth completes 360 deg over the day, monotonically through the wrap, so
 *  what the client interpolates is one continuous forward rotation rather than
 *  a backwards sweep or a jump at midnight (0x6cf6c0 lerps in value space and
 *  its cyclic search wraps in time, celestial.md#tracks), and whose polar
 *  angle goes through the horizon; the disc azimuth is kept the antipode of
 *  the light azimuth so the shading direction and the visible sun agree. The
 *  key COUNTS are the edx to 0x6cf6c0 and the tracks are adjacent -- 0xce8d90
 *  + 0x20 is 0xce8db0, 0xce8d4c + 0x18 is 0xce8d64 -- so a preset that added a
 *  key would run into its neighbour and every preset keeps 4 / 4 / 3 / 5.
 *
 *  Which set gets written is the client's own clock's decision, and the DLL
 *  never makes a clock of its own: the day-night state at 0xce9b60 holds the
 *  game clock word, the time of day in [0,1) and the day count at +0, +4 and
 *  +8, stamped every rendered world frame (benilla-re lighting/daynight.md
 *  #light-state), and the light table build reads that same float at 0xce9b64
 *  while turning it into half-minutes. sv14 turns it the same way -- round(tod
 *  * 2880), 2880 being the half-minutes in a day -- and picks the preset whose
 *  window of half-minutes holds it: the day is cut into two windows, arc-swnw
 *  from midnight to 20:00 and arc-stock from 20:00 to midnight, so the hour the
 *  world was entered at names the arc, and arc-stock is the stock sun. sv14 cut
 *  it into five windows holding five different arcs, which was never one sun
 *  crossing one sky -- see the sv15 note below -- and sv15 also writes the stock
 *  keys back in this window instead of asking for nothing. An
 *  unstamped block reads 0, which is midnight and the arc's window, so the
 *  first chunk visit already writes an arc and every visit after it reads the
 *  clock again -- the choice settles as soon as the world stamps the block.
 *  The applied line prints
 *  the clock as the game reads it, the preset, and the arc's own numbers off
 *  the table that was just written, so the run says what was written rather
 *  than what a reader hoped for.
 *
 *  The clock is read and the keys are written at the per-chunk fixup, the one
 *  place this DLL already runs on every loaded chunk: that is where the world
 *  frames have started stamping the block, and the re-assert at every visit
 *  after it is also what puts a key set back if the day-night file rebuilt its
 *  tracks behind the guard bits at 0x6d3a10 and 0x6d3b80 after install
 *  (celestial.md#static-tracks builds them once, at first use, out of the
 *  static initializers at 0x6ce4a0 and 0x6ce560). A rung that never carries it
 *  -- n, a and c never call the fixup, and b and d return above it, silent --
 *  says so in the install lines, and h and f carry the arc. Before any write the four tracks are pinned against
 *  the stock values, or against an all-zero track whose builder has not run:
 *  a track holding neither is not the shape the sun state reads, and the sun
 *  is left alone with one log line while the shadows keep working. shadow-mod
 *  .sun, next to the mode file, overrides the clock with off or a preset name,
 *  which is how one arc is watched without waiting for the hour. docs/sun-arc-
 *  note.md, next to this branch, is what the arc does in game and what it
 *  deliberately leaves alone.
 *
 * What sv15 adds on top of sv14: the stock window puts the stock keys back, and
 *  a digit in the mode file makes the game clock run faster, for testing.
 *  sv14's arc-stock window asked for nothing at all, which is wrong on more than
 *  one continent: the four tracks are one global block in WoW.exe's data, and
 *  the day-night file builds them once behind the guard bits at 0x6d3a10 and
 *  0x6d3b80, so the last arc written survives into every hour that does not
 *  overwrite it -- and outside a preset's own key span 0x6cf6c0 takes the wrap
 *  leg from its last key back to its first and interpolates ALONG it, so hours
 *  the preset never covered get an extrapolated sun. That is what an in-game
 *  visit showed: Kalimdor at 11:30 wrote arc-low, then Eastern Kingdoms at 23:30
 *  fell in arc-stock's window and kept arc-low's keys, whose wrap leg at 0.979
 *  puts the disc 10 deg under the horizon but the LIGHT polar back to 80 deg --
 *  a light direction pointing above the horizon, which zeroes the diffuse term
 *  of every up-facing normal and reads as mirrored, reflected ground. sv15's
 *  stock window writes the stock keys instead, so the arc that another
 *  continent left cannot carry into these hours. The second half is a test hook
 *  only: the client's clock object at 0xce8538 keeps its game minutes per real
 *  second at +0x2c (gametime.md#clock-state), 0x643a70 multiplies the real
 *  seconds since the last minute by it to get the day phase, and 0x6435a0 adds
 *  dt times it every frame, so that one float IS the speed of the day. The
 *  server sends it in the same packets as the time -- the opcode 0x47 handler
 *  0x6c5de0 and the 0x42 handler 0x6c5e80 both end in 0x643820, which stores it
 *  clamped to [1/60, 60] -- so digits up to 99 anywhere in shadow-mod.mode
 *  multiplies whatever the server sent, re-asserted at every chunk visit the way
 *  the key sets are, because a time message would otherwise undo it. Nothing
 *  else in this file reads the clock object, and nothing about the shadow path
 *  moves.
 *
 *  The third thing sv15 changes is the shape of the arc itself, because an
 *  in-game evening and morning said sv14's tables were wrong in two ways. The
 *  lighting polar went as low as 80 deg, under the 90 deg horizon, so the light
 *  direction pointed UP: the ground read mirrored and over-bright -- the desert
 *  that was lit far past what stock does -- and the shading seemed to come from
 *  the far side of the disc. And sv14's polar key times put the disc 67 deg up
 *  at 08:50 where stock has it 39 deg up, and made its day rise at 03:39 and
 *  set at 22:04 instead of stock's 06:11 and 20:30, so the sun was too high and
 *  the sunset came at the wrong hours. sv15 keeps the polar keys of all four
 *  tracks exactly as stock has them, the disc at 100 deg at 05:30 and 21:30 and
 *  5 deg at noon, the light at 127/110/127/110 on stock's own times, so the
 *  height, the rise at 06:11, the set at 20:30 and the strength of the day are
 *  stock's hour for hour, and moves only the azimuth: the disc runs 315 deg at
 *  sunrise, due west at noon, 405 deg at sunset -- a south west sunrise and a
 *  north west sunset on the map, the direction the player asked for -- with the
 *  light azimuth kept the disc's antipode as stock keeps it, so the shading
 *  comes from where the disc is. One arc up to 20:00 and stock's own keys from
 *  20:00, not four windows of four arcs: four arcs were never one sun crossing
 *  one sky, and the visits showed the disc, the ground and the objects naming
 *  three different places for the same light.
 *
 * runs came back with their install lines and no new crash: mode a (the whole
 * 34 MB arena resident, no hook), mode c (the store and landing at 0x6AF5F0,
 * no call) and mode d (the call with its arguments read from [ebp+8] on the
 * client's own frame) entered the world clean, and mode h and mode f refused
 * to install at all with "build defect: the detour byte table and its slot
 * offsets disagree". So the store, the landing, both resume points, the arena
 * and the .dat read are all proven, and the fault sv8 saw is only b's [esp+48]
 * argument lookup through pushad/pushfd. sv10 acts on that in three places:
 * StubShape puts the rungs that do real work (h and f) on d's shape and leaves
 * b on its own, the slot check stopped assuming every resume was a 0x68 push --
 * b's resume is the operand of a 0xB8 mov, which is the disagreement that
 * refused h and f -- and every hooking rung now prints one "first fixup call"
 * line naming the two pointers it got, so a clean d run means "the call
 * happened" instead of "nothing happened". Re-running b against these is what
 * confirms the lookup was the whole difference:
 *   c  the detour ONLY: replay the 6 prologue bytes, push <resume>, ret. No
 *      pushad, no call, no C code on the path at all -- 12 bytes, one patch
 *      slot, and eax/ecx/edx/esp/flags all come back as the client had them,
 *      so a clean c against a crashing b says the frame math was wrong and a
 *      crashing c says the store, the landing or the resume point is wrong
 *   d  the same empty call at a site 3 bytes later, 0x6AF5F3, where the 5-byte
 *      E9 covers mov eax,[ebp+8] / push ebx / push esi whole and resumes on the
 *      instruction boundary 0x6AF5F8 (push edi) with no stray byte; the stub
 *      takes the record from [ebp+8] and this from ecx, i.e. from the client's
 *      own frame, so no [esp+48] displacement is involved at all
 *   the instrument: AddVectoredExceptionHandler (resolved from kernel32 at
 *      runtime, since some of the toolchains this file builds with predate the
 *      vectored-handler API) plus a handler that logs ONE line for the first
 *      0xC0000005 -- the exception's address, the address the client
 *      dereferenced, and the last chunk this DLL saw: this, record, and what
 *      the object itself holds at this+0xf08..+0xf30, which is the resolver's
 *      own answer for the same eight ofss. Comparing the faulting address
 *      against this+0xf14 in that one line says whether the client's resolver
 *      produced the poison cursor or this file's write did. The handler returns
 *      EXCEPTION_CONTINUE_SEARCH, so WoW's own reporter still writes its
 *      normal report, and the per-chunk dump cap drops to 2000 because this
 *      line, not the log's tail, names the dying chunk. README.md's mode
 *      ladder section says what each rung proves:
 *   n  inert -- the .dat is not read, nothing is allocated, nothing is patched
 *   a  the .dat and the resident arena, whole -- and NO hook, NO write
 *   b  the hook and an empty call only, on b's own [esp+48] frame -- no .dat,
 *      no arena, no header read
 *   c  the hook alone at 0x6AF5F0, no call -- the stub's store, landing and
 *      replay only
 *   d  the same empty call at 0x6AF5F3, args from the client's own frame
 *   h  the hook and the header audit only -- no .dat, no arena, no write, and
 *      riding d's shape now
 *   f  full: .dat, arena, hook and the matched-chunk writes (shadows draw) --
 *      d's shape too, so b is the only rung left on b's frame
 *
 * Lifecycle follows ClassicAPI's src/DllMain.cpp. DllMain installs nothing:
 * under VanillaFixes the install runs from the exported `Load` (VF calls
 * GetProcAddress(module,"Load") on the game's main thread after every dlls.txt
 * DllMain has finished, outside the loader lock). When VfPatcher.dll is NOT in
 * the process we were loaded by something else and no one will call `Load`, so
 * DllMain spawns a worker thread instead -- it cannot run its body until
 * DllMain returns and the loader lock releases, and nothing waits on it.
 * Honest difference from ClassicAPI: its install is ~90 MinHook hooks and that
 * thread-freeze pattern is why the install had to leave DllMain; ours is ONE
 * 5-byte store, so there is no freeze to dodge -- but a store means a thread
 * that was executing inside CMapChunk::Load at that exact moment could see
 * half a jmp. In practice the client loads no chunk that early, and this file
 * refuses to patch at all unless the pinned bytes match the 1.12.1 build the
 * whole contract was read from.
 *
 * No MinHook and no trickery: the detour is machine code emitted once at
 * install time into a private executable page this DLL owns -- the same shape
 * MinHook's heap trampolines use (VirtualAlloc with execute rights, never a
 * rewrite of a file-backed page). It is emitted as bytes rather than written
 * as MSVC inline asm on purpose: inline asm hands the block out to a separate
 * assembler (ml.exe / cl_as.exe for the i686 target), so a mod written that
 * way depends on that tool being installed and on PATH, and its block is
 * fixed up by hand once the link has placed the code. A const array of bytes
 * is the same machine code, the build has no assembler to find, the two
 * addresses are patched into named slots at install, and verify_dll.py
 * disassembles the array straight out of the linked image, so the stack math
 * can be re-read from real code rather than from a comment.
 */

#include <windows.h>
#include <excpt.h> /* EXCEPTION_CONTINUE_SEARCH: 0 = "keep searching", so after
                   * the instrument logs its line the SEH walk continues into the
                   * client's own crash reporter and the usual report is written*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shadow_dat.h"

#define STAMP "sv15" /* build stamp, printed in every log line: an old build of
                     * this DLL sitting next to a new .dat is the most common
                     * confusion, so the line says which build made it, and the
                     * mode line below names which of the seven rungs ran. sv11
                     * and sv12 are sv10 with only the .dat's chunk addressing
                     * changed -- sv12 fixes which byte the payload area starts
                     * at, and neither touches the hook, the stubs or the writes
                     * -- so an sv10, sv11 or sv12 log line means the same thing
                     * about the hook and differs only in which .dat shape built
                     * it. sv13 changes neither the hook nor the format: it moves
                     * the diagnostics behind a letter, so an sv13 log is nearly
                     * empty by default and holds sv12's lines once the mode file
                     * carries a v. A mirrored-and-rotated ground under an sv11
                     * line is that seek, not the bake. sv14 adds the sun: the
                     * client's own sun key tables are rewritten so the sun
                     * travels an arc rather than rocking on one bearing, and
                     * which arc gets written is picked from the client's own
                     * in-game clock -- see the sun state below, docs/sun-arc-
                     * note.md says what the arc does and what it does not do.
                     * The hook, the stubs, the writes and the .dat are sv13's
                     * unchanged, so an sv14 line about a shadow is an sv13
                     * line about the same shadow. */

/* ---------------------------------------------------------------- pinned **/

#define ADDR_LOAD        0x6AF5F0u /* CMapChunk::Load(this, record, first) */
#define ADDR_GATE        0x6AF6EAu /* the shadow gate inside Load          */
#define ADDR_MAP_NAME    0xC961A0u /* s_mapName: char[256], written by CMap::Load */

/* push ebp / mov ebp,esp / mov eax,[ebp+8] -- the 6 bytes the trampoline
 * replays; the E9 covers the first 5, resume is at +6. */
static const unsigned char k_prologue[] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08
};
/* mode d's site: 3 bytes into the SAME prologue, pinned as 6 bytes because the
 * 5-byte E9 covers mov eax,[ebp+8] / push ebx / push esi exactly and the 6th
 * byte is where it resumes -- 0x57 push edi at 0x6AF5F8. Pinning that byte is
 * what proves the resume is an instruction boundary and not the middle of one:
 * probe_hooksite.py prints the same window from a real client, and capstone
 * reads these six bytes as three whole instructions. */
#define ADDR_LOAD_D      0x6AF5F3u /* CMapChunk::Load, 3 bytes in            */
static const unsigned char k_prologue_d[] = {
    0x8B, 0x45, 0x08,             /* mov eax,[ebp+8]  (record) */
    0x53, 0x56,                   /* push ebx / push esi       */
    0x57                          /* push edi: the resume byte, not patched */
};
/* the whole gate chain, pinned as one run so a mismatch refuses to patch:
 * test [hdr],1 / je off / test [0xc7b2a4],0x40 / je off /
 * mov ecx,[hdr+0x30] / mov edx,[this+0xf1c] */
static const unsigned char k_gate[] = {
    0xF6, 0x00, 0x01,             /* test byte ptr [eax], 1          */
    0x74, 0x22,                   /* je 0x6AF711 (shadow off)        */
    0xF6, 0x05, 0xA4, 0xB2, 0xC7, 0x00, 0x40, /* test [0xc7b2a4],0x40 */
    0x74, 0x19,                   /* je 0x6AF711                     */
    0x8B, 0x48, 0x30,             /* mov ecx,[eax+0x30]  (sizeMCSH)  */
    0x8B, 0x96, 0x1C, 0x0F, 0x00, 0x00 /* mov edx,[esi+0xf1c] (ofs)  */
};

#define FIELD_GLOBAL_COL 0x8Cu /* CMapChunk: area base + column (gx) */
#define FIELD_GLOBAL_ROW 0x90u /* CMapChunk: area base + row (gy)    */
#define FIELD_MCSH_OFS   0x2Cu /* MCNK header: ofsMCSH               */
#define FIELD_MCSH_SIZE  0x30u /* MCNK header: sizeMCSH              */
/* The object's own copy of the same facts, which is what the instrument reads:
 * the resolver at 0x6AF970 writes hdr at +0xf08 and one ADDRESS per sub-record
 * slot (+0xf14 MCVT .. +0xf30 MCSE, i.e. hdr + ofs already added), Load stores
 * the record it was handed at +0x168, and BuildVertices walks the MCVT one at
 * 0x6B0FB7. A crash report's ebx is one of these fields, so the handler prints
 * them and the report can be matched against the object without another run. */
#define FIELD_HDR_OFS    0xf08u /* resolver: lea edx,[record+8] -> hdr        */
#define FIELD_RECORD     0x168u /* Load: mov [this+0x168],eax = the record    */
#define FIELD_PTR_MCVT   0xf14u
#define FIELD_PTR_MCNR   0xf18u
#define FIELD_PTR_MCSH   0xf1cu
#define FIELD_PTR_MCLY   0xf20u
#define FIELD_PTR_MCAL   0xf24u
#define FIELD_PTR_MCRF   0xf28u
#define FIELD_PTR_MCLQ   0xf2cu
#define FIELD_PTR_MCSE   0xf30u

/* The sub-record ofss the client's own resolver (0x6AF970) turns into
 * addresses, every one of them but the shadow slot this file writes:
 *   mov eax,[ecx+0x168]        eax = the record Load was handed
 *   lea edx,[eax+8]            edx = hdr, kept at this+0xf08
 *   mov esi,[edx+0x14] / lea esi,[esi+eax+8] / mov [ecx+0xf14],esi
 * ... for +0x14 +0x18 +0x1c +0x20 +0x24 +0x2c +0x58 +0x60. They are the only
 * thing between the write below and a crash: BuildVertices (0x6B0E50) walks
 * 145 floats off the MCVT one, and its inner loop at 0x6B0FB7 is `mov
 * ecx,[ebx]` with ebx = this+0xf14 advanced 4 a vertex -- the ACCESS_VIOLATION
 * reading the MCVT cursor is what a wrong FIELD_MCSH_OFS looks like in-game.
 * So the seven untouched ofss are sampled before and after the write, and a
 * sample that moved is proof the shadow field is not at hdr+0x2c here: the
 * ofss are put back so the vertex walk survives, the shift is logged with the
 * addresses to recompute FIELD_MCSH_OFS from, and mutation stops for the rest
 * of the run rather than dying in that loop. */
static const unsigned k_probe_ofs[] = {0x14u, 0x18u, 0x1cu,
                                       0x20u, 0x24u, 0x58u, 0x60u};
#define PROBE_COUNT 7u

/* The same table whole, each slot with the fourcc whose sub-record sits at
 * `hdr + <value in that slot>` -- that is the resolver's own math (`mov
 * esi,[edx+ofs] / lea esi,[esi+eax+8]`, i.e. hdr + ofs) and the same thing
 * extract_shadowmaps.py checks inside a real .adt before it writes one entry:
 * it wants b"MCSH" at hdr + ofsMCSH - 8, so in a stock file the value
 * addresses the 512-byte BODY, magic+size in front of it, and the header's own
 * sizeMCSH carries 520. AuditHeader below prints the value, the address it
 * resolves to and the fourcc found there for all eight slots, which is what
 * separates a wrong hdr basis from a wrong FIELD_MCSH_OFS and both of those
 * from a mapping that simply is not there. */
static const unsigned k_table_ofs[] = {0x14u, 0x18u, 0x1cu, 0x20u, 0x24u,
                                       0x2cu, 0x58u, 0x60u};
static const char *k_table_cc[] = {"MCVT", "MCNR", "MCLY", "MCRF", "MCAL",
                                   "MCSH", "MCSE", "MCLQ"};
#define TABLE_COUNT 8u
/* one MCNK record is at most a few tens of KB, so a slot value under this is
 * inside the record the header belongs to and reading its fourcc cannot fault;
 * a bigger value is printed, never dereferenced */
#define AUDIT_OFS_MAX 0x40000u
/* the per-chunk header dump is capped so a run cannot write an endless log. It
 * used to be 40000 because the LAST of these lines was the only way to name the
 * chunk whose BuildVertices died; sv9's exception handler names that chunk
 * directly, so this is now only a trail that says how far the walk got, and
 * 2000 lines is enough for that without a log the size of the run */
#define AUDIT_LOG_MAX 2000u
/* sv13 keeps the log bounded so several launches cannot leave a file the size of
 * the run: at load, a log already past LOG_MAX_BYTES is cut to its newest LOG_KEEP
 * _BYTES, and the cut starts at a newline so a line is never split in half. A log
 * under LOG_MAX_BYTES is left whole, which is what keeps the previous launch's one
 * quiet line there to read. 2000 audit lines are ~600 KB on their own, so a
 * verbose run still grows inside one launch and is cut at the start of the next */
#define LOG_MAX_BYTES  (512u * 1024u)
#define LOG_KEEP_BYTES (64u * 1024u)

/* ------------------------------------------------------- runtime state ***/

static HMODULE g_self;
static DWORD g_t0;                    /* GetTickCount at DllMain: LogMsg's zero */
static char g_dir[MAX_PATH];          /* the directory this DLL sits in     */
static char g_datPath[MAX_PATH];      /* <dir>\shadowmaps.dat, the fixed name */
static char g_logPath[MAX_PATH];      /* <dir>\shadow-mod.log.txt           */
static volatile int g_ready;          /* gate for fixup: set after install  */
static unsigned g_entryCount;
static unsigned *g_keys;              /* sorted ascending, binary searched  */
static unsigned *g_offs;              /* payload byte offset inside arena   */
/* one .dat can carry several maps (see shadow_dat.h's map-record table): a
 * chunk key is only unique INSIDE a map -- tile 14,20 chunk 3,5 of Azeroth and
 * of Kalimdor are the same number -- so the file's entries are kept in one
 * contiguous run per map and the per-chunk name gate picks the run to search.
 * A file that predates the map table (its header's mapCount word is 0) is
 * treated as exactly one run named by the header's map field, which is what
 * every single-map .dat written so far already is. g_dat_map is then only the
 * file's own label, and g_dat_maps_txt is the joined list of gate names, used
 * by the one mismatch line the per-chunk gate prints. */
static unsigned g_mapCount;                                   /* 1..64 runs */
static unsigned g_map_start[SHADOW_DAT_MAP_MAX];    /* first entry of a run  */
static unsigned g_map_len[SHADOW_DAT_MAP_MAX];      /* entries in that run    */
static char g_map_name[SHADOW_DAT_MAP_MAX][24];     /* gate name of a run     */
static char g_dat_maps_txt[512];                    /* every run, for the log */
static unsigned char *g_arena;        /* payloadUnique x 512 live shadow maps */
static unsigned char *g_stub;         /* the executable page holding the detour */
static unsigned char g_dat_map[24];   /* header's map name, NUL-terminated  */
static DWORD g_resume;                /* runtime resume address for the asm */
static volatile DWORD g_hits;         /* chunks that found a payload        */
/* what the exception instrument reads (see DyingChunkHandler): the LAST chunk
 * the fixup was handed. Load runs the fixup before its own body walks the
 * header it was given, so when a BuildVertices dies the chunk that dies is the
 * one stored here, and the handler can print the object's own fields off it
 * without waiting for the log's tail. volatile because any thread that loads a
 * chunk writes them, and the handler may run on any of them. */
static volatile void *g_last_this;
static volatile void *g_last_record;
static volatile DWORD g_chunk_visits; /* chunks that reached the fixup      */
static int g_first_call;              /* the "the call happened" line is printed once per run, by every hooking rung that calls the fixup at all. It is the cheapest possible proof that a rung's stub reaches the call with a pair of pointers, which is what makes a clean d run mean "the call works" rather than "nothing happened": mode c prints no such line because its landing calls nothing. */
static int g_handler_logged;          /* the dying-chunk line is logged once */
static int g_handler_on;              /* the vectored handler is registered  */
/* UCRT's fread is NOT the standard C three-argument form: <stdio.h> declares
   size_t fread(void *_Buffer, size_t _ElementSize, size_t _ElementCount,
   FILE *_Stream) and returns BYTES read, so a byte block is (p, 1, n, f). The
   three-argument call is what a glibc reader expects and MSVC rejects it with
   C2198 "too few arguments", which is why the whole file reads blocks through
   this one helper: the arity appears once, and the comment says what the extra
   argument means. */
static size_t ReadBlock(FILE *f, void *p, size_t n)
{
    return fread(p, 1u, n, f);
}

/* one-shot log flags, fixup is hot: each of these prints at most one line per
   * run. sv13 splits them by level: g_log_mismatch (this map has no run in the
   * payload) and g_log_field (the write moved something) stay on the always-
   * written level because both say why the ground looks stock; the rest are the
   * diagnostic level and are only ever set when the diagnostics are on */
static int g_log_mismatch;            /* one-shot log flags, fixup is hot   */
static int g_log_nomatch;
static int g_log_first;
static int g_log_hits;                /* how many progress lines were logged */
static int g_log_field;               /* a probe dword moved: logged once   */
static int g_log_audit;               /* the first chunk entering Load audited */
static unsigned g_audit_seen;         /* per-chunk header dumps logged so far */
static int g_silent;                  /* modes b and d: the call returns before
                                          doing anything at all, file I/O included */
static char g_mode = 'f';             /* the mode letter, see ReadModeFile     */
static int g_sun_audit;               /* mode letter q: the per-chunk header dump */
static int g_verbose;                 /* sv13: the diagnostic level is on. Set by
                                         ReadModeFile from a v in the mode file and
                                         forced on by every rung but f, so a shipping
                                         run (no file at all) keeps it off */
static int g_allow_write;             /* only mode f lets fixup_chunk write    */
static int g_diagnostic;              /* installed nothing on purpose: Load returns 4 */
static int g_time_speed;              /* sv15: the mode file's digits, to 99, */
                                      /* 0 means the file had none. Read with */
                                      /* the mode letter, used by the arc's own */
                                      /* per-chunk hook, and 1 means the */
                                      /* server's own speed, nothing written. */
static float g_time_wrote;            /* the scale this DLL last wrote, 0 none */
static int g_time_said;               /* its applied line prints once          */

static DWORD WINAPI InitWorker(LPVOID p);
static int Install(void);
/* one of the three detour shapes, picked by the rung's letter: BuildStub looks
 * the table and its two imm32 slot offsets up in mod.c's own defines, so the
 * caller cannot pair a table with another table's slots */
static int BuildStub(char rung);
static int PatchHook(char rung);
static void InstallHandler(void);
static DWORD WINAPI DyingChunkHandler(PEXCEPTION_POINTERS info);

/* ------------------------------------------------------------ logging ***/

/* One log line per event, appended to shadow-mod.log.txt next to the DLL. The
 * stamp is the number of milliseconds since THIS DLL was loaded, not GetTicks
 * (count) itself: GetTickCount counts milliseconds since the SYSTEM booted, so
 * the raw number is the same clock for every client start on the same boot and
 * a log read cannot tell one start from the next -- which is exactly how an
 * earlier read of this file went wrong, attributing lines to eleven starts.
 * ms-since-load restarts at 0 in every process, so a run's block is obvious and
 * the stamp plus the mode letter names both the build and the rung that made
 * the line. sv13 no longer asks the user to delete the file between runs: the
 * default writes one line per launch, and TrimLogFile below cuts the file to its
 * newest bytes at load once a verbose run left it big. */
static void LogMsg(const char *text)
{
    FILE *f = fopen(g_logPath, "a");
    if (!f)
        return;
    fprintf(f, "[%u ms after this DLL was loaded] %s\n",
            (unsigned)(GetTickCount() - g_t0), text);
    fclose(f);
}

/* sv13's two levels, both writing through that one writer. LogMsg is the level a
 * shipping user can ever see, and it is deliberately only about things that need
 * a reader: the install line that says what the DLL loaded and patched, every
 * refusal (wrong client build, missing or malformed .dat, out of memory, the
 * field contract moving), the mode-n line, and the exception instrument's
 * capture, which only ever runs on a launch that was already dying. LogVerb is
 * the diagnostic level: the audits, the per-chunk header dump, the first-match
 * and progress lines, the long mode legend and the ladder rungs' own verdicts,
 * i.e. everything that exists to be read against the client rather than watched
 * while playing. A good mode-f run with no mode file writes ONE line, so the log
 * stays a few hundred bytes across launches instead of the ~600 KB the 2000
 * audit lines used to cost every launch. */
static void LogVerb(const char *text)
{
    if (!g_verbose)
        return;
    LogMsg(text);
}

/* Called once per process, before the mode file is read: a log already past LOG_
 * MAX_BYTES is cut to its newest LOG_KEEP_BYTES. Both tables stay, the bytes in
 * the middle are what grew. The window is read into memory, then the file is
 * rewritten as one banner line plus the whole lines that survive, so the cut never
 * starts in the middle of a line and the newest run's evidence is still readable.
 * A log under the limit is left alone, which keeps the previous launch's quiet
 * install line in the file to compare against. */
static void TrimLogFile(void)
{
    FILE *f;
    long size;
    unsigned char *buf;
    size_t got, start;

    f = fopen(g_logPath, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    if (size < 0 || (unsigned long)size <= LOG_MAX_BYTES) {
        fclose(f);
        return;
    }
    buf = (unsigned char *)malloc(LOG_KEEP_BYTES);
    if (!buf) {
        fclose(f);
        return;
    }
    fseek(f, (long)((unsigned long)size - LOG_KEEP_BYTES), SEEK_SET);
    got = ReadBlock(f, buf, LOG_KEEP_BYTES);
    fclose(f);
    start = 0;
    while (start < got && buf[start] != '\n')
        ++start;
    if (start < got)
        ++start; /* the bytes up to and including this newline are the partial
                   * line the window cut in half: keep from the next one */
    f = fopen(g_logPath, "wb");
    if (!f) {
        free(buf);
        return;
    }
    fprintf(f, "[%u ms after this DLL was loaded] sv13 shadow-mod.log.txt was %u"
               " bytes at load, past the %u byte limit: the newest %u bytes are"
               " kept, older lines are gone\n",
            (unsigned)(GetTickCount() - g_t0), (unsigned)size,
            (unsigned)LOG_MAX_BYTES, (unsigned)got);
    if (got > start)
        fwrite(buf + start, 1, got - start, f);
    fclose(f);
    free(buf);
}

/* sv13's mode file: <dir>\shadow-mod.mode may name both the rung and the logging
 * level. The FIRST letter that is one of n/a/b/c/d/h/f is the rung, and a v
 * anywhere in the file turns the diagnostics on, so "f" ships quietly and "f v"
 * (or "v f") is the same full run with sv12's whole trail. Letters are lower-
 * cased, blanks skipped and anything else ignored. A missing, empty or junk file
 * means f with quiet logging: shipping is the default, so users get no file at
 * all, and the file is what a debug session adds. A rung other than f implies
 * verbose -- those rungs exist to be read against the client, so walking the
 * README ladder does not have to remember the v, and every ladder log keeps
 * saying the same thing an sv10..sv12 ladder log said. sv15 adds a third thing
 * the same file can say: digits up to 99 anywhere in it multiply the game clock's
 * speed, so "f v 4" is the full run, verbose, with the day running four times as
 * fast -- which is how one arc is watched cross the sky without waiting for the
 * hour. A run of digits is ONE number and two of them are allowed, so "45" is
 *  forty-five times and not four then five: at the client's own 1/60 game
 * minutes per real second a plain 9 leaves a whole day at 160 real minutes,
 * which is still slow to watch, while 45 puts a day at 32 real minutes and one
 * hour of sky at 80 seconds. A run after a gap replaces the run before it, so "2 7" means
 * 7. 99 is the top: past that the day runs in under 15 real minutes and the
 * client's own per-minute work starts dropping minutes. A 1 is the server's
 * own speed, and no digit at all leaves the clock untouched, so shipping and
 * every older mode file keep meaning the same thing. */
static char ReadModeFile(void)
{
    char path[MAX_PATH];
    FILE *f;
    int c;
    char m;
    int gap = 1;              /* a space, tab or newline closes the run of digits
                                * the reader is in, so "4 5" is two runs */
    int run = 0;              /* digits counted in the current run */
    int d;

    snprintf(path, sizeof path, "%s\\shadow-mod.mode", g_dir);
    f = fopen(path, "r");
    if (!f)
        return 'f';
    m = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            /* a gap closes the digit run it was in, so "4 5" is two runs and
             * the last one is what the clock gets */
            gap = 1;
            continue;
        }
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c == 'v') {
            g_verbose = 1;
            gap = 1;
            continue;
        }
        if (c == 'q') {
            /* the per-chunk header dump, back on. It is what filled the log in
             * a fast session: 2,000 lines of ~300 bytes each is more than the
             * newest LOG_KEEP_BYTES TrimLog keeps, so the sun lines were
             * pushed out of what survived. A q asks for them again, and a q is
             * not a rung letter, so it cannot change what runs. */
            g_sun_audit = 1;
            gap = 1;
            continue;
        }
        if (c >= '0' && c <= '9') {
            /* a run of digits is ONE number, two digits deep: "45" is forty-five
             * game minutes a real second, not four then forty-five or four then
             * five. A run that starts after a gap replaces the run before it, so
             * a file that says 2 then 7 is a session that changed its mind, and
             * the clock is a test control, not something a rung letter competes
             * about. 99 is the top the mode file can ask for and the client's
             * own setter clamps it to 60, a game hour a real second and a whole
             * day in 24 real seconds; the digits are the scale itself, game
             * minutes per real second, not a multiplier of what the client has,
             * because the client's own scale is 1/60. */
            d = c - '0';
            if (gap) {
                g_time_speed = d;
                run = 1;
            } else if (run == 1) {
                /* the second digit joins the run: 4 then 5 is forty-five, and
                 * 0 then 5 is five */
                g_time_speed = g_time_speed * 10 + d;
                ++run;
            }
            /* a third digit in the same run is past 99 and stays out of the
             * clock */
            gap = 0;
            continue;
        }
        if (c == 'n' || c == 'a' || c == 'b' || c == 'c' || c == 'd'
            || c == 'h' || c == 'f') {
            if (!m)
                m = (char)c; /* the first rung letter wins; later ones are junk
                               * the file cannot mean */
        }
    }
    fclose(f);
    return m ? m : 'f';
}

/* case-insensitive compare of the .dat map name against the client's own */
static int CiEqName(const char *dat, const char *client)
{
    int i;
    for (i = 0; i < 24; ++i) {
        char a = dat[i], b = client[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (a != b)
            return 0;
        if (!a)
            return 1;
    }
    return 1;
}

/* a sub-record's fourcc as printable text: a junk magic has to stay readable in
 * the audit line, so a non-printable byte becomes '.' */
static void FourccAt(const unsigned char *p, char *out)
{
    int i;
    for (i = 0; i < 4; ++i) {
        unsigned char c = p[i];
        out[i] = (c >= 0x20u && c < 0x7fu) ? (char)c : '.';
    }
    out[4] = 0;
}

/* one chunk's header, printed the way this client has it. Two questions have
 * to be answered from real memory rather than from the file, and this answers
 * both: is the record Load was handed really an MCNK record (so hdr = record +
 * 8 is the basis the shadow write's offsets are measured from), and what does
 * each of the resolver's eight slots resolve to -- the address printed for MCVT
 * is exactly the cursor whose `mov ecx,[ebx]` at 0x6B0FB7 faulted in the crash
 * reports, so the line says whether the value in hdr+0x14 is the .adt's own
 * (the fourcc 'MCLY'-shaped bytes sit 8 before the value resolves to, which is
 * what the resolver and the .adt reader both agree on) or something this DLL
 * moved. The record's own fourcc prints as the client has it in memory, which
 * is byte-reversed against ASCII -- an MCNK record reads 'KNCM' here, and that
 * is the same reversed spelling top_chunks() in extract_shadowmaps.py uses on
 * disk. A slot of 0 means the .adt carries no such sub-record; a value too big
 * to be inside one record is printed but never dereferenced. sv13: the whole
 * audit is on the diagnostic level, so a shipping run prints none of it and a
 * mode file with a v prints the same block sv10..sv12 printed. */
static void AuditHeader(const void *record_, const char *who)
{
    const unsigned char *hdr;
    const unsigned char *rec;
    unsigned v;
    char cc[5];
    char line[224];
    unsigned i;

    rec = (const unsigned char *)record_;
    hdr = rec + 8;
    FourccAt(rec, cc);
    snprintf(line, sizeof line,
             "audit %s: record %#x fourcc '%s' size %#x, hdr %#x flags %#x"
             " indexX %#x indexY %#x",
             who, (unsigned)(size_t)rec, cc, ((const unsigned *)rec)[1],
             (unsigned)(size_t)hdr, ((const unsigned *)hdr)[0],
             ((const unsigned *)hdr)[1], ((const unsigned *)hdr)[2]);
    LogVerb(line);

    for (i = 0; i < TABLE_COUNT; ++i) {
        v = ((const unsigned *)(hdr + k_table_ofs[i]))[0];
        cc[0] = 0;
        /* the value addresses the sub-record's BODY, so its fourcc is the 4
         * bytes EIGHT before it: the resolver's own math is `mov esi,[edx+ofs]
         * / lea esi,[esi+eax+8]` with eax = record = hdr - 8, i.e. hdr + ofs,
         * and extract_shadowmaps.py looks for b"MCSH" at hdr + ofs - 8. The
         * slot's own address is printed so a line can be re-read against the
         * .adt bytes without a second guess. */
        if (v >= 8u && v < AUDIT_OFS_MAX)
            FourccAt(hdr + v - 8u, cc);
        snprintf(line, sizeof line,
                 "audit %s: %s at hdr+%#x is %#x -> address %#x, fourcc at -8"
                 " '%s'",
                 who, k_table_cc[i], k_table_ofs[i], v,
                 (unsigned)(size_t)(hdr + v), cc);
        LogVerb(line);
    }
}

/* The handler's own types. The kernel calls the handler with the same
 * __stdcall convention every WINAPI export uses, and hands it the EXCEPTION_POINTERS
 * (excpt.h / winnt.h) pair, so the record's ExceptionInformation is read the
 * way a debugger reads it: [0] = 0 read / 1 write, [1] = the address. PFN_ADDVEH
 * is AddVectoredExceptionHandler's own shape, used once by InstallHandler. */
typedef DWORD (WINAPI *PFN_VEHPTR)(PEXCEPTION_POINTERS);
/* AddVectoredExceptionHandler answers with a handle to the handler it added, or
 * NULL when it refused, not with a status code, so the handle is what the
 * registration check below looks at */
typedef PVOID (WINAPI *PFN_ADDVEH)(DWORD, PFN_VEHPTR);

/* --------------------------------------------------- the instrument ***/

/* What the crash reports all say is one thing: an ACCESS_VIOLATION at 0x6B0FB7
 * -- the `mov ecx,[ebx]` inside BuildVertices whose ebx is the MCVT cursor the
 * resolver put at this+0xf14 -- and until sv9 the only way to know WHICH chunk
 * died was to trust that the last "last chunk seen" line in the log is it. This
 * handler does not trust that. On the first 0xC0000005 it prints the exception's
 * own two facts, the address the client dereferenced and where it did it, plus
 * the chunk this DLL last saw and what that object itself holds at this+0xf08
 * ..+0xf30 -- the resolver's answer for the same eight ofss, read off the
 * object rather than recomputed. Putting the faulting address and this+0xf14 on
 * one line says whether the client's resolver produced the poison cursor or a
 * write from this file did, in one run rather than by comparing log tails with
 * crash reports.
 *
 * The handler returns EXCEPTION_CONTINUE_SEARCH (0): it claims nothing, so the
 * SEH walk continues into the client's own reporter and the usual Errors\*.txt
 * report is still written. It is registered with AddVectoredExceptionHandler
 * resolved from kernel32 at runtime rather than called through a header,
 * because not every <windows.h> this file builds with declares that API -- VC98
 * 's winbase.h has no vectored-handler declaration at all -- and a missing
 * registration is logged and not fatal: the run keeps its normal report, only
 * the direct naming of the dying chunk is lost. */
static DWORD WINAPI DyingChunkHandler(PEXCEPTION_POINTERS info)
{
    PEXCEPTION_RECORD rec;
    const unsigned char *obj;
    const unsigned char *recp;
    const unsigned char *hdr;
    char msg[1024];
    unsigned code;

    rec = (info != NULL) ? info->ExceptionRecord : NULL;
    if (rec == NULL)
        return EXCEPTION_CONTINUE_SEARCH;
    code = rec->ExceptionCode;
    /* only the access violation every report shares, and only its first
     * occurrence: the client raises other conditions on paths of its own, and
     * one line is what is needed to know which chunk was being built */
    if (code != 0xC0000005u || g_handler_logged)
        return EXCEPTION_CONTINUE_SEARCH;
    g_handler_logged = 1;

    snprintf(msg, sizeof msg,
             "handler: ACCESS_VIOLATION %#x at %#x, %s of %#x -- the address the"
             " client dereferenced. last chunk seen: this %#x record %#x,"
             " fixup visits %u",
             code, (unsigned)(size_t)rec->ExceptionAddress,
             ((rec->NumberParameters > 0u && rec->ExceptionInformation[0] == 1u)
                  ? "write" : "read"),
             (unsigned)rec->ExceptionInformation[1],
             (unsigned)(size_t)g_last_this, (unsigned)(size_t)g_last_record,
             (unsigned)g_chunk_visits);
    LogMsg(msg);
    if (g_last_this == NULL || g_last_record == NULL) {
        /* the rung that hooks but never calls the fixup (mode c) has nothing to
         * name: the line above still says which instruction faulted and which
         * address it read, which is the whole question mode c asks */
        LogMsg(STAMP " handler: no chunk was captured, the hooked rung calls nothing -- read the crash report's own ecx for this");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    /* the object's own fields. A fault here is caught by the __except below and
     * logged, not raised again through the handler chain, so a bad `this` cannot
     * turn the diagnostic into a second mystery crash. */
    __try {
        obj = (const unsigned char *)g_last_this;
        recp = (const unsigned char *)g_last_record;
        hdr = recp + 8u;
        snprintf(msg, sizeof msg,
                 "handler: the object this chunk carries: hdr@%#x %#x, record"
                 "@%#x %#x, MCVT@%#x %#x, MCNR@%#x %#x, MCSH@%#x %#x, MCLY@%#x"
                 " %#x, MCAL@%#x %#x, MCRF@%#x %#x, MCLQ@%#x %#x, MCSE@%#x %#x."
                 " this+0xf14 %s the address the client read",
                 (unsigned)FIELD_HDR_OFS,
                 ((const unsigned *)(obj + FIELD_HDR_OFS))[0],
                 (unsigned)FIELD_RECORD,
                 ((const unsigned *)(obj + FIELD_RECORD))[0],
                 (unsigned)FIELD_PTR_MCVT,
                 ((const unsigned *)(obj + FIELD_PTR_MCVT))[0],
                 (unsigned)FIELD_PTR_MCNR,
                 ((const unsigned *)(obj + FIELD_PTR_MCNR))[0],
                 (unsigned)FIELD_PTR_MCSH,
                 ((const unsigned *)(obj + FIELD_PTR_MCSH))[0],
                 (unsigned)FIELD_PTR_MCLY,
                 ((const unsigned *)(obj + FIELD_PTR_MCLY))[0],
                 (unsigned)FIELD_PTR_MCAL,
                 ((const unsigned *)(obj + FIELD_PTR_MCAL))[0],
                 (unsigned)FIELD_PTR_MCRF,
                 ((const unsigned *)(obj + FIELD_PTR_MCRF))[0],
                 (unsigned)FIELD_PTR_MCLQ,
                 ((const unsigned *)(obj + FIELD_PTR_MCLQ))[0],
                 (unsigned)FIELD_PTR_MCSE,
                 ((const unsigned *)(obj + FIELD_PTR_MCSE))[0],
                 (((const unsigned *)(obj + FIELD_PTR_MCVT))[0]
                      == (unsigned)rec->ExceptionInformation[1]) ? "==" : "!=");
        LogMsg(msg);
        snprintf(msg, sizeof msg,
                 "handler: the dying chunk's header: hdr %#x flags %#x indexX"
                 " %#x indexY %#x, ofs MCVT %#x MCNR %#x MCLY %#x MCRF %#x MCAL"
                 " %#x MCSH %#x MCSE %#x MCLQ %#x, global col %#x row %#x",
                 (unsigned)(size_t)hdr,
                 ((const unsigned *)hdr)[0],
                 ((const unsigned *)hdr)[1],
                 ((const unsigned *)hdr)[2],
                 ((const unsigned *)(hdr + 0x14u))[0],
                 ((const unsigned *)(hdr + 0x18u))[0],
                 ((const unsigned *)(hdr + 0x1cu))[0],
                 ((const unsigned *)(hdr + 0x20u))[0],
                 ((const unsigned *)(hdr + 0x24u))[0],
                 ((const unsigned *)(hdr + FIELD_MCSH_OFS))[0],
                 ((const unsigned *)(hdr + 0x58u))[0],
                 ((const unsigned *)(hdr + 0x60u))[0],
                 ((const unsigned *)(obj + FIELD_GLOBAL_COL))[0],
                 ((const unsigned *)(obj + FIELD_GLOBAL_ROW))[0]);
        LogMsg(msg);
    } __except(1) {
        LogMsg(STAMP " handler: the dying chunk's fields could not be read (the handler faulted inside itself, caught here), so only the line above is usable");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Registering it. AddVectoredExceptionHandler is a kernel32 export, resolved
 * here rather than called through a header for the reason above. CALL_FIRST (1)
 * is what makes it first in the vectored chain, i.e. asked before the SEH
 * handlers the client's own modules carry, which is the only way to see the
 * fault before something else swallows it. sv13: the lines about REGISTERING are
 * diagnostic level, so a shipping run does not print that the instrument is up;
 * the lines the handler itself writes when a launch dies are always on, which is
 * what keeps a crash diagnosable whatever the level is. */
static void InstallHandler(void)
{
    HMODULE k;
    PVOID tag;
    PFN_ADDVEH adder;
    char msg[256];

    k = GetModuleHandleA("kernel32.dll");
    if (k == NULL) {
        LogVerb(STAMP " handler: no kernel32 to resolve AddVectoredExceptionHandler from, the dying-chunk instrument stays off");
        return;
    }
    adder = (PFN_ADDVEH)GetProcAddress(k, "AddVectoredExceptionHandler");
    if (adder == NULL) {
        LogVerb(STAMP " handler: kernel32 does not export AddVectoredExceptionHandler, the dying-chunk instrument stays off");
        return;
    }
    tag = adder(1u, DyingChunkHandler);
    if (tag == NULL) {
        snprintf(msg, sizeof msg, STAMP " handler: AddVectoredExceptionHandler refused"
                 " (GetLastError %#x), the dying-chunk instrument stays off",
                 GetLastError());
        LogVerb(msg);
        return;
    }
    g_handler_on = 1;
    LogVerb(STAMP " handler: first-chance vectored handler registered, so the next access violation logs the chunk that was being built and the address the client read (the client's own report is still written afterwards)");
}

/* ------------------------------------------------------- the sun's arc **/

/* sv14. The day-night file builds four static key tables that its sun state
 * reads every rendered frame through one evaluator, 0x6cf6c0 (keys, count,
 * phase), and derives the sun's position as (cos a sin p, sin a sin p, cos p)
 * of the same phase (benilla-re systems/lighting/celestial.md#static-tracks
 * and #sun-direction). The stock tables make no arc: the lighting azimuth is
 * 225 deg on all four keys and the disc azimuth 45 deg on all three, so the
 * sun rocks between two polar angles on one fixed bearing -- a wheel in a
 * vertical plane, rising and setting on the same bearing -- and the world's
 * shading never turns. What is written here are key sets that obey the two
 * rules the evaluator imposes: the azimuth runs monotonically through the
 * wrap and completes exactly 360 deg over the cycle, because 0x6cf6c0 lerps in
 * VALUE space and its cyclic search wraps in TIME, so a value that wraps back
 * is a backwards sweep and a value that leaves a gap is a jump at midnight;
 * and the light azimuth is kept the disc azimuth plus 180 with the light
 * polar the disc polar subtracted from 180, so the direction the shading comes
 * from and the sun you can see are the same sun all day. The count is the
 * edx to the evaluator, not a length inside the block, and the blocks are
 * adjacent -- 0xce8d90 + 0x20 is the polar light track, 0xce8d4c + 0x18 is the
 * disc polar one -- so the key times sit right after the values at kv + n and
 * a preset that added a key would write over its neighbour: every preset keeps
 * the stock 4 / 4 / 3 / 5.
 *
 * The azimuths are left unwrapped (495 deg, not 135) because wrapping is what
 * reverses the sweep; sin and cos read them the same either way. */
#define SUN_TOD_STATE  0xce9b60u   /* the day-night state's block of globals */
#define SUN_TOD_OFF    0x04u       /* +4: time of day, kept in [0,1)         */
#define SUN_HMIN_DAY   2880u       /* half-minutes in a day, the unit the     */
                                   /* LightParams records are keyed by        */
#define SUN_EVAL_ADDR  0x6cf6c0u   /* named only in a log line               */

/* sv15's test hook: the game clock object and the one float that is the day's
 * speed. benilla-re systems/gametime.md#clock-state: the clock is one object at
 * 0xce8538, 0x70 bytes, and +0x2c is its scale in game minutes per real second,
 * initialised to 1/60. 0x643a70's day phase is (tick - [+0x38]) * 0.001f *
 * [+0x2c] + [+0x3c] times 1/1440, so the real seconds since the last minute are
 * scaled by it, and 0x6435a0's per-frame advance adds dt * [+0x2c] to the
 * minute accumulator, so that one float is the speed of everything the clock
 * drives: the time of day the sun's phase comes from, the minute and hour
 * Script::GetGameTime returns, and the day count. 0x643820 stores what the
 * server sent clamped to [1/60, 60] -- it is reached from the opcode 0x47
 * handler 0x6c5de0 and from the 0x42 handler 0x6c5e80, the packet that also
 * carries the time -- so the server re-sends it through the whole session and
 * the same clamp is applied here. */
#define GT_CLOCK_STATE 0xce8538u  /* the client's game clock object           */
#define GT_SCALE_OFF   0x2cu       /* +0x2c: game minutes per real second     */
#define GT_SCALE_LO    (1.0f / 60.0f) /* 0x3c888889, 0x643820's low bound      */
#define GT_SCALE_HI    60.0f            /* 0x42700000, its high bound          */
#define GT_SCALE_ADDR  (GT_CLOCK_STATE + GT_SCALE_OFF) /* 0xce8564, the float  */
#define GT_CLOCK_TXT   "0xce8538"      /* the same object, spelled in a line   */

/* the four tracks, and their stock keys in degrees. The counts are the
 * evaluator's own: 4 / 4 for the lighting sun, 3 / 5 for the disc. */
static const unsigned int sun_addr[4] = { 0xce8d90u, 0xce8db0u, 0xce8d4cu, 0xce8d64u };
static const unsigned int sun_n[4] = { 4, 4, 3, 5 };
static const float sun_stock_v[4][5] = {
    { 225.0f, 225.0f, 225.0f, 225.0f },
    { 127.0f, 110.0f, 127.0f, 110.0f },
    {  45.0f,  45.0f,  45.0f },
    { 100.0f,   5.0f,   5.0f,   5.0f, 100.0f }
};
static const float sun_stock_t[4][5] = {
    { 0.0f, 0.25f, 0.5f, 0.75f },
    { 0.0f, 0.25f, 0.5f, 0.75f },
    { 0.2291667f, 0.5f, 0.8958333f },
    { 0.2291667f, 0.4965278f, 0.5f, 0.5034722f, 0.8958333f }
    /* sv15: the sun disc's polar keys are 11:55, 12:00 and 12:05, not three
     * keys at noon. celestial.md says the day-night file builds these from the
     * constants at 0x811510 with 0.0034722 (five minutes) added and subtracted,
     * and the pin below compares the live client tracks against this table to
     * 0.0001 -- a table whose noon keys are all 0.5 calls the client's own
     * tracks neither-stock and not-unbuilt, which refuses the pin and leaves
     * the sun alone. sv14 had all three at 0.5, so its arc never got written
     * on a client that had already built its day-night file. */
};

/* A sun preset: the key times and values for one shape, and the window of the
 * day it answers for, in half-minutes. Times are the fraction of the cycle the
 * clock's phase means; angles are degrees, which SunPut turns into the
 * radians the client's floats hold. Two rules, both read off the stock tables
 * (benilla-re systems/lighting/celestial.md#static-tracks), and both of them
 * wrong in sv14's tables, which is what an in-game visit showed:
 *
 * the polar angle is measured FROM THE ZENITH, so a body's height over the
 * horizon is 90 - polar. The sun disc's stock polar is 5 deg at noon (85 deg
 * UP) and 100 deg at 05:30 and 21:30 (10 deg UNDER the horizon), so stock has
 * no sun disc after 20:30 and its sunset sits on the horizon there -- the
 * brighter horizon at 21:00 the player sees with no DLL is that. The lighting
 * sun's polar is the same angle of the direction light TRAVELS, so stock keeps
 * it between 110 and 127 deg, always past 90: the light direction always
 * points DOWN, and its source sits at the antipode, pol - 90 deg above the
 * horizon, 20 to 37 deg up all day. sv14 put pol_l as low as 80 deg, under
 * 90, which points the light direction UP: the diffuse and specular terms
 * then invert, the ground reads mirrored and over-bright and the shading
 * seems to come from the far side of the sun -- the exact symptom a morning
 * visit showed. sv15 keeps the polar keys as stock's own, unchanged, so the
 * day's height, its rise at 06:11, its set at 20:30 and its strength match
 * stock hour for hour, and only the azimuth moves.
 *
 * and the azimuth keeps stock's own relation between the two tracks: the light
 * azimuth is the disc azimuth plus 180, so the shading comes from where the
 * disc is. Stock has 45 deg on the disc and 225 on the light, and both read on
 * the map as NORTH WEST -- the fixed point every arc is pinned to.
 *
 * One arc, not four. Azimuth runs 0 = W, 90 = N, 180 = E, 270 = S, clockwise,
 * so a sun that rises in the south west is at 315 deg and the same sun setting
 * in the north west is at 405 deg, one quarter turn later, and it passes due
 * west at noon -- which is what stock's own hours and heights give once the
 * azimuth is free to move at all. Four windows holding four different arcs
 * were never one sun crossing one sky, and the visits showed it: the disc, the
 * ground and the objects disagreed about where the light was. */
static const struct SunPreset {
    const char name[16];
    unsigned short h0, h1;                 /* the clock's window             */
    float az_l[4], pol_l[4];               /* the lighting sun's keys        */
    float az_d[3], pol_d[5];               /* the sun disc's keys            */
    float t_az_l[4], t_pol_l[4];           /* their key times                */
    float t_az_d[3], t_pol_d[5];
} sun_presets[] = {
    { "arc-swnw",    0, 2400,
      { 495.0f, 517.5f, 540.0f, 585.0f }, { 127.0f, 110.0f, 127.0f, 110.0f },
      { 315.0f, 360.0f, 405.0f },         { 100.0f,   5.0f,   5.0f,   5.0f, 100.0f },
      { 0.2291667f, 0.3645833f, 0.5f, 0.8958333f },
      { 0.0f, 0.25f, 0.5f, 0.75f },
      { 0.2291667f, 0.5f, 0.8958333f },
      { 0.2291667f, 0.4965278f, 0.5f, 0.5034722f, 0.8958333f } },
    { "arc-stock",   2400, SUN_HMIN_DAY,
      { 225.0f, 225.0f, 225.0f, 225.0f }, { 127.0f, 110.0f, 127.0f, 110.0f },
      {  45.0f,  45.0f,  45.0f },         { 100.0f,   5.0f,   5.0f,   5.0f, 100.0f },
      { 0.0f, 0.25f, 0.5f, 0.75f },
      { 0.0f, 0.25f, 0.5f, 0.75f },
      { 0.2291667f, 0.5f, 0.8958333f },
      { 0.2291667f, 0.4965278f, 0.5f, 0.5034722f, 0.8958333f } }
};
#define SUN_PRESETS ((unsigned)(sizeof sun_presets / sizeof sun_presets[0]))
#define SUN_STOCK_PRESET (SUN_PRESETS - 1u) /* sv15: writes the stock keys back */

/* pi/180 and 180/pi as the float the client's own constants round to. The DLL
 * carries no math library (it links kernel32 and ntdll only), and the sun's
 * sine and cosine are the client's: all this does is turn degrees into the
 * radians the key floats hold. */
#define SUN_D2R 0.01745329251994329186404805098050064450281532733429333166503906250f
#define SUN_R2D 57.2957795130823246137866726764679354908450925425769347584686279296875f

static int g_sun_forced = -1;     /* shadow-mod.sun's name, -1 is the clock  */
static int g_sun_off;             /* shadow-mod.sun said off                */
static int g_sun_pin;             /* 0 not checked, 1 pinned, -1 not pinned */
static int g_sun_applied = -1;    /* preset index last written              */
static unsigned g_sun_said_preset;  /* one line per preset applied, bit per  */
                                    /* index: a clock crossing says so once  */
static int g_sun_said_stock;
static int g_sun_said_off;
static int g_sun_said_back;
/* how the sun lines spend the log. A chunk visit is hot, and at the test speeds
 * the clock crosses a window boundary every few seconds, so the line with all
 * 22 live keys in it is only what the first writes of a session get; after that
 * every write gets the short line, which carries the numbers that decide -- the
 * disc's bearing and elevation and the shading's. Both are capped, because the
 * log itself is capped: TrimLog keeps the newest LOG_KEEP_BYTES, and 2,000
 * per-chunk header dumps alone are more than that, which is why the sun lines
 * were unreadable in a fast session. The per-chunk dump is what grew, so a q in
 * the mode file is what puts it back. */
static unsigned g_sun_full;                /* giant sun lines this session logged */
static unsigned g_sun_short;               /* short sun lines this session logged */
static unsigned g_sun_lost;                /* lost-key lines this session logged  */
#define SUN_FULL_LINES  4u                 /* giant readbacks per session         */
#define SUN_SHORT_LINES 60u                /* short status lines per session      */
#define SUN_LOST_LINES  4u                 /* lost-key lines per session          */

/* the clock, in the unit the presets' windows use. The DLL never makes a
 * clock of its own: this is the float the client stamps every rendered world
 * frame at 0xce9b64, the same one the light table build at 0x6d2210 reads
 * while turning it into half-minutes. An all-zero block reads 0, which is
 * midnight and the arc's window, so the first chunk visit already writes an
 * arc and every visit after it reads the clock again: a 0 float is read the
 * same as midnight, so the difference between "not stamped yet" and "midnight"
 * is not visible here and does not need to be -- the keys are written either
 * way, and a visit whose clock arrived later corrects the choice. */
static unsigned SunHalfMinute(unsigned *clock_out)
{
    const float *st = (const float *)(size_t)SUN_TOD_STATE;
    float tod;
    unsigned hmin;
    tod = st[SUN_TOD_OFF / 4u];
    if (!(tod >= 0.0f) || tod >= 1.0f)
        tod = 0.0f;
    if (clock_out) {
        *clock_out = (unsigned)(tod * 10000.0f + 0.5f);
        if (*clock_out > 9999u)
            *clock_out = 9999u;
    }
    hmin = (unsigned)(tod * (float)SUN_HMIN_DAY + 0.5f);
    if (hmin >= SUN_HMIN_DAY)
        hmin = SUN_HMIN_DAY - 1u;
    return hmin;
}

static int SunPick(unsigned hmin)
{
    unsigned i;
    for (i = 0; i < SUN_PRESETS; ++i)
        if (hmin >= sun_presets[i].h0 && hmin < sun_presets[i].h1)
            return (int)i;
    return -1;
}

/* do the four tracks still hold the stock keys, within the degree a float
 * rounds to? also says whether an arc's own keys were lost since, which is
 * what a rebuilt day-night file looks like */
static int SunHoldsStock(void)
{
    unsigned tr, i;
    const float *kv;
    int same = 1;
    for (tr = 0; tr < 4u; ++tr) {
        kv = (const float *)(size_t)sun_addr[tr];
        for (i = 0; i < sun_n[tr]; ++i) {
            double deg = (double)kv[i] * SUN_R2D;
            if (deg < (double)sun_stock_v[tr][i] - 0.02 ||
                deg > (double)sun_stock_v[tr][i] + 0.02)
                same = 0;
            if (kv[sun_n[tr] + i] < sun_stock_t[tr][i] - 0.0001f ||
                kv[sun_n[tr] + i] > sun_stock_t[tr][i] + 0.0001f)
                same = 0;
        }
    }
    return same;
}

/* the tracks must be the ones this whole block was read from before anything
 * is written: stock values, or an all-zero block the sun state has not built
 * yet, since 0x6d3a10 and 0x6d3b80 build these at first use behind the guard
 * bits at 0x6d3b44 and 0x6d3ba4 and that is after this DLL was loaded. A
 * track holding neither is not the shape 0x6cf6c0 names, and the sun is left
 * alone. */
static int SunPin(void)
{
    unsigned tr, i;
    int same, zero;
    const float *kv;
    char msg[384];
    for (tr = 0; tr < 4u; ++tr) {
        same = 1;
        zero = 1;
        kv = (const float *)(size_t)sun_addr[tr];
        for (i = 0; i < sun_n[tr]; ++i) {
            double deg = (double)kv[i] * SUN_R2D;
            if (deg < (double)sun_stock_v[tr][i] - 0.02 ||
                deg > (double)sun_stock_v[tr][i] + 0.02)
                same = 0;
            if (kv[sun_n[tr] + i] < sun_stock_t[tr][i] - 0.0001f ||
                kv[sun_n[tr] + i] > sun_stock_t[tr][i] + 0.0001f)
                same = 0;
            if (kv[i] != 0.0f || kv[sun_n[tr] + i] != 0.0f)
                zero = 0;
        }
        if (!same && !zero) {
            snprintf(msg, sizeof msg,
                     STAMP " sun: the track at %#x holds neither the stock keys this build was"
                            " read from nor an unbuilt zero block, so it is not the shape the"
                            " sun state reads: the sun key tables are left as they are and the"
                            " shadows keep working",
                     sun_addr[tr]);
            LogMsg(msg);
            return 0;
        }
    }
    return 1;
}

/* one preset into the client's own shapes: values first, then the same count
 * of times, which is where the next track begins and why a preset that added
 * a key would write over its neighbour. */
static void SunPut(unsigned tr, const float *t, const float *deg)
{
    float *kv = (float *)(size_t)sun_addr[tr];
    unsigned i;
    for (i = 0; i < sun_n[tr]; ++i) {
        kv[i] = deg[i] * SUN_D2R;
        kv[sun_n[tr] + i] = t[i];
    }
}

static void SunWrite(unsigned which)
{
    const struct SunPreset *p = &sun_presets[which];
    SunPut(0u, p->t_az_l,  p->az_l);
    SunPut(1u, p->t_pol_l, p->pol_l);
    SunPut(2u, p->t_az_d,  p->az_d);
    SunPut(3u, p->t_pol_d, p->pol_d);
}

/* the client's own evaluator rule, replayed over whatever its tracks hold
 * NOW rather than over the tables this file means to write, which is what
 * separates a write that did not land, a track whose key times got a
 * neighbour's, and a phase that is not the hour the UI shows -- all of which
 * read the same in game and say nothing until the live bytes are read back.
 * benilla-re systems/lighting/celestial.md#tracks: the phase is clamped to
 * [0,1] with NaN as 1; the first key i whose time is at or above the phase
 * is taken, cyclically, with j the key before it -- the last key when i is
 * the first or when no key qualifies; with d = time[i] - time[j] a |d| below
 * 0.001 returns value[j] whole, and a negative d and a negative e = phase -
 * time[j] each take +1 before the lerp, which is what makes a pair whose
 * times straddle midnight a rising leg across it. the angles are the
 * radians the client holds, so they are turned back into degrees here. */
static float SunEvalTrack(unsigned tr, float phase)
{
    const float *kv = (const float *)(size_t)sun_addr[tr];
    unsigned n = sun_n[tr];
    unsigned i, j, k;
    float d, e, tj;

    if (!n)
        return 0.0f;
    if (phase < 0.0f)
        phase = 0.0f;
    else if (!(phase >= 0.0f))
        phase = 1.0f; /* the NaN's, the client's own answer */
    else if (phase > 1.0f)
        phase = 1.0f;
    i = n;
    for (k = 0; k < n; ++k) {
        if (phase <= kv[n + k]) {
            i = k;
            break;
        }
    }
    if (i >= n)
        i = 0; /* no key qualified: the leg from the last key to the first */
    j = (i + n - 1u) % n;
    tj = kv[n + j];
    d = kv[n + i] - tj;
    if (d > -0.001f && d < 0.001f)
        return kv[j] * SUN_R2D;
    if (d < 0.0f)
        d += 1.0f;
    e = phase - tj;
    if (e < 0.0f)
        e += 1.0f;
    return (kv[j] + (kv[i] - kv[j]) * (e / d)) * SUN_R2D;
}

/* a float's degrees to whole degrees, away from zero at the half. The readback
 * prints degrees the client's radians round-trip to, and a truncating cast
 * turns 495 deg, held as the nearest float's radians, into 494 on the line --
 * which reads as a key one degree off its own table. Rounding is what makes
 * the line say the key it holds. */
static int SunRoundDeg(float deg)
{
    return (int)(deg >= 0.0f ? deg + 0.5f : deg - 0.5f);
}

/* degrees to the map's, in the frame the stock sun pins: the client's azimuth
 * 0 is due west and the angle grows through north, so a bearing is the
 * azimuth with whole turns taken out. */
static int SunBearing(float az)
{
    int b = SunRoundDeg(az) % 360;
    if (b < 0)
        b += 360;
    return b;
}

/* is what this DLL wrote still in the client? sv14's per-visit check looks for
 * the STOCK keys, which is enough to catch the day-night file rebuilding its
 * tracks, but it says nothing about a write that landed at the wrong address
 * or a client that put its own values there that are not stock. This one asks
 * what the armed preset's own table says each first slot holds, so a lost
 * write is named by what replaced it, whatever that was, and the four live
 * readings are kept for the line that prints them next to the four wanted.
 * One key per track, so four float reads per chunk visit: no cost. */
static float g_sun_live4[4];

static int SunKeysLive(unsigned which)
{
    const struct SunPreset *p = &sun_presets[which];
    unsigned k;
    int ok = 1;

    for (k = 0; k < 4u; ++k) {
        const float *kv = (const float *)(size_t)sun_addr[k];
        double want, deg, d;
        if (k == 0u)
            want = (double)p->az_l[0];
        else if (k == 1u)
            want = (double)p->pol_l[0];
        else if (k == 2u)
            want = (double)p->az_d[0];
        else
            want = (double)p->pol_d[0];
        deg = (double)kv[0] * SUN_R2D;
        g_sun_live4[k] = (float)deg;
        d = deg - want;
        if (d < 0.0)
            d = -d;
        if (d > 0.02)
            ok = 0;
    }
    return ok;
}

/* what the client is showing right now, from its own bytes: the four tracks
 * as they read now, and the two bearings that come out of them at the
 * client's own phase. this is the one line that tells an in-game visit which
 * side of the write is wrong, because a disc in the wrong place, a shading
 * bearing 180 deg off the disc, and a bearing that never matches the sky
 * under any reading are three different facts about live memory and the
 * azimuth's handedness, and only the live bytes can sort them out. one line
 * per preset window, next to the line that names the preset written -- and
 * only for the first SUN_FULL_LINES writes: after that the same function
 * prints the short line, the last five numbers of the giant one with the
 * phase and the preset named, because at the test speeds the day crosses a
 * window boundary every few seconds and a session that watched a whole
 * 24-real-second day would otherwise print the 22 live keys two hundred
 * times and push everything else out of the log's kept tail. */
static void SunReport(unsigned which, unsigned hmin)
{
    const struct SunPreset *p = &sun_presets[which];
    const float *st = (const float *)(size_t)SUN_TOD_STATE;
    float phase = st[SUN_TOD_OFF / 4u];
    float az_l, pol_l, az_d, pol_d;
    int disc_b, disc_el, src_b, src_el, gap;
    int big;
    char msg[1792]; /* the line prints all four tracks' live key pairs plus the
                      * phase, the preset name and the two bearings: rendered at
                      * the widest addresses and a long preset name it runs past
                      * 1410, so a 1280 buffer cut its tail off in the log */

    int dv[4][5];
    unsigned tv[4][5];
    unsigned tr, i;
    const float *kv;

    if (g_sun_full >= SUN_FULL_LINES && g_sun_short >= SUN_SHORT_LINES)
        return;                      /* both kinds of sun line are used up    */
    big = (g_sun_full < SUN_FULL_LINES);
    if (big)
        ++g_sun_full;
    else
        ++g_sun_short;

    if (!(phase >= 0.0f) || phase >= 1.0f)
        phase = 0.0f;
    for (tr = 0; tr < 4u; ++tr) {
        kv = (const float *)(size_t)sun_addr[tr];
        for (i = 0; i < sun_n[tr]; ++i) {
            dv[tr][i] = SunRoundDeg(kv[i] * SUN_R2D);
            tv[tr][i] = (unsigned)(kv[sun_n[tr] + i] * 10000.0f + 0.5f);
        }
    }
    az_l = SunEvalTrack(0u, phase);
    pol_l = SunEvalTrack(1u, phase);
    az_d = SunEvalTrack(2u, phase);
    pol_d = SunEvalTrack(3u, phase);
    disc_b = SunBearing(az_d);
    src_b = SunBearing(az_l + 180.0f); /* the direction light comes from is the
                                        * antipode of the direction it travels,
                                        * celestial.md#sun-direction */
    disc_el = 90 - SunRoundDeg(pol_d);
    src_el = SunRoundDeg(pol_l) - 90;
    gap = src_b - disc_b;
    if (gap > 180)
        gap -= 360;
    if (gap < -180)
        gap += 360;

    if (big) {
        snprintf(msg, sizeof msg,
             STAMP " sun read back: what the client's own tracks hold now, and what its evaluator"
                    " makes them give at the client's own phase. the phase at 0x%x+%x is 0.%04u,"
                    " half-minute %u of %u, and %s is the preset this visit wrote. the live keys,"
                    " values then their times, degrees as the radians at each address read back:"
                    " light azimuth 0x%x = %d %d %d %d deg at 0.%04u 0.%04u 0.%04u 0.%04u;"
                    " light polar 0x%x = %d %d %d %d deg at 0.%04u 0.%04u 0.%04u 0.%04u;"
                    " disc azimuth 0x%x = %d %d %d deg at 0.%04u 0.%04u 0.%04u;"
                    " disc polar 0x%x = %d %d %d %d %d deg at 0.%04u 0.%04u 0.%04u 0.%04u 0.%04u."
                    " interpolating those at the phase gives the disc at azimuth %d deg and polar"
                    " %d deg and the light azimuth %d deg and polar %d deg, so the disc sits at"
                    " bearing %d deg, %d deg above the horizon, and the shading comes from bearing"
                    " %d deg, %d deg high: they are %d deg apart. read the two bearings against the"
                    " sky you are looking at: a disc bearing that matches makes the tables and the"
                    " handedness right and the arc's own angles are what to change; a shading bearing"
                    " 180 apart from the disc is the light pointing at the sun rather than away from"
                    " it; and a disc polar that should keep it below the horizon at a night hour and"
                    " does not is a key time belonging to another track. the azimuths are 0 = west,"
                    " 90 = north, 180 = east, 270 = south, the frame the stock sun's own NORTH WEST"
                    " pins",
             SUN_TOD_STATE, SUN_TOD_OFF,
             (unsigned)(phase * 10000.0f + 0.5f), hmin, SUN_HMIN_DAY, p->name,
             sun_addr[0],
             dv[0][0], dv[0][1], dv[0][2], dv[0][3],
             tv[0][0], tv[0][1], tv[0][2], tv[0][3],
             sun_addr[1],
             dv[1][0], dv[1][1], dv[1][2], dv[1][3],
             tv[1][0], tv[1][1], tv[1][2], tv[1][3],
             sun_addr[2],
             dv[2][0], dv[2][1], dv[2][2],
             tv[2][0], tv[2][1], tv[2][2],
             sun_addr[3],
             dv[3][0], dv[3][1], dv[3][2], dv[3][3], dv[3][4],
             tv[3][0], tv[3][1], tv[3][2], tv[3][3], tv[3][4],
             SunRoundDeg(az_d), SunRoundDeg(pol_d), SunRoundDeg(az_l), SunRoundDeg(pol_l),
             disc_b, disc_el, src_b, src_el, gap);
    } else {
        snprintf(msg, sizeof msg,
                 STAMP " sun status: the client's own phase at 0x%x+%x is 0.%04u, half-minute %u"
                 " of %u, and %s was the key set this visit wrote into the live keys. what the"
                 " client's evaluator makes those bytes give: the disc at azimuth %d deg and"
                 " polar %d deg and the light at azimuth %d deg and polar %d deg, so the disc"
                 " sits at bearing %d deg, %d deg above the horizon, and the shading comes from"
                 " bearing %d deg, %d deg high: they are %d deg apart. the live keys themselves"
                 " are in the giant line, which this session already printed %u times"
                 ,
                 SUN_TOD_STATE, SUN_TOD_OFF,
                 (unsigned)(phase * 10000.0f + 0.5f), hmin, SUN_HMIN_DAY, p->name,
                 SunRoundDeg(az_d), SunRoundDeg(pol_d), SunRoundDeg(az_l), SunRoundDeg(pol_l),
                 disc_b, disc_el, src_b, src_el, gap, g_sun_full);
    }
    LogMsg(msg);
}

/* shadow-mod.sun, next to the mode file: absent or clock means the clock
 * decides, which is the concept; off means leave the stock sun alone; a
 * preset name forces that one, which is how a preset is watched without
 * waiting for the clock to come round to it. */
static void SunReadChoice(void)
{
    char path[MAX_PATH];
    char tok[24];
    FILE *f;
    int c, n = 0;
    unsigned i;

    snprintf(path, sizeof path, "%s\\shadow-mod.sun", g_dir);
    f = fopen(path, "r");
    if (!f)
        return;
    while (n < 23 && (c = fgetc(f)) != EOF) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (n)
                break;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
            (c >= '0' && c <= '9'))
            tok[n++] = (char)c;
    }
    fclose(f);
    tok[n] = 0;
    if (!n)
        return;
    if (CiEqName(tok, "off") || CiEqName(tok, "stock")) {
        g_sun_off = 1;
        return;
    }
    if (CiEqName(tok, "clock"))
        return;
    for (i = 0; i < SUN_PRESETS; ++i) {
        if (CiEqName(tok, sun_presets[i].name)) {
            g_sun_forced = (int)i;
            return;
        }
    }
    LogMsg(STAMP " sun: shadow-mod.sun says something that is not clock, off or one of the"
           " preset names, so the clock decides");
}

/* keep the arc up. The preset is picked from the client's own clock at every
 * chunk visit, so it settles onto the real clock as soon as the world frames
 * start stamping the block, and the visit is also what puts the key set back
 * if the day-night file rebuilt its tracks behind the guard bits after
 * install. The clock crossing a window boundary applies the next preset and
 *  says so once per preset: a chunk visit is hot, so nothing here logs twice
 * for the same preset. */
static void SunKeepArc(void)
{
    unsigned hmin, clock = 0, which;
    char msg[1280]; /* the applied line names the clock, the preset and the arc's
                      * own numbers, and sv15's stock-window line also explains
                      * why the keys get written back: both run past 1024 */

    if (g_sun_off) {
        if (!g_sun_said_off) {
            g_sun_said_off = 1;
            LogMsg(STAMP " sun: shadow-mod.sun said off, so the stock sun key tables are left"
                   " alone and the world keeps its fixed 45 deg bearing");
        }
        return;
    }
    if (!g_sun_pin)
        g_sun_pin = SunPin() ? 1 : -1;
    if (g_sun_pin < 0)
        return;
    hmin = SunHalfMinute(&clock);
    if (g_sun_forced >= 0)
        which = (unsigned)g_sun_forced;
    else {
        int w = SunPick(hmin);
        if (w < 0)
            w = (int)SUN_STOCK_PRESET;
        which = (unsigned)w;
    }
    /* the same preset armed again: before anything else, is its own first key
     * still what the client holds? a visit that finds it gone found a write
     * that was REPLACED between visits, which is a different fact from the
     * write never landing -- and the two halves are what sort them out. The
     * giant read back right after a write says the write landed; this line
     * says it stayed, and the four live readings say what replaced it. */
    if ((int)which == g_sun_applied && !SunKeysLive(which)) {
        if (g_sun_lost < SUN_LOST_LINES) {
            ++g_sun_lost;
            snprintf(msg, sizeof msg,
                     STAMP " sun keys lost: %s was armed again and the client's own tables no"
                            " longer hold what this DLL wrote there. the first key of each track,"
                            " as the live radians read back, then what the preset's own table"
                            " wants there: light azimuth 0x%x reads %d deg and wants %d;"
                            " light polar 0x%x reads %d deg wants %d; disc azimuth 0x%x reads"
                            " %d deg wants %d; disc polar 0x%x reads %d deg wants %d. the stock"
                            " keys are 225 127 45 100, so a live four of exactly those is the"
                            " day-night file building its own tracks behind the guard bits at"
                            " 0x6d3a10 and 0x6d3b80 and that guard's state is what to look at"
                            " next; any other four is a write that went somewhere the client"
                            " does not read. this session prints this line %u of %u times",
                     sun_presets[which].name,
                     sun_addr[0], SunRoundDeg(g_sun_live4[0]),
                     (int)sun_presets[which].az_l[0], sun_addr[1],
                     SunRoundDeg(g_sun_live4[1]), (int)sun_presets[which].pol_l[0],
                     sun_addr[2], SunRoundDeg(g_sun_live4[2]),
                     (int)sun_presets[which].az_d[0], sun_addr[3],
                     SunRoundDeg(g_sun_live4[3]), (int)sun_presets[which].pol_d[0],
                     (unsigned)g_sun_lost, (unsigned)SUN_LOST_LINES);
            LogMsg(msg);
        }
    }
    if (which >= SUN_STOCK_PRESET) {
        if (!g_sun_said_stock) {
            g_sun_said_stock = 1;
            snprintf(msg, sizeof msg,
                     STAMP " sun: the client's clock at 0x%x+%x reads %02u:%02u (the float is 0.%04u,"
                            " half-minute %u of %u), which is %s's window: the stock sun keys are WRITTEN"
                            " back into the client's own tables at 0x%x and 0x%x, so an arc another"
                            " continent or an earlier hour left cannot survive here. sv14 wrote nothing"
                            " in this window and that was wrong twice over: the four tracks are one"
                            " global block and the day-night file builds them once behind the guard bits"
                            " at 0x6d3a10 and 0x6d3b80, so the last arc written keeps living in every"
                            " hour that does not overwrite it -- and outside a preset's own key span"
                            " 0x6cf6c0 takes the leg from its last key back to its first and interpolates"
                            " ALONG it, so hours the preset never covered get an extrapolated sun whose"
                            " light polar can come back above 90 deg, a light direction pointing above"
                            " the horizon, which zeroes the diffuse term of every up-facing normal and"
                            " reads as mirrored, reflected ground. the world keeps its stock sun here,"
                            " and enter at another hour and the next line names the arc that gets written",
                     SUN_TOD_STATE, SUN_TOD_OFF, hmin / 120u, (hmin % 120u) / 2u, clock, hmin, SUN_HMIN_DAY,
                     sun_presets[which].name, sun_addr[0], sun_addr[2]);
            LogMsg(msg);
        }
        SunWrite(which);
        SunReport(which, hmin);
        g_sun_applied = (int)which;
        return;
    }
    if ((int)which == g_sun_applied) {
        if (!SunHoldsStock())
            return;
        /* the tracks are back to the stock keys under a preset that is not
         * stock: the day-night file built its tracks after install. One line,
         * then the keys are put back, and the arc carries on. */
        if (!g_sun_said_back && which != SUN_STOCK_PRESET) {
            /* sv15 wrote the stock keys itself in the stock window, so the
             * stock keys under the stock preset are expected and say nothing:
             * only an arc whose own keys went back to stock is a rebuild. */
            g_sun_said_back = 1;
            snprintf(msg, sizeof msg,
                     STAMP " sun: the key tables at 0x%x and 0x%x hold the stock keys again"
                            " under %s, so the day-night file built its tracks after install:"
                            " the keys are written again here, and the next chunk visit keeps"
                            " them, so a stock sun in game after this line is that rebuild",
                     sun_addr[0], sun_addr[2], sun_presets[which].name);
            LogMsg(msg);
        }
        SunWrite(which);
        SunReport(which, hmin);
        return;
    }
    SunWrite(which);
    g_sun_applied = (int)which;
    if (!(g_sun_said_preset & (1u << which))) {
        g_sun_said_preset |= (1u << which);
        snprintf(msg, sizeof msg,
                 STAMP " sun arc: the client's clock at 0x%x+%x reads %02u:%02u (the float is 0.%04u,"
                        " half-minute %u of %u), so %s is the preset written into the client's own key"
                        " tables at 0x%x (light azimuth and polar), 0x%x (disc azimuth) and"
                        " 0x%x (disc polar): the disc's azimuth runs from %u deg to %u deg,"
                        " %u deg over the day, and its polar angle goes from %u deg through"
                        " %u deg back to %u deg, so the sun crosses the horizon at both ends"
                        " instead of rocking on one bearing; the light azimuth is the disc azimuth plus"
                        " 180, so the shading comes from the bearing the disc is in and the two never"
                        " disagree about it, while both tracks keep stock's own polar angles, which is"
                        " what keeps the light between +20 and +37 deg above the horizon all day; 0x%x"
                        " interpolates it every frame. re-asserted at every chunk visit",
                 SUN_TOD_STATE, SUN_TOD_OFF, hmin / 120u, (hmin % 120u) / 2u, clock, hmin, SUN_HMIN_DAY,
                 sun_presets[which].name, sun_addr[0], sun_addr[2], sun_addr[3],
                 /* the arc's own numbers, read off the table so the line can be checked */
                 (unsigned)sun_presets[which].az_d[0], (unsigned)sun_presets[which].az_d[2],
                 (unsigned)(sun_presets[which].az_d[2] - sun_presets[which].az_d[0]),
                 (unsigned)sun_presets[which].pol_d[0], (unsigned)sun_presets[which].pol_d[2],
                 (unsigned)sun_presets[which].pol_d[4], SUN_EVAL_ADDR);
        LogMsg(msg);
    }
    SunReport(which, hmin);
}

/* sv15's test hook, next to the arc: the day runs faster, so an arc can be
 * watched cross the sky in one visit instead of one hour per sky. Nothing here
 * makes a clock: this is the client's own scale float at 0xce8564, the game
 * clock at 0xce8538's +0x2c, set to what the mode file's digits say.
 *
 * The number is the SCALE itself, game minutes per real second, not a
 * multiplier of the live float, and that is not a style choice: the server
 * sends this scale as 0.01666667f (0x3c888889, 1/60), which is also what
 * 0x643300 builds the clock with, so multiplying a live scale by 99 asked for
 * 1.65 game minutes per real second -- one hour of sky in 36 real seconds --
 * which is what an in-game visit showed the day actually doing, an hour of
 * sky in well under a minute but nowhere near a minute of sky a second. The
 * mode file's number is what the day runs at, so 60 is one game hour per
 * real second and a whole day is 24 real seconds. 0x643820 keeps its own
 * clamp, [1/60, 60], so digits above 60 land on 60 rather than on a scale
 * the client can never ask for, and the clamp is what makes the printed
 * before/after pair readable: the stock scale prints as 17 milli.
 *
 * The scale is only re-written when the float is NOT already what this DLL
 * wrote, so the server's time messages -- whose opcode 0x47 and 0x42 handlers
 * both end in 0x643820 -- are picked up and set again rather than compounded
 * off: the base is always whatever the client last had, never our own output,
 * and that is also what lets a server that sends its own scale be seen. */
static void TimeSpeedKeep(void)
{
    float *sc;
    float cur, want;
    char msg[1536]; /* sv15's line now also explains why the number is the scale
                      * itself and not a multiplier of the server's 1/60: rendered
                      * it runs past 1180 bytes, and a shorter buffer cut its tail
                      * off in the log, which is what made the line unreadable */

    if (g_time_speed <= 1)
        return;
    sc = (float *)(size_t)(GT_CLOCK_STATE + GT_SCALE_OFF);
    cur = *sc;
    if (g_time_wrote != 0.0f && cur == g_time_wrote)
        return; /* still ours: the day is already running at this speed */
    want = (float)g_time_speed;
    if (!(want >= GT_SCALE_LO))
        want = GT_SCALE_LO; /* 0x643820's low bound, the same guard the client
                              * keeps: the digits cannot reach it, so this line
                              * is here only so the store below is always a
                              * scale the client itself could have asked for */
    if (want > GT_SCALE_HI)
        want = GT_SCALE_HI; /* 0x643820's high bound, 60: one game hour per
                              * real second, a whole day in 24 real seconds.
                              * the parser caps the digits at 99, so this is
                              * where a run of digits lands from 60 up */
    *sc = want;
    g_time_wrote = want;
    if (!g_time_said) {
        g_time_said = 1;
        snprintf(msg, sizeof msg,
                 STAMP " clock: shadow-mod.mode had %u, so the game clock object's scale at 0x%x, the"
                        " game clock at 0xce8538's +0x2c, went"
                        " from %u to %u milli-game-minutes per real second. %u is the mode file's"
                        " number used as the scale itself rather than as a multiplier of what was"
                        " there, because the server sends this scale as 0.01666667f, 1/60, which is"
                        " also what 0x643300 builds the clock with: multiplying that live scale asked"
                        " for %u milli, an hour of sky in well under a minute but nowhere near a game"
                        " minute a second. the float is what 0x643a70 multiplies the real seconds since"
                        " the last minute by to get the day phase and what 0x6435a0 adds dt times of to"
                        " the minute accumulator, so a whole game day is %u real seconds and one hour"
                        " of sky turns in %u real seconds: the sun's phase, the clock the UI shows and"
                        " the day count all move together. 0x643820 clamps the scale to [1/60, 60], 17"
                        " to 60000 milli, so digits at or above 60 land on 60, one game hour a real"
                        " second and a day in 24 real seconds; the server re-sends its own scale in its"
                        " time messages -- the 0x47 handler 0x6c5de0 and the 0x42 handler 0x6c5e80 both"
                        " end in 0x643820 -- so the number is set again at every chunk visit",
                 (unsigned)g_time_speed, (unsigned)(GT_CLOCK_STATE + GT_SCALE_OFF),
                 (unsigned)(cur * 1000.0f + 0.5f), (unsigned)(want * 1000.0f + 0.5f),
                 (unsigned)g_time_speed,
                 (unsigned)(cur * (float)g_time_speed * 1000.0f + 0.5f),
                 (unsigned)(1440.0f / want + 0.5f),
                 (unsigned)(60.0f / want + 0.5f));
        LogMsg(msg);
    }
}

/* ---------------------------------------------------- the per-chunk fix **/

/* Called by the asm entry with thiscall layout: this in ecx, record on the
 * stack. Runs on whatever thread loads the chunk; touches only immutable
 * post-install state, the chunk's own header, and one-shot log flags. */
static void __cdecl fixup_chunk(void *self_, const void *record_)
{
    const unsigned char *client_map;
    unsigned gx, gy, key;
    unsigned i, moved;
    unsigned before[PROBE_COUNT];
    int lo, hi, mid, hit, which;
    unsigned char *hdr;
    char msg[768];

    /* the instrument's capture, before every gate: whichever rung calls this,
     * the chunk named here is the chunk Load is building, and DyingChunkHandler
     * reads it when an access violation happens further inside Load. The store
     * is two volatile pointer writes, no read of the chunk and no I/O, so it
     * costs nothing on a path that runs once per loaded chunk. */
    g_last_this = self_;
    g_last_record = (void *)record_; /* the const drops here only: the handler
                                      * reads it through a volatile void*, and a
                                      * const-qualified global would then need
                                      * every write casted instead */
    ++g_chunk_visits;

    /* one line per run, and pointers only: nothing here reads the chunk, so a
     * rung whose arguments were garbage still prints this rather than faulting
     * inside the fixup. This is what separates "the stub reached the call with
     * a pair of pointers" from "the stub never called at all" on a run that
     * entered the world cleanly -- mode c emits no landing call, so mode c
     * never prints it, and a mode c run that printed everything except this is
     * exactly what a working store and a working landing look like. */
    if (g_verbose && !g_first_call) {
        g_first_call = 1;
        snprintf(msg, sizeof msg,
                 STAMP " first fixup call: this %#x record %#x after %#u visits, mode %c -- the hooking rungs print this once, mode c never does",
                 (unsigned)(size_t)self_, (unsigned)(size_t)record_,
                 (unsigned)g_chunk_visits, g_mode);
        LogVerb(msg);
    }

    if (!g_ready || !self_ || !record_)
        return;

    /* mode b and mode d: the hook is in and the call happens, but this returns
     * before reading anything, before logging a byte, before looking at the
     * header. The capture above still happened, so even these rungs name the
     * chunk that dies. If a mode-b run still crashes at 0x6B0FB7 while mode a
     * entered the world clean, the fault is in the detour itself -- the store,
     * the landing, or the frame the stub keeps -- and nothing else in this DLL
     * is involved. If mode b is clean and mode h is not, the fault is in what
     * the call DOES inside Load, which today means the audit and its file I/O
     * on the client's own loading path. */
    if (g_silent)
        return;

    /* sv14's sun, before the map check and before the header is read: the
     * arc only writes the client's own key tables, so it does not care which
     * map or payload this chunk belongs to, and it runs in every rung that
     * reaches here at all -- h and f; b and d returned just above, and n, a and
     * c never call the fixup. Cheap on the hot path: one float read off the
     * client's day-night state, a window compare, and a dozen float writes when
     * the preset changed or the keys were lost. */
    SunKeepArc();

    /* sv15's clock hook, right next to the arc and with exactly the same reach:
     * it is the arc that needs watching cross the sky, and the rungs that never
     * got the arc never get the speed-up either. */
    TimeSpeedKeep();

    /* the very first chunk to reach Load is audited whole, before the map
     * check and before anything could be written: the crash reports say the
     * MCVT cursor was unreadable on a chunk this DLL never matched, so the
     * first thing to settle is what its header actually holds in this client.
     * It is audited in EVERY mode that hooks, so the same block of lines is
     * what mode h and mode f print -- sv13: with the diagnostics on, which is
     * automatic for h and needs a v in the mode file for f. */
    if (g_verbose && !g_log_audit) {
        g_log_audit = 1;
        AuditHeader(record_, "first chunk");
    }

    /* every chunk that gets here also gets a one-line dump of the header
     * values the client is about to read -- ofss relative to hdr = record + 8,
     * the same values 0x6AF605's resolver copies into this+0xf14..+0xf30, plus
     * the object's own global chunk coordinates. A crash later in Load has its
     * chunk named by the LAST line of this kind in the log: Load runs fixup
     * first, so the chunk whose BuildVertices dereferenced an unreadable MCVT
     * cursor is the last one this DLL saw, and the line says whether hdr+0x14
     * really held the .adt's own small ofs (then the cursor in the object was
     * moved by something other than this DLL) or a big value already (then the
     * header the client has is not the header the .adt file has). sv13: this
     * dump is on the diagnostic level, so the quiet path skips the block whole
     * rather than formatting 300 characters per chunk and dropping them, and
     * what names a dying chunk on the quiet level is the exception handler, not
     * this line's tail.
     *
     * sv15: the dump is also now opt-in, and it has to be. A session that runs
     * the day at 5 game minutes a real second loads hundreds of chunks, and
     * hundreds of these lines at ~300 bytes each is more than the newest LOG_KEEP_BYTES
     * TrimLog keeps, so the sun's own lines were pushed out of what survived and
     * no sky report could be read against a log. A q in the mode file puts them
     * back on, for the run whose chunk is crashing; without a q only the first
     * chunk is audited whole, above, and a fast day's sun evidence stays inside
     * what the log keeps. */
    if (g_verbose && g_sun_audit && g_audit_seen < AUDIT_LOG_MAX) {
        ++g_audit_seen;
        hdr = (unsigned char *)record_ + 8;
        snprintf(msg, sizeof msg,
                 "last chunk seen: this %#x record %#x hdr %#x flags %#x indexX"
                 " %#x indexY %#x, ofs MCVT %#x MCNR %#x MCLY %#x MCRF %#x MCAL"
                 " %#x MCSH %#x MCSE %#x MCLQ %#x, global col %#x row %#x",
                 (unsigned)(size_t)self_, (unsigned)(size_t)record_,
                 (unsigned)(size_t)hdr,
                 ((const unsigned *)hdr)[0],
                 ((const unsigned *)hdr)[1],
                 ((const unsigned *)hdr)[2],
                 ((const unsigned *)(hdr + 0x14u))[0],
                 ((const unsigned *)(hdr + 0x18u))[0],
                 ((const unsigned *)(hdr + 0x1cu))[0],
                 ((const unsigned *)(hdr + 0x20u))[0],
                 ((const unsigned *)(hdr + 0x24u))[0],
                 ((const unsigned *)(hdr + FIELD_MCSH_OFS))[0],
                 ((const unsigned *)(hdr + 0x58u))[0],
                 ((const unsigned *)(hdr + 0x60u))[0],
                 ((const unsigned *)((char *)self_ + FIELD_GLOBAL_COL))[0],
                 ((const unsigned *)((char *)self_ + FIELD_GLOBAL_ROW))[0]);
        LogVerb(msg);
        if (g_audit_seen == AUDIT_LOG_MAX) {
            snprintf(msg, sizeof msg,
                     "per-chunk header dump cap reached (%u chunks): no more of these"
                     " lines will be written, so a chunk that dies after this point"
                     " is not named by one -- raise AUDIT_LOG_MAX to see further",
                     (unsigned)AUDIT_LOG_MAX);
            LogVerb(msg);
        }
    }

    /* modes n and h (and a, which never hooks) arrive with no payload state
     * at all: the audit above is the whole visit, nothing is looked up and
     * nothing is written, so the header the client has stays exactly as it is
     * and a mode-h run that still crashes at 0x6B0FB7 says the fault is not in
     * the shadow write at all -- it is in the stub or in the client's own read
     * of the .adt */
    if (!g_allow_write)
        return;

    /* the gate is per chunk on the client's own s_mapName, so it keeps working
     * when a session changes map, and it names which of the file's runs to
     * search: a key that two maps share cannot answer with the wrong map's
     * shadow. A single-map file is one run and one string compare, exactly as
     * before, so nothing about the common case got slower. */
    client_map = (const unsigned char *)ADDR_MAP_NAME;
    which = -1;
    for (i = 0; i < g_mapCount; ++i) {
        if (CiEqName((const char *)g_map_name[i], (const char *)client_map)) {
            which = (int)i;
            break;
        }
    }
    if (which < 0) {
        if (!g_log_mismatch) {
            g_log_mismatch = 1;
            snprintf(msg, sizeof msg,
                     "fixup: client map '%s' is not one the payload names (%s): the payload is ignored on this map",
                     (const char *)client_map, g_dat_maps_txt);
            LogMsg(msg);
        }
        return;
    }

    gx = ((const unsigned *)(const char *)((char *)self_ + FIELD_GLOBAL_COL))[0];
    gy = ((const unsigned *)(const char *)((char *)self_ + FIELD_GLOBAL_ROW))[0];
    if (gx > SHADOW_DAT_COORD_MAX || gy > SHADOW_DAT_COORD_MAX)
        return;
    key = (gy << SHADOW_DAT_KEY_SHIFT) | gx;

    /* only the current map's run is searched: tile 14,20 chunk 3,5 of Azeroth and
     * of Kalimdor are the SAME key, so searching the whole file could answer a
     * chunk with another map's shadow. Entries are grouped per map at load, so
     * the run is contiguous and keys stay sorted inside it. */
    lo = (int)g_map_start[which];
    hi = (int)(g_map_start[which] + g_map_len[which]) - 1;
    hit = -1;
    while (lo <= hi) {
        mid = lo + ((hi - lo) >> 1);
        if (g_keys[mid] == key) {
            hit = mid;
            break;
        }
        if (g_keys[mid] < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (hit < 0) {
        if (g_verbose && !g_log_nomatch) {
            g_log_nomatch = 1;
            snprintf(msg, sizeof msg,
                     "fixup: first chunk with no payload: gy %u gx %u key %u (the bake left it without a shadow map, or its tile is not in this set)",
                     gy, gx, key);
            LogVerb(msg);
        }
        return;
    }

    /* hdr is what the client itself calls record + 8: the MCNK magic is
     * record's start, the header fields start after it (0x6AF970: lea
     * edx,[eax+8]). ofsMCSH is relative to hdr, and the game resolves every
     * sub-record as hdr + ofs, so hdr + ofs lands in the arena. */
    if (g_verbose && !g_log_first)
        AuditHeader(record_, "first matched");
    hdr = (unsigned char *)record_ + 8;
    for (i = 0; i < PROBE_COUNT; ++i)
        before[i] = ((const unsigned *)(hdr + k_probe_ofs[i]))[0];
    hdr[0] |= 1; /* MCNK flag bit 0: the shadow path's own gate */
    ((unsigned *)(hdr + FIELD_MCSH_OFS))[0] =
        (unsigned)((size_t)(g_arena + g_offs[hit]) - (size_t)hdr);
    ((unsigned *)(hdr + FIELD_MCSH_SIZE))[0] = SHADOW_DAT_PAYLOAD_BYTES;

    /* proof that the write stayed in the shadow slot: none of the other
     * sub-record ofss may move. One that did says hdr is not the header the
     * resolver has, so the ofss are put back -- BuildVertices would otherwise
     * read 145 floats off an MCVT cursor that is really an arena displacement,
     * i.e. the 0x6B0FB7 crash -- and mutation is switched off for the rest of
     * the run so the session finishes and the log can be read whole. */
    moved = 0;
    for (i = 0; i < PROBE_COUNT; ++i) {
        unsigned now = ((unsigned *)(hdr + k_probe_ofs[i]))[0];
        if (now == before[i])
            continue;
        if (!g_log_field) {
            g_log_field = 1;
            snprintf(msg, sizeof msg,
                     "fixup: the shadow write moved a sub-record ofs: hdr %#x,"
                     " ofs at hdr+%#x was %#x and is %#x now, ofs written %#x"
                     " (= arena %#x - hdr), FIELD_MCSH_OFS 0x%x is wrong for"
                     " this client; the ofss are restored and no more chunks"
                     " are patched so the run finishes",
                     (unsigned)(size_t)hdr, k_probe_ofs[i], before[i], now,
                     (unsigned)((size_t)(g_arena + g_offs[hit]) - (size_t)hdr),
                     (unsigned)(size_t)g_arena, FIELD_MCSH_OFS);
            LogMsg(msg);
        }
        ((unsigned *)(hdr + k_probe_ofs[i]))[0] = before[i];
        moved = 1;
    }
    if (moved) {
        g_ready = 0;
        if (g_log_field)
            LogMsg(STAMP " fixup: the field contract does not hold on this"
                         " client, shadows left stock for the rest of the run");
        return;
    }

    ++g_hits;

    /* one progress line every 10000 matched chunks, capped at 60 lines: cheap
     * enough for a path that runs once per loaded chunk, and it is the in-game
     * evidence that the arena is what is being drawn (g_entryCount is the
     * ceiling, so the last line under it names the tiles the set does not
     * cover). sv13: diagnostic level, so the shipping run writes none of them
     * and never takes the modulo either */
    if (g_verbose && (g_hits % 10000u) == 0u && g_log_hits < 60) {
        ++g_log_hits;
        snprintf(msg, sizeof msg, "fixup: %u chunks matched so far, of %u entries",
                 (unsigned)g_hits, g_entryCount);
        LogVerb(msg);
    }

    /* the one line that says the write landed and where in the arena the
     * payload came from -- sv13: diagnostic level, so the shipping run's
     * evidence that shadows were drawn is what the ground shows, and this
     * line is what a debug session reads with a v in the mode file */
    if (g_verbose && !g_log_first) {
        g_log_first = 1;
        snprintf(msg, sizeof msg,
                 "fixup: first chunk matched: gy %u gx %u key %u, shadow drawn"
                 " from arena+%u, this %#x record %#x hdr %#x arena %#x,"
                 " hdr flags %#x size %#x ofs %#x, mcvtof %#x mcnrof %#x"
                 " mclyof %#x mcrof %#x mcalof %#x mcseof %#x mclof %#x",
                 gy, gx, key, g_offs[hit],
                 (unsigned)(size_t)self_, (unsigned)(size_t)record_,
                 (unsigned)(size_t)hdr, (unsigned)(size_t)g_arena,
                 ((const unsigned *)(hdr + 0x00u))[0],
                 ((const unsigned *)(hdr + FIELD_MCSH_SIZE))[0],
                 ((const unsigned *)(hdr + FIELD_MCSH_OFS))[0],
                 ((const unsigned *)(hdr + 0x14u))[0],
                 ((const unsigned *)(hdr + 0x18u))[0],
                 ((const unsigned *)(hdr + 0x1cu))[0],
                 ((const unsigned *)(hdr + 0x20u))[0],
                 ((const unsigned *)(hdr + 0x24u))[0],
                 ((const unsigned *)(hdr + 0x58u))[0],
                 ((const unsigned *)(hdr + 0x60u))[0]);
        LogVerb(msg);
    }
}

/* --------------------------------------------------------- the detour ***/

/* The detour, as bytes: three hand-written x86-32 stubs, one per rung, emitted
 * once at install into a private executable page with the absolute addresses
 * below patched into their imm32 slots. It is why this DLL needs no assembler,
 * and its stack math is checked against real bytes by verify_dll.py.
 *
 * Stack on entry: [esp]=ret, [esp+4]=record, [esp+8]=first; `this` is in ecx
 * (the client's Load is thiscall). pushfd/pushad preserve every register the
 * game had; fixup_chunk is passed (this, record) on the stack, and MSVC gives a
 * static C function like this a PLAIN ret -- opcode c3, no imm16 -- so the
 * caller, not the callee, has to drop the two argument dwords. This is not a
 * mistake a compiler would catch: cleanup off by eight means popad runs 4 bytes
 * off the window pushad left, so every general register comes back from the
 * wrong slot -- ebp included, which is what the replay depends on -- and Load
 * then works from a garbage record. That bug is visible twice in the crash
 * reports: the sv3 build had `add esp,8` here and faulted at Load's own next
 * act, mov [esi+0x168], eax at 0x6AF5FF, because esi = ecx came back as the
 * client's old ebx. sv4 corrected the add to 0x10 -- verify_dll.py now checks
 * it against the linked image -- and the crash that stays, at 0x6B0FB7 reading
 * 0x6CF97F32, is what the mode ladder is settling: with mode a (the whole
 * payload resident, no hook) entering the world clean and mode h (hook, one
 * empty call per chunk, nothing allocated, no header touched) crashing there,
 * the fault is in the detour or in what the call does inside Load, not in the
 * footprint and not in the shadow write. The arithmetic the add must satisfy
 * (E = entry esp, the value at the E9's landing):
 *   -4 -32 -8 (pushfd pushad sub) = E-44: [esp+48] = E+4 = record, and the two
 *     argument dwords sit at [esp+0] and [esp+4] inside the 8 scratch bytes
 *   -4 -4 -4 (push eax, push ecx, the call's own return address), so esp is
 *     E-56 when the callee starts; a plain ret pops that return address back
 *     off, so the callee comes back at E-52 with the args still behind it
 *   +16 = E-36: this add drops the 8 arg bytes plus the 8 scratch bytes (the
 *     call's return address is already gone), landing popad on exactly the
 *     window pushad left -- EDI at [E-36], EBP at [E-28], ESP restored from
 *     pushad's own saved value E-4, so after popad popfd closes the last dword
 *     and the entry frame is whole before the replayed prologue pushes ebp
 * The 6 replayed bytes then set up ebp and eax = record exactly as the game
 * expects (ecx is back from pushad), and the jmp resumes Load's own body.
 *
 * The c stub (12 bytes, same hook) does no call at all: push ebp / mov ebp,esp
 * / mov eax,[ebp+8] then push <resume> / ret. Nothing on the stack is left
 * between entry and exit -- the push is popped by the ret at the same level --
 * so eax and flags are what the client's own first three instructions left and
 * ecx/ebx/edx/esp never moved at all, which is exactly what a clean c proves
 * about the client's state at 0x6AF5F6 with no call of this DLL involved.
 *
 * The d stub (29 bytes, hook at 0x6AF5F3) has the frame math above in its
 * simplest form: entry E -> pushad E-36 -> pushfd E-40 -> the two args E-48 and
 * E-44 -> the call's return address at E-52, so esp is E-52 when fixup_chunk
 * starts; a plain ret pops the return address back to E-48 and the add has to
 * drop only what the pushad/pushfd block did NOT already drop, 8 bytes of args,
 * landing popad on E-40 -- the window pushad left. And the record is read from
 * [ebp+8] on the client's own frame, so no esp displacement is part of finding
 * the arguments at all -- the one thing that can make b crash and d not.
 *
 * Encoding note: x86-32 has no `sub r/m32, imm8` -- the one-byte-immediate
 * group (opcode 83) only covers add/or/adc/sbb/and/xor/cmp, and its reg field
 * picks the operation (83 /5 would BE sub, but with a zero-extended 0..255
 * immediate). So the 8-byte reservation is spelled `add esp, -8` (83 c4 f8:
 * the /0 add form, whose imm8 IS sign-extended), which on a stack pointer is
 * the same wrap as sub esp,8 and is three bytes instead of the imm32 form's
 * six (81 ec 08 00 00 00). A disassembler prints the imm8 as it likes.
 *
 * A second encoding trap, caught here by verify_dll.py's disassembly: esp is
 * only usable as an address base through a SIB byte, so `mov eax,[esp+48]` is
 * four bytes -- 8b 44 24 30, i.e. modrm 44 (mod 01, register eax, rm 100 = the
 * "needs a SIB" case) then SIB 24 (scale 0, index 100 = none, base 100 = esp)
 * then disp8 30. The three bytes 8b 44 30 are a different instruction
 * altogether, and a wrong SIB is silently a wrong address: 8b 44 44 30 reads
 * [esp + eax*2 + 0x30], because SIB bits are scale|index|base and 44 splits as
 * 01|000|100, not as the "no index" pattern. That is why STUB_BYTES is 36 and
 * the patch slots are 12 and 30. ebp needs no SIB, so [ebp+8] really is 8b 45
 * 08 and replays the client's prologue byte for byte.
 */
static const unsigned char stub_code[] = {
    0x9C,                         /* pushfd                    */
    0x60,                         /* pushad                    */
    0x83, 0xC4, 0xF8,             /* add  esp, -8  (= sub esp,8) */
    0x8B, 0x44, 0x24, 0x30,       /* mov  eax, [esp+48]  = record */
    0x50,                         /* push eax                   */
    0x51,                         /* push ecx  (this)           */
    0xB8, 0x00, 0x00, 0x00, 0x00, /* mov  eax, <fixup_chunk>    */
    0xFF, 0xD0,                   /* call eax                   */
    0x83, 0xC4, 0x10,             /* add  esp, 16  (8 arg bytes the callee left,
                                        8 scratch bytes)      */
    0x61,                         /* popad                      */
    0x9D,                         /* popfd                      */
    0x55,                         /* push ebp        -- replayed */
    0x8B, 0xEC,                   /* mov  ebp, esp     prologue  */
    0x8B, 0x45, 0x08,             /* mov  eax, [ebp+8] = record  */
    0xB8, 0x00, 0x00, 0x00, 0x00, /* mov  eax, <resume>          */
    0xFF, 0xE0                    /* jmp  eax                    */
};
#define STUB_BYTES   36u
/* the two imm32 slots in stub_code, patched at install */
#define STUB_SLOT_FIXUP  12u
#define STUB_SLOT_RESUME 30u

/* What stub_code's last two bytes really did, settled by the sv10 mode-b run
 * instead of guessed at: `mov eax,<resume> / jmp eax` resumes at 0x6AF5F6 with
 * eax = 0x6AF5F6, the resume address, not the record. Load then runs its own
 * body on that value: 0x6AF5FF mov [esi+0x168],eax files the resume address as
 * the chunk's record, 0x6AF60A mov eax,[esi+0xf08] and the resolver's lea at
 * 0x6AF970 make hdr = eax+8 = 0x6AF5FE -- a dword inside CMapChunk::Load's own
 * code -- and the eight "sub-record ofss" become whatever the .text bytes at
 * 0x6AF5FE+ofs hold. The one the crash reports keep naming is MCVT: the dword at
 * 0x6AF5FE+0x14 = 0x6AF612 is 0x6C8E8934, so this+0xf14 = hdr+ofs = 0x6AF5FE +
 * 0x6C8E8934 = 0x6CF97F32, and BuildVertices' loop at 0x6B0FB7 dereferences it:
 * ACCESS_VIOLATION reading 0x6CF97F32, exactly the report every run got. The
 * run's own audit printed the real header beside it (MCVT 0x88, MCSH 0x4ec ...),
 * which says the .adt's table was intact and only the resume form broke it. stub
 * c and d therefore return through push <resume> / ret, which leaves eax alone,
 * and this rung stays in the ladder only as the evidence for that rule. */
/* sv9's other two detour shapes, same page, same patch rule.
 *
 * stub_code_c is mode c: the landing replays the prologue and jumps back, and
 * NOTHING else -- no pushad, no call, no C code on the path -- so the question
 * this rung asks is only "did the 5-byte store at 0x6AF5F0 land, does the
 * landing run, is 0x6AF5F6 the right resume point", and a real sv9 run answered
 * all three: clean. At the landing esp is the function's own entry esp E, since
 * nothing has been pushed yet at 0x6AF5F0; the replayed push ebp takes esp to
 * E-4, which is exactly what 0x6AF5F6 expects, and `push <resume> / ret` is the
 * jmp form that leaves eax = record, ecx = this, edx and the flags alone. The
 * alternative, `mov eax,<resume> / jmp eax`, does not: it hands the client its
 * own resume address in the register that holds the record argument, and that is
 * the whole reason every sv3..sv8 and sv10-b run died. See the note on stub_code
 * below for the arithmetic. 12 bytes, one imm32 slot at +7, the operand of the
 * 0x68 push.
 *
 * stub_code_d is mode d: the same empty call, but the store moved to 0x6AF5F3,
 * three bytes into the same prologue, where ebp is already the client's own
 * frame and esp is E-4 -- push ebp made it E-4 and the frame pointer points
 * there. So the record comes from [ebp+8] and `this` from ecx, and no ESP
 * displacement is part of finding the arguments at all: mode d is what
 * separates a wrong [esp+48] displacement from a wrong store site. esp then
 * walks E-4 -32 -4 (pushad, pushfd) = E-40, -4 (push [ebp+8]) = E-44, -4 (push
 * ecx) = E-48, so the callee starts at E-52 with [esp+4] = this and [esp+8] =
 * record; a plain ret comes back to E-48, and add esp,8 lands on the pushfd
 * dword at E-40. popfd closes it, popad restores the eight registers and esp
 * from the 32-byte window pushad left at E-36, which ends at E-4 = the esp at
 * the landing. The replay of mov eax,[ebp+8] / push ebx / push esi then leaves
 * esp at E-12 and eax = record, exactly what the client has at 0x6AF5F8 after
 * executing those three instructions, and push <resume> / ret jumps there
 * without touching eax. 29 bytes: slots at +7 (b8's imm32) and +24 (68's).
 *
 * Encoding note: capstone reads pushal/popal for 0x60/0x61 and prints the
 * imm32 of `add esp, -8` as 0xfffffff8; both tables above are checked against
 * the linked image by verify_dll.py, including these slot offsets. */
static const unsigned char stub_code_c[] = {
    0x55,                         /* push ebp        -- replayed */
    0x8B, 0xEC,                   /* mov  ebp, esp     prologue  */
    0x8B, 0x45, 0x08,             /* mov  eax, [ebp+8] = record  */
    0x68, 0x00, 0x00, 0x00, 0x00, /* push <resume>               */
    0xC3                          /* ret                           */
};
#define STUB_C_BYTES        12u
#define STUB_C_SLOT_RESUME   7u

static const unsigned char stub_code_d[] = {
    0x60,                         /* pushad                      */
    0x9C,                         /* pushfd                      */
    0xFF, 0x75, 0x08,             /* push dword [ebp+8] = record */
    0x51,                         /* push ecx  (this)            */
    0xB8, 0x00, 0x00, 0x00, 0x00, /* mov  eax, <fixup_chunk>     */
    0xFF, 0xD0,                   /* call eax                    */
    0x83, 0xC4, 0x08,             /* add  esp, 8 (the 2 arg dwords the
                                        callee left: no scratch bytes
                                        are reserved in this shape) */
    0x9D,                         /* popfd                       */
    0x61,                         /* popad                       */
    0x8B, 0x45, 0x08,             /* replay: mov  eax, [ebp+8]   */
    0x53,                         /* replay: push ebx            */
    0x56,                         /* replay: push esi            */
    0x68, 0x00, 0x00, 0x00, 0x00, /* push <resume>               */
    0xC3                          /* ret                           */
};
#define STUB_D_BYTES        29u
#define STUB_D_SLOT_FIXUP    7u
#define STUB_D_SLOT_RESUME  24u

/* Which detour shape a rung rides. This is the one place the ladder says which
 * frame is in play, and it changed after real runs: sv8's mode b (the 0x6AF5F0
 * store, pushad/pushfd and [esp+48] record lookup) crashed entering the world at
 * 0x6B0FB7, while the sv9 runs of mode c (that same store and landing, no call)
 * and mode d (the 0x6AF5F3 store, the call, arguments taken from [ebp+8] on the
 * client's own live frame) BOTH entered the world clean. The sv10 runs then put
 * h and f on d's shape and both did real work -- h printed its audit block and
 * f drew the baked set on the ground -- while b, re-run, crashed again and the
 * instrument named why: b's `mov eax,<resume> / jmp eax` landing leaves the resume
 * address in eax where Load wants the record, so the client resolves its header
 * inside its own code (the note above stub_code has the arithmetic). The [esp+48]
 * lookup was never the problem: b's own first-call line printed a valid this and
 * record pair. So the rule the ladder now carries is about the RETURN form, and
 * only rung b still emits the shape that breaks it. */
static char StubShape(char rung)
{
    if (rung == 'c') return 'c';
    if (rung == 'b') return 'b';
    return 'd';
}

/* Emits the detour into a page of its own: VirtualAlloc with execute rights,
 * the same as MinHook's heap trampolines, and never a rewrite of a file-backed
 * page. The two imm32 slots are patched with the values this process sees now,
 * which is what lets the DLL relocate: fixup_target is fixup_chunk's address in
 * the relocated image, resume_at is a client address (the client never
 * relocates, so it is a plain constant). */
static int BuildStub(char rung)
{
    unsigned char *page;
    const unsigned char *code;
    unsigned code_bytes, slot_fixup, slot_resume;
    unsigned fixup_target, resume_at;
    char msg[256];
    char shape;
    unsigned resume_prefix;

    /* which of the three detour shapes to emit, and where its two imm32 slots
     * are; the sizes and slot offsets are asserted against the tables right
     * here, and verify_dll.py checks the same numbers out of the linked image,
     * so a table edited without its slots (or slots edited without a table) is
     * caught twice, once at install and once before the DLL is ever shipped. */
    shape = StubShape(rung);
    if (shape == 'c') {
        code = stub_code_c;
        code_bytes = STUB_C_BYTES;
        slot_fixup = 0u; /* mode c calls nothing, so there is no fixup slot */
        slot_resume = STUB_C_SLOT_RESUME;
        /* c and d carry the resume address as the operand of a push, 0x68 */
        resume_prefix = 0x68u;
    } else if (shape == 'd') {
        code = stub_code_d;
        code_bytes = STUB_D_BYTES;
        slot_fixup = STUB_D_SLOT_FIXUP;
        slot_resume = STUB_D_SLOT_RESUME;
        resume_prefix = 0x68u;
    } else {
        code = stub_code;
        code_bytes = STUB_BYTES;
        slot_fixup = STUB_SLOT_FIXUP;
        slot_resume = STUB_SLOT_RESUME;
        /* b's shape reaches its resume through mov eax,<resume> / jmp eax, so
         * the byte before its resume slot is 0xB8, not 0x68: the shape decides
         * the prefix, and a check that assumed the push form refused b's table
         * at install -- which is what made the sv9 h and f runs print a build
         * defect instead of installing anything */
        resume_prefix = 0xB8u;
    }
    /* the table and its slots must agree before any byte is written; verify_dll
     * .py checks the same three (bytes, slots, opcode prefixes) in the linked
     * image, so this is the runtime half of the same rule */
    if ((size_t)code_bytes != (shape == 'c' ? sizeof stub_code_c
                               : (shape == 'd' ? sizeof stub_code_d : sizeof stub_code))
        || (slot_fixup != 0u && code[slot_fixup - 1u] != 0xB8u)
        || code[slot_resume - 1u] != (unsigned char)resume_prefix) {
        LogMsg(STAMP " build defect: the detour byte table and its slot offsets disagree, refusing to patch");
        return 0;
    }
    page = (unsigned char *)VirtualAlloc(NULL, 0x1000u, 0x00003000u,
                                         0x00000040u /* PAGE_EXECUTE_READWRITE from
                                         <winnt.h>, the same value MinHook asks for
                                         every trampoline page: this page holds
                                         machine code the client jumps into. fl
                                         Protect is a LEVEL from winnt.h's family,
                                         not a bit-or, so the 0x7 this file had
                                         before was refused with GetLastError 0x57
                                         and the store never happened; the arena
                                         comment above names the whole family. */);
    if (!page) {
        snprintf(msg, sizeof msg, STAMP " VirtualAlloc for the detour page failed"
                 " (asked 0x1000 bytes, alloc 0x3000, protect 0x40), GetLastError"
                 " %#x: refusing to patch", GetLastError());
        LogMsg(msg);
        return 0;
    }
    memcpy(page, code, code_bytes);
    fixup_target = (unsigned)(size_t)fixup_chunk;
    resume_at = g_resume;
    /* mode c has no fixup slot: its imm32 is only the resume address, and the
     * 4 bytes it would have patched stay as they are in the table */
    if (slot_fixup != 0u)
        memcpy(page + slot_fixup, &fixup_target, 4);
    memcpy(page + slot_resume, &resume_at, 4);
    g_stub = page;
    return 1;
}

/* The hook itself, factored out of Install so mode h (hook plus the audit, no
 * payload state) and mode f (full) put the SAME five bytes in through the SAME
 * checks: a mode-h run that crashes while a mode-n run is clean is proof about
 * this code path, not about anything the shadow write does, and the two modes
 * have to agree on how the store is asked for, attempted and verified. */
/* Which shape a rung rides, through StubShape: the c rung is the 0x6AF5F0
 * store with a landing that calls nothing, and the d, h and f rungs are the
 * 0x6AF5F3 store with a landing that calls the fixup off the client's own
 * frame. The store is 5 bytes everywhere, so the site and the resume are the
 * whole difference between the two sites; the pin below is what ties each site
 * to the same prologue. */
static int PatchHook(char rung)
{
    unsigned char *site;
    unsigned char *page_base;
    unsigned char patched[8];
    DWORD old_protect;
    long rel;
    int perms;

    if (StubShape(rung) == 'd') {
        /* d's shape, which h and f ride too: the E9 spans 0x6AF5F3..0x6AF5F7,
         * 5 bytes, and resumes on the 0x57 push edi at 0x6AF5F8 that k_prologue_d
         * pins as the byte right after the span. */
        site = (unsigned char *)(size_t)ADDR_LOAD_D;
        g_resume = ADDR_LOAD_D + 5u;
        if (memcmp((const void *)(size_t)ADDR_LOAD_D, k_prologue_d,
                   sizeof k_prologue_d) != 0) {
            LogMsg(STAMP " the d shape's site is not the pinned 6 bytes of the same prologue: refusing to patch this client");
            return 0;
        }
    } else {
        site = (unsigned char *)(size_t)ADDR_LOAD;
        g_resume = ADDR_LOAD + 6u; /* after the 6 replayed bytes, 0x6AF5F6 */
    }
    if (!BuildStub(rung))
        return 0;
    rel = (long)((size_t)g_stub - ((size_t)site + 5u));
    /* E9 rel32 reaches the whole 4 GB space, so the DLL's own base and the
     * client's 0x400000 need no magnitude test; the read-back below is what
     * proves the displacement points at the detour. */

    /* the site sits in the client's code section, which the PE loader leaves
     * writable only when the image carries the NTv4 hack flag in its section
     * headers. Ask for a writable page the way MinHook does for every hook;
     * if VirtualProtect refuses, the store is attempted anyway and the
     * read-back decides, so a page that was already writable needs no dance
     * and a page that is not cannot leave the client half-patched. */
    page_base = (unsigned char *)((size_t)site & ~(size_t)0xFFFu);
    perms = VirtualProtect(page_base, 0x1000u, 0x00000040u /* PAGE_EXECUTE_READWRITE
                            from <winnt.h>, the same value MinHook passes for every
                            hook; 0x7 is refused with GetLastError 0x57 because fl
                            NewProtect is the same level family as VirtualAlloc's */,
                           (LPDWORD)&old_protect);
    patched[0] = 0xE9; /* jmp rel32 */
    memcpy(patched + 1, &rel, 4);
    memcpy(site, patched, 5);
    /* the client may already have this function's bytes in its front-side
     * pipeline; wow-mods' patch path flushes the same way (wow-mods windows
     * hook lib.rs declares FlushInstructionCache for exactly this) */
    FlushInstructionCache(GetCurrentProcess(), site, 5);

    if (memcmp(site, patched, 5) != 0) {
        LogMsg(STAMP " the 5-byte detour store did not land (write-protected code page): refusing to run half-patched");
        return 0;
    }
    if (perms)
        VirtualProtect(page_base, 0x1000u, old_protect, (LPDWORD)&old_protect);
    return 1;
}

/* --------------------------------------------------------- install *****/

static int Install(void)
{
    FILE *f;
    ShadowDatHeader head;
    size_t table_bytes, expect_size, rec_bytes;
    long fsize;
    unsigned char *ent;
    unsigned char *rec;
    unsigned i, prev_key, mapCount, stride, run, start, shown;
    int fresh; /* the entry loop: 1 at a run's first entry */
    unsigned char *arena;
    unsigned char *site = (unsigned char *)(size_t)ADDR_LOAD;
    char msg[1024]; /* sv13's mode-f line now also names the logging level, so
                      * the longest line Install writes is a little longer; snprintf
                      * caps at the buffer either way */

    snprintf(g_datPath, sizeof g_datPath, "%s\\shadowmaps.dat", g_dir);
    snprintf(g_logPath, sizeof g_logPath, "%s\\shadow-mod.log.txt", g_dir);

    /* sv13 trims the log before anything of this run is written to it, so a
     * launch that follows a verbose one cannot leave a file the size of the run */
    TrimLogFile();

    /* the mode first, before anything else can happen: the letter decides how
     * much of this function runs at all, and the line names it so a log can
     * never be read as "which DLL did this" again -- stamp plus mode, on every
     * step. sv13 adds the level: a v anywhere in the same file turns the
     * diagnostics on, and every rung but f is a ladder step, whose whole point
     * is the log, so its diagnostics come on whether or not the file says v.
     * No file at all means f and quiet logging, which is what ships. */
    g_mode = ReadModeFile();
    if (g_mode != 'f')
        g_verbose = 1;
    snprintf(msg, sizeof msg,
             STAMP " mode '%c' read from '%s\\shadow-mod.mode' (no such file means f): n = inert, a = .dat plus"
             " arena, no hook, b = hook plus an empty call, c = the detour alone"
             " with no call at all, d = the same call from the client's own frame"
             " at 0x6AF5F3, h = hook plus audit, no payload, f = full; a v"
             " anywhere in the file also turns the decision lines on, digits up to 99 anywhere in it"
             " set the game clock's scale to that many game minutes per real second, which 0x643820"
             " clamps to at most 60 (one game hour a real second, a whole day in 24), and a q is"
             " what puts the per-chunk header dump back on -- without a q the log holds the sun,"
             " the clock and the audit of the FIRST chunk only, which is what keeps a fast day's"
             " evidence inside the newest 64 kB that TrimLog keeps of a 512 kB log",
             g_mode, g_dir);
    LogVerb(msg);

    /* sv14's sun choice, read next to the mode file and before any mode can
     * return: absent or clock means the clock written at the first chunk visit
     * decides, which is the concept; off leaves the stock sun alone; a preset
     * name forces that arc, which is how one arc is watched without waiting for
     * the day to come round to it. Only h and f reach the sun write: n, a and c
     * never call the fixup, and b and d return above it, so those five say so
     * here -- the clock is only worth reading once the world is stamping it. */
    SunReadChoice();
    if (g_mode != 'h' && g_mode != 'f')
        LogVerb(STAMP " sun arc: this rung cannot carry the arc -- n, a and c never call the per-chunk"
                " fixup, and b and d return before it -- so the client's own sun key tables at"
                " 0xce8d90, 0xce8db0, 0xce8d4c and 0xce8d64 stay as the day-night file built them."
                " mode h needs no .dat and prints the clock, the preset and the arc it wrote, so"
                " h is the rung to test the sun on; f carries the arc too");

    /* sv15's clock digit: the mode file's third thing, read with the mode above
     * it so a rung that cannot carry the arc cannot carry the speed-up either.
     * The number is the scale itself, game minutes per real second, because the
     * scale the server sends is 1/60 -- one game minute a real minute -- so
     * multiplying that live float by 99 only asked for 1.65, an hour of sky in
     * 36 real seconds. 0x643820's clamp keeps it in [1/60, 60], so 60 is the
     * fastest the client can be asked to run: one game hour a real second, a
     * whole day in 24 real seconds. */
    if (g_time_speed > 1) {
        snprintf(msg, sizeof msg,
                 STAMP " clock: shadow-mod.mode had %u, so the game clock's scale at 0x%x, the game clock"
                        " at " GT_CLOCK_TXT "'s +0x%x, is set to %u game minutes per real second at every"
                        " chunk visit -- h and f only. the number is the scale itself, not a multiplier"
                        " of the live float: the server sends this scale as 1/60, one game minute a real"
                        " minute, so a multiplier asked for a fraction of a game minute a second. the day"
                        " and the sun's phase run at that scale once the clamp takes it, so a day is %u"
                        " real seconds and one hour of sky turns in %u real seconds. 0x643820's clamp"
                        " keeps the scale in [1/60, 60], so 60 is as fast as the client can be asked to"
                        " run and a mode file's digits at or above that land there and print as a day"
                        " in 24 real seconds",
                 (unsigned)g_time_speed, (unsigned)GT_SCALE_ADDR, GT_SCALE_OFF,
                 (unsigned)g_time_speed,
                 (unsigned)(1440.0f / (float)(g_time_speed > 60 ? 60 : g_time_speed) + 0.5f),
                 (unsigned)(60.0f / (float)(g_time_speed > 60 ? 60 : g_time_speed) + 0.5f));
        LogVerb(msg);
    } else if (g_time_speed == 1) {
        snprintf(msg, sizeof msg,
                 STAMP " clock: shadow-mod.mode had 1, so the game clock's scale at 0x%x, the game clock"
                        " at " GT_CLOCK_TXT "'s +0x%x, is left exactly as the server sent it",
                 (unsigned)GT_SCALE_ADDR, GT_SCALE_OFF);
        LogVerb(msg);
    }

    if (g_mode == 'n') {
        /* the inert rung: the .dat is not opened, nothing is allocated, the
         * client's bytes are not even read, so a run that still crashes at
         * 0x6B0FB7 has nothing to do with this DLL -- the fault is in the
         * client and its patch state on its own. Load returns 4 here, and
         * VanillaFixes answering with its generic "was not loaded properly"
         * popup is expected: this mode patches nothing on purpose. */
        LogMsg(STAMP " mode n: nothing read, allocated or patched, the client is untouched; shadows stay stock, the popup is expected");
        g_diagnostic = 1;
        return 0;
    }

    /* pin the client first: the .dat costs tens of MB to read, the pins cost
     * a memcmp, and a client that is not the build the whole contract was read
     * from has to be refused before any of that work happens */
    if (memcmp(site, k_prologue, sizeof k_prologue) != 0) {
        LogMsg(STAMP " CMapChunk::Load bytes are not the pinned 1.12.1 prologue: refusing to touch this client");
        return 0;
    }
    if (memcmp((const void *)(size_t)ADDR_GATE, k_gate, sizeof k_gate) != 0) {
        LogMsg(STAMP " the shadow gate inside Load is not the pinned run: refusing to touch this client");
        return 0;
    }

    /* the instrument comes up BEFORE the hook is patched, so a fault inside the
     * detour itself -- the thing sv8's mode b produced -- is already named by the
     * time the store exists, and a fault in a mode-a run's own allocation is too.
     * It claims nothing and returns EXCEPTION_CONTINUE_SEARCH, so the only
     * visible effect is one extra line in this log per first 0xC0000005. */
    InstallHandler();

    if (g_mode == 'c') {
        /* the c rung's own verdict, from a real run: it entered the world with
         * its three install lines and nothing else, no crash and no new shadows,
         * which is what was still being claimed here about the store, the
         * landing, the replayed prologue and the resume point. All four are
         * settled now. What this rung has NOT settled is the call, because its
         * landing makes no call -- mode d's shape is the same landing plus the
         * call, and the sv9 d run that entered the world clean is what settled
         * the call and its [ebp+8] argument lookup. */
        if (!PatchHook('c'))
            return 0;
        g_ready = 0; /* nothing may write, there is nothing to write with anyway */
        snprintf(msg, sizeof msg,
                 STAMP " mode c installed: hook 0x6AF5F0 patched, resume 0x6AF5F6,"
                 " detour at %#x, the landing replays the prologue and returns"
                 " without calling anything -- no .dat read, nothing allocated,"
                 " no chunk header read or touched (shadows stay stock;"
                 " this mode exists to test the store and the landing alone)",
                 (unsigned)(size_t)g_stub);
        LogVerb(msg);
        return 1;
    }

    if (g_mode == 'd') {
        /* mode b's call, moved 3 bytes into the prologue so that the 5-byte E9
         * covers mov eax,[ebp+8] / push ebx / push esi whole and the resume is
         * the next instruction boundary, 0x6AF5F8. ebp is already the client's
         * own frame there, so the stub reads the record from [ebp+8] and this
         * from ecx: no ESP displacement is part of finding the arguments. This
         * rung RAN on real client and entered the world clean, with no crash and
         * no new shadows (it allocates nothing and writes nothing, so no shadows
         * is the expected look), which is what makes d's shape the one h and f
         * ride through StubShape and leaves b's [esp+48] lookup as the only
         * known-bad piece of the detour. */
        if (!PatchHook('d'))
            return 0;
        g_silent = 1;
        g_ready = 1;
        snprintf(msg, sizeof msg,
                 STAMP " mode d installed: hook 0x6AF5F3 patched (the 5-byte E9"
                 " covers mov eax,[ebp+8] / push ebx / push esi), resume 0x6AF5F8"
                 " (push edi), detour at %#x, fixup returns immediately -- no .dat"
                 " read, nothing allocated, no chunk header read or touched (shadows"
                 " stay stock; this mode exists to test the stub's argument lookup)",
                 (unsigned)(size_t)g_stub);
        LogVerb(msg);
        return 1;
    }

    if (g_mode == 'b') {
        /* the one rung left on b's shape, and it is now the ONLY rung on that
         * shape: h and f ride d's shape instead (StubShape). sv8 ran this and it
         * crashed at 0x6B0FB7 reading 0x6CF97F32, and the sv9 runs put mode c
         * (this same store at 0x6AF5F0 and this same resume point, landing that
         * calls nothing) and mode d (the 0x6AF5F3 store, the call, arguments
         * from [ebp+8]) both clean entering the world with no new crash. The
         * sv10 re-run of b crashed again, same instruction, same referenced
         * address, and the instrument named the reason in the run's own log:
         * b's landing ends in mov eax,<resume> / jmp eax, so the client comes
         * back to 0x6AF5F6 with eax = 0x6AF5F6 instead of the record, Load
         * files that value as the chunk's record at 0x6AF5FF and resolves hdr
         * = eax+8 = 0x6AF5FE, inside its own code -- see the note above stub_code
         * for the arithmetic that lands exactly on 0x6CF97F32. The [esp+48]
         * lookup was never wrong: b's own first-call line printed a valid this
         * and record pair. Nothing that ships rides this shape. */
        if (!PatchHook('b'))
            return 0;
        g_silent = 1;
        g_ready = 1;
        snprintf(msg, sizeof msg,
                 STAMP " mode b installed: hook 0x6AF5F0 patched, resume 0x6AF5F6,"
                 " detour at %#x, fixup returns immediately -- no .dat read, nothing"
                 " allocated, no chunk header read or touched (shadows stay stock;"
                 " this mode exists to test the stub alone)",
                 (unsigned)(size_t)g_stub);
        LogVerb(msg);
        return 1;
    }

    if (g_mode == 'h') {
        /* the stub plus the call's work: the detour, the first-chunk audit and
         * one compact header dump per chunk, with no .dat opened, nothing
         * allocated and no chunk header touched. sv9 tried this on b's shape and
         * the install refused it -- the slot check expected a 0x68 push before
         * every resume slot, and b's resume is the operand of a 0xB8 mov, so h
         * and f logged "build defect" and patched nothing. This rung now rides
         * d's shape, and the sv10 run of it answered the question that is left:
         * it entered the world and printed its first fixup call line (this 0x2cd20008
         * record 0x2c3dca46), the whole first-chunk audit block with all eight
         * slots resolving as hdr+ofs on their own fourcc, and 2000 per-chunk
         * header dumps -- so the audit and its file I/O survive inside Load, and
         * nothing about the client's loading path is left unexplained. */
        if (!PatchHook('h'))
            return 0;
        g_ready = 1;
        snprintf(msg, sizeof msg,
                 STAMP " mode h installed: hook 0x6AF5F3 patched, resume 0x6AF5F8"
                 " (push edi) -- d's shape, the one the clean mode-d run proved,"
                 " NOT b's [esp+48] frame, detour at %#x, fixup audits the first"
                 " chunk whole and dumps every chunk's ofss -- no .dat read, nothing"
                 " allocated, no chunk header touched (shadows stay stock)",
                 (unsigned)(size_t)g_stub);
        LogVerb(msg);
        return 1;
    }

    f = fopen(g_datPath, "rb");
    if (!f) {
        /* the path is printed because that is the one thing this refusal is
         * always about: the .dat lives next to the DLL, under the name
         * shadowmaps.dat, and a variant file that was not renamed lands here
         * with nothing in the log to point at it */
        snprintf(msg, sizeof msg,
                 STAMP " no shadowmaps.dat where the DLL is: the client keeps"
                 " stock shadows; looked in '%s' (rename the variant .dat to that"
                 " name, it belongs beside the DLL, not beside the game)",
                 g_datPath);
        LogMsg(msg);
        return 0;
    }

    if (sizeof(ShadowDatHeader) != SHADOW_DAT_HEADER_BYTES) {
        LogMsg(STAMP " build defect: shadow_dat.h header size mismatch, refusing to patch");
        fclose(f);
        return 0;
    }
    if (ReadBlock(f, &head, SHADOW_DAT_HEADER_BYTES) != SHADOW_DAT_HEADER_BYTES) {
        LogMsg(STAMP " shadowmaps.dat is too short: refusing to patch");
        fclose(f);
        return 0;
    }
    if (head.magic[0] != 'S' || head.magic[1] != 'H' || head.magic[2] != 'D' || head.magic[3] != 'W') {
        LogMsg(STAMP " shadowmaps.dat has the wrong magic (want SHDW): refusing to patch");
        fclose(f);
        return 0;
    }
    if (head.version != SHADOW_DAT_VERSION) {
        LogMsg(STAMP " shadowmaps.dat version is not 1 (re-extract with the current script): refusing to patch");
        fclose(f);
        return 0;
    }
    if (head.map[23] != 0) { /* need a NUL inside the 24 to compare safely */
        LogMsg(STAMP " shadowmaps.dat map name is not NUL-terminated: refusing to patch");
        fclose(f);
        return 0;
    }
    if (head.variant[39] != 0) { /* the log line prints it, so it needs a NUL too */
        LogMsg(STAMP " shadowmaps.dat variant label is not NUL-terminated: refusing to patch");
        fclose(f);
        return 0;
    }
    if (head.payloadBytes % SHADOW_DAT_PAYLOAD_BYTES != 0 ||
        head.payloadBytes / SHADOW_DAT_PAYLOAD_BYTES != head.payloadUnique) {
        /* division, not multiply: payloadUnique * 512 can wrap in a 32-bit
         * field, and a wrapped equality would let a crafted header through */
        LogMsg(STAMP " shadowmaps.dat payload size disagrees with its payload count: refusing to patch");
        fclose(f);
        return 0;
    }
    if (head.payloadBytes > 0x40000000u) { /* 1 GiB arena: far past any real set */
        LogMsg(STAMP " shadowmaps.dat payload area is too big to keep resident: refusing to patch");
        fclose(f);
        return 0;
    }
    if (head.entryCount == 0 || head.payloadUnique == 0 || head.entryCount > 20u * 1000 * 1000) {
        LogMsg(STAMP " shadowmaps.dat entry counts are not sane: refusing to patch");
        fclose(f);
        return 0;
    }

    /* the map runs, before the size rule: mapCount decides the entry stride, and
     * the rule has to say how long the file is WITH the record table. mapCount 0
     * is the single-map file the format had before -- one run, named by the
     * header's map field, 8-byte entries -- so every .dat written so far still
     * loads unchanged. mapCount 1..64 puts a table of 28-byte records between the
     * header and the entries, and every entry then carries its run's index, which
     * is what lets one .dat carry several maps: a chunk key is only unique inside
     * one map, so the run the per-chunk gate searches is what keeps Azeroth's
     * shadows off Kalimdor's chunks. */
    mapCount = head.mapCount;
    stride = 8u;
    rec_bytes = 0;
    rec = NULL;
    if (mapCount > SHADOW_DAT_MAP_MAX) {
        LogMsg(STAMP " shadowmaps.dat names too many maps (want at most 64): refusing to patch");
        fclose(f);
        return 0;
    }
    if (mapCount) {
        unsigned total = 0;
        stride = 12u;
        rec_bytes = (size_t)mapCount * SHADOW_DAT_MAP_RECORD_BYTES;
        rec = (unsigned char *)malloc(rec_bytes);
        if (!rec) {
            LogMsg(STAMP " out of memory for the map record table: refusing to patch");
            fclose(f);
            return 0;
        }
        if (ReadBlock(f, rec, rec_bytes) != rec_bytes) {
            LogMsg(STAMP " map record table read failed: refusing to patch");
            free(rec);
            fclose(f);
            return 0;
        }
        start = 0;
        for (i = 0; i < mapCount; ++i) {
            const unsigned char *r = rec + (size_t)i * SHADOW_DAT_MAP_RECORD_BYTES;
            if (r[0] == 0 || r[23] != 0) {
                LogMsg(STAMP " a map record has no name or one that does not fit its 24 bytes: refusing to patch");
                free(rec);
                fclose(f);
                return 0;
            }
            memcpy(g_map_name[i], r, 24);
            g_map_name[i][23] = 0;
            /* the record's entryCount is at +24 */
            g_map_start[i] = start;
            g_map_len[i] = ((const unsigned *)(r + 24))[0];
            start += g_map_len[i];
            total += g_map_len[i];
        }
        if (total != head.entryCount) {
            LogMsg(STAMP " the map records' entry counts do not add up to the header's entryCount: refusing to patch");
            free(rec);
            fclose(f);
            return 0;
        }
        g_mapCount = mapCount;
    } else {
        /* one run, named by the header */
        g_mapCount = 1;
        g_map_start[0] = 0;
        g_map_len[0] = head.entryCount;
        memcpy(g_map_name[0], head.map, 24);
        g_map_name[0][23] = 0;
    }
    memcpy(g_dat_map, head.map, 24); /* the file's label: a several-map writer
                                       * puts "MULTI" here, and the names that
                                       * gate a chunk are g_map_name's */
    /* the joined list the mismatch line prints, so a wrong-map run says every
     * name the file does answer for, not just one */
    g_dat_maps_txt[0] = 0;
    shown = 0;
    for (i = 0; i < g_mapCount; ++i) {
        if (shown + 32 >= sizeof g_dat_maps_txt)
            break;
        snprintf(g_dat_maps_txt + shown, sizeof g_dat_maps_txt - shown,
                 "%s%s", shown ? " " : "", g_map_name[i]);
        shown += (unsigned)strlen(g_map_name[i]) + (shown ? 1u : 0u);
    }

    table_bytes = (size_t)head.entryCount * stride;
    expect_size = SHADOW_DAT_HEADER_BYTES + rec_bytes + table_bytes + head.payloadBytes;
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    if (fsize < 0 || (size_t)fsize != expect_size) {
        LogMsg(STAMP " shadowmaps.dat size does not match its header (truncated or junk appended): refusing to patch");
        free(rec);
        fclose(f);
        return 0;
    }

    ent = (unsigned char *)malloc(table_bytes);
    if (!ent) {
        LogMsg(STAMP " out of memory for the entry table: refusing to patch");
        free(rec);
        fclose(f);
        return 0;
    }
    fseek(f, (long)(SHADOW_DAT_HEADER_BYTES + rec_bytes), SEEK_SET);
    if (ReadBlock(f, ent, table_bytes) != table_bytes) {
        LogMsg(STAMP " entry table read failed: refusing to patch");
        free(ent);
        free(rec);
        fclose(f);
        return 0;
    }
    if (rec)
        free(rec); /* the names are copied out into g_map_name; the table itself
                     * is not needed again, and a several-map file is read once */

    g_entryCount = head.entryCount;
    g_keys = (unsigned *)malloc((size_t)head.entryCount * 4u);
    g_offs = (unsigned *)malloc((size_t)head.entryCount * 4u);
    if (!g_keys || !g_offs) {
        LogMsg(STAMP " out of memory for the key arrays: refusing to patch");
        free(ent);
        fclose(f);
        return 0;
    }
    prev_key = 0;
    run = 0;
    start = 0;
    fresh = 1; /* the first entry of a run has nothing to be compared against */
    for (i = 0; i < head.entryCount; ++i) {
        unsigned base = i * (stride / 4u);
        unsigned k = ((unsigned *)ent)[base];
        unsigned off = ((unsigned *)ent)[base + 1u];
        if (stride == 12u) {
            unsigned mp = ((unsigned *)ent)[base + 2u];
            /* entries are grouped by map, in record order, so the run boundary
             * is where the entries pass this run's length; a jump back to an
             * earlier index would make a run non-contiguous, and the per-chunk
             * search counts on a run being one span of the arrays */
            while (run + 1u < mapCount && i >= start + g_map_len[run]) {
                start += g_map_len[run];
                ++run;
                fresh = 1;
            }
            if (mp >= mapCount || mp != run) {
                LogMsg(STAMP " an entry's map index does not follow the record table's grouping: refusing to patch");
                free(ent);
                fclose(f);
                return 0;
            }
        }
        if (k > ((SHADOW_DAT_COORD_MAX << SHADOW_DAT_KEY_SHIFT) | SHADOW_DAT_COORD_MAX)) {
            LogMsg(STAMP " an entry key is outside the 10-bit chunk grid: refusing to patch");
            free(ent);
            fclose(f);
            return 0;
        }
        if (off % SHADOW_DAT_PAYLOAD_BYTES != 0 || off + SHADOW_DAT_PAYLOAD_BYTES > head.payloadBytes) {
            LogMsg(STAMP " an entry payload offset is outside the payload area: refusing to patch");
            free(ent);
            fclose(f);
            return 0;
        }
        /* keys ascend strictly inside a run. The same key in two runs is the
         * same chunk number of two different maps, which is legitimate: the
         * per-chunk gate only ever searches one of the two runs. */
        if (!fresh && k <= prev_key) {
            LogMsg(STAMP " entry keys are not strictly ascending inside a map's run: refusing to patch");
            free(ent);
            fclose(f);
            return 0;
        }
        g_keys[i] = k;
        g_offs[i] = off;
        prev_key = k;
        fresh = 0;
    }
    free(ent);

    /* the resident payload arena, whole (modes a and f). The call below is the
     * one sv3 and sv4 installed with, so mode a re-asks the memory question
     * with a form that is already known to succeed here: dwFlags 0x3000 is
     * MEMRESIDENT_ | MEMCOMMIT_, which IS a bit-or, while flProtect takes ONE
     * level from <winnt.h>'s family -- 0x4 for data (0x40 is for the executable
     * detour page), and every OR of two levels (0x3, 0x7, 0xF) is refused with
     * GetLastError 0x57, which is what sv2's three refusals were. Mode a is the
     * run that proved this reservation harmless: it holds the whole 34 MB arena
     * resident and returns before the hook, and a real sv9 run of it entered the
     * world with no crash, so nothing about the earlier 0xC0000005 is memory. */
    arena = (unsigned char *)VirtualAlloc(NULL, (SIZE_T)head.payloadBytes,
                                          0x00003000u, 0x00000004u);
    if (!arena) {
        snprintf(msg, sizeof msg, STAMP " VirtualAlloc for the shadow arena failed"
                 " (asked %u bytes, dwFlags 0x3000, protect 0x4), GetLastError"
                 " %#x: refusing to patch", head.payloadBytes, GetLastError());
        LogMsg(msg);
        fclose(f);
        return 0;
    }
    /* the offset fits in a long: the payload area is capped at 1 GiB above, and
     * the table at 20 million entries x 8 bytes, so this file is well under the
     * 2 GiB ftell limit on either target */
    /* the payload area follows BOTH tables: the entry table sits behind the
     * record table, so skipping only the entries shifts every payload by
     * rec_bytes (= 336 for a 12-map file) and hands a chunk the tail of one
     * map and the head of the next, which is what made a multi-map set look
     * mirrored and rotated in game while every legacy file (rec_bytes 0) drew
     * correctly. The size rule above already counts both tables; the read has
     * to agree with it */
    fseek(f, (long)(SHADOW_DAT_HEADER_BYTES + rec_bytes + table_bytes), SEEK_SET);
    if (ReadBlock(f, arena, (size_t)head.payloadBytes)
        != (size_t)head.payloadBytes) {
        LogMsg(STAMP " payload area read failed: refusing to patch");
        fclose(f);
        return 0;
    }
    fclose(f);
    /* the offsets stay the real ones: every entry addresses its own payload
     * inside this arena, which is what makes the arena live memory the client
     * can point at (sv6's single-page stand-in is gone, and the sv10 mode-f run
     * really drew this set: its first match logged "shadow drawn from arena+0"
     * for gy 517 gx 448 and the ground showed the baked shadows) */
    g_arena = arena;

    /* one line naming every run the file answers for, entry counts included, so
     * a run that draws nothing can be read against the client's own map without
     * guessing which map the file was cut for -- and a file that carries several
     * says so here, which is the point of the map table */
    snprintf(msg, sizeof msg, STAMP " shadowmaps.dat answers for %u map(s):", g_mapCount);
    {
        size_t used = strlen(msg);
        for (i = 0; i < g_mapCount; ++i) {
            int n;
            n = snprintf(msg + used, sizeof msg - used, " %s %u entries", g_map_name[i], g_map_len[i]);
            if (n < 0)
                break;
            used += (size_t)n;
            if (used >= sizeof msg) {
                msg[sizeof msg - 1] = 0; /* snprintf NUL-terminates inside the
                                           * buffer; this only stops the trail
                                           * getting any further, the gate above
                                           * does not read this line */
                break;
            }
        }
    }
    LogVerb(msg);

    if (g_mode == 'a') {
        snprintf(msg, sizeof msg,
                 STAMP " mode a installed footprint only: map '%s' variant '%s',"
                 " entries %u, payloads %u, arena %u MB resident at %#x..%#x."
                 " NO hook, no fixup call, no chunk header touched, so the"
                 " client runs its own code path whole: if it crashes entering"
                 " the world here while mode n was clean, the reservation is"
                 " the cause and the arena has to become bounded, and the popup"
                 " is expected because this mode patches nothing on purpose",
                 (const char *)head.map, (const char *)head.variant,
                 head.entryCount, head.payloadUnique,
                 (unsigned)(head.payloadBytes / (1024u * 1024u)),
                 (unsigned)(size_t)arena,
                 (unsigned)(size_t)((size_t)arena + (size_t)head.payloadBytes));
        LogVerb(msg);
        g_diagnostic = 1;
        return 1;
    }

    /* mode f: hook plus writes, the only rung that is expected to draw -- and the
     * sv10 run of it did: the world loaded, the baked morning set showed on the
     * ground, the first match logged "shadow drawn from arena+0" (sv13: that
     * line, and every other audit line, is on the diagnostic level now, so an
     * sv13 mode-f run without a v in the mode file says all of this by showing
     * it on the ground and writes exactly one install line), and the run
     * ended with a chunk the set legitimately has no payload for. It rides d's
     * shape, the one the clean mode-d run proved, not b's return form. */
    g_allow_write = 1;
    if (!PatchHook('f'))
        return 0;

    memcpy(g_dat_map, head.map, 24);
    g_ready = 1;
    snprintf(msg, sizeof msg,
             STAMP " mode f installed: map '%s' variant '%s', entries %u,"
                    " payloads %u, arena %u MB at %#x..%#x, hook 0x6AF5F3 patched"
                    " with resume 0x6AF5F8 -- d's shape, the one the clean mode-d run proved,"
                    " NOT b's frame, detour at %#x; matched chunks"
                    " get flags |= 1, hdr+0x2c = arena buffer - hdr and"
                    " hdr+0x30 = 512, so the client's own gate, CreateShadowTexture"
                    " and the decoders read live .dat bytes. logging: %s (add a v"
                    " to '%s\\shadow-mod.mode' for the audit and per-chunk trail)",
             (const char *)head.map, (const char *)head.variant, head.entryCount,
             head.payloadUnique,
             (unsigned)(head.payloadBytes / (1024u * 1024u)),
             (unsigned)(size_t)arena,
             (unsigned)(size_t)((size_t)arena + (size_t)head.payloadBytes),
             (unsigned)(size_t)g_stub,
             (g_verbose ? "verbose" : "quiet"), g_dir);
    LogMsg(msg);
    return 1;
}

/* the single latch both triggers funnel through, ClassicAPI's pattern */
static volatile LONG g_claimed = 0, g_done = 0, g_result = 1;

static DWORD EnsureInitialized(void)
{
    if (InterlockedCompareExchange(&g_claimed, 1, 0) == 0) {
        /* the return code is VanillaFixes' contract: 0 = success, anything
         * else gets its generic "was not loaded properly (load function failed)"
         * popup. 1 = a real refusal (wrong client build, unreadable .dat,
         * allocation refused, half-patched store). 4 = a mode that installed
         * NOTHING ON PURPOSE (n and a), which is not a failure of the module:
         * the popup is still shown, README says so nobody reads it as a bug */
        g_result = Install() ? 0l : (g_diagnostic ? 4l : 1l);
        InterlockedExchange(&g_done, 1);
    } else {
        while (InterlockedCompareExchange(&g_done, 0, 0) == 0)
            Sleep(1);
    }
    return (DWORD)g_result;
}

static DWORD WINAPI InitWorker(LPVOID p)
{
    (void)p;
    EnsureInitialized();
    return 0;
}

/* VanillaFixes' post-injection entry point: called on the game's MAIN thread
 * (GetProcAddress(module,"Load")), outside the loader lock, no timeout.
 * Exported undecorated as "Load" via shadowmaps.def. 0 = success. */
DWORD Load(void)
{
    return EnsureInitialized();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_self = hModule;
        /* LogMsg's zero: GetTickCount here is the moment this DLL was loaded,
         * and every line is stamped relative to it. GetTickCount itself counts
         * milliseconds since the SYSTEM booted, so its raw value is the same
         * clock for every client start on one boot and cannot separate runs --
         * which is how an earlier read of this file attributed eleven lines to
         * one start. ms-since-load restarts at 0 in each process. */
        g_t0 = GetTickCount();
        /* Our own directory, from our own handle: GetModuleFileNameA on the
         * HMODULE DllMain was handed gives the full path of this DLL, and
         * cutting at the last backslash leaves the directory it sits in. No
         * DDK/ATL helper (GetImageDirectoryA is a WTL loader helper, not a
         * Win32 export) and no assumption that the DLL lives in the same
         * folder as Wow.exe: dlls.txt may name an absolute path anywhere. */
        if (!GetModuleFileNameA(hModule, g_dir, MAX_PATH))
            g_dir[0] = 0;
        else {
            char *slash;
            unsigned n, last;
            n = (unsigned)strlen(g_dir);
            slash = NULL;
            for (last = 0; last < n; ++last)
                if (g_dir[last] == '\\' || g_dir[last] == '/')
                    slash = g_dir + last;
            if (slash)
                slash[0] = 0;
            else
                g_dir[0] = 0;
        }

        /* Install nothing here -- loader lock, and under VanillaFixes the
         * worker would race the DllMains of the dlls.txt DLLs injected after
         * us. VfPatcher.dll is injected before any dlls.txt DLL, so its
         * presence means VF will call our `Load` on the main thread later. */
        if (GetModuleHandleW(L"VfPatcher.dll") == NULL) {
            HANDLE worker = CreateThread(NULL, 0, InitWorker, NULL, 0, NULL);
            if (worker != NULL)
                CloseHandle(worker);
        }
    }
    /* DLL_PROCESS_DETACH: the arena is left allocated. The client has no
     * unload path that re-injects mid-session, and leaking at process exit
     * is honest and cheap. */
    return TRUE;
}
