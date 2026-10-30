#!/usr/bin/env python3
"""Lay out self-hosted game data for the Warcraft Survivors browser page.

Copies the Warcraft III 1.29 archives and the survivor arena maps (the maps
listed in src/engine_boot.lua, SURVIVOR_ENGINE) into <out>/data and writes
<out>/data/files.json, which survivor.js downloads into the browser on a
player's first visit. The id changes whenever a file changes, so players
download again only then.

The archives are trimmed (see trim_archives): files the survivor build never
reads are dropped, everything else is copied byte for byte with the same
hash table, so lookups behave as in the full archives. --full keeps them whole.

This is Blizzard data: never commit it or publish it to GitHub.

  python port/web/make_survivor_data.py "C:/Program Files (x86)/Warcraft III (Legacy)" build/web/survivor
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import struct

ROOT = pathlib.Path(__file__).resolve().parents[2]
ARCHIVES = ("War3.mpq", "War3Local.mpq", "War3x.mpq", "War3xLocal.mpq")  # Lowest priority first.
LISTFILE = ROOT / "tools" / "WarcraftIIIFileListZezulaLadik.txt"

# --- MPQ v1 trim -------------------------------------------------------------
# The engine opens a file from the highest archive that has it (FS_OpenFile), so
# a lower copy of a model, texture or sound is never read. Text data is merged
# across archives (FS_ReadFileAll), so every copy of it stays. Unnamed files
# (not in the listfile) always stay.
MEDIA = (".BLP", ".MDX", ".MDL", ".WAV", ".MP3", ".TGA", ".TTF", ".DDS", ".JPG", ".MPQ")  # Names are upper case.
DROP_PREFIXES = (
    "SOUND\\DIALOGUE\\",  # Campaign and cinematic voices (DialogSounds.slk).
    "MAPS\\CAMPAIGN\\",
    "MAPS\\FROZENTHRONE\\CAMPAIGN\\",
)
KEEP_PREFIXES = ("SOUND\\DIALOGUE\\GENERICWARNINGS\\",)  # UISounds.slk warnings.
# Low/medium texture quality sets; the engine never mounts them (it does mount
# the single-letter tileset archives, R_W3OpenTilesetArchive).
DROP_FILES = {"WAR3_LOW.MPQ", "WAR3_MED.MPQ", "WAR3X_LOW.MPQ", "WAR3X_MED.MPQ"}

M32 = 0xFFFFFFFF
FILE_EXISTS, FILE_ENCRYPTED, FILE_FIX_KEY = 0x80000000, 0x00010000, 0x00020000
FILE_SINGLE_UNIT, FILE_SECTOR_CRC, FILE_COMPRESSED = 0x01000000, 0x04000000, 0x00000300
HASH_DELETED = 0xFFFFFFFE


def crypt_table():
    table, seed = [0] * 0x500, 0x00100001
    for i in range(0x100):
        for j in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            high = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            table[i + j * 0x100] = high | (seed & 0xFFFF)
    return table


CRYPT = crypt_table()


def mpq_hash(name, kind):
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for c in name.upper().replace("/", "\\").encode("latin-1"):
        s1 = (CRYPT[(kind << 8) + c] ^ (s1 + s2)) & M32
        s2 = (c + s1 + s2 + (s2 << 5) + 3) & M32
    return s1


def crypt(data, key, encrypt):
    # Whole dwords only; trailing bytes are stored in the clear.
    count = len(data) // 4
    words = struct.unpack_from("<%dI" % count, data)
    out = []
    s2 = 0xEEEEEEEE
    for v in words:
        s2 = (s2 + CRYPT[0x400 + (key & 0xFF)]) & M32
        x = (v ^ (key + s2)) & M32
        out.append(x)
        plain = v if encrypt else x
        key = (((~key << 0x15) + 0x11111111) | (key >> 0x0B)) & M32
        s2 = (plain + s2 + (s2 << 5) + 3) & M32
    return struct.pack("<%dI" % count, *out) + bytes(data[count * 4:])


def rekey(data, name, old_pos, new_pos, size, flags, sector_size):
    """Re-encrypt a FIX_KEY file, whose key depends on its offset in the archive."""
    base = mpq_hash(re.split(r"[\\/]", name)[-1], 3)
    old_key, new_key = ((base + old_pos) ^ size) & M32, ((base + new_pos) ^ size) & M32
    swap = lambda chunk, delta: crypt(crypt(chunk, (old_key + delta) & M32, False), (new_key + delta) & M32, True)
    if flags & FILE_SINGLE_UNIT:
        return swap(data, 0)
    sectors = (size + sector_size - 1) // sector_size
    if not flags & FILE_COMPRESSED:
        return b"".join(swap(data[i * sector_size:(i + 1) * sector_size], i) for i in range(sectors))
    entries = sectors + 1 + (1 if flags & FILE_SECTOR_CRC else 0)
    table = crypt(data[:entries * 4], (old_key - 1) & M32, False)
    offsets = struct.unpack("<%dI" % entries, table)
    if offsets[0] != entries * 4:
        raise SystemExit("unexpected sector table in " + name)
    out = [crypt(table, (new_key - 1) & M32, True)]
    out += [swap(data[offsets[i]:offsets[i + 1]], i) for i in range(sectors)]
    out.append(bytes(data[offsets[sectors]:]))  # Sector CRCs are not encrypted.
    return b"".join(out)


def read_archive(path):
    data = memoryview(path.read_bytes())
    magic, header_size, _, version, shift, hash_pos, block_pos, hash_count, block_count = \
        struct.unpack_from("<4sIIHHIIII", data, 0)
    if magic != b"MPQ\x1a" or version != 0:
        raise SystemExit("%s: expected an MPQ v1 header at offset 0" % path)
    hashes = struct.unpack("<%dI" % (hash_count * 4),
                           crypt(data[hash_pos:hash_pos + hash_count * 16], mpq_hash("(hash table)", 3), False))
    blocks = struct.unpack("<%dI" % (block_count * 4),
                           crypt(data[block_pos:block_pos + block_count * 16], mpq_hash("(block table)", 3), False))
    hashes = [list(hashes[i:i + 4]) for i in range(0, len(hashes), 4)]
    blocks = [list(blocks[i:i + 4]) for i in range(0, len(blocks), 4)]
    return {"data": data, "header_size": header_size, "sector_size": 512 << shift, "shift": shift,
            "hashes": hashes, "blocks": blocks}


def archive_names(archive, keys):
    """Listfile names found in the archive: {block index: upper-case name}."""
    by_hash = {(e[0], e[1]): e[3] for e in archive["hashes"] if e[3] < len(archive["blocks"])}
    return {by_hash[k]: name for k, name in keys.items() if k in by_hash}


def dropped(name, higher):
    if any(name.startswith(p) for p in KEEP_PREFIXES):
        return False
    if name in DROP_FILES or any(name.startswith(p) for p in DROP_PREFIXES):
        return True
    return name.endswith(MEDIA) and name in higher


def write_trimmed(archive, names, drop, target):
    data, blocks = archive["data"], archive["blocks"]
    keep = {i for i, b in enumerate(blocks) if b[3] & FILE_EXISTS and i not in drop}
    body, pos, moved = [], archive["header_size"], {}
    for i in sorted(keep, key=lambda i: blocks[i][0]):
        old_pos, csize, size, flags = blocks[i]
        if old_pos in moved:  # Shared payload: keep sharing it.
            continue
        chunk = data[old_pos:old_pos + csize]
        if flags & FILE_ENCRYPTED and flags & FILE_FIX_KEY:
            if i not in names:
                raise SystemExit("cannot re-encrypt unnamed block %d" % i)
            chunk = rekey(chunk, names[i], old_pos, pos, size, flags, archive["sector_size"])
        moved[old_pos] = pos
        body.append(chunk)
        pos += csize
    new_blocks = []
    for i, (old_pos, csize, size, flags) in enumerate(blocks):
        if i in keep:
            new_blocks.append((moved[old_pos], csize, size, flags))
        else:
            new_blocks.append((0, 0, 0, 0))
    new_hashes = []
    for h1, h2, locale, block in archive["hashes"]:
        if block < len(blocks) and block not in keep:
            h1, h2, block = M32, M32, HASH_DELETED  # Deleted, not free: probe chains go on.
        new_hashes.append((h1, h2, locale, block))
    hash_pos = pos
    block_pos = hash_pos + len(new_hashes) * 16
    total = block_pos + len(new_blocks) * 16
    hash_table = crypt(struct.pack("<%dI" % (len(new_hashes) * 4), *(v for e in new_hashes for v in e)),
                       mpq_hash("(hash table)", 3), True)
    block_table = crypt(struct.pack("<%dI" % (len(new_blocks) * 4), *(v for e in new_blocks for v in e)),
                        mpq_hash("(block table)", 3), True)
    header = struct.pack("<4sIIHHIIII", b"MPQ\x1a", 32, total, 0, archive["shift"],
                         hash_pos, block_pos, len(new_hashes), len(new_blocks))
    header += bytes(data[32:archive["header_size"]])
    temp = target.with_name(target.name + ".tmp")
    target.parent.mkdir(parents=True, exist_ok=True)
    with open(temp, "wb") as out:
        out.write(header)
        for chunk in body:
            out.write(chunk)
        out.write(hash_table)
        out.write(block_table)
    # Replace the directory entry: an old hard link to the install is unlinked, never written through.
    os.replace(temp, target)
    return total


def trim_archives(wc3, data):
    """Write trimmed copies of ARCHIVES into data/. Returns {name: (kept, full) sizes}."""
    keys = {}
    for line in LISTFILE.read_text(encoding="latin-1").splitlines():
        name = line.strip().upper().replace("/", "\\")
        if name:
            keys[(mpq_hash(name, 1), mpq_hash(name, 2))] = name
    archives = [read_archive(wc3 / name) for name in ARCHIVES]
    names = [archive_names(a, keys) for a in archives]
    sizes = {}
    for index, name in enumerate(ARCHIVES):
        higher = set()
        for later in names[index + 1:]:
            higher.update(later.values())
        drop = {i for i, n in names[index].items() if dropped(n, higher)}
        sizes[name] = (write_trimmed(archives[index], names[index], drop, data / name),
                       (wc3 / name).stat().st_size)
        print("  %-15s %4.0f MB -> %4.0f MB (%d files dropped)" %
              (name, sizes[name][1] / 1048576, sizes[name][0] / 1048576, len(drop)))
    return sizes


def survivor_maps(boot):
    # map = 'Maps\\FrozenThrone\\(6)Moonglade.w3x' (Lua string escapes).
    maps = []
    for raw in re.findall(r"map\s*=\s*'((?:[^'\\]|\\.)*)'", boot.read_text(encoding="utf-8")):
        name = re.sub(r"\\(.)", r"\1", raw).replace("\\", "/")
        if name not in maps:
            maps.append(name)
    return maps


def place(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        if target.stat().st_size == source.stat().st_size and target.stat().st_mtime >= source.stat().st_mtime:
            return
        target.unlink()
    try:
        os.link(source, target)  # Same volume: no 1.2 GB copy.
    except OSError:
        shutil.copy2(source, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("wc3", help="Warcraft III 1.29 folder (War3.mpq, Maps/)")
    parser.add_argument("out", help="built survivor page (build/web/survivor)")
    parser.add_argument("--boot", default=str(ROOT.parent / "src" / "engine_boot.lua"))
    parser.add_argument("--full", action="store_true", help="link the archives whole instead of trimming them")
    args = parser.parse_args()
    wc3 = pathlib.Path(args.wc3)
    data = pathlib.Path(args.out) / "data"
    names = list(ARCHIVES) + survivor_maps(pathlib.Path(args.boot))
    for name in names:
        if not (wc3 / name).is_file():
            raise SystemExit("missing " + str(wc3 / name))
    trimmed = set() if args.full else set(trim_archives(wc3, data))
    files = []
    digest = hashlib.sha1()
    for name in names:
        source = wc3 / name
        if name in trimmed:
            source = data / name
        else:
            place(source, data / name)
        size = source.stat().st_size
        files.append({"name": name, "size": size})
        with open(source, "rb") as handle:
            # Size plus the first and last MiB: cheap, and any patch changes them.
            digest.update(name.encode() + str(size).encode() + handle.read(1 << 20))
            handle.seek(max(0, size - (1 << 20)))
            digest.update(handle.read())
    manifest = {"id": digest.hexdigest()[:16], "files": files}
    (data / "files.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    total = sum(f["size"] for f in files)
    print("%d files, %.0f MB, id %s -> %s" % (len(files), total / 1048576, manifest["id"], data))


if __name__ == "__main__":
    main()
