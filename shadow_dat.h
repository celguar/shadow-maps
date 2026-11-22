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
 *   mapCount ShadowDatMapRecord        28 bytes each, only when mapCount != 0
 *   entryCount ShadowDatEntry          8 bytes when mapCount == 0, else 12
 *   payload area                       payloadUnique x 512-byte shadow maps
 *
 * mapCount is the header's last word, the one that was `reserved` in the single-map
 * file; 0 there means "one map, named by the header's map field, 8-byte entries",
 * which is what a hand-made single-map .dat keeps writing and what the DLL still
 * reads. mapCount > 0 means the map-record table follows the header and every entry
 * carries its map's index into that table, which is how ONE .dat carries SEVERAL
 * maps -- see below for the size rule.
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
 * Entries are ordered by (mapIndex, key): one contiguous run per map, in map-record
 * order, keys strictly ascending inside a run (the same key twice inside one map is
 * the same chunk listed twice, and is refused). The DLL binary-searches only the
 * matched map's run, so a key that two maps both carry can never be answered with
 * the other map's shadow.
 *
 * Size rules the loader validates against, exactly:
 *   mapCount == 0:  124 + entryCount*8                + payloadBytes
 *   mapCount >  0:  124 + mapCount*28 + entryCount*12  + payloadBytes
 * (extra trailing bytes are ignored, short files are refused)
 *
 * Both tables come BEFORE the pool: the map-record table sits right after the
 * header, the entry table after that, so an entry's payloadOffset addresses file
 * offset 124 + mapCount*28 + entryCount*stride (stride 8 legacy, 12 with a table).
 * A reader that seeks past the entries only fills its arena rec_bytes (= mapCount
 * * 28) short of every payload, and a 12-map file then draws a chunk's shadow as
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
 */
#ifndef SHADOW_DAT_H
#define SHADOW_DAT_H 1

typedef struct ShadowDatHeader {
    char magic[4];        /* "SHDW" on disk, S H D W in order */
    unsigned version;     /* 1 */
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
                                 1..64 = one map record per named map, entries
                                 are 12 bytes and carry the record index */
} ShadowDatHeader;

typedef struct ShadowDatMapRecord {
    char name[24];          /* map name, NUL-terminated, padded; compared
                               case-insensitively against the client's s_mapName */
    unsigned entryCount;    /* entries this map owns; the counts of all records
                                must add up to the header's entryCount */
} ShadowDatMapRecord;

typedef struct ShadowDatEntry {
    unsigned key;           /* (gy << 10) | gx, see above */
    unsigned payloadOffset; /* offset into the payload area, 512-byte aligned */
    unsigned mapIndex;      /* index into the map-record table; absent (this entry
                               is 8 bytes) when the header's mapCount is 0 */
} ShadowDatEntry;

/* the header size the DLL's loader validates against; see the two size rules
 * above -- with mapCount map records the file is exactly
 * 124 + mapCount*28 + entryCount*12 + payloadBytes bytes, and with mapCount 0 it
 * is 124 + entryCount*8 + payloadBytes */
#define SHADOW_DAT_HEADER_BYTES 124
#define SHADOW_DAT_VERSION 1u
#define SHADOW_DAT_PAYLOAD_BYTES 512u
#define SHADOW_DAT_KEY_SHIFT 10u
#define SHADOW_DAT_COORD_MAX 1023u
#define SHADOW_DAT_MAP_RECORD_BYTES 28u
#define SHADOW_DAT_MAP_MAX 64u

#endif
