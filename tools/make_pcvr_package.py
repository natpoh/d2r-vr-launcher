#!/usr/bin/env python3
"""A package for d2r-pcvr, made from a copy of the game folder it is installed in - until its author ships a package of his own.

    make_pcvr_package.py --game <game folder or a copy of it> --out <package folder> [--helper <his "D2R PCVR" folder>]
                         [--example examples/d2r-pcvr/manifest.json]

What it reads (nothing is written there): the installed files, and the record his installer left, d2rloader\\pcvr-install.json ->
setup-backups\\game\\<stamp>\\manifest.json, whose "installed" list is his own account of every file he put into the game folder (741 of them in
0.3.15). With --helper (his %LOCALAPPDATA%\\Programs\\D2R PCVR) the release's pristine d2r_vr.ini becomes the seed of the ini; without it the
installed ini is the seed.

What it makes: <out>\\manifest.json + <out>\\payload\\..., from the hand-written example manifest (examples/d2r-pcvr/manifest.json, which
says WHICH of his files are programs, assets, settings, shared or generated) with the version, the identity (the DLL's SHA-256 and a
marker string that only his DLL has) and every file's SHA-256 filled in from what is really installed. Every file his record lists must be
classified: one that is neither in the manifest nor one of the shared D2RLoader / ReShade files stops the run (a new file in a later release
should not be missed).

It is a stand-in: the package is not published by his author, and it holds the player's own bytes (and no icons: they are extracted from the
player's own game and stay "generated").
"""
import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

SHARED_WITH_D2RLOADER = {"d2rcore.dll", "d2rloader.exe", "d2rloader.mpq", "reshade64.dll", "d2rloader/config/d2rloader.toml"}
MARKER = "d2r_vr_icons"          # in his DLL as UTF-16 (the cache folder's name); ours has d2r_vr_skill_icons instead


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--helper")
    ap.add_argument("--example", default=str(Path(__file__).resolve().parent.parent / "examples" / "d2r-pcvr" / "manifest.json"))
    a = ap.parse_args()
    game, out = Path(a.game), Path(a.out)
    manifest = json.loads(Path(a.example).read_text(encoding="utf-8"))

    record = game / "d2rloader" / "pcvr-install.json"
    if not record.is_file():
        sys.exit(f"{record} is not there: this game folder holds no installation of d2r-pcvr")
    rec = json.loads(record.read_text(encoding="utf-8"))
    installed = None
    for snap in rec.get("snapshots", []):
        mf = Path(snap) / "manifest.json"
        if mf.is_file():
            installed = [p.replace("\\", "/") for p in json.loads(mf.read_text(encoding="utf-8"))["installed"]]
            break
    if installed is None:
        sys.exit("none of the snapshots listed in pcvr-install.json has a manifest.json: his list of installed files is not available")

    # the files the example manifest names, as game-relative paths; folder entries end with "/"
    names = [(f["dst"].replace("{game}/", ""), f) for f in manifest["files"]]
    def covered(rel):
        low = rel.lower()
        if low in SHARED_WITH_D2RLOADER:
            return True
        for dst, _ in names:
            if dst.endswith("/") and low.startswith(dst.lower()):
                return True
            if dst.lower() == low:
                return True
        return False
    unknown = [p for p in installed if not covered(p)]
    if unknown:
        sys.exit("files his record lists that the example manifest does not know (classify them first):\n  " + "\n  ".join(unknown[:20]))
    missing = [p for p in installed if not (game / p).exists() and p.lower() not in SHARED_WITH_D2RLOADER]
    if missing:
        print(f"note: {len(missing)} of the files his record lists are not in {game} (a partial copy?), first: {missing[0]}", file=sys.stderr)

    version, tag = manifest["version"], None
    if a.helper and (Path(a.helper) / "release-manifest.json").is_file():
        rm = json.loads((Path(a.helper) / "release-manifest.json").read_text(encoding="utf-8"))
        tag = rm.get("release_tag")
        m = re.match(r"v?(\d+\.\d+\.\d+)", tag or "")
        if m:
            version = m.group(1)

    if out.exists():
        shutil.rmtree(out)
    pay = out / "payload"

    def put(src, rel):
        dst = pay / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        return sha(dst)

    files = []
    for f in manifest["files"]:
        rel = f["dst"].replace("{game}/", "")
        e = dict(f)
        if f["role"] == "generated":
            e.pop("src", None)
        elif rel.endswith("/"):                           # a folder of the game folder: pack what the installed folder holds
            folder = game / rel
            if not folder.is_dir():
                print(f"note: {rel} is not in {game}; the entry is optional", file=sys.stderr)
                e["optional"] = True
                e["src"] = "payload/" + rel
                (pay / rel).mkdir(parents=True, exist_ok=True)
            else:
                e["src"] = "payload/" + rel
                for p in sorted(folder.rglob("*")):
                    if p.is_file():
                        put(p, rel + p.relative_to(folder).as_posix())
        else:
            live = game / rel
            if not live.is_file():
                sys.exit(f"{live} is not there")
            e["src"] = "payload/" + rel
            e["sha256"] = put(live, rel)
            if f["role"] == "config":
                seed = None
                if rel.endswith("d2rloader/plugins/d2r_vr.ini") and a.helper and (Path(a.helper) / "d2r_vr.ini").is_file():
                    seed = Path(a.helper) / "d2r_vr.ini"
                    e["seed"] = "payload/seed/d2r_vr.ini"
                    put(seed, "seed/d2r_vr.ini")
                else:
                    e["seed"] = e["src"]
        files.append(e)
    # the folder entry for the sky pictures is listed twice in the example (shared + asset): both point at the same payload folder
    manifest["files"] = files

    plugin = game / "d2rloader" / "plugins" / "d2rl-vrcam.dll"
    data = plugin.read_bytes()
    wide = MARKER.encode("utf-16-le")
    if MARKER.encode() not in data and wide not in data:
        sys.exit(f"the marker {MARKER!r} is not in {plugin}: this is not the build this script knows (look for a string only his DLL has)")
    manifest["version"] = version
    manifest["identity"]["dll_sha256"] = [sha(plugin)]
    manifest["identity"]["markers"] = [MARKER]
    manifest["identity"].pop("about_file", None)
    manifest["build"] = {"channel": tag or "windows-preview"}
    manifest["x-captured"] = {
        "note": "made by tools/make_pcvr_package.py from an installed copy and his own record (pcvr-install.json -> the snapshot's manifest.json); not published by his author",
        "release_tag": tag,
        "files_in_his_record": len(installed),
    }
    manifest["x-unverified"] = [u for u in manifest.get("x-unverified", []) if not u.startswith(("/identity/dll_sha256", "/identity/markers", "/identity/about_file", "/files/*/src"))]
    manifest["x-unverified"].append("/identity/plugin_id (his PluginInfo id is still d2r-vr-vrcam, name vrcam, version 0.152.0 in 0.3.15: the same as D2R VR's was)")
    out.mkdir(parents=True, exist_ok=True)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    n = sum(1 for p in pay.rglob("*") if p.is_file())
    print(f"d2r-pcvr {version} ({tag}): {n} files packed from {game} -> {out}")
    print(f"  plugin SHA-256 {manifest['identity']['dll_sha256'][0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
