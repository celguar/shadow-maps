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
 * wrong map is ignored rather than half-applied.
 *
 * This build is the diagnostic step sv6, not the shipping shape: it audits
 * the first chunk's MCNK header and the first matched chunk's whole sub-
 * record table into the log, and it points every matched chunk at ONE
 * 4 KB page instead of the 34 MB arena, so the hook and the shadow write
 * can be tested without the reservation the crash reports point at.
 * README.md's "current diagnostic" section says what the run has to prove.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shadow_dat.h"

#define STAMP "sv6" /* build stamp, printed in every log line: an old build of
                    * this DLL sitting next to a new .dat is the most common
                    * confusion, so the line says which build made it. sv6 is
                    * the header-audit, one-page diagnostic described in README
                    * .md's "current diagnostic" section */

/* ---------------------------------------------------------------- pinned **/

#define ADDR_LOAD        0x6AF5F0u /* CMapChunk::Load(this, record, first) */
#define ADDR_GATE        0x6AF6EAu /* the shadow gate inside Load          */
#define ADDR_MAP_NAME    0xC961A0u /* s_mapName: char[256], written by CMap::Load */

/* push ebp / mov ebp,esp / mov eax,[ebp+8] -- the 6 bytes the trampoline
 * replays; the E9 covers the first 5, resume is at +6. */
static const unsigned char k_prologue[] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08
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

/* ------------------------------------------------------- runtime state ***/

static HMODULE g_self;
static char g_dir[MAX_PATH];          /* the directory this DLL sits in     */
static char g_datPath[MAX_PATH];      /* <dir>\shadowmaps.dat, the fixed name */
static char g_logPath[MAX_PATH];      /* <dir>\shadow-mod.log.txt           */
static volatile int g_ready;          /* gate for fixup: set after install  */
static unsigned g_entryCount;
static unsigned *g_keys;              /* sorted ascending, binary searched  */
static unsigned *g_offs;              /* payload byte offset inside arena   */
static unsigned char *g_arena;        /* payloadUnique x 512 live shadow maps */
static unsigned char *g_stub;         /* the executable page holding the detour */
static unsigned char g_dat_map[24];   /* header's map name, NUL-terminated  */
static DWORD g_resume;                /* runtime resume address for the asm */
static volatile DWORD g_hits;         /* chunks that found a payload        */
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

static int g_log_mismatch;            /* one-shot log flags, fixup is hot   */
static int g_log_nomatch;
static int g_log_first;
static int g_log_hits;                /* how many progress lines were logged */
static int g_log_field;               /* a probe dword moved: logged once   */
static int g_log_audit;               /* the first chunk entering Load audited */

static DWORD WINAPI InitWorker(LPVOID p);
static int Install(void);
static int BuildStub(unsigned fixup_target, unsigned resume_at);

/* ------------------------------------------------------------ logging ***/

static void LogMsg(const char *text)
{
    FILE *f = fopen(g_logPath, "a");
    if (!f)
        return;
    fprintf(f, "[%u] %s\n", (unsigned)GetTickCount(), text);
    fclose(f);
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
 * (a fourcc of 'MCVT' sits at it, and the fault is a mapping the client did
 * not get) or something this DLL moved. A slot of 0 means the .adt carries no
 * such sub-record; a value too big to be inside one record is printed but
 * never dereferenced. */
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
    LogMsg(line);

    for (i = 0; i < TABLE_COUNT; ++i) {
        v = ((const unsigned *)(hdr + k_table_ofs[i]))[0];
        cc[0] = 0;
        if (v != 0 && v < AUDIT_OFS_MAX)
            FourccAt(hdr + v, cc);
        snprintf(line, sizeof line,
                 "audit %s: %s at hdr+%#x is %#x -> hdr+%#x, fourcc there '%s'",
                 who, k_table_cc[i], k_table_ofs[i], v,
                 (unsigned)(size_t)(hdr + v), cc);
        LogMsg(line);
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
    int lo, hi, mid, hit;
    unsigned char *hdr;
    char msg[768];

    if (!g_ready || !self_ || !record_)
        return;

    /* the very first chunk to reach Load is audited whole, before the map
     * check and before anything could be written: the crash reports say the
     * MCVT cursor was unreadable on a chunk this DLL never matched, so the
     * first thing to settle is what its header actually holds in this client */
    if (!g_log_audit) {
        g_log_audit = 1;
        AuditHeader(record_, "first chunk");
    }

    client_map = (const unsigned char *)ADDR_MAP_NAME;
    if (!CiEqName((const char *)g_dat_map, (const char *)client_map)) {
        if (!g_log_mismatch) {
            g_log_mismatch = 1;
            snprintf(msg, sizeof msg,
                     "fixup: client map '%s' vs payload map '%s': the payload is ignored on this map",
                     (const char *)client_map, (const char *)g_dat_map);
            LogMsg(msg);
        }
        return;
    }

    gx = ((const unsigned *)(const char *)((char *)self_ + FIELD_GLOBAL_COL))[0];
    gy = ((const unsigned *)(const char *)((char *)self_ + FIELD_GLOBAL_ROW))[0];
    if (gx > SHADOW_DAT_COORD_MAX || gy > SHADOW_DAT_COORD_MAX)
        return;
    key = (gy << SHADOW_DAT_KEY_SHIFT) | gx;

    lo = 0;
    hi = (int)g_entryCount - 1;
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
        if (!g_log_nomatch) {
            g_log_nomatch = 1;
            snprintf(msg, sizeof msg,
                     "fixup: first chunk with no payload: gy %u gx %u key %u (the bake left it without a shadow map, or its tile is not in this set)",
                     gy, gx, key);
            LogMsg(msg);
        }
        return;
    }

    /* hdr is what the client itself calls record + 8: the MCNK magic is
     * record's start, the header fields start after it (0x6AF970: lea
     * edx,[eax+8]). ofsMCSH is relative to hdr, and the game resolves every
     * sub-record as hdr + ofs, so hdr + ofs lands in the arena. */
    if (!g_log_first)
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
     * ceiling, so the last line under it names the tiles the set does not cover) */
    if ((g_hits % 10000u) == 0u && g_log_hits < 60) {
        ++g_log_hits;
        snprintf(msg, sizeof msg, "fixup: %u chunks matched so far, of %u entries",
                 (unsigned)g_hits, g_entryCount);
        LogMsg(msg);
    }

    if (!g_log_first) {
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
        LogMsg(msg);
    }
}

/* --------------------------------------------------------- the detour ***/

/* The detour, as bytes: the one hand-written x86-32 stub the E9 at 0x6AF5F0
 * lands in, emitted once at install into a private executable page with the
 * two absolute addresses below patched into its two imm32 slots. It is why
 * this DLL needs no assembler, and its stack math is checked against real
 * bytes by verify_dll.py.
 *
 * Stack on entry: [esp]=ret, [esp+4]=record, [esp+8]=first; `this` is in ecx
 * (the client's Load is thiscall). pushfd/pushad preserve every register the
 * game had; fixup_chunk is passed (this, record) on the stack, and MSVC gives a
 * static C function like this a PLAIN ret -- opcode c3, no imm16 -- so the
 * caller, not the callee, has to drop the two argument dwords. This is not a
 * mistake a compiler would catch: cleanup off by eight means popad runs at esp-44
 * instead of esp-36, every register comes back from the wrong slot (ecx holds
 * the client's old ebx), and Load's own next act is mov [esi+0x168], eax at
 * 0x6AF5FF with esi = ecx = this -- an ACCESS_VIOLATION on the first chunk
 * loaded, which is exactly what an in-game crash at 0x6AF5FF means. verify_dll.py
 * reads the epilogue out of the linked image and checks this add against it:
 *   -4 -32 -8 (pushfd pushad sub) = esp-44: [esp+48] = entry esp+4 = record
 *   -4 -4 (push eax push ecx), then call: plain ret leaves the args behind, so
 *     the callee returns at esp-52
 *   +16 (add esp,16 = the 8 arg bytes + the 8 scratch bytes), +32 (popad, which
 *     starts at esp-36 where pushad put its first slot), +4 (popfd) = entry esp
 * The 6 replayed bytes then set up ebp and eax = record exactly as the game
 * expects (ecx is back from pushad), and the jmp resumes Load's own body.
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

/* Emits the detour into a page of its own: VirtualAlloc with execute rights,
 * the same as MinHook's heap trampolines, and never a rewrite of a file-backed
 * page. The two imm32 slots are patched with the values this process sees now,
 * which is what lets the DLL relocate: fixup_target is fixup_chunk's address in
 * the relocated image, resume_at is a client address (the client never
 * relocates, so it is a plain constant). */
static int BuildStub(unsigned fixup_target, unsigned resume_at)
{
    unsigned char *page;
    char msg[256];

    if (sizeof stub_code != STUB_BYTES) {
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
    memcpy(page, stub_code, sizeof stub_code);
    memcpy(page + STUB_SLOT_FIXUP, &fixup_target, 4);
    memcpy(page + STUB_SLOT_RESUME, &resume_at, 4);
    g_stub = page;
    return 1;
}

/* --------------------------------------------------------- install *****/

static int Install(void)
{
    FILE *f;
    ShadowDatHeader head;
    size_t table_bytes, expect_size;
    long fsize;
    unsigned char *ent;
    unsigned i, prev_key;
    unsigned char *arena;
    unsigned char *site = (unsigned char *)(size_t)ADDR_LOAD;
    unsigned char *page_base;
    unsigned char patched[8];
    DWORD old_protect;
    int perms;
    long rel;
    char msg[512];

    snprintf(g_datPath, sizeof g_datPath, "%s\\shadowmaps.dat", g_dir);
    snprintf(g_logPath, sizeof g_logPath, "%s\\shadow-mod.log.txt", g_dir);

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

    table_bytes = (size_t)head.entryCount * 8u;
    expect_size = SHADOW_DAT_HEADER_BYTES + table_bytes + head.payloadBytes;
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    if (fsize < 0 || (size_t)fsize != expect_size) {
        LogMsg(STAMP " shadowmaps.dat size does not match its header (truncated or junk appended): refusing to patch");
        fclose(f);
        return 0;
    }

    ent = (unsigned char *)malloc(table_bytes);
    if (!ent) {
        LogMsg(STAMP " out of memory for the entry table: refusing to patch");
        fclose(f);
        return 0;
    }
    fseek(f, SHADOW_DAT_HEADER_BYTES, SEEK_SET);
    if (ReadBlock(f, ent, table_bytes) != table_bytes) {
        LogMsg(STAMP " entry table read failed: refusing to patch");
        free(ent);
        fclose(f);
        return 0;
    }

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
    for (i = 0; i < head.entryCount; ++i) {
        unsigned k = ((unsigned *)ent)[i * 2];
        unsigned off = ((unsigned *)ent)[i * 2 + 1];
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
        if (i && k <= prev_key) {
            LogMsg(STAMP " entry keys are not strictly ascending: refusing to patch");
            free(ent);
            fclose(f);
            return 0;
        }
        g_keys[i] = k;
        g_offs[i] = off;
        prev_key = k;
    }
    free(ent);

    /* sv6, the diagnostic step. sv4's runs died at 0x6B0FB7 -- `mov ecx,[ebx]`
     * with ebx = this+0xf14, the MCVT cursor the resolver made from hdr+0x14 --
     * on a chunk that has NO payload in the set, i.e. one this DLL never wrote
     * to, and every value in that header came straight from the .adt file the
     * client was reading. The one thing in this DLL that can make the client's
     * own mapping of an ADT fail is the 34 MB resident arena: this client is
     * 32-bit and Azeroth alone already asks it for hundreds of MB, so a 34 MB
     * reservation is exactly the kind of thing that pushes a later allocation
     * over the limit, and an allocation that failed while the header offsets
     * still look right is exactly an unreadable MCVT cursor. So this build asks
     * the same hook, the same shadow write and a real payload at a 4 KB
     * footprint instead: one page, only the first payload of the .dat read into
     * it, and every matched chunk pointed at that one payload. The shadows
     * drawn are wrong from the second matched chunk on -- this build's job is
     * to answer whether the memory path is safe, not to look right -- and the
     * audit lines say what the header of a chunk really holds. The page is
     * still allocated the way the real arena was: MEMRESIDENT_(0x1000) |
     * MEMCOMMIT_(0x2000), and the protection is a LEVEL from <winnt.h>'s
     * family, not a bit-or -- 0x1 readable, 0x2 write-combine (written but
     * never read back, a read of it faults 0xc0000005), 0x4 readable and
     * writable, 0x40 PAGE_EXECUTE_READWRITE over every mapping, and every OR
     * of two of them (0x3, 0x7, 0xF) is refused outright with GetLastError
     * 0x57; an arena of data wants 0x4, the detour page below wants 0x40. */
    arena = (unsigned char *)VirtualAlloc(NULL, 0x1000u, 0x00003000u,
                                          0x00000004u);
    if (!arena) {
        snprintf(msg, sizeof msg, STAMP " VirtualAlloc for the one diagnostic"
                 " page failed (asked 4096 bytes, alloc 0x3000, protect 0x4),"
                 " GetLastError %#x: refusing to patch", GetLastError());
        LogMsg(msg);
        fclose(f);
        return 0;
    }
    /* the offset fits in a long: the payload area is capped at 1 GiB above, and
     * the table at 20 million entries x 8 bytes, so this file is well under the
     * 2 GiB ftell limit on either target */
    fseek(f, (long)(SHADOW_DAT_HEADER_BYTES + table_bytes), SEEK_SET);
    if (ReadBlock(f, arena, SHADOW_DAT_PAYLOAD_BYTES)
        != SHADOW_DAT_PAYLOAD_BYTES) {
        LogMsg(STAMP " reading the first payload out of the .dat failed:"
                   " refusing to patch");
        fclose(f);
        return 0;
    }
    fclose(f);
    /* every entry now addresses the one payload that was read: the lookup still
     * proves a chunk is in the set, and the write still hands the client live
     * arena bytes, which is the whole point of this step */
    for (i = 0; i < head.entryCount; ++i)
        g_offs[i] = 0u;
    g_arena = arena;

    /* the detour: its own page first, then the E9 that reaches it */
    g_resume = ADDR_LOAD + 6u; /* after the 6 replayed bytes, 0x6AF5F6 */
    if (!BuildStub((unsigned)(size_t)fixup_chunk, g_resume))
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

    memcpy(g_dat_map, head.map, 24);
    g_ready = 1;
    snprintf(msg, sizeof msg,
             STAMP " diagnostic installed: map '%s' variant '%s', entries %u"
                   " payloads %u, ONE 4 KB page at %#x holds the .dat's first"
                   " payload, every matched chunk points at it, the %u MB"
                   " payload area is NOT read: if the client enters the world"
                   " the 34 MB arena was what broke it. hook 0x6AF5F0 patched,"
                   " resume 0x6AF5F6, detour at %#x",
             (const char *)head.map, (const char *)head.variant, head.entryCount,
             head.payloadUnique, head.payloadBytes / (1024u * 1024u),
             (unsigned)(size_t)arena, (unsigned)(size_t)g_stub);
    LogMsg(msg);
    return 1;
}

/* the single latch both triggers funnel through, ClassicAPI's pattern */
static volatile LONG g_claimed = 0, g_done = 0, g_result = 1;

static DWORD EnsureInitialized(void)
{
    if (InterlockedCompareExchange(&g_claimed, 1, 0) == 0) {
        g_result = Install() ? 0 : 1;
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
