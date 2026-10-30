#!/usr/bin/env python3
"""Lay out self-hosted game data for the Warcraft Survivors browser page.

Copies the Warcraft III 1.29 archives and the survivor arena maps (the maps
listed in src/engine_boot.lua, SURVIVOR_ENGINE) into <out>/data and writes
<out>/data/files.json, which survivor.js downloads into the browser on a
player's first visit. The id changes whenever a file changes, so players
download again only then.

The archives are trimmed (see trim_archives) to the files the game reads,
recorded by tools/web_trace.py in the survivor repository (--reads, its
tools/web_trace_reads.txt). Kept files are copied byte for byte with the same
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

# --- MPQ v1 trim -------------------------------------------------------------
# The reads list names, per archive, every file the engine opened in the traced
# sessions (the highest archive's copy, and every copy of merged text data);
# "*" stands for the highest archive that has the file. Everything else is
# dropped, except the archives' own special files.
SPECIAL = ("(listfile)", "(attributes)", "(signature)")

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


def find_blocks(archive, name):
    """Block indexes of every locale's copy of name (empty when absent)."""
    hashes, blocks = archive["hashes"], archive["blocks"]
    mask = len(hashes) - 1
    h1, h2 = mpq_hash(name, 1), mpq_hash(name, 2)
    i, found = mpq_hash(name, 0) & mask, []
    for _ in range(len(hashes)):
        a, b, _, block = hashes[i]
        if block == 0xFFFFFFFF:  # Free entry: the probe chain ends.
            break
        if a == h1 and b == h2 and block < len(blocks) and blocks[block][3] & FILE_EXISTS:
            found.append(block)
        i = (i + 1) & mask
    return found


def read_list(path):
    """{archive name (lower case) or '*': [file names]} from a reads list."""
    wanted = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        archive, sep, name = line.partition("\t")
        if not sep or not name:
            continue
        wanted.setdefault(archive.lower(), []).append(name.replace("/", "\\"))
    return wanted


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


def trim_archives(wc3, data, reads):
    """Write trimmed copies of ARCHIVES into data/. Returns {name: (kept, full) sizes}."""
    wanted = read_list(reads)
    unknown = set(wanted) - {"*"} - {name.lower() for name in ARCHIVES}
    if unknown:
        raise SystemExit("reads list names unknown archives: " + ", ".join(sorted(unknown)))
    archives = [read_archive(wc3 / name) for name in ARCHIVES]
    keep = [{} for _ in ARCHIVES]  # Per archive: {block index: name}.
    for index, name in enumerate(ARCHIVES):
        for path in wanted.get(name.lower(), []) + list(SPECIAL):
            found = find_blocks(archives[index], path)
            if not found and path not in SPECIAL:
                raise SystemExit("%s has no %s: was the list traced on another install?" % (name, path))
            keep[index].update((block, path) for block in found)
    absent = []  # "*" entries come from the listfile too (sound variants): not all exist.
    for path in wanted.get("*", []):
        for index in reversed(range(len(ARCHIVES))):
            found = find_blocks(archives[index], path)
            if found:
                keep[index].update((block, path) for block in found)
                break
        else:
            absent.append(path)
    if absent:
        print("  %d listed files are in no archive (first: %s)" % (len(absent), absent[0]))
    sizes = {}
    for index, name in enumerate(ARCHIVES):
        blocks = archives[index]["blocks"]
        drop = {i for i, b in enumerate(blocks) if b[3] & FILE_EXISTS and i not in keep[index]}
        sizes[name] = (write_trimmed(archives[index], keep[index], drop, data / name),
                       (wc3 / name).stat().st_size)
        print("  %-15s %4.0f MB -> %4.0f MB (%d files kept)" %
              (name, sizes[name][1] / 1048576, sizes[name][0] / 1048576, len(keep[index])))
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
    parser.add_argument("--reads", default=str(ROOT.parent / "tools" / "web_trace_reads.txt"),
                        help="files to keep (tools/web_trace.py reads)")
    parser.add_argument("--full", action="store_true", help="link the archives whole instead of trimming them")
    args = parser.parse_args()
    wc3 = pathlib.Path(args.wc3)
    data = pathlib.Path(args.out) / "data"
    names = list(ARCHIVES) + survivor_maps(pathlib.Path(args.boot))
    for name in names:
        if not (wc3 / name).is_file():
            raise SystemExit("missing " + str(wc3 / name))
    trimmed = set() if args.full else set(trim_archives(wc3, data, pathlib.Path(args.reads)))
    files = []
    digest = hashlib.sha1()
    for name in names:
        source = wc3 / name
        if name in trimmed:
            source = data / name
        else:
            place(source, data / name)
        size = source.stat().st_size
        # Whole-file sha1: the page re-downloads only files whose sha1 changed.
        sha1 = hashlib.sha1()
        with open(source, "rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 22), b""):
                sha1.update(chunk)
        files.append({"name": name, "size": size, "sha1": sha1.hexdigest()})
        digest.update((name + str(size) + sha1.hexdigest()).encode())
    manifest = {"id": digest.hexdigest()[:16], "files": files}
    (data / "files.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    total = sum(f["size"] for f in files)
    print("%d files, %.0f MB, id %s -> %s" % (len(files), total / 1048576, manifest["id"], data))


if __name__ == "__main__":
    main()
