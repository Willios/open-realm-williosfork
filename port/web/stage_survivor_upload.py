#!/usr/bin/env python3
"""Stage the Warcraft Survivors page for upload: only what the site serves.

Copies the runtime files of a survivor build (build-engine.py --survivor),
its .htaccess (site/survivor.htaccess) and its data/ folder (hard links when
possible) into one folder whose contents go to the web root as they are.

  python port/web/stage_survivor_upload.py build/web/survivor build/web/survivor-upload
      [--download ../release/WarcraftSurvivors.zip]

--download adds the portable Windows build (tools/release/make_portable.ps1)
as download/WarcraftSurvivors.zip, the lobby's Download button
(SURVIVOR_ENGINE.download in src/engine_boot.lua).
"""
import argparse
import os
import pathlib
import shutil

ROOT = pathlib.Path(__file__).resolve().parents[2]
# Build intermediates, not served.
SKIP_SUFFIXES = (".o", ".c")
SKIP_NAMES = {"survivor-shell.html", "warcraft-3-share", "data"}


def place(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        target.unlink()
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build", help="survivor build folder (build/web/survivor)")
    parser.add_argument("out", help="upload folder to create")
    parser.add_argument("--download", help="portable build zip, served as download/WarcraftSurvivors.zip")
    args = parser.parse_args()
    build, out = pathlib.Path(args.build), pathlib.Path(args.out)
    if not (build / "index.wasm").is_file():
        raise SystemExit("not a survivor build: " + str(build))
    if not (build / "data" / "files.json").is_file():
        raise SystemExit("no game data: run port/web/make_survivor_data.py first")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    for path in build.iterdir():
        if path.name in SKIP_NAMES or path.suffix in SKIP_SUFFIXES:
            continue
        if path.is_dir():
            shutil.copytree(path, out / path.name)
        else:
            shutil.copy2(path, out / path.name)
    for path in (build / "data").rglob("*"):
        if path.is_file():
            place(path, out / "data" / path.relative_to(build / "data"))
    if args.download:
        place(pathlib.Path(args.download), out / "download" / "WarcraftSurvivors.zip")
    shutil.copy2(ROOT / "port/web/site/survivor.htaccess", out / ".htaccess")
    total = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print("%d files, %.0f MB -> %s" % (sum(1 for p in out.rglob("*") if p.is_file()), total / 1048576, out))


if __name__ == "__main__":
    main()
