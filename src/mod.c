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
 * What the sv9 ladder settled, and what sv10 changes because of it. Four real
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

#define STAMP "sv10" /* build stamp, printed in every log line: an old build of
                     * this DLL sitting next to a new .dat is the most common
                     * confusion, so the line says which build made it, and the
                     * mode line below names which of the seven rungs ran */

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

static int g_log_mismatch;            /* one-shot log flags, fixup is hot   */
static int g_log_nomatch;
static int g_log_first;
static int g_log_hits;                /* how many progress lines were logged */
static int g_log_field;               /* a probe dword moved: logged once   */
static int g_log_audit;               /* the first chunk entering Load audited */
static unsigned g_audit_seen;         /* per-chunk header dumps logged so far */
static int g_silent;                  /* mode b: the call returns before doing
                                          anything at all, file I/O included */
static char g_mode = 'f';             /* the mode letter, see ReadModeFile     */
static int g_allow_write;             /* only mode f lets fixup_chunk write    */
static int g_diagnostic;              /* installed nothing on purpose: Load returns 4 */

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
 * the line. The user is asked to delete the log between the rungs anyway. */
static void LogMsg(const char *text)
{
    FILE *f = fopen(g_logPath, "a");
    if (!f)
        return;
    fprintf(f, "[%u ms after this DLL was loaded] %s\n",
            (unsigned)(GetTickCount() - g_t0), text);
    fclose(f);
}

/* sv7's mode letter: the first non-blank character of <dir>\shadow-mod.mode,
 * lower-cased, and only n/a/b/c/d/h/f are honoured -- an unreadable, empty or
 * junk file means f, so forgetting the file gives the full path, never a silent
 * no-op. A mode FILE rather than several DLLs is what lets the user walk README
 * .md's ladder by editing one letter, with no rebuild between the steps and no
 * risk of shipping an old DLL for the wrong step (every log line still names
 * the build stamp and the mode that ran). */
static char ReadModeFile(void)
{
    char path[MAX_PATH];
    FILE *f;
    int c;
    char m;

    snprintf(path, sizeof path, "%s\\shadow-mod.mode", g_dir);
    f = fopen(path, "r");
    if (!f)
        return 'f';
    m = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c == 'n' || c == 'a' || c == 'b' || c == 'c' || c == 'd'
            || c == 'h' || c == 'f')
            m = (char)c;
        break;
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
 * to be inside one record is printed but never dereferenced. */
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
        LogMsg(line);
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
 * fault before something else swallows it. */
static void InstallHandler(void)
{
    HMODULE k;
    PVOID tag;
    PFN_ADDVEH adder;
    char msg[256];

    k = GetModuleHandleA("kernel32.dll");
    if (k == NULL) {
        LogMsg(STAMP " handler: no kernel32 to resolve AddVectoredExceptionHandler from, the dying-chunk instrument stays off");
        return;
    }
    adder = (PFN_ADDVEH)GetProcAddress(k, "AddVectoredExceptionHandler");
    if (adder == NULL) {
        LogMsg(STAMP " handler: kernel32 does not export AddVectoredExceptionHandler, the dying-chunk instrument stays off");
        return;
    }
    tag = adder(1u, DyingChunkHandler);
    if (tag == NULL) {
        snprintf(msg, sizeof msg, STAMP " handler: AddVectoredExceptionHandler refused"
                 " (GetLastError %#x), the dying-chunk instrument stays off",
                 GetLastError());
        LogMsg(msg);
        return;
    }
    g_handler_on = 1;
    LogMsg(STAMP " handler: first-chance vectored handler registered, so the next access violation logs the chunk that was being built and the address the client read (the client's own report is still written afterwards)");
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
    if (!g_first_call) {
        g_first_call = 1;
        snprintf(msg, sizeof msg,
                 STAMP " first fixup call: this %#x record %#x after %#u visits, mode %c -- the hooking rungs print this once, mode c never does",
                 (unsigned)(size_t)self_, (unsigned)(size_t)record_,
                 (unsigned)g_chunk_visits, g_mode);
        LogMsg(msg);
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

    /* the very first chunk to reach Load is audited whole, before the map
     * check and before anything could be written: the crash reports say the
     * MCVT cursor was unreadable on a chunk this DLL never matched, so the
     * first thing to settle is what its header actually holds in this client.
     * It is audited in EVERY mode that hooks, so the same block of lines is
     * what mode h and mode f print. */
    if (!g_log_audit) {
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
     * header the client has is not the header the .adt file has). */
    if (g_audit_seen < AUDIT_LOG_MAX) {
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
        LogMsg(msg);
        if (g_audit_seen == AUDIT_LOG_MAX) {
            snprintf(msg, sizeof msg,
                     "per-chunk header dump cap reached (%u chunks): no more of these"
                     " lines will be written, so a chunk that dies after this point"
                     " is not named by one -- raise AUDIT_LOG_MAX to see further",
                     (unsigned)AUDIT_LOG_MAX);
            LogMsg(msg);
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
    size_t table_bytes, expect_size;
    long fsize;
    unsigned char *ent;
    unsigned i, prev_key;
    unsigned char *arena;
    unsigned char *site = (unsigned char *)(size_t)ADDR_LOAD;
    char msg[512];

    snprintf(g_datPath, sizeof g_datPath, "%s\\shadowmaps.dat", g_dir);
    snprintf(g_logPath, sizeof g_logPath, "%s\\shadow-mod.log.txt", g_dir);

    /* the mode first, before anything else can happen: the letter decides how
     * much of this function runs at all, and the line names it so a log can
     * never be read as "which DLL did this" again -- stamp plus mode, on every
     * step */
    g_mode = ReadModeFile();
    snprintf(msg, sizeof msg,
             STAMP " mode '%c' read from '%s\\shadow-mod.mode' (no such file means f): n = inert, a = .dat plus"
             " arena, no hook, b = hook plus an empty call, c = the detour alone"
             " with no call at all, d = the same call from the client's own frame"
             " at 0x6AF5F3, h = hook plus audit, no payload, f = full",
             g_mode, g_dir);
    LogMsg(msg);

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
        LogMsg(msg);
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
        LogMsg(msg);
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
        LogMsg(msg);
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
        LogMsg(msg);
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
    fseek(f, (long)(SHADOW_DAT_HEADER_BYTES + table_bytes), SEEK_SET);
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
        LogMsg(msg);
        g_diagnostic = 1;
        return 1;
    }

    /* mode f: hook plus writes, the only rung that is expected to draw -- and the
     * sv10 run of it did: the world loaded, the baked morning set showed on the
     * ground, the first match logged "shadow drawn from arena+0", and the run
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
                    " and the decoders read live .dat bytes",
             (const char *)head.map, (const char *)head.variant, head.entryCount,
             head.payloadUnique,
             (unsigned)(head.payloadBytes / (1024u * 1024u)),
             (unsigned)(size_t)arena,
             (unsigned)(size_t)((size_t)arena + (size_t)head.payloadBytes),
             (unsigned)(size_t)g_stub);
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
