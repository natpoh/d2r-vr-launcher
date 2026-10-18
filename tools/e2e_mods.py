#!/usr/bin/env python3
"""End to end for the D2RLoader-mod design, on a COPY of the neutral files of a real game folder (the real folder is only read, and never touched).

    e2e_mods.py --cli vrmodlauncher.exe --fake-loader fake_loader.exe --work <empty work folder> [--gui D2RVRLauncher.exe]
                [--real <game folder>] [--authentic-setup <setup exe built for tests>] [--pcvr-manifest <vrmod-manifest.json>]

The game copy holds the real D2R.exe, D2RLoader.exe, D2RCore.dll, d2rloader.mpq, the store marker and d2rloader.toml (about 90 MB) - no mod. Then:

 1. both mods are put in as D2RLoader mods: D2R VR Authentic by its own Setup when --authentic-setup is given (a test build of it: per user, silent), else as a fixture;
    D2R VR Unleashed (d2r-pcvr) as a fixture (his files are not ours to build here) with the manifest of --pcvr-manifest or the launcher's example;
 2. `mods` sees both with their names and versions, the reviewed D2RLoader files, the right store and build;
 3. `launch` of each mod with a stand-in D2RLoader.exe: Authentic through D2R_VR_Settings.exe --start (when the real Setup put it there) or the fixture, Unleashed
    as `-mod D2R-PCVR` with his Steam environment; the runtime of the start reaches the game as XR_RUNTIME_JSON; nothing in the game folder changes between the
    starts - the switch is only which `-mod` is started;
 4. the refusals: the game running, a mod not installed, D2RLoader missing, a runtime that is not there;
 5. with --gui: the window's `--play --dry-run` for both mods, and a screenshot.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("--cli", required=True)
ap.add_argument("--fake-loader", required=True)
ap.add_argument("--work", required=True)
ap.add_argument("--gui")
ap.add_argument("--real", default=r"D:\SteamLibrary\steamapps\common\Diablo II Resurrected")
ap.add_argument("--authentic-setup")
ap.add_argument("--pcvr-manifest")
a = ap.parse_args()
cli, fake, work, real = str(Path(a.cli).resolve()), Path(a.fake_loader).resolve(), Path(a.work).resolve(), Path(a.real)
here = Path(__file__).resolve().parent.parent
game, home = work / "game", work / "home"
failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


GENERATED = {"fake_loader_started.txt", "d2rloader/logs/d2r_vr_launch.txt"}


def snap(root):
    return {p.relative_to(root).as_posix(): sha(p) for p in sorted(root.rglob("*")) if p.is_file() and p.relative_to(root).as_posix() not in GENERATED}


def run(*args, show=True, env=None):
    cmd = [cli, "--home", str(home), "--game", str(game), *args]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if show:
        print(f"\n$ vrmodlauncher {' '.join(args)}   [exit {r.returncode}]")
        lines = (r.stdout + r.stderr).rstrip().splitlines()
        print("\n".join(lines[:14]) + (f"\n  ... ({len(lines) - 14} more lines)" if len(lines) > 14 else ""))
    return r


def started(secs=15):
    f = game / "fake_loader_started.txt"
    t0 = time.time()
    while time.time() - t0 < secs:
        if f.exists() and f.stat().st_size > 0:
            time.sleep(0.2)
            return dict(l.split("=", 1) for l in f.read_text(encoding="utf-8").splitlines() if "=" in l)
        time.sleep(0.1)
    return {}


def tasklist(name):
    r = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    return name.lower() in r.lower()


if tasklist("D2R.exe") or tasklist("D2RLoader.exe"):
    sys.exit("The game is running: close it first.")
if work.exists():
    shutil.rmtree(work)

# ------------------------------------------------------------------ 0. the neutral files of the real folder (read only)
print(f"copying the neutral files of {real} (read only) to {game} ...")
for rel in ["D2R.exe", "D2RLoader.exe", "D2RCore.dll", "d2rloader.mpq", "steam_appid.txt", "Data/.build.config", "data/.build.config", "d2rloader/config/d2rloader.toml"]:
    if (real / rel).exists():
        (game / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(real / rel, game / rel)
check((game / "D2R.exe").exists() and (game / "D2RLoader.exe").exists(), "the copy has D2R.exe and D2RLoader.exe")
REAL_LOADER = sha(game / "D2RLoader.exe")
vdxr = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Virtual Desktop Streamer" / "OpenXR" / "virtualdesktop-openxr.json"
rt_file = work / "runtime" / "runtime.json"
rt_file.parent.mkdir(parents=True)
rt_file.write_text('{"file_format_version":"1.0.0","runtime":{"library_path":"x.dll"}}', encoding="utf-8")


def modinfo(folder, desc):
    p = game / "mods" / folder / f"{folder}.mpq" / "modinfo.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({"name": folder, "savepath": "../", "description": desc}), encoding="utf-8")


# ------------------------------------------------------------------ 1. the two mods
print("\n=== 1. the two mods in the game folder")
if a.authentic_setup:
    r = subprocess.run([a.authentic_setup, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", f"/LOG={work / 'authentic_setup.log'}", f"/DIR={game}"],
                       capture_output=True, text=True, timeout=600)
    check(r.returncode == 0, f"D2R VR Authentic's Setup runs silently (exit {r.returncode})")
    check((game / "mods" / "D2R-VR" / "vrmod-manifest.json").exists(), "...and wrote mods\\D2R-VR\\vrmod-manifest.json")
else:
    modinfo("D2R-VR", "D2R VR Authentic (fixture)")
    shutil.copy2(here / "examples" / "d2rloader-mods" / "d2r-vr" / "vrmod-manifest.json", game / "mods" / "D2R-VR" / "vrmod-manifest.json")
    (game / "D2R_VR_Settings.exe").write_bytes(b"stand-in settings program")
modinfo("D2R-PCVR", "D2R VR Unleashed (fixture)")
for rel, data in {"d2rloader/plugins/d2rl-pcvr.dll": b"stand-in plugin", "d2rloader/plugins/d2r_pcvr.ini": b"[mode]\nplatform=1\n"}.items():
    p = game / "mods" / "D2R-PCVR" / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
shutil.copy2(a.pcvr_manifest or here / "examples" / "d2rloader-mods" / "d2r-pcvr" / "vrmod-manifest.json", game / "mods" / "D2R-PCVR" / "vrmod-manifest.json")
S0 = snap(game)
print(f"{len(S0)} files in the copy")

# ------------------------------------------------------------------ 2. what the launcher sees
print("\n=== 2. what the launcher sees")
r = run("mods", "--json", show=False)
doc = json.loads(r.stdout) if r.returncode == 0 else {}
mods = {m["id"]: m for m in doc.get("mods", [])}
check(r.returncode == 0 and set(mods) >= {"d2r-vr", "d2r-pcvr"}, "mods lists both VR mods")
check(mods.get("d2r-vr", {}).get("installed") and mods.get("d2r-pcvr", {}).get("installed"), "...both installed")
check(mods.get("d2r-vr", {}).get("name") == "D2R VR Authentic" and mods.get("d2r-pcvr", {}).get("name") == "D2R VR Unleashed", "...with their display names")
check(mods.get("d2r-vr", {}).get("source") == "manifest" and mods.get("d2r-pcvr", {}).get("source") == "manifest", "...read from their vrmod-manifest.json")
check(doc.get("store") == "steam" and doc.get("build"), f"...store {doc.get('store')}, build {doc.get('build')}")
check(doc.get("d2rloader", {}).get("present") and doc.get("d2rloader", {}).get("reviewed"), "D2RLoader is there and its files are the reviewed 1.3.1-beta ones")
run("mods")
check(snap(game) == S0, "looking changes nothing")

# ------------------------------------------------------------------ 3. starting each, with a stand-in D2RLoader.exe
print("\n=== 3. starting each mod (a stand-in D2RLoader.exe)")
shutil.copy2(fake, game / "D2RLoader.exe")
S1 = snap(game)
if a.authentic_setup:
    ini = game / "mods" / "D2R-VR" / "d2rloader" / "plugins" / "d2r_vr.ini"
    # D2R_VR_Settings.exe --start in a safe mode: native OpenXR with its own controls, no square window (that would write the real Saved Games\Settings.json)
    txt = ini.read_text(encoding="utf-8", errors="replace") if ini.exists() else ""
    ini.write_text(txt + "\n[mode]\nplatform=1\n[openxr]\non=1\ndirect_input=1\nsquare_window=0\n", encoding="utf-8")
    S1 = snap(game)

r = run("launch", "d2r-pcvr", "--runtime", "file", "--runtime-file", str(rt_file), "--dry-run")
check(r.returncode == 0 and "-mod" in r.stdout and "D2R-PCVR" in r.stdout, "launch d2r-pcvr --dry-run: the plan is D2RLoader.exe -mod D2R-PCVR")
(game / "fake_loader_started.txt").unlink(missing_ok=True)
r = run("launch", "d2r-pcvr", "--runtime", "file", "--runtime-file", str(rt_file))
s = started()
check(r.returncode == 0 and bool(s), "launch d2r-pcvr starts the game")
check(s.get("arg1") == "-mod" and s.get("arg2") == "D2R-PCVR", "...as D2RLoader.exe -mod D2R-PCVR")
check(s.get("SteamAppId") == "2536520" and s.get("SteamGameId") == "2536520" and s.get("SteamOverlayGameId", "") == "", "...with his Steam environment")
check(s.get("VRMOD_ACTIVE_ID") == "d2r-pcvr" and s.get("VRMOD_RUNTIME") == "file" and os.path.samefile(s.get("XR_RUNTIME_JSON", "?"), rt_file), "...the runtime of this start is XR_RUNTIME_JSON")
check(s.get("cwd", "").rstrip("\\").lower() == str(game).lower(), "...from the game folder")
check(snap(game) == S1, "the game folder is exactly as before the start")

(game / "fake_loader_started.txt").unlink(missing_ok=True)
if vdxr.exists():
    r = run("launch", "d2r-vr", "--runtime", "vdxr", "--dry-run")
    check(r.returncode == 0 and "virtualdesktop-openxr.json" in r.stdout, "launch d2r-vr --runtime vdxr --dry-run: Virtual Desktop's runtime for this start")
r = run("launch", "d2r-vr", "--runtime", "file", "--runtime-file", str(rt_file), "--dry-run")
check(r.returncode == 0 and "D2R_VR_Settings.exe" in r.stdout and "--start" in r.stdout, "launch d2r-vr --dry-run: the plan is D2R_VR_Settings.exe --start")
if a.authentic_setup:
    r = run("launch", "d2r-vr", "--runtime", "file", "--runtime-file", str(rt_file))
    s = started()
    check(r.returncode == 0 and bool(s), "launch d2r-vr starts the game through D2R_VR_Settings.exe --start")
    check(s.get("arg1") == "-mod" and s.get("arg2") == "D2R-VR", "...which runs D2RLoader.exe -mod D2R-VR")
    check(s.get("VRMOD_ACTIVE_ID") == "d2r-vr" and os.path.samefile(s.get("XR_RUNTIME_JSON", "?"), rt_file), "...with the launcher's environment")
    check(snap(game) == S1, "the game folder is exactly as before this start too")
S2 = snap(game)
for i in range(2):                                   # there and back again: the switch is only which -mod is started
    (game / "fake_loader_started.txt").unlink(missing_ok=True)
    run("launch", "d2r-pcvr", show=False)
    first = started().get("arg2")
    (game / "fake_loader_started.txt").unlink(missing_ok=True)
    if a.authentic_setup:
        run("launch", "d2r-vr", show=False)
        second = started().get("arg2")
    else:
        second = "D2R-VR"
    check(first == "D2R-PCVR" and second == "D2R-VR", f"round {i + 1}: -mod {first}, then -mod {second}")
check(snap(game) == S2, "after the round trips no file of the game folder (and of either mod) changed")

# ------------------------------------------------------------------ 4. refusals
print("\n=== 4. refusals")
fakegame = work / "fakegame" / "D2R.exe"
fakegame.parent.mkdir(parents=True)
shutil.copy2(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "PING.EXE", fakegame)
proc = subprocess.Popen([str(fakegame), "-n", "40", "127.0.0.1"], stdout=subprocess.DEVNULL)
time.sleep(0.7)
(game / "fake_loader_started.txt").unlink(missing_ok=True)
r = run("launch", "d2r-pcvr")
check(r.returncode == 10 and "E_GAME_RUNNING" in r.stderr and not (game / "fake_loader_started.txt").exists(), "a running D2R.exe stops the start (exit 10)")
proc.kill()
proc.wait()
r = run("launch", "d2r-pcvr", "--runtime", "steamvr", "--dry-run")
if "E_RUNTIME" in r.stderr:
    check(r.returncode == 24, "a runtime that is not installed stops the start (exit 24)")
shutil.rmtree(game / "mods" / "D2R-PCVR")
r = run("launch", "d2r-pcvr")
check(r.returncode == 11 and "E_MOD_NOT_INSTALLED" in r.stderr, "a mod that is not installed here is refused (exit 11)")
(game / "D2RLoader.exe").unlink()
r = run("launch", "d2r-vr")
check(r.returncode == 11 and "E_LOADER_MISSING" in r.stderr, "no D2RLoader.exe: refused (exit 11)")

# ------------------------------------------------------------------ 5. the window
if a.gui:
    print("\n=== 5. the window")
    shutil.copy2(real / "D2RLoader.exe", game / "D2RLoader.exe")
    modinfo("D2R-PCVR", "D2R VR Unleashed (fixture)")
    shutil.copy2(here / "examples" / "d2rloader-mods" / "d2r-pcvr" / "vrmod-manifest.json", game / "mods" / "D2R-PCVR" / "vrmod-manifest.json")
    gui = str(Path(a.gui).resolve())
    for mod, needle in (("d2r-vr", "D2R_VR_Settings.exe"), ("d2r-pcvr", "D2RLoader.exe")):
        r = subprocess.run([gui, "--game", str(game), "--home", str(home), "--select", mod, "--play", "--dry-run"], capture_output=True, text=True, timeout=60)
        out = r.stdout
        gl = home / "logs" / "gui_last.txt"
        if not out and gl.exists():
            out = gl.read_text(encoding="utf-8", errors="replace")
        check(r.returncode == 0 and needle in out, f"the window's Play (dry run) for {mod}: {needle}")
    png = work / "window.png"
    r = subprocess.run([gui, "--game", str(game), "--home", str(home), "--screenshot", str(png)], capture_output=True, text=True, timeout=60)
    check(r.returncode == 0 and png.exists() and png.stat().st_size > 5000, "the window renders a screenshot")

print(f"\n{'ALL PASSED' if not failures else str(len(failures)) + ' FAILED'}")
for f in failures:
    print("  FAIL:", f)
if not failures:
    shutil.rmtree(work, ignore_errors=True)
else:
    print("work folder kept:", work)
sys.exit(1 if failures else 0)
