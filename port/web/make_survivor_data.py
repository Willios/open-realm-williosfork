#!/usr/bin/env python3
"""Lay out self-hosted game data for the Warcraft Survivors browser page.

Copies the Warcraft III 1.29 archives and the survivor arena maps (the maps
listed in src/engine_boot.lua, SURVIVOR_ENGINE) into <out>/data and writes
<out>/data/files.json, which survivor.js downloads into the browser on a
player's first visit. The id changes whenever a file changes, so players
download again only then.

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

ROOT = pathlib.Path(__file__).resolve().parents[2]
ARCHIVES = ("War3.mpq", "War3Local.mpq", "War3x.mpq", "War3xLocal.mpq")


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
    args = parser.parse_args()
    wc3 = pathlib.Path(args.wc3)
    data = pathlib.Path(args.out) / "data"
    names = list(ARCHIVES) + survivor_maps(pathlib.Path(args.boot))
    files = []
    digest = hashlib.sha1()
    for name in names:
        source = wc3 / name
        if not source.is_file():
            raise SystemExit("missing " + str(source))
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
