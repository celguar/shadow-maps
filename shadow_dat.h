/* shadow_dat.h -- the shadow payload file ("shadowmaps.dat") that ShadowMaps.dll
 * loads, and the layout extract_shadowmaps.py writes. Keep the two in sync.
 *
 * The .dat is what a shadow variant IS: not patched .adt files, not an archive,
 * just every chunk's 512-byte packed shadow map, once each, keyed by the chunk's
 * GLOBAL coordinates -- the same key Salmiak uses for its bake selections, so a
 * bake can be exported, stripped out of the patch archive, and handed to the
 * client without the shadows ever round-tripping through a parsed .adt.
 *
 * File layout, all little-endian, nothing aligned to anything but itself:
 *
 *   ShadowDatHeader            124 bytes
 *   entryCount ShadowDatEntry  8 bytes each, sorted ASCENDING by key
 *   payload area               payloadUnique x 512-byte shadow maps
 *
 * entryCount + payloadUnique is what dedupe leaves: chunks that share identical
 * shadow bytes share one 512-byte payload, including one shared all-zero buffer
 * so a cleared shadow really draws nothing. payloadBytes == payloadUnique*512.
 * entry.payloadOffset addresses the payload area (byte 0 = first payload), and
 * payloadOffset + 512 <= payloadBytes is enforced when the .dat loads.
 *
 * The key mirrors Salmiak's _bakeChunks (GlViewport.Bake.cs): a chunk's global
 * column gx = 16*tileX + indexX -- which is what the client itself keeps at
 * CMapChunk+0x8c -- and gy = 16*tileY + indexY at +0x90, both 0..1023:
 *
 *   key = (gy << 10) | gx
 *
 * Chunks the bake left without a shadow map have NO entry: the DLL then changes
 * nothing about them and the client keeps whatever stock shadow state they have,
 * which is honest -- we never force the shadow flag on without a payload.
 *
 * Bounds fields are for reporting only (the .dat's own extent); the DLL matches
 * chunks by key and by the map name, never by the bounds.
 */
#ifndef SHADOW_DAT_H
#define SHADOW_DAT_H 1

typedef struct ShadowDatHeader {
    char magic[4];        /* "SHDW" on disk, S H D W in order */
    unsigned version;     /* 1 */
    char map[24];         /* map name, NUL-terminated, padded; the DLL compares
                               this against the client's own s_mapName (0xc961a0)
                               case-insensitively at every chunk, so a payload
                               for the wrong map is ignored, not half-applied */
    char variant[40];     /* informational label ("morning", a stamp, whatever
                               you wrote when baking); the DLL ignores it */
    unsigned entryCount;
    unsigned payloadUnique;
    unsigned payloadBytes;
    int tileMinX, tileMinY, tileMaxX, tileMaxY;    /* inclusive tile range */
    int chunkMinX, chunkMinY, chunkMaxX, chunkMaxY; /* inclusive gx/gy range */
    unsigned tileCount;
    unsigned reserved;    /* 0 now, free for later */
} ShadowDatHeader;

typedef struct ShadowDatEntry {
    unsigned key;           /* (gy << 10) | gx, see above */
    unsigned payloadOffset; /* offset into the payload area, 512-byte aligned */
} ShadowDatEntry;

/* the header size the DLL's loader validates against; the .dat file itself must
 * be exactly 124 + entryCount*8 + payloadBytes bytes long (extra trailing bytes
 * are ignored, short files are refused) */
#define SHADOW_DAT_HEADER_BYTES 124
#define SHADOW_DAT_VERSION 1u
#define SHADOW_DAT_PAYLOAD_BYTES 512u
#define SHADOW_DAT_KEY_SHIFT 10u
#define SHADOW_DAT_COORD_MAX 1023u

#endif
