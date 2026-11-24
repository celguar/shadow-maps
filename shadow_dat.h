/* shadow_dat.h -- the shadow payload file ("shadowmaps.dat") that ShadowMaps.dll
 * loads, and the layout extract_shadowmaps.py writes. Keep the two in sync.
 *
 * The .dat is what a shadow variant IS: not patched .adt files, not an archive,
 * just every chunk's 512-byte packed shadow map, once each, keyed by the chunk's
 * GLOBAL coordinates -- the same key Salmiak uses for its bake selections, so a
 * bake can be exported, stripped out of the patch archive, and handed to the
 * client without the shadows ever round-tripping through a parsed .adt.
 *
 * The key mirrors Salmiak's _bakeChunks (GlViewport.Bake.cs): a chunk's global
 * column gx = 16*tileX + indexX -- which is what the client itself keeps at
 * CMapChunk+0x8c -- and gy = 16*tileY + indexY at +0x90, both 0..1023:
 *
 *   key = (gy << 10) | gx
 *
 * A KEY IS NOT UNIQUE ACROSS MAPS. Tile 14,20 chunk 3,5 of Azeroth and of Kalimdor
 * are the same number, so the key alone cannot say which map's shadow to draw. That
 * is what the map name in front of every entry is for.
 *
 * File layout, all little-endian, nothing aligned to anything but itself:
 *
 *   ShadowDatHeader                    124 bytes
 *   mapCount ShadowDatMapRecord        28 bytes each at version 1, 32 at version 2,
 *                                      only when mapCount != 0
 *   entryCount ShadowDatEntry          8 bytes when mapCount == 0, else 12
 *   payload area                       payloadUnique x 512-byte shadow maps
 *
 * mapCount is the header's last word, the one that was `reserved` in the single-map
 * file; 0 there means "one map, named by the header's map field, 8-byte entries",
 * which is what a hand-made single-map .dat keeps writing and what the DLL still
 * reads. mapCount > 0 means the map-record table follows the header and every entry
 * carries its record's index into that table, which is how ONE .dat carries SEVERAL
 * maps -- and, from version 2 on, several shadow sets of ONE map: mapCount counts
 * records, and several records may share a map name, each owning a window of the
 * day (its h0 word). See below for the size rule and for what the clock word means.
 *
 * entryCount + payloadUnique is what dedupe leaves: chunks that share identical
 * shadow bytes share one 512-byte payload, including one shared all-zero buffer
 * so a cleared shadow really draws nothing. payloadBytes == payloadUnique*512.
 * entry.payloadOffset addresses the payload area (byte 0 = first payload), and
 * payloadOffset + 512 <= payloadBytes is enforced when the .dat loads. Payloads
 * are interned across the whole file, so two maps whose chunk bodies happen to be
 * identical share one 512-byte blob: sharing bytes is safe because what decides
 * whether a payload may be used at all is the entry's map index, not the blob.
 *
 * Entries are ordered by (mapIndex, key): one contiguous run per RECORD, in record
 * order, keys strictly ascending inside a run (the same key twice inside one record
 * is the same chunk listed twice, and is refused). Several records of ONE map are
 * therefore several runs of the same name back to back, and the same chunk key may
 * appear in each of them -- they are the same chunk of the same map at different
 * clocks, which is exactly what a preset set is. The DLL binary-searches only the
 * matched record's run, picked by clock out of the records that share the name, so
 * a key that two maps both carry can never be answered with the other map's shadow
 * and a chunk never gets another clock's shadow.
 *
 * Size rules the loader validates against, exactly (a file whose size is not the
 * rule's answer is refused, truncated or with junk appended alike):
 *   mapCount == 0:  124 + entryCount*8                  + payloadBytes
 *   mapCount >  0:  124 + mapCount*REC + entryCount*12   + payloadBytes
 *   where REC is 28 at version 1 and 32 at version 2.
 *
 * Both tables come BEFORE the pool: the map-record table sits right after the
 * header, the entry table after that, so an entry's payloadOffset addresses file
 * offset 124 + mapCount*REC + entryCount*stride (stride 8 legacy, 12 with a table).
 * A reader that seeks past the entries only fills its arena rec_bytes (= mapCount
 * * REC) short of every payload, and a 12-map file then draws a chunk's shadow as
 * the tail of one payload over the head of the next -- mirrored-and-rotated
 * shapes in about the right places, which is exactly what an sv11 run showed in
 * game. mapCount 0 means rec_bytes 0, so the two offsets are the same offset and
 * every legacy file reads the same either way.
 *
 * Bounds fields are for reporting only (the .dat's own extent, over every map it
 * carries); the DLL matches chunks by key and by the map name, never by the bounds.
 *
 * Chunks the bake left without a shadow map have NO entry: the DLL then changes
 * nothing about them and the client keeps whatever stock shadow state they have,
 * which is honest -- we never force the shadow flag on without a payload.
 *
 * SEVERAL SHADOW SETS OF ONE MAP (version 2)
 *
 * A shadow set is one bake, and a bake is one clock: the presets this project
 * ships are "the shadows as baked for 06:30", "as baked for 09:00", ... and the
 * stock set stamped 21:30, which is the one that also carries the custom areas.
 * The .dat carries them all in ONE file, as one record per set: six records named
 * Azeroth, whose h0 words are 780 (06:30), 1080 (09:00), 1380 (11:30), 1860 (15:30),
 * 2340 (19:30) and 2580 (21:30). Each record owns a run of entries like any other
 * map, so a set is a full map's worth of payloads and the pool pays for it: six
 * sets of a full continent is six times that continent's payloads, and dedupe only
 * helps where two sets' bytes are literally equal, which they are not once the
 * shadows move. That is the honest memory cost, and mode a is the rung that prints
 * it (see README) rather than leaving it to a VirtualAlloc that fails in game.
 *
 * What picks a set at runtime is the client's own day clock, read the same way the
 * sun's key tables are picked, and the rule is the same one: the set whose window
 * the clock falls in -- the record whose h0 is the greatest at or below the clock,
 * wrapping to the biggest h0 when the clock is earlier than all of them. With the
 * stamps above, 21:30's stock set owns 21:30 through midnight through 05:49 and the
 * 06:30 set starts where the light starts moving, so the night is stock shadows for
 * free and the morning set takes over at the same crossing the sun's arc makes.
 * Windows tile the day: a record's window is [h0, next h0), so there is no clock
 * with no set and no clock with two.
 */
#ifndef SHADOW_DAT_H
#define SHADOW_DAT_H 1

typedef struct ShadowDatHeader {
    char magic[4];        /* "SHDW" on disk, S H D W in order */
    unsigned version;     /* 1 or 2: 2 is what extract_shadowmaps.py writes now and
                               what the DLL reads natively; 1 is accepted as the same
                               layout with 28-byte records and no clock word, so the
                               files written so far still load (the loader refuses
                               anything else, and refuses to GUESS: a version-2 file
                               read with 28-byte records would slide the clock word
                               into the next record's name) */
    char map[24];         /* single-map file: the map these payloads belong to.
                               The DLL compares this against the client's own
                               s_mapName (0xc961a0) case-insensitively, so a
                               payload for the wrong map is ignored, not
                               half-applied. With mapCount > 0 the names that
                               gate are the map records' and this word is a
                               label only: the writer puts the map name when
                               there is one map and "MULTI" when there are
                               several, so a loader that does not know about
                               map records refuses a several-map file instead
                               of drawing one map's shadows onto another's. */
    char variant[40];     /* informational label ("morning", a stamp, whatever
                               you wrote when baking); the DLL ignores it */
    unsigned entryCount;
    unsigned payloadUnique;
    unsigned payloadBytes;
    int tileMinX, tileMinY, tileMaxX, tileMaxY;    /* inclusive tile range */
    int chunkMinX, chunkMinY, chunkMaxX, chunkMaxY; /* inclusive gx/gy range */
    unsigned tileCount;       /* distinct (map, tileX, tileY) across the file */
    unsigned mapCount;        /* 0 = single map named by `map`, 8-byte entries;
                                 1..SHADOW_DAT_MAP_MAX = one RECORD per run: a
                                 named map with several shadow sets is several
                                 records sharing one name, and entries are 12
                                 bytes carrying the record's index */
} ShadowDatHeader;

/* A version-2 record is this struct's first three fields, 32 bytes; a version-1
 * record is the first two, 28, and has no clock word (its clock is read as
 * SHADOW_DAT_H0_ALL, "this run answers for the whole day"). The DLL picks the
 * stride from the header's version, so a file an older writer wrote keeps
 * loading and a several-preset file cannot be mistaken for a one-preset one. */
typedef struct ShadowDatMapRecord {
    char name[24];          /* map name, NUL-terminated, padded; compared
                               case-insensitively against the client's s_mapName.
                               NOT unique any more: one map may be several records,
                               one per shadow preset of that map -- see the
                               SEVERAL SHADOW SETS OF ONE MAP section above */
    unsigned entryCount;    /* entries this record owns; the counts of all records
                                must add up to the header's entryCount */
    unsigned h0;            /* version 2 only: where this run's window of the day
                               OPENS, in half-minutes from midnight, the same unit
                               the DLL's SunHalfMinute returns -- 120 of them to an
                               hour, so 06:30 is 780 and 21:30 is 2580, and the
                               whole 2880 of a day. SHADOW_DAT_H0_ALL means the run
                               answers for every clock, which is what a file with
                               one run per map writes.
                               A map's several records are contiguous in this table
                               and their h0 values are distinct; the chunk gate walks
                               the run whose name matches and takes the record whose
                               h0 is the greatest at or below the client's own clock,
                               wrapping to the biggest h0 when the clock is before
                               all of them -- which is what makes a preset stamped
                               21:30 own the night for free */
} ShadowDatMapRecord;

typedef struct ShadowDatEntry {
    unsigned key;           /* (gy << 10) | gx, see above */
    unsigned payloadOffset; /* offset into the payload area, 512-byte aligned */
    unsigned mapIndex;      /* index into the map-record table; absent (this entry
                               is 8 bytes) when the header's mapCount is 0 */
} ShadowDatEntry;

/* the header size the DLL's loader validates against; see the size rules
 * above -- with mapCount records the file is exactly
 * 124 + mapCount*REC + entryCount*12 + payloadBytes bytes, where REC is
 * SHADOW_DAT_MAP_RECORD_V1 (28) at version 1 and SHADOW_DAT_MAP_RECORD_V2 (32)
 * at version 2, and with mapCount 0 it is 124 + entryCount*8 + payloadBytes */
#define SHADOW_DAT_HEADER_BYTES 124
#define SHADOW_DAT_VERSION 2u        /* what this header and the loader speak     */
#define SHADOW_DAT_VERSION_MIN 1u    /* also accepted: the 28-byte-record layout  */
#define SHADOW_DAT_PAYLOAD_BYTES 512u
#define SHADOW_DAT_KEY_SHIFT 10u
#define SHADOW_DAT_COORD_MAX 1023u
#define SHADOW_DAT_MAP_RECORD_V1 28u  /* name + entryCount: no clock word          */
#define SHADOW_DAT_MAP_RECORD_V2 32u  /* name + entryCount + h0                    */
#define SHADOW_DAT_MAP_MAX 256u       /* RECORDS, not names: a map with six presets
                                          costs six records, so a two-continent six-
                                          preset file plus its dungeons has room left
                                          here. Kept as one constant because the
                                          loader's arrays are sized by it */
#define SHADOW_DAT_HMIN_DAY 2880u     /* half-minutes in a day, the unit h0 uses;
                                          the DLL's own SUN_HMIN_DAY, and 120 of
                                          them to one clock hour */
#define SHADOW_DAT_H0_ALL 0xFFFFFFFFu /* h0 of a run that answers for every clock */

#endif
