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
                ONE file is written whatever the archives carry: a .dat names the
                maps its payloads belong to, so an Azeroth set and a Kalimdor set
                sit in the same file and the DLL picks the run that matches the
                map the client is on. See the map table in shadow_dat.h.
    --map NAME          Only take tiles of that map. Repeat the flag to keep
                        several maps in the one .dat (--map Azeroth --map
                        Kalimdor). Default: every map the archives carry.
    --variant NAME      Label stored in the header only, for your own records.
                        Default: the archive's file stem.
    --no-dedupe         Keep every payload even when several chunks share the same
                        512 bytes. Only useful for debugging the dedupe step.
    --probe NAME        Read one file, print its chunk walk and its MCSH bodies,
                        and stop: the way to check an archive you were handed.
    --set HH:MM         The clock this bake was made at, one --set per archive in
                        the same order: 06:30 says these shadows were baked with
                        the sun where it is at 06:30. Repeat the flag with several
                        archives to put several shadow SETS of a map into the one
                        .dat, which is what the client's own day clock then picks
                        between -- a baked set does not rotate, so a sun that walks
                        an arc only matches the ground at its own clock. Without
                        --set the file stays version 1, the byte-identical one
                        today's archives produce.
    --open HH:MM        Where in the day a set's window starts, one per --set in
                        the same order. Default: the midpoint of a set's bake stamp
                        and the one before it -- except the earliest set, whose
                        window opens at the arc's sunrise crossing (05:50), and a
                        set stamped at the arc's sunset handover (21:30), whose
                        window opens at its own stamp, so the stock set owns the
                        night. The openings are what the loader's window search
                        reads; the bake stamps are only what the sun was doing.

Example
    python extract_shadowmaps.py "D:\games\wow-1.12.1\patch-S.mpq" ^
        --map Azeroth -o "D:\games\wow-1.12.1\shadows-Azeroth.dat"
    python extract_shadowmaps.py "D:\games\wow-1.12.1\shadows-Azeroth-morning.mpq" ^
        -o "D:\games\wow-1.12.1\shadows-Azeroth-morning.dat"

What it prints
    The one .dat written, and per map inside it: the map name, the tile bounds, how
    many chunks were read per tile, and how many bytes the file ended up being after
    dedupe. A full Azeroth set is 741 tiles times 256 chunks; dedupe usually keeps it
    near 25 MB, which is why the .dat is not a 340 MB archive.

Reading an archive
    Salmiak's own exports are unencrypted, sectored zlib, sector shift 3, and carry a
    (listfile). Archives written by other tools are read the same way: stock writers
    leave deleted hash slots, mark a file encrypted (its sector-offset table and its
    sectors are then WEP-encrypted, and the seed is recovered from the table's known
    first dword), mark a file as one single sector, and use PKWARE DCL sectors. Files
    compressed with implode or bzip2 are refused with a message, because this script's
    job is to read shadow archives, not to be a whole MPQ library.

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


# an empty slot, and a deleted one: stock tools leave 0xFFFFFFFE where they removed a
# file, and a probe chain walks past those, so they are neither "used" nor "free"
HASH_EMPTY = 0xFFFFFFFF
HASH_DELETED = 0xFFFFFFFE

# block-table flags, named as libmpq names them (its mpq-internal.h is the reference
# that spells the bits out): a writer that encrypts a file's bytes WEP-encrypts the
# sector-offset table first and then every sector, a "single" file is one run of bytes
# with only the two table entries of its own, and "extra" is a table with one more
# entry, which the sector walk never reads
FLAG_ENCRYPTED = 0x00010000
FLAG_SINGLE = 0x01000000
FLAG_EXTRA = 0x04000000


def find_seed(words, plain0, block_size):
    """Undo libmpq's libmpq__decrypt_key: an encrypted sector table starts with its own
    byte size, and one known dword plus "the next offset is at most one sector away" is
    enough to pin the seed, which is the same seed the file's sectors then decrypt with.
    """
    if len(words) < 2:
        return None
    temp = ((words[0] ^ plain0) - 0xEEEEEEEE) & M32
    for i in range(0x100):
        seed1 = (temp - CRYPT_TABLE[0x400 + i]) & M32
        seed2 = (0xEEEEEEEE + CRYPT_TABLE[0x400 + (seed1 & 0xFF)]) & M32
        ch = (words[0] ^ ((seed1 + seed2) & M32)) & M32
        if ch != plain0:
            continue
        saveseed1 = (seed1 + 1) & M32
        seed1 = ((((~seed1 & M32) << 0x15) + 0x11111111) & M32) | (seed1 >> 0x0B)
        seed2 = (ch + seed2 + ((seed2 << 5) + 3)) & M32
        seed2 = (seed2 + CRYPT_TABLE[0x400 + (seed1 & 0xFF)]) & M32
        ch2 = (words[1] ^ ((seed1 + seed2) & M32)) & M32
        if ((ch2 - ch) & M32) <= block_size:
            return saveseed1
    return None


def crypt_words(piece, key):
    """WEP one way over whole dwords, which is how libmpq decrypts a sector: bytes
    past the last whole dword are left alone, so a tail is never invented here."""
    n = len(piece) // 4
    if n == 0:
        return piece
    words = list(struct.unpack("<%dI" % n, piece[: n * 4]))
    decrypt_block(words, key)
    return b"".join(struct.pack("<I", w) for w in words) + piece[n * 4:]


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
        self.unreadable = []
        self.unresolved = 0
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
            # a free slot is 0xFFFFFFFF and a deleted one is 0xFFFFFFFE in the block
            # index word: stock writers delete entries when they rewrite an archive, so
            # counting the deleted ones as used makes a table of 352 files look like
            # thousands of them, and the sane check below rejects the whole decryption
            used = sum(1 for i in range(self.hash_count)
                       if hw[i * 4 + 3] not in (HASH_EMPTY, HASH_DELETED))
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
        # the chain ends at the first free slot, and a deleted slot is skipped rather
        # than matched, which is how the client walks a table another tool edited
        i = probe
        while True:
            blk = hw[i * 4 + 3]
            if blk == HASH_EMPTY:
                return None
            if blk != HASH_DELETED and hw[i * 4] == ha and hw[i * 4 + 1] == hb:
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
            if e[3] == HASH_EMPTY:
                return None
            if e[3] != HASH_DELETED and e[0] == ha and e[1] == hb:
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
        # a single-sector file has no table on disk beyond its own two entries, and the
        # walk never splits it, so its one run of bytes is the whole file; that also
        # leaves nothing to recover an encryption seed from, which is libmpq's own
        # limit: it only decrypts a file whose table it actually read
        single = bool(flags & FLAG_SINGLE)
        n = 1 if single else max(1, (raw + self.sector - 1) // self.sector)
        words = n + 1 + (1 if flags & FLAG_EXTRA else 0)
        seed = None
        if single:
            offs = [0, comp]
        else:
            offs = list(struct.unpack_from("<%dI" % words, blob, 0))
            if len(offs) < words:
                raise MpqError("%s: block %d wants %d table words, its %d bytes hold %d"
                               % (self.path, index, words, len(blob), len(offs)))
            if offs[0] != words * 4:
                # the table is encrypted: recover its seed and decrypt it, which is also
                # the seed every sector of this file is encrypted with
                seed = find_seed(offs, words * 4, self.sector)
                if seed is None:
                    raise MpqError("%s: block %d has an encrypted table whose seed is not recoverable"
                                   % (self.path, index))
                offs = decrypt_block(offs, (seed - 1) & M32)
                if offs[0] != words * 4:
                    raise MpqError("%s: block %d decrypts to a table that disagrees with its size"
                                   % (self.path, index))
        out = bytearray()
        for s in range(n):
            want = raw if single else min(self.sector, raw - s * self.sector)
            start, end = offs[s], offs[s + 1]
            if end <= start or start >= len(blob):
                raise MpqError("%s: sector %d of %d is missing" % (self.path, s, n))
            piece = blob[start:end]
            if seed is not None:
                piece = crypt_words(piece, (seed + s) & M32)
            out += self._decode_sector(piece, want, salmiak_style)
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
        self.unresolved = 0
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
            if not line:
                continue
            index = self._lookup(line)
            # a name the hash table does not carry is not a file: an archive edited by
            # another tool can list a name whose entry it deleted, and it stays counted
            # here so a wrong name-table guess cannot pass silently
            self.by_name[line] = index
            if index is None:
                self.unresolved += 1

    def tiles(self):
        """Every tile ADT as (map name, tileX, tileY, bytes, name).

        The map is the folder the tile sits in, because that is the name the client
        keeps in s_mapName and the one the DLL compares the .dat's map field against.
        The tile-name prefix is only a fallback for writers that flattened the path.
        A tile whose bytes this script cannot read is left out and named in
        self.unreadable, so one file written by one tool does not end an archive.
        """
        out = []
        self.unreadable = []
        for name in self.by_name:
            m = TILE_RE.match(name)
            if not m:
                continue
            parts = name.split("\\")
            mp = parts[-2] if len(parts) >= 2 and parts[-2] else parts[-1].rsplit("_", 2)[0]
            try:
                data = self.file_data(name)
            except MpqError as e:
                self.unreadable.append((name, str(e)))
                continue
            if data is None:
                continue
            out.append((mp, int(m.group(1)), int(m.group(2)), data, name))
        return out

    def map_names(self):
        """Map names this archive declares through its World\\<MAP>.wdt files."""
        out = []
        for name in self.by_name:
            if WDT_RE.match(name):
                out.append(name.split("\\")[-1].split(".")[0])
        return out


TILE_RE = re.compile(r"^world\\.+?_(\d+)_(\d+)\.adt$", re.IGNORECASE)
WDT_RE = re.compile(r"^world\\.+?\.wdt$", re.IGNORECASE)


# fourccs this codebase names, in ASCII. The 1.12.1 client writes them onto disk as
# dwords, so an ADT it produced spells 'MVER' as the bytes REVM and the reversed-ASCII
# spelling is what the walk sees. A tool that wrote the plain ASCII spelling is still
# read correctly: the raw bytes are matched against this set before they are reversed,
# so both conventions land on the same name
KNOWN_MAGICS = {
    b"MVER", b"MHDR", b"MCIN", b"MTEX", b"MMDX", b"MMID", b"MWMO", b"MWID",
    b"MDDF", b"MODF", b"MH2O", b"MCNK", b"MCCT", b"MCSH", b"MCLY", b"MLYI",
    b"MSTA", b"MFBO", b"MHHF", b"PORT", b"TIME",
}


def fourcc(raw):
    """A fourcc's bytes as the ASCII name, either spelling it is stored in."""
    if raw in KNOWN_MAGICS or raw[::-1] not in KNOWN_MAGICS:
        return raw.decode("latin1")
    return bytes(reversed(raw)).decode("latin1")


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
        out.append((fourcc(raw), pos, size))
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
        if data[hdr + ofs - 8:hdr + ofs - 4] not in (b"MCSH", b"MCSH"[::-1]):
            continue  # ofsMCSH does not land on an MCSH sub-chunk header
        out.append((index_x, index_y, data[hdr + ofs:hdr + ofs + 512]))
    return out


# the clock unit the .dat's h0 word and the DLL's SunHalfMinute share: half-
# minutes from midnight, so a day is 2880 of them and 06:30 is 780. HH:MM is
# what the user reads, so it is only ever the spelling, never the unit.
HMIN_DAY = 2880
# the arc mod.c installs: the sun's own travel crosses the horizon at 05:50 and
# hands over to stock's keys at 21:30, so those are the two crossings a set's
# window should sit on, and a set baked at any other clock only matters between
# them. Not invented here: they are SUN_HMIN_DAWN and the arc's handover in mod.c
HMIN_DAWN = 700
HMIN_DUSK = 2580


def parse_clock(txt, what):
    """'06:30' -> 780 half-minutes. Refuses anything else, because a silently
    wrong clock is a file whose sets answer the wrong hours."""
    m = re.fullmatch(r"([01]?\d|2[0-3]):([0-5]\d)", txt.strip())
    if not m:
        raise SystemExit("%s is not HH:MM: %s" % (txt, what))
    h, mi = int(m.group(1)), int(m.group(2))
    return h * 120 + 2 * mi


def fmt_clock(h):
    return "%02d:%02d" % (h // 120, (h % 120) // 2)


def main(argv):
    ap = argparse.ArgumentParser(add_help=True, description="Salmiak shadow archives -> shadowmaps.dat")
    ap.add_argument("archives", nargs="+", help=".mpq files or directories containing them")
    ap.add_argument("-o", "--out", default="shadowmaps.dat")
    ap.add_argument("--map", dest="maps", action="append", default=None,
                    help="only take tiles of this map; repeat the flag to put several "
                         "maps into the one .dat (default: every map the archives carry)")
    ap.add_argument("--variant", default=None, help="label stored in the header only")
    ap.add_argument("--no-dedupe", action="store_true")
    ap.add_argument("--probe", metavar="NAME", default=None, help="print one file's ADT walk and stop")
    ap.add_argument("--set", dest="sets", action="append", default=None, metavar="HH:MM",
                    help="the clock these shadows were baked at, one --set per archive "
                         "in the same order; several of them put several shadow sets "
                         "of a map in the one .dat (version 2)")
    ap.add_argument("--open", dest="opens", action="append", default=None, metavar="HH:MM",
                    help="where a set's window starts in the day, one per --set in the "
                         "same order (default: the midpoint of the neighbouring bake "
                         "stamps, with the dawn and dusk crossings at the ends)")
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

    # --set is one bake stamp per archive, so the alignment is checked against the
    # EXPANDED archive list: a directory counts as every .mpq inside it, and that is
    # why the check comes after the walk rather than next to the flag. One --set for
    # several archives means they are all the same clock, which is what a version-1
    # file already is, and a mismatch refuses rather than pairing a clock with the
    # wrong bake -- an off-by-one here would put noon's shadows in morning's window.
    stamps = None
    if args.sets:
        if len(args.sets) == 1:
            stamps = [parse_clock(args.sets[0], "--set")] * len(paths)
        elif len(args.sets) == len(paths):
            stamps = [parse_clock(s, "--set #%d of %s" % (i + 1, p))
                      for i, (s, p) in enumerate(zip(args.sets, paths))]
        else:
            raise SystemExit(
                "--set given %d time(s) for %d archive(s): one per archive, in the same order, "
                "so the %d archives (%s) cannot be matched to their clocks"
                % (len(args.sets), len(paths), len(paths), " ".join(paths))
            )
    opens = None
    opens_by_stamp = {}
    if args.opens:
        if not args.sets:
            raise SystemExit("--open without --set: a version-1 .dat has no window word to write")
        if len(args.opens) != len(args.sets):
            raise SystemExit("--open given %d time(s) against %d --set(s): one opening per set, "
                             "in the same order" % (len(args.opens), len(args.sets)))
        uniq = list(dict.fromkeys(args.sets))
        if len(uniq) != len(args.sets) and len(args.sets) != 1:
            raise SystemExit("--set repeats %s: name the set once, its archives merge into one"
                             % " ".join(args.sets))
        opens_by_stamp = {parse_clock(s, "--set #%d" % (i + 1)): parse_clock(o, "--open #%d" % (i + 1))
                          for i, (s, o) in enumerate(zip(args.sets, args.opens))}

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

    variant_seen = []
    # one gather per map name: a tool-built archive such as a stock patch MPQ carries
    # several maps' tiles side by side, and the .dat the DLL reads is keyed to one map
    # (it checks the map field against the client's own s_mapName), so each map gets
    # its own payloads, its own tile bounds and its own output file. Later archives
    # still win over earlier ones, per map, the way the client resolves a name
    # --map repeats, and a map is one group keyed case-insensitively, so the
    # filter is a set of lower-cased names rather than one name to equal
    wanted = [m.lower() for m in args.maps] if args.maps else None
    gathered = {}
    seen_maps = set()
    display_name = {}

    for pi, path in enumerate(paths):
        try:
            arc = Mpq(path)
        except MpqError as e:
            print("!! %s" % e)
            continue
        print("-- %s  %d files  names %s" % (path, len(arc.by_name), arc.mapping_name))
        if arc.unresolved:
            print(
                "   %d of %d listfile names have no hash entry: the archive was edited by "
                "a tool after it was written" % (arc.unresolved, len(arc.by_name))
            )
        wdts = arc.map_names()
        if wdts:
            print("   World\\<MAP>.wdt present for: %s" % " ".join(sorted(set(wdts))))
        variant_seen.append(os.path.splitext(os.path.basename(path))[0])
        if stamps:
            print("   baked at %s, so this archive is that shadow set of every map it carries"
                  % fmt_clock(stamps[pi]))
        read = 0
        tiles_here = set()
        for (mp, tx, ty, data, name) in arc.tiles():
            seen_maps.add(mp)
            # the same map can be spelled differently across archives: one patch says
            # World\Maps\Azeroth and another World\maps\azeroth, and that is one map, so
            # the group is keyed case-insensitively and keeps the first spelling it saw
            key = mp.lower()
            if args.maps and key not in wanted:
                continue
            display_name.setdefault(key, mp)
            # with --set the gather is keyed by (map, bake stamp) instead of by map: a
            # set is one record of the .dat's table, and two archives baked at the same
            # clock for the same map are one set whose later tiles win, which is how the
            # client resolves a duplicated name inside one set
            gkey = (key, stamps[pi]) if stamps else key
            payloads, tiles_seen, per_tile, missing = gathered.setdefault(
                gkey, ({}, set(), {}, [0])
            )
            tiles_seen.add((tx, ty))
            tiles_here.add((tx, ty))
            chunks = adt_chunks(data)
            per_tile[(tx, ty)] = len(chunks)
            for (ix, iy, body) in chunks:
                gx, gy = tx * 16 + ix, ty * 16 + iy
                payloads[(gy << 10) | gx] = body
            read += len(chunks)
            missing[0] += 256 - len(chunks)
        print("   tiles %d  chunks %d" % (len(tiles_here), read))
        for (nm, why) in arc.unreadable[:8]:
            print("   unreadable %s: %s" % (nm, why))
        if len(arc.unreadable) > 8:
            print("   ... %d more unreadable tiles" % (len(arc.unreadable) - 8))

    if not gathered and seen_maps:
        raise SystemExit(
            "no tile of map %s in these archives: they carry %s"
            % (", ".join(args.maps), ", ".join(sorted(seen_maps)))
        )
    if not any(payloads for (payloads, _, _, _) in gathered.values()):
        raise SystemExit("no MCSH blocks found in any archive")

    variant = args.variant or (variant_seen[-1] if variant_seen else "set")
    # the record order. A map's records have to be CONTIGUOUS in the table -- the
    # DLL's per-chunk gate walks a name's records as one group and refuses a name
    # that appears in two places -- and inside a group they have to ascend in h0,
    # because the pick is "the last record at or below the clock". So: maps by
    # total payloads descending then name, and inside a map its bake stamps
    # ascending, which is also the order the printed windows read in.
    by_map = {}
    for gkey in gathered:
        mp = gkey[0] if isinstance(gkey, tuple) else gkey
        by_map.setdefault(mp, []).append(gkey)
    totals = {mp: sum(len(gathered[g][0]) for g in gs) for mp, gs in by_map.items()}
    order = []
    h0 = {}
    for mp in sorted(by_map, key=lambda m: (-totals[m], m)):
        groups = sorted(by_map[mp], key=lambda g: (g[1] if isinstance(g, tuple) else 0))
        groups = [g for g in groups if gathered[g][0]]
        if not groups:
            continue
        stamps_here = [g[1] for g in groups if isinstance(g, tuple)]
        for g in groups:
            if not isinstance(g, tuple):
                h0[g] = None
                continue
            s = g[1]
            if s in opens_by_stamp:
                h0[g] = opens_by_stamp[s]
            elif stamps_here and s == max(stamps_here) and s == HMIN_DUSK:
                # a set stamped exactly at the arc's sunset handover owns the night:
                # its window starts at the handover, not at the midpoint, which is
                # what keeps stock's shadows from sunset through the following dawn
                h0[g] = HMIN_DUSK
            elif s == min(stamps_here):
                # the earliest set's window opens at the arc's sunrise crossing, not
                # at the midpoint with the night set: before that crossing the light
                # is stock's, so stock shadows are the ones that match the ground
                h0[g] = HMIN_DAWN
            else:
                i = stamps_here.index(s)
                h0[g] = (stamps_here[i - 1] + s) // 2
            if not (0 <= h0[g] < HMIN_DAY):
                raise SystemExit("set %s of map %s would open its window at %d half-minutes, "
                                 "outside a day" % (fmt_clock(s), display_name[mp], h0[g]))
        order.extend(groups)
    version = 2 if stamps else 1
    if version == 2:
        print(
            "windows: %s" % "  ".join(
                "%s %s -> %s" % (display_name[g[0]], fmt_clock(g[1]), fmt_clock(h0[g]))
                for g in order
            )
        )
        print(
            "   the opening is where the day clock starts using that set; the stamp says when the sun was"
            " where these shadows were baked. Between two stamps the pick is a midpoint guess, so pass"
            " --open for a set whose window you know better; the set stamped at the arc's dusk crossing"
            " (21:30) opens there, so it owns the night, and the first set opens at its dawn crossing"
            " (05:50)."
        )
    if len(by_map) > 1:
        print(
            "these archives carry %d maps with tiles, so the one .dat carries %d records over "
            "%d map name(s): a chunk key is only unique INSIDE a map, and the .dat's map table "
            "says which run every entry belongs to"
            % (len(by_map), len(order), len(by_map))
        )
    _write_dat(args.out, order, variant, gathered, display_name, not args.no_dedupe, version, h0)


def _write_dat(out, order, variant, gathered, display_name, dedupe, version, h0):
    """One .dat, several maps inside it (see shadow_dat.h's map-record table).

    A chunk key is (gy<<10)|gx, which is only unique INSIDE a map: tile 14,20
    chunk 3,5 of Azeroth and of Kalimdor are the same number. So the file carries
    a map table after the header, entries are 12 bytes instead of 8 and the third
    one is the entry's row in that table, and the entries are grouped per map in
    table order with keys ascending inside a group -- which is what lets the DLL
    binary-search one group and never answer a chunk with another map's shadow.

    From version 2 a row of that table is a shadow SET of a map rather than a map,
    so it carries one more dword, h0: where in the day this row's entries start,
    half-minutes from midnight, the same unit the DLL's SunHalfMinute reads the
    client's clock in. A map with several sets is several rows sharing one name,
    adjacent and ascending in h0 -- which is the order `order` is built in -- and
    the DLL answers a chunk with the row whose h0 is the greatest at or below its
    clock, wrapping to the biggest when the clock precedes all of them. Version 1
    keeps the 28-byte row and no clock word, so it stays byte-identical to what
    every archive so far produced.

    The payload area is one interned pool for the whole file: identical bodies
    share one 512-byte blob even across maps and sets, and that is safe because
    what decides whether a payload is used at all is the entry's row, not the
    blob it points at.
    """
    # the distinct map NAMES the table carries: a map with six sets is one name, so
    # the name checks below run over this, not over the records
    maps_here = []
    for group in order:
        gmap = group[0] if isinstance(group, tuple) else group
        if gmap not in maps_here:
            maps_here.append(gmap)
    # the gate the DLL runs is on the names written here, so two maps whose names
    # collapse to the same 23 bytes would be indistinguishable: refuse rather than
    # write a file whose runs cannot be told apart
    short = {}
    for gmap in maps_here:
        s = display_name[gmap][:23].lower()
        if s in short:
            raise SystemExit(
                "maps %s and %s have the same name once cut to 23 characters: the .dat's runs "
                "could not be told apart, so nothing is written" % (short[s], display_name[gmap])
            )
        short[s] = display_name[gmap]

    # a version-2 map's windows must be distinct and ascending in table order,
    # because the DLL refuses a table where two rows of one name claim one window
    # (it would pick whichever comes last and half the day would draw the wrong
    # shadows), and refuses a window outside a day (the clock it compares against
    # never gets there, so the row could only ever be reached by the wrap).
    rec_bytes = 32 if version >= 2 else 28
    if version >= 2:
        prev_win = {}
        for group in order:
            gmap = group[0] if isinstance(group, tuple) else group
            w = h0[group]
            if w is None or not (0 <= w < HMIN_DAY):
                raise SystemExit("set %s of map %s has a window clock outside a day: nothing written"
                                 % (fmt_clock(group[1]), display_name[gmap]))
            if gmap in prev_win and w <= prev_win[gmap]:
                raise SystemExit("map %s has two sets opening at %s or earlier in a row: the DLL "
                                 "reads one window as two, so nothing is written"
                                 % (display_name[gmap], fmt_clock(w)))
            prev_win[gmap] = w

    # one payload per distinct 512-byte block; identical chunks share it, which is
    # what keeps a full Azeroth set near 25 MB instead of 741 tiles x 256 x 512 B
    blobs = {}
    bodies = []
    entries = []
    map_counts = []
    for (mi, group) in enumerate(order):
        payloads = gathered[group][0]
        count = 0
        for key in sorted(payloads):
            body = payloads[key]
            if dedupe:
                off = blobs.get(body)
                if off is None:
                    off = len(bodies) * 512
                    blobs[body] = off
                    bodies.append(body)
            else:
                off = len(bodies) * 512
                bodies.append(body)
            entries.append((mi, key, off))
            count += 1
        map_counts.append(count)

    keys = [e[1] for e in entries]
    xs = [k & 0x3FF for k in keys]
    ys = [(k >> 10) & 0x3FF for k in keys]
    gx0, gx1 = min(xs), max(xs)
    gy0, gy1 = min(ys), max(ys)
    tiles_all = set()
    for group in order:
        tiles_all |= gathered[group][1]
    # per-axis tile bounds, not first/last of the sorted list, which mis-orders them
    tile_x = [t[0] for t in tiles_all]
    tile_y = [t[1] for t in tiles_all]
    tx0, tx1, ty0, ty1 = min(tile_x), max(tile_x), min(tile_y), max(tile_y)
    # the header's own map word names the file: one map NAME when there is one, so
    # an older reader that ignores the table still gates on the right map -- and a
    # six-set continent is still one name here, not MULTI -- and MULTI when there
    # are several maps, so an older reader refuses outright rather than drawing
    # one map's shadows onto another's
    if len(maps_here) == 1:
        label = display_name[maps_here[0]]
    else:
        label = "MULTI"
    map_name = label.encode("ascii", "replace")[:23]
    var_name = variant.encode("ascii", "replace")[:39]
    # layout, in the order written: the 124-byte header, then mapCount records of
    # 28 bytes (version 1) or 32 (version 2, whose extra dword is the set's window
    # clock), then entryCount entries of 12, then the pool. So payload byte 0 is
    # file offset 124 + mapCount*rec_bytes + entryCount*12, and a reader that seeks
    # past the entries only fills its arena rec_bytes short of every payload --
    # 336 bytes on a 12-map version-1 file, 42 of the 64 texel rows of every chunk,
    # which is what made an in-game set look mirrored and rotated (sv11's loader
    # did exactly that; sv12's seeks both tables).
    with open(out, "wb") as f:
        f.write(struct.pack(
            "<4sI24s40sIII8iII",
            b"SHDW",
            version,
            map_name.ljust(24, b"\0"),
            var_name.ljust(40, b"\0"),
            len(entries),
            len(bodies),
            len(bodies) * 512,
            tx0,
            ty0,
            tx1,
            ty1,
            gx0,
            gy0,
            gx1,
            gy1,
            len(tiles_all),   # distinct tiles across the file, sets share them
            len(order),       # mapCount: records, so a six-set map is six of them
        ))
        for (mi, group) in enumerate(order):
            # the DLL refuses a name that does not fit with a NUL inside the 24,
            # so the record name is truncated to 23 the same way the header's is
            gmap = group[0] if isinstance(group, tuple) else group
            rec = display_name[gmap].encode("ascii", "replace")[:23]
            if version >= 2:
                f.write(struct.pack("<24sII", rec.ljust(24, b"\0"), map_counts[mi], h0[group]))
            else:
                f.write(struct.pack("<24sI", rec.ljust(24, b"\0"), map_counts[mi]))
        for (mi, key, off) in entries:
            f.write(struct.pack("<III", key, off, mi))
        for body in bodies:
            f.write(body)

    size = os.path.getsize(out)
    expect = 124 + len(order) * rec_bytes + len(entries) * 12 + len(bodies) * 512
    print(
        "-> %s  %d bytes  version %d  rule 124 + %d records*%d + %d entries*12 + %d payloads*512 = %d"
        % (out, size, version, len(order), rec_bytes, len(entries), len(bodies), expect)
    )
    if size != expect:
        raise SystemExit("the .dat written does not match its own size rule: %d vs %d" % (size, expect))
    print("map names in this .dat: %s" % " ".join(display_name[g] for g in maps_here))
    print("map records in this .dat: %d (%s)"
          % (len(order), " ".join(
              "%s@%s" % (display_name[g[0]], fmt_clock(h0[g])) if version >= 2
              else display_name[g] for g in order)))
    for (mi, group) in enumerate(order):
        payloads, tiles_seen, per_tile, missing = gathered[group]
        gmap = group[0] if isinstance(group, tuple) else group
        set_txt = "" if version < 2 else "  set baked %s  window %s.." % (
            fmt_clock(group[1]), fmt_clock(h0[group]))
        print(
            "map %s%s  variant %s\n"
            "tiles %d (%d..%d, %d..%d)  entries %d"
            % (
                display_name[gmap],
                set_txt,
                variant,
                len(tiles_seen),
                min(t[0] for t in tiles_seen),
                max(t[0] for t in tiles_seen),
                min(t[1] for t in tiles_seen),
                max(t[1] for t in tiles_seen),
                len(payloads),
            )
        )
        counts = defaultdict(int)
        for (tx, ty), n in per_tile.items():
            counts[n] += 1
        print("chunks per tile: %s  chunks without an MCSH: %d"
              % (dict(sorted(counts.items())), missing[0]))
    print("whole-file tile bounds %d..%d, %d..%d  chunk grid x %d..%d  y %d..%d"
          % (tx0, tx1, ty0, ty1, gx0, gx1, gy0, gy1))


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except MpqError as e:
        print("!! %s" % e)
        raise SystemExit(1)
