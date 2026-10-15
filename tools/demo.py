#!/usr/bin/env python3
"""A walk through the launcher on a FAKE game folder (nothing real is touched).

    demo.py <vrmodlauncher executable> <work folder>

Builds <work>/game (a stand-in for the D2R folder), <work>/home (the launcher's folder) and two packages from the example
manifests with dummy files, then: register both, activate D2R VR, tune its ini, switch to D2R PCVR and back, show status,
verify, a dry-run launch of each, a simulated crash-free recovery check. The output is what a player's support log would show.
"""
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

exe = sys.argv[1]
work = Path(sys.argv[2])
here = Path(__file__).resolve().parent.parent
if work.exists():
    shutil.rmtree(work)
game, home = work / "game", work / "home"

def put(p, text):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")

# the fake game
build = "3.3.93787"
(game).mkdir(parents=True)
(game / "D2R.exe").write_bytes(b"MZ fake " + build.encode("utf-16-le"))
put(game / "Data" / ".build.config", "x")
put(game / "D2RLoader.exe", "fake D2RLoader")
put(game / "ReShade64.dll", "fake reshade")
put(game / "d2rloader" / "config" / "d2rloader.toml", "[d2rloader.advanced]\nallow_global_extensions = true\n")
put(game / "d2rloader" / "logs" / "d2rloader.log", f"[x] D2RLoader v1.3.1-beta (Release) started.\n[x] [BUILD] D2R version: {build}.\n")

# the packages: the example manifests + a dummy for every src
for mod in ("d2r-vr", "d2r-pcvr"):
    pkg = home / "mods" / mod
    text = (here / "examples" / mod / "manifest.json").read_text(encoding="utf-8")
    put(pkg / "manifest.json", text)
    for f in json.loads(text)["files"]:
        if f["role"] == "generated" or (f.get("optional") and f.get("tag") == "vr"):
            continue
        src = f["src"]
        if src.endswith("/"):
            names = ["D2R_Ceiling_act1_caves.png"] if "sky_ours" in src else ["D2R_Sky_act1.png", "D2R_Crosshair.png"]
            for n in names:
                put(pkg / src / n, "picture " + n)
        else:
            put(pkg / src, f"{mod}: {src}")
        if "seed" in f:
            put(pkg / f["seed"], f"; {mod} defaults for {f['dst']}\n")

ENV = {k: v for k, v in os.environ.items() if k.upper() not in ("PROGRAMFILES", "PROGRAMW6432")}   # the demo brings its own

def run(*args, show=True):
    cmd = [exe, "--home", str(home), "--game", str(game), *args]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV)
    if show:
        print("\n$ vrmodlauncher " + " ".join(args))
        print((r.stdout + r.stderr).rstrip() or "(no output)")
        if r.returncode:
            print(f"(exit {r.returncode})")
    return r

run("register", str(home / "mods" / "d2r-vr" / "manifest.json"))
run("register", str(home / "mods" / "d2r-pcvr" / "manifest.json"))
run("list")
run("activate", "d2r-vr")
run("status")
ini = game / "d2rloader" / "plugins" / "d2r_vr.ini"
print(f"\n(the player tunes D2R VR: {ini.name} had: {ini.read_text().strip()!r})")
put(ini, "[openxr]\non=1\nmy_tuning=yes\n")
run("activate", "d2r-pcvr", "--dry-run")
run("activate", "d2r-pcvr")
print(f"\n(D2R PCVR's ini: {ini.read_text().strip()!r}; openxr_loader.dll there: {(game / 'openxr_loader.dll').exists()})")
run("verify")
run("activate", "d2r-vr")
print(f"\n(back in D2R VR, the ini again: {ini.read_text().strip()!r})")
run("launch", "d2r-vr", "--dry-run")
run("activate", "d2r-pcvr")
# the runtime folders for this demo
pf = work / "Program Files"
put(pf / "Virtual Desktop Streamer" / "OpenXR" / "virtualdesktop-openxr.json", "{}")
ENV["ProgramFiles"] = ENV["ProgramW6432"] = str(pf)          # where this demo's "Virtual Desktop Streamer" is
run("launch", "d2r-pcvr", "--runtime", "vdxr", "--dry-run")
run("launch", "d2r-pcvr", "--runtime", "vdxr", "--store", "battlenet", "--dry-run")
run("launch", "d2r-pcvr", "--runtime", "system", "--dry-run")
run("requirements", "d2r-vr")
run("status", "--json", show=False)
print("\ndone; the fake game and the launcher's folder are in", work)
