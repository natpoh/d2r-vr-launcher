#!/usr/bin/env python3
"""End to end, on a COPY of the mod files of a real game folder (the real folder is only read, and the 80 GB of game data are not copied):
D2R VR and d2r-pcvr, installed or not, switched by the launcher and started through it.

    e2e_switch.py --launcher vrmodlauncher.exe --work <empty work folder> --vr-package <D2R_VR_launcher_package_vX> --fake-loader fake_loader.exe
                  [--real <game folder>] [--helper <his "D2R PCVR" folder>] [--settings-from <a backup of D2R VR's files>]

The starting point is the real one: d2r-pcvr (0.3.15) was installed over D2R VR by its own installer, and nothing knows about the launcher.

 1. the d2r-pcvr package is made from the copy and his own record (make_pcvr_package.py); both packages are registered;
 2. `verify` notices his plugin without the launcher; `import d2r-vr` is refused (the plugin there is his); `import d2r-pcvr` takes the install over
    without moving a file, saves his settings and collects his snapshot;
 3. D2R VR's own settings (a backup the player made, or his snapshot's `previous`) are saved for this game: `import-settings`;
 4. `activate d2r-vr`: his files go out (into the launcher, nothing is deleted), D2R VR's come in (the new build), with the player's own settings;
    what was in the way (an older D2R_VR_Settings.exe ...) is kept and given back later; D2RLoader, ReShade64.dll, d2rloader.toml and the rest are not touched;
 5. `launch d2r-vr` with a stand-in D2RLoader.exe: D2R_VR_Settings.exe --start runs headless, and the "game" gets XR_RUNTIME_JSON and VRMOD_*;
 6. settings are kept apart: an edit of D2R VR's ini is not in his, and survives a round trip;
 7. `activate d2r-pcvr`: the copy is byte for byte what it was at the start; `launch d2r-pcvr` gives the "game" his Steam environment;
 8. at every step: every file of the starting point is still somewhere (the game copy or the launcher's folder), and the unrelated files never changed.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("--launcher", required=True)
ap.add_argument("--work", required=True)
ap.add_argument("--vr-package", required=True)
ap.add_argument("--fake-loader", required=True)
ap.add_argument("--real", default=r"D:\SteamLibrary\steamapps\common\Diablo II Resurrected")
ap.add_argument("--helper", default=os.path.expandvars(r"%USERPROFILE%\AppData\Local\Programs\D2R PCVR"))
ap.add_argument("--settings-from", default=r"D:\d2r_backup_before_pcvr_0315")
a = ap.parse_args()
exe, work, real = str(Path(a.launcher).resolve()), Path(a.work).resolve(), Path(a.real)
a.vr_package, a.fake_loader = str(Path(a.vr_package).resolve()), str(Path(a.fake_loader).resolve())
here = Path(__file__).resolve().parent
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


def snap(root, skip=()):
    return {str(p.relative_to(root)).replace("\\", "/"): sha(p) for p in sorted(root.rglob("*")) if p.is_file() and str(p.relative_to(root)).replace("\\", "/") not in skip}


def diff(x, y):
    out = [f"gone: {k}" for k in x if k not in y] + [f"new: {k}" for k in y if k not in x]
    return out + [f"changed: {k}" for k in x if k in y and x[k] != y[k]]


def run(*args, show=True, env=None):
    cmd = [exe, "--home", str(home), "--game", str(game), *args]
    t = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if show:
        print(f"\n$ vrmodlauncher {' '.join(args)}   [{time.time() - t:.2f} s, exit {r.returncode}]")
        lines = (r.stdout + r.stderr).rstrip().splitlines()
        print("\n".join(lines[:16]) + (f"\n  ... ({len(lines) - 16} more lines)" if len(lines) > 16 else ""))
    return r


def cp(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def ini_get(path, section, key):
    sec = ""
    for ln in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        ln = ln.strip()
        m = re.match(r"\[(.+)\]$", ln)
        if m:
            sec = m.group(1).lower()
        elif "=" in ln and sec == section and ln.split("=", 1)[0].strip().lower() == key:
            return ln.split("=", 1)[1].strip()
    return None


def ini_set(path, section, key, value):
    lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines(keepends=True)
    sec, out, done = "", [], False
    for ln in lines:
        m = re.match(r"\s*\[(.+)\]\s*$", ln)
        if m:
            if sec == section and not done:
                out.append(f"{key}={value}\n")
                done = True
            sec = m.group(1).lower()
        elif "=" in ln and sec == section and ln.split("=", 1)[0].strip().lower() == key:
            out.append(f"{key}={value}\n")
            done = True
            continue
        out.append(ln)
    if not done:
        out.append(f"\n[{section}]\n{key}={value}\n")
    Path(path).write_text("".join(out), encoding="utf-8", newline="")


def same_path(x, y):
    try:
        return bool(x) and os.path.samefile(x, y)
    except OSError:
        return False


def tasklist(name):
    r = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    return name.lower() in r.lower()


if tasklist("D2R.exe") or tasklist("D2RLoader.exe"):
    sys.exit("The game (D2R.exe / D2RLoader.exe) is running: close it first (the launcher would refuse, and so should this test).")
if work.exists():
    shutil.rmtree(work)
plug = game / "d2rloader" / "plugins"
GENERATED = {"d2rloader/logs/d2r_vr_launch.txt", "fake_loader_started.txt"}   # what the stand-in "game" and --start write
home_files = lambda: [p for p in (home / "games").rglob("*") if p.is_file()]


def nothing_lost(S0, label):
    """Every file of the starting point (by content) is in the game copy or in the launcher's folders (backups, foreign, store, imports)."""
    have = set(snap(game, GENERATED).values()) | {sha(p) for p in home_files()}
    gone = [k for k, v in S0.items() if v not in have]
    check(not gone, f"{label}: nothing lost - every byte of the starting point is in the game copy or the launcher's folder" + ("" if not gone else f" (missing: {gone[:4]})"))


# ------------------------------------------------------------------ 0. the mod files of the real folder, copied (read only)
print(f"copying the mod files of {real} (read only) to {game} ...")
for name in ["D2R.exe", "D2RLoader.exe", "D2RCore.dll", "d2rloader.mpq", "ReShade64.dll", "ReShade.ini", "ReShadePreset.ini", "D2R_Frame_ReShade.ini",
             "openxr_loader.dll", "D2R_VR_Settings.exe", "D2R_VR_LICENSE.txt", "FlatVR_DepthProvider.addon64", "FlatVR_Keepalive.addonfx", "steam_appid.txt"]:
    if (real / name).exists():
        cp(real / name, game / name)
for rel in ["data/.build.config", "d2rloader/pcvr-install.json"]:
    if (real / rel).exists():
        cp(real / rel, game / rel)
for p in (real / "d2rloader" / "config").glob("*"):
    if p.is_file():
        cp(p, game / "d2rloader" / "config" / p.name)
for p in (real / "d2rloader" / "plugins").iterdir():
    if p.is_file() and not p.name.startswith("uitex_") and p.suffix != ".csv" and p.stat().st_size < 12_000_000:
        cp(p, plug / p.name)
for d in ["d2r_vr_icons", "d2r_vr_potions", "d2r_vr_skill_icons"]:
    if (real / "d2rloader" / "plugins" / d).is_dir():
        shutil.copytree(real / "d2rloader" / "plugins" / d, plug / d)
for p in (real / "reshade-shaders" / "Shaders").glob("*"):
    if p.is_file() and ".before" not in p.name and not p.name.endswith(".prev"):
        cp(p, game / "reshade-shaders" / "Shaders" / p.name)
for sub in ["reshade-shaders/Textures/D2R_Sky_ours", "D2R_Sky_ours"]:
    for p in (real / sub).glob("*"):
        cp(p, game / sub / p.name)
S0 = snap(game)
print(f"{len(S0)} files, {sum(f.stat().st_size for f in game.rglob('*') if f.is_file()) / 1e6:.0f} MB")
bear_dll = sha(plug / "d2rl-vrcam.dll")
check((game / "d2rloader" / "pcvr-install.json").exists(), "the copy holds d2r-pcvr's installation record (the real folder has his install now)")
UNRELATED = ["D2R.exe", "D2RCore.dll", "d2rloader.mpq", "ReShade64.dll", "d2rloader/config/d2rloader.toml", "data/.build.config", "d2rloader/pcvr-install.json", "steam_appid.txt"]
UNRELATED = [k for k in UNRELATED if k in S0]
D2RLOADER_REAL = sha(game / "D2RLoader.exe")

# ------------------------------------------------------------------ 1. the packages
print("\n=== 1. the two packages")
pcvr_pkg = work / "pkgs" / "d2r-pcvr"
r = subprocess.run([sys.executable, "-I", str(here / "make_pcvr_package.py"), "--game", str(game), "--out", str(pcvr_pkg), "--helper", a.helper], capture_output=True, text=True)
print(r.stdout.strip(), r.stderr.strip())
check(r.returncode == 0, "d2r-pcvr's package made from the copy and his own record")
mods = home / "mods"
shutil.copytree(a.vr_package, mods / "d2r-vr")
shutil.copytree(pcvr_pkg, mods / "d2r-pcvr")
check(run("register", str(mods / "d2r-vr" / "manifest.json")).returncode == 0, "register d2r-vr (the package of this build)")
check(run("register", str(mods / "d2r-pcvr" / "manifest.json")).returncode == 0, "register d2r-pcvr")
vr_manifest = json.loads((mods / "d2r-vr" / "manifest.json").read_text(encoding="utf-8"))
vr_dll = next(f["sha256"] for f in vr_manifest["files"] if f["dst"].endswith("d2rl-vrcam.dll"))
check(vr_dll != bear_dll, "the two plugins are different files with the same name")
r = run("requirements", "d2r-vr")
check(r.returncode == 0 and "ok      d2rloader" in r.stdout, "the copy's D2RLoader files are the ones d2r-vr requires")
r = run("requirements", "d2r-pcvr")
check(r.returncode == 0 and "ok      d2rloader" in r.stdout, "... and the ones d2r-pcvr requires")

# ------------------------------------------------------------------ 2. what is there, and taking it over
print("\n=== 2. his install, found and taken over")
r = run("verify")
check("UNMANAGED_MOD_FILES" in r.stdout and "d2r-pcvr" in r.stdout, "verify notices his plugin without the launcher")
r = run("import", "d2r-vr")
check(r.returncode == 12 and "E_IDENTITY" in r.stderr, "import d2r-vr is refused: the plugin there is not D2R VR's (exit 12)")
check(snap(game) == S0, "and nothing changed")
r = run("import", "d2r-pcvr")
check(r.returncode == 0 and "imported d2r-pcvr" in r.stdout, "import d2r-pcvr takes the install over")
check(snap(game) == S0, "importing moved nothing")
gdir = next((home / "games").iterdir())
check(sha(gdir / "store" / "d2r-pcvr" / "d2rloader" / "plugins" / "d2r_vr.ini") == S0["d2rloader/plugins/d2r_vr.ini"], "his d2r_vr.ini was saved, byte for byte")
snapdir = gdir / "imports" / "d2r-pcvr" / "record" / "0"
check((snapdir / "manifest.json").exists() and (snapdir / "previous").is_dir(), "his snapshot (setup-backups\\game\\<stamp>) was collected into the launcher's folder")
check(run("verify").returncode == 0, "verify is clean")

# ------------------------------------------------------------------ 3. D2R VR's own settings
print("\n=== 3. D2R VR's own settings, saved for this game")
src = Path(a.settings_from)
if not (src / "d2rloader" / "plugins" / "d2r_vr.ini").exists():
    print(f"  (no backup at {src}: his snapshot's `previous` - what his installer replaced - is used)")
    src = snapdir / "previous"
r = run("import-settings", "d2r-vr", "--from", str(src))
check(r.returncode == 0 and "imported settings of d2r-vr" in r.stdout, f"import-settings d2r-vr --from {src}")
mine_ini = sha(src / "d2rloader" / "plugins" / "d2r_vr.ini")
check(sha(gdir / "store" / "d2r-vr" / "d2rloader" / "plugins" / "d2r_vr.ini") == mine_ini, "the player's own d2r_vr.ini is saved for d2r-vr")
check(snap(game) == S0, "the game copy did not change")

# ------------------------------------------------------------------ 4. d2r-pcvr -> d2r-vr
print("\n=== 4. activate d2r-vr")
r = run("activate", "d2r-vr", "--dry-run")
check(r.returncode == 0, "the plan (dry run)")
check(snap(game) == S0, "a dry run changes nothing")
t = time.time()
r = run("activate", "d2r-vr")
print(f"  ({time.time() - t:.1f} s)")
check(r.returncode == 0, "activate d2r-vr")
check(sha(plug / "d2rl-vrcam.dll") == vr_dll, "D2R VR's plugin (this build) is in")
check(not (game / "openxr_loader.dll").exists(), "his openxr_loader.dll is out")
check(sha(plug / "d2r_vr.ini") == mine_ini, "the player's own d2r_vr.ini is there (not the seed, not his)")
check(sha(game / "reshade-shaders/Shaders/D2R_DepthFog.fx") != S0["reshade-shaders/Shaders/D2R_DepthFog.fx"], "D2R VR's shader replaced his")
check(not (game / "D2R_Frame_ReShade.ini").exists(), "his D2R_Frame_ReShade.ini is out (kept in the launcher)")
check(sha(game / "D2R_VR_Settings.exe") != S0["D2R_VR_Settings.exe"], "D2R VR Settings.exe is the package's (the older copy that was there is kept)")
check(all(snap(game).get(k) == S0[k] for k in UNRELATED), "D2R.exe, D2RCore.dll, d2rloader.mpq, ReShade64.dll, d2rloader.toml, his install record: untouched")
check(len(list((game / "d2rloader/plugins/d2r_vr_icons").glob("*.png"))) == len(list((real / "d2rloader/plugins/d2r_vr_icons").glob("*.png"))), "his cache of icons (generated) is untouched")
nothing_lost(S0, "after the switch to d2r-vr")
check(run("verify").returncode == 0, "verify after the switch")
S_vr = snap(game)

# ------------------------------------------------------------------ 5. start D2R VR through the launcher (a stand-in D2RLoader.exe)
print("\n=== 5. launch d2r-vr")
runtime = work / "fake_runtime" / "runtime.json"
runtime.parent.mkdir(parents=True)
runtime.write_text('{"file_format_version":"1.0.0","runtime":{"library_path":"x.dll"}}', encoding="utf-8")
r = run("launch", "d2r-vr", "--runtime", "file", "--runtime-file", str(runtime), "--dry-run")
check(r.returncode == 0 and "D2R_VR_Settings.exe" in r.stdout and "--start" in r.stdout, "the plan: D2R_VR_Settings.exe --start, with the runtime of this start")
# the game copy's D2RLoader.exe becomes a stand-in for this one step, and the ini cannot touch the real Saved Games\\Settings.json (square_window)
ini_bytes = (plug / "d2r_vr.ini").read_bytes()
shutil.copy2(a.fake_loader, game / "D2RLoader.exe")
for sec, key, val in [("mode", "platform", "1"), ("openxr", "on", "1"), ("openxr", "direct_input", "1"), ("openxr", "square_window", "0")]:
    ini_set(plug / "d2r_vr.ini", sec, key, val)
check(ini_get(plug / "d2r_vr.ini", "openxr", "direct_input") == "1" and ini_get(plug / "d2r_vr.ini", "openxr", "square_window") == "0",
      "(the copy's ini is set to native OpenXR with its own controls and no square window: no BodyWalk, no write to the real Settings.json)")
r = run("launch", "d2r-vr", "--runtime", "file", "--runtime-file", str(runtime))
check(r.returncode == 0, "launch d2r-vr")
started = {}
t0 = time.time()
while time.time() - t0 < 15 and not started:
    f = game / "fake_loader_started.txt"
    if f.exists() and f.stat().st_size > 0:
        time.sleep(0.2)
        started = dict(l.split("=", 1) for l in f.read_text(encoding="utf-8").splitlines() if "=" in l)
    else:
        time.sleep(0.1)
check(bool(started), "the stand-in game was started")
check(same_path(started.get("XR_RUNTIME_JSON", ""), runtime), "...with the runtime of this start as XR_RUNTIME_JSON")
check(started.get("VRMOD_ACTIVE_ID") == "d2r-vr" and started.get("VRMOD_ACTIVE_VERSION") == vr_manifest["version"], "...and VRMOD_ACTIVE_ID / VERSION")
check(started.get("VRMOD_STORE") == "steam", "...and VRMOD_STORE")
check(started.get("cwd", "").rstrip("\\").lower() == str(game).lower(), "...from the game folder")
log = (game / "d2rloader/logs/d2r_vr_launch.txt").read_text(encoding="utf-8") if (game / "d2rloader/logs/d2r_vr_launch.txt").exists() else ""
check("D2RLoader.exe started (VR)" in log and "by the VR mod launcher as d2r-vr" in log, "D2R_VR_Settings.exe --start logged who started it")
time.sleep(0.5)
shutil.copy2(real / "D2RLoader.exe", game / "D2RLoader.exe")
(plug / "d2r_vr.ini").write_bytes(ini_bytes)
for f in GENERATED:
    (game / f).unlink(missing_ok=True)
check(sha(game / "D2RLoader.exe") == D2RLOADER_REAL, "(the real D2RLoader.exe is back in the copy)")
check(snap(game) == S_vr, "the copy is as it was after the switch")

# ------------------------------------------------------------------ 6. settings kept apart
print("\n=== 6. D2R VR's settings are its own")
with open(plug / "d2r_vr.ini", "ab") as f:
    f.write(b"\r\n; edited while D2R VR was active\r\n")
edited = sha(plug / "d2r_vr.ini")
r = run("activate", "d2r-pcvr")
check(r.returncode == 0, "activate d2r-pcvr")
check(sha(plug / "d2rl-vrcam.dll") == bear_dll, "his plugin is back")
check(sha(plug / "d2r_vr.ini") == S0["d2rloader/plugins/d2r_vr.ini"], "his d2r_vr.ini is back, byte for byte - without the edit made in D2R VR's")
S_back = snap(game)
d = diff(S0, S_back)
check(not d, "the game copy is byte for byte as at the start" + ("" if not d else f" ({len(d)} differences: {d[:6]})"))
check(run("verify").returncode == 0, "verify after the switch back")
nothing_lost(S0, "after the switch back")
r = run("activate", "d2r-vr")
check(r.returncode == 0 and sha(plug / "d2r_vr.ini") == edited, "and D2R VR's edited ini returns when D2R VR does")
check(snap(game) != S0 and sha(plug / "d2rl-vrcam.dll") == vr_dll, "D2R VR is active again")
# the other order: his settings edited while he is active do not reach D2R VR
r = run("activate", "d2r-pcvr")
with open(plug / "d2r_vr.ini", "ab") as f:
    f.write(b"\r\n; edited while d2r-pcvr was active\r\n")
his_edited = sha(plug / "d2r_vr.ini")
run("activate", "d2r-vr", show=False)
check(sha(plug / "d2r_vr.ini") == edited, "his edit is not in D2R VR's ini")
run("activate", "d2r-pcvr", show=False)
check(sha(plug / "d2r_vr.ini") == his_edited, "and D2R VR's is not in his")

# ------------------------------------------------------------------ 7. launch d2r-pcvr (his options: Steam environment, runtime)
print("\n=== 7. launch d2r-pcvr")
shutil.copy2(a.fake_loader, game / "D2RLoader.exe")
vdxr = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Virtual Desktop Streamer" / "OpenXR" / "virtualdesktop-openxr.json"
if not vdxr.exists():
    print(f"  (no {vdxr} here: the runtime file of this test is used for d2r-pcvr as well, via a copy of his manifest that offers `file`)")
rt_args = ["--runtime", "vdxr"] if vdxr.exists() else ["--runtime", "steamvr"]
r = run("launch", "d2r-pcvr", *rt_args, "--dry-run")
check(r.returncode == 0, "the plan for d2r-pcvr")
(game / "fake_loader_started.txt").unlink(missing_ok=True)
r = run("launch", "d2r-pcvr", *rt_args)
started = {}
t0 = time.time()
while time.time() - t0 < 20 and not started:
    f = game / "fake_loader_started.txt"
    if f.exists() and f.stat().st_size > 0:
        time.sleep(0.2)
        started = dict(l.split("=", 1) for l in f.read_text(encoding="utf-8").splitlines() if "=" in l)
    else:
        time.sleep(0.1)
check(r.returncode == 0 and bool(started), "launch d2r-pcvr starts the stand-in game")
check(started.get("VRMOD_ACTIVE_ID") == "d2r-pcvr", "...as d2r-pcvr")
check(started.get("SteamAppId") == "2536520" and started.get("SteamGameId") == "2536520" and started.get("SteamOverlayGameId", "") == "",
      "...with his Steam environment rules (SteamAppId/SteamGameId 2536520, no overlay id)")
check(bool(started.get("XR_RUNTIME_JSON")) and started.get("VRMOD_RUNTIME") == rt_args[1], "...and the runtime of this start (" + rt_args[1] + ")")
time.sleep(0.5)
shutil.copy2(real / "D2RLoader.exe", game / "D2RLoader.exe")
for f in GENERATED:
    (game / f).unlink(missing_ok=True)

# ------------------------------------------------------------------ 8. a running game
print("\n=== 8. a running D2R.exe")
fake = work / "fakegame" / "D2R.exe"
fake.parent.mkdir(parents=True)
shutil.copy2(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "PING.EXE", fake)
S_now = snap(game)
proc = subprocess.Popen([str(fake), "-n", "40", "127.0.0.1"], stdout=subprocess.DEVNULL)
time.sleep(0.7)
r = run("activate", "d2r-vr")
check(r.returncode == 10 and "E_GAME_RUNNING" in r.stderr, "activate is refused while D2R.exe runs (exit 10)")
check(snap(game) == S_now, "and the copy did not change")
proc.kill()
proc.wait()

print(f"\n{'ALL PASSED' if not failures else str(len(failures)) + ' FAILED'}")
for f in failures:
    print("  FAIL:", f)
sys.exit(1 if failures else 0)
