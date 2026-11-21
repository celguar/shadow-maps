#!/usr/bin/env python3
r"""
extract_shadowmaps.py -- turn Salmiak's shadow-patched ADTs into a shadowmaps.dat.

Salmiak bakes shadows and exports them as a patch archive whose ADTs carry new MCSH
blocks. The client can already draw those, but only if it re-reads the whole patched
ADT. This script strips the MCSH payloads out of that archive and writes one compact
shadowmaps.dat that the ShadowMaps DLL feeds to the game at map load, so a shadow set
is a small file you can swap instead of a 340 MB archive.

Usage
    python extract_shadowmaps.py <archive> [<archive> ...] [-o OUT.dat]

    <archive>   An .mpq file, or a directory to scan recursively for .mpq files.
                Later archives win over earlier ones, the same way the client
                resolves a duplicated name: the last archive that has it is the one
                whose shadows land in the .dat.

    -o OUT.dat  Output file. Default: shadowmaps.dat in the current directory.
    --map NAME          Forces the map name. Default: read from the archive's
                        World\<MAP>.wdt names, which is what the DLL compares
                        against the client's own s_mapName.
    --variant NAME      Label stored in the header only, for your own records.
                        Default: the archive's file stem.
    --no-dedupe         Keep every payload even when several chunks share the same
                        512 bytes. Only useful for debugging the dedupe step.

Example
    python extract_shadowmaps.py "G:\Games\twmoa_1181\shadows-Azeroth-morning.mpq" ^
        -o "G:\Games\twmoa_1181\shadows-Azeroth-morning.dat"

What it prints
    The map name, the tile bounds, how many chunks were read per tile, and how many
    bytes the .dat ended up being after dedupe. A full Azeroth set is 741 tiles times
    256 chunks; dedupe usually keeps it near 25 MB, which is why the .dat is not a
    340 MB archive.

Reading an archive
    Salmiak's own exports are unencrypted, sectored zlib, sector shift 3, and carry a
    (listfile). Other archives written by stock tools are usually readable too:
    stored files, sectored zlib and PKWARE DCL are handled. Files compressed with
    implode or bzip2 are refused with a message, because this script's job is to read
    Salmiak's exports, not to be a whole MPQ library.

The .dat layout is specified in shadow_dat.h and README.md.
"""

import argparse
import os
import re
import struct
import sys
import zlib
from collections import defaultdict

M32 = 0xFFFFFFFF


class MpqError(Exception):
    """A reader error the caller turns into a message rather than a traceback."""


# The MPQ crypt table. The nested loop is the one every MPQ writer uses: the k-th
# pair made for i lands in index i + 0x100*k, which is the same order Salmiak.Core/
# IO/MpqArchiveWriter.cs keeps in BuildCryptTable, so the archives Salmiak exports
# and the ones the game ships both decrypt here.
def _pairs(count):
    out = []
    seed = 0x00100001
    for _ in range(count):
        seed = (seed * 125 + 3) % 0x2AAAAB
        hi = (seed & 0xFFFF) << 16
        seed = (seed * 125 + 3) % 0x2AAAAB
        lo = seed & 0xFFFF
        out.append((hi | lo) & M32)
    return out


def _build_crypt_table():
    pairs = _pairs(0x100 * 5)
    t = [0] * 0x500
    k = 0
    for i in range(0x100):
        for j in range(5):
            t[i + j * 0x100] = pairs[k]
            k += 1
    return t


CRYPT_TABLE = _build_crypt_table()


def hash_string(s, htype):
    """Battle.net string hash, same as Salmiak.Core/IO/MpqArchiveWriter.cs."""
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in s.upper().encode("ascii", "replace"):
        s1 = (CRYPT_TABLE[htype * 256 + ch] ^ (s1 + s2)) & M32
        s2 = (ch + s1 + s2 + ((s2 << 5) + 3)) & M32
    return s1 & M32


def decrypt_block(words, key):
    """Undo the writers' EncryptBlock: the running seed is fed by the value decrypted."""
    seed = 0xEEEEEEEE
    for i in range(len(words)):
        seed = (seed + CRYPT_TABLE[0x400 + (key & 0xFF)]) & M32
        out = (words[i] ^ (key + seed)) & M32
        key = ((((~key & M32) << 0x15) + 0x11111111) & M32) | (key >> 0x0B)
        seed = (out + seed + ((seed << 5) + 3)) & M32
        words[i] = out
    return words


NAME_TABLE = {
    # probe hash type, hashA type, hashB type. Salmiak shifts the classic set by one.
    "classic": (2, 0, 1),
    "salmiak": (0, 1, 2),
}


class Mpq:
    """A minimal read-only MPQ reader: tables, (listfile), per-file bytes."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            head = f.read(0x20)
        if len(head) < 0x20 or int.from_bytes(head[0:4], "little") != 0x1A51504D:
            raise MpqError("%s is not an MPQ" % path)
        self.header_size, self.archive_size, self.version, self.shift, self.hash_offset, self.block_offset = struct.unpack_from(
            "<IIHHII", head, 4
        )
        self.sector = 512 << self.shift
        size = os.path.getsize(path)
        # a written header's counts can disagree with the table positions, so trust the
        # positions, which is exactly what the client does when it maps the archive
        self.hash_count = (self.block_offset - self.hash_offset) // 16
        self.block_count = (size - self.block_offset) // 16
        hdr_hash, hdr_block = struct.unpack_from("<II", head, 24)
        if hdr_hash and hdr_hash != self.hash_count:
            print("   note: %s header hash entries %d, table position says %d" % (path, hdr_hash, self.hash_count))
        if hdr_block and hdr_block != self.block_count:
            print("   note: %s header block entries %d, file size says %d" % (path, hdr_block, self.block_count))
        if self.hash_count <= 0 or self.block_count <= 0:
            raise MpqError("%s has no tables" % path)
        self._read_tables()
        self._read_listfile()

    def _read_tables(self):
        with open(self.path, "rb") as f:
            f.seek(self.hash_offset)
            hb = bytearray(f.read(self.hash_count * 16))
            f.seek(self.block_offset)
            bb = bytearray(f.read(self.block_count * 16))
        best = None
        for mname in NAME_TABLE:
            probe_t, a_t, b_t = NAME_TABLE[mname]
            hw = list(struct.unpack("<%dI" % (self.hash_count * 4), hb))
            bw = list(struct.unpack("<%dI" % (self.block_count * 4), bb))
            decrypt_block(hw, hash_string("(hash table)", 3))
            decrypt_block(bw, hash_string("(block table)", 3))
            # a free slot is 0xFFFFFFFF in the block-index word
            used = sum(1 for i in range(self.hash_count) if hw[i * 4 + 3] != 0xFFFFFFFF)
            sane = 0 < used <= self.block_count and used <= self.hash_count
            hit = self._slot("(listfile)", hw, probe_t, a_t, b_t) if sane else None
            score = (1 if sane else 0) + (1 if hit is not None else 0)
            if best is None or score > best[0]:
                best = (score, mname, hw, bw)
        if best is None or best[0] == 0:
            raise MpqError("%s: the hash table did not decrypt with the known table" % self.path)
        _, self.mapping_name, hw, bw = best
        self.hash_entries = [tuple(hw[i * 4 : i * 4 + 4]) for i in range(self.hash_count)]
        self.blocks = [tuple(bw[i * 4 : i * 4 + 4]) for i in range(self.block_count)]
        self._name_types = NAME_TABLE[self.mapping_name]

    def _slot(self, name, hw, probe_t, a_t, b_t):
        probe = hash_string(name, probe_t) % self.hash_count
        ha = hash_string(name, a_t)
        hb = hash_string(name, b_t)
        i = probe
        while True:
            if hw[i * 4 + 3] != 0xFFFFFFFF and hw[i * 4] == ha and hw[i * 4 + 1] == hb:
                return i
            i = (i + 1) % self.hash_count
            if i == probe:
                return None

    def _lookup(self, name):
        probe_t, a_t, b_t = self._name_types
        probe = hash_string(name, probe_t) % self.hash_count
        ha = hash_string(name, a_t)
        hb = hash_string(name, b_t)
        i = probe
        while True:
            e = self.hash_entries[i]
            if e[3] != 0xFFFFFFFF and e[0] == ha and e[1] == hb:
                return e[3]
            i = (i + 1) % self.hash_count
            if i == probe:
                return None

    def _read(self, index):
        if index >= self.block_count:
            raise MpqError("block %d out of range" % index)
        offset, comp, raw, flags = self.blocks[index]
        with open(self.path, "rb") as f:
            f.seek(offset)
            blob = f.read(comp)
        kind = flags & 0x0000FFFF
        if kind == 0:
            return blob
        # Salmiak.Core/IO/MpqArchiveWriter.cs marks compressed files with its own
        # FlagExists|FlagCompress pair, and a sector it cannot shrink is written as
        # bare bytes with no 0x00 prefix, unlike stock writers
        salmiak_style = flags == 0x80000200
        n = max(1, (raw + self.sector - 1) // self.sector)
        offs = list(struct.unpack("<%dI" % (n + 1), blob[: (n + 1) * 4]))
        out = bytearray()
        for s in range(n):
            want = min(self.sector, raw - s * self.sector)
            start, end = offs[s], offs[s + 1]
            if end <= start or start >= len(blob):
                raise MpqError("%s: sector %d of %d is missing" % (self.path, s, n))
            out += self._decode_sector(blob[start:end], want, salmiak_style)
        return bytes(out[:raw])

    def _decode_sector(self, piece, want, salmiak_style):
        if not piece:
            return b""
        kind, body = piece[0], piece[1:]
        if kind == 0x00:
            return body[:want]
        if kind == 0x02:
            try:
                return zlib.decompress(body)[:want]
            except zlib.error:
                if not salmiak_style:
                    raise
                # Salmiak's writer leaves a stored sector as bare bytes, so a payload
                # that happens to start with 0x02 is not a zlib stream at all
                return piece[:want]
        if kind == 0x03:
            # PKWARE DCL, with the 2-byte Adler-16 the format puts at the front
            return zlib.decompress(b"\x78\x9c" + body[2:])[2:][:want]
        if salmiak_style:
            return piece[:want]
        raise MpqError(
            "sector kind %#x is not supported (implode/bzip2/ADPCM); re-export from Salmiak "
            "so the archive is sectored zlib" % kind
        )

    def file_data(self, name):
        index = self.by_name.get(name)
        if index is None:
            return None
        return self._read(index)

    def names(self):
        return list(self.by_name.keys())

    def _read_listfile(self):
        self.by_name = {}
        data = None
        for candidate in ("(listfile)", "(Listfile)", "(LISTFILE)"):
            index = self._lookup(candidate)
            if index is not None and index < self.block_count:
                data = self._read(index)
                break
        if not data:
            raise MpqError("%s has no readable (listfile): names cannot be resolved" % self.path)
        text = data.decode("latin1")
        for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
            line = line.strip()
            if line:
                self.by_name[line] = self._lookup(line)

    def tiles(self):
        """Every tile ADT as (tileX, tileY, bytes, name)."""
        out = []
        for name in self.by_name:
            m = TILE_RE.match(name)
            if not m:
                continue
            data = self.file_data(name)
            if data is None:
                continue
            out.append((int(m.group(1)), int(m.group(2)), data, name))
        return out

    def map_names(self):
        """The maps this archive carries, from its World\\<MAP>.wdt and tile names."""
        out = []
        for name in self.by_name:
            if WDT_RE.match(name):
                out.append(name.split("\\")[-1].split(".")[0])
        return out

    def tile_maps(self):
        """Map names taken from tile file names, as a fallback when no WDT is present."""
        out = []
        for name in self.by_name:
            m = TILE_RE.match(name)
            if m:
                out.append(name.split("\\")[-1].rsplit("_", 2)[0])
        return out


TILE_RE = re.compile(r"^world\\.+?_(\d+)_(\d+)\.adt$", re.IGNORECASE)
WDT_RE = re.compile(r"^world\\.+?\.wdt$", re.IGNORECASE)


def top_chunks(data):
    """The file's own list of top-level chunks: (magic ASCII, magicPos, size).

    FourCCs sit on disk byte-reversed relative to the ASCII names this codebase
    uses, which is how Salmiak.Core/IO/AdtShadowInjector.cs reads them too.
    """
    out = []
    pos = 0
    while pos + 8 <= len(data):
        raw = data[pos:pos + 4]
        if not all(32 <= c < 127 for c in raw):
            break
        size = int.from_bytes(data[pos + 4:pos + 8], "little")
        if pos + 8 + size > len(data):
            break
        out.append((bytes(reversed(raw)).decode("latin1"), pos, size))
        pos += 8 + size
    return out


def adt_chunks(data):
    """Yield (indexX, indexY, 512-byte body) for every MCNK in one ADT.

    This follows Salmiak.Core/IO/AdtShadowInjector.cs and the client's own chunk
    loader: an MCNK's own header keeps flags at +0, indexX at +4, indexY at +8,
    ofsMCSH at +44 (0x2c, the field the client copies to CMapChunk+0xf1c) and
    sizeMCSH at +48 (0x30, the field it copies to +0x148). ofsMCSH addresses the
    MCSH payload, so the sub-chunk header is the 8 bytes in front of it.
    """
    out = []
    for (magic, pos, size) in top_chunks(data):
        if magic != "MCNK" or size < 128:
            continue
        hdr = pos + 8
        index_x = int.from_bytes(data[hdr + 4:hdr + 8], "little")
        index_y = int.from_bytes(data[hdr + 8:hdr + 12], "little")
        ofs = int.from_bytes(data[hdr + 44:hdr + 48], "little")
        declared = int.from_bytes(data[hdr + 48:hdr + 52], "little")
        if ofs == 0:
            continue  # the chunk carries no shadow map
        if declared not in (0, 512):
            continue  # not a packed 1.12 shadow map
        if ofs + 512 > size:
            continue
        if data[hdr + ofs - 8:hdr + ofs - 4] != b"MCSH"[::-1]:
            continue  # ofsMCSH does not land on an MCSH sub-chunk header
        out.append((index_x, index_y, data[hdr + ofs:hdr + ofs + 512]))
    return out


def main(argv):
    ap = argparse.ArgumentParser(add_help=True, description="Salmiak shadow archives -> shadowmaps.dat")
    ap.add_argument("archives", nargs="+", help=".mpq files or directories containing them")
    ap.add_argument("-o", "--out", default="shadowmaps.dat")
    ap.add_argument("--map", default=None, help="override the map name")
    ap.add_argument("--variant", default=None, help="label stored in the header only")
    ap.add_argument("--no-dedupe", action="store_true")
    ap.add_argument("--probe", metavar="NAME", default=None, help="print one file's ADT walk and stop")
    args = ap.parse_args(argv)

    paths = []
    for a in args.archives:
        if os.path.isdir(a):
            for root, _, files in os.walk(a):
                for f in files:
                    if f.lower().endswith(".mpq"):
                        paths.append(os.path.join(root, f))
        elif a.lower().endswith(".mpq"):
            paths.append(a)
        else:
            raise SystemExit("%s is neither an .mpq nor a directory" % a)
    if not paths:
        raise SystemExit("no .mpq given")

    if args.probe:
        arc = Mpq(paths[0])
        print("probe %s: %d files" % (paths[0], len(arc.by_name)))
        found = [n for n in arc.names() if n.lower() == args.probe.lower()]
        if not found:
            raise SystemExit("no %s in the archive" % args.probe)
        data = arc.file_data(found[0])
        print("file %s  %d bytes" % (found[0], len(data)))
        print("top chunks: %s" % " ".join(m for (m, _, _) in top_chunks(data)))
        chunks = adt_chunks(data)
        print("MCSH bodies %d" % len(chunks))
        for (ix, iy, b) in chunks[:4]:
            print("  chunk %d,%d  %d texels set of %d" % (ix, iy, sum(bin(x).count("1") for x in b), 4096))
        return

    maps_seen, variant_seen = [], []
    payloads = {}
    tiles_seen = set()
    missing = 0
    per_tile = {}

    for path in paths:
        try:
            arc = Mpq(path)
        except MpqError as e:
            print("!! %s" % e)
            continue
        print("-- %s  %d files  names %s" % (path, len(arc.by_name), arc.mapping_name))
        maps_seen.extend(arc.map_names())
        maps_seen.extend(arc.tile_maps())
        variant_seen.append(os.path.splitext(os.path.basename(path))[0])
        read = 0
        for (tx, ty, data, name) in arc.tiles():
            tiles_seen.add((tx, ty))
            chunks = adt_chunks(data)
            per_tile[(tx, ty)] = len(chunks)
            for (ix, iy, body) in chunks:
                gx, gy = tx * 16 + ix, ty * 16 + iy
                payloads[(gy << 10) | gx] = body
            read += len(chunks)
            missing += 256 - len(chunks)
        print("   tiles %d  chunks %d" % (len(tiles_seen), read))

    if not payloads:
        raise SystemExit("no MCSH blocks found in any archive")

    name = args.map
    if not name:
        counts = defaultdict(int)
        for m in maps_seen:
            counts[m] += 1
        if not counts:
            raise SystemExit("the archives carry no World\\<MAP>.wdt, so pass --map")
        name = max(counts, key=counts.get)
    variant = args.variant or (variant_seen[-1] if variant_seen else "set")

    groups = None
    if not args.no_dedupe:
        groups = {}
        for body in payloads.values():
            groups[body] = groups.get(body, 0) + 1
    _write_dat(args.out, name, variant, payloads, groups, tiles_seen, missing, per_tile)


def _write_dat(out, name, variant, payloads, groups, tiles_seen, missing, per_tile):
    # one payload per distinct 512-byte block; identical chunks share it, which is what
    # keeps a full Azeroth set near 25 MB instead of 741 tiles x 256 chunks x 512 B
    order = []
    blobs = {}
    if groups is None:
        for key in sorted(payloads):
            blobs[(key, payloads[key])] = len(order) * 512
            order.append(payloads[key])
        entries = [(key, blobs[(key, payloads[key])]) for key in sorted(payloads)]
    else:
        first_key = {}
        for key in sorted(payloads):
            first_key.setdefault(payloads[key], key)
        for body in sorted(set(payloads.values()), key=lambda b: first_key[b]):
            blobs[body] = len(order) * 512
            order.append(body)
        entries = [(key, blobs[payloads[key]]) for key in sorted(payloads)]

    keys = sorted(payloads)
    xs = [k & 0x3FF for k in keys]
    ys = [(k >> 10) & 0x3FF for k in keys]
    gx0, gx1 = min(xs), max(xs)
    gy0, gy1 = min(ys), max(ys)
    # per-axis tile bounds, not first/last of the sorted list, which mis-orders them
    tile_x = [t[0] for t in tiles_seen]
    tile_y = [t[1] for t in tiles_seen]
    tx0, tx1, ty0, ty1 = min(tile_x), max(tile_x), min(tile_y), max(tile_y)
    map_name = name.encode("ascii", "replace")[:23]
    var_name = variant.encode("ascii", "replace")[:39]
    with open(out, "wb") as f:
        f.write(struct.pack(
            "<4sI24s40sIII8iII",
            b"SHDW",
            1,
            map_name.ljust(24, b"\0"),
            var_name.ljust(40, b"\0"),
            len(keys),
            len(order),
            len(order) * 512,
            tx0,
            ty0,
            tx1,
            ty1,
            gx0,
            gy0,
            gx1,
            gy1,
            len(tiles_seen),
            0,
        ))
        for key in sorted(payloads):
            f.write(struct.pack("<II", key, blobs[payloads[key]]))
        for body in order:
            f.write(body)
    print(
        "map %s  variant %s\n"
        "tiles %d (%d..%d, %d..%d)  chunks %d  payloads %d -> %d after dedupe  -> %s  %.1f MiB"
        % (
            name,
            variant,
            len(tiles_seen),
            tx0,
            tx1,
            ty0,
            ty1,
            len(payloads),
            len(payloads),
            len(order),
            out,
            os.path.getsize(out) / 1048576.0,
        )
    )
    print("chunk grid x %d..%d  y %d..%d" % (gx0, gx1, gy0, gy1))
    print("chunks without an MCSH: %d" % missing)
    counts = defaultdict(int)
    for (tx, ty), n in per_tile.items():
        counts[n] += 1
    print("chunks per tile: %s" % dict(sorted(counts.items())))


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except MpqError as e:
        print("!! %s" % e)
        raise SystemExit(1)
