#!/usr/bin/env python3
"""A test on a COPY of the real game's files (the real folder is only read).

    real_data_test.py <vrmodlauncher executable> <work folder> [real game folder] [kills]

What it does, on <work>/game (a partial copy of the real game folder: the exe, D2RLoader, ReShade, the plugin with its real ini,
the settings program, the shader, the ~60 MB of sky pictures, a part of the *.before_* backups) and <work>/home (the launcher's folder):

 1. makes the package of D2R VR from those very files (what its installer would leave) and a stand-in package for d2r-pcvr
    (the 25 sky pictures are the real ones, the rest is dummy: there is no build of his here);
 2. `requirements`: the D2RLoader hashes of the example manifest against the real files;
 3. `import d2r-vr` (the copy IS an old-style installation), `verify`;
 4. switch to d2r-pcvr and back, timed - the game copy must be byte for byte as before, the real ini must come back;
 5. the real process probe: a stand-in D2R.exe is started and the launcher must refuse;
 6. KILL tests: the launcher process is killed (TerminateProcess) at many moments of a switch; `recover` must bring the
    game copy back to exactly one of the two valid states, every time.
"""
import hashlib
import os
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path

exe = sys.argv[1]
work = Path(sys.argv[2])
real = Path(sys.argv[3]) if len(sys.argv) > 3 else Path(r"D:\SteamLibrary\steamapps\common\Diablo II Resurrected")
kills = int(sys.argv[4]) if len(sys.argv) > 4 else 30
here = Path(__file__).resolve().parent.parent
game, home = work / "game", work / "home"
if work.exists():
    shutil.rmtree(work)
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


def snap(root):
    return {str(p.relative_to(root)).replace("\\", "/"): sha(p) for p in sorted(root.rglob("*")) if p.is_file()}


def diff(a, b):
    out = [f"gone: {k}" for k in a if k not in b] + [f"new: {k}" for k in b if k not in a]
    out += [f"changed: {k}" for k in a if k in b and a[k] != b[k]]
    return out


def run(*args, show=True, env=None):
    cmd = [exe, "--home", str(home), "--game", str(game), *args]
    t = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if show:
        print(f"\n$ vrmodlauncher {' '.join(args)}   [{time.time() - t:.2f} s, exit {r.returncode}]")
        out = (r.stdout + r.stderr).rstrip()
        lines = out.splitlines()
        print("\n".join(lines[:14]) + (f"\n  ... ({len(lines) - 14} more lines)" if len(lines) > 14 else ""))
    return r


def cp(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


# ------------------------------------------------------------------ 0. a partial copy of the real game folder (read only)
print(f"copying from {real} (read only) to {game} ...")
for name in ["D2R.exe", "D2RLoader.exe", "D2RCore.dll", "d2rloader.mpq", "ReShade64.dll", "ReShade.ini", "ReShadePreset.ini",
             "D2R_VR_Settings.exe", "D2R_VR_LICENSE.txt", "FlatVR_DepthProvider.addon64", "FlatVR_Keepalive.addonfx", "steam_appid.txt"]:
    if (real / name).exists():
        cp(real / name, game / name)
for rel in ["data/.build.config", "d2rloader/config/d2rloader.toml", "d2rloader/logs/d2rloader.log"]:
    if (real / rel).exists():
        cp(real / rel, game / rel)
plug = real / "d2rloader" / "plugins"
for p in plug.iterdir():
    if p.is_file() and not p.name.startswith("uitex_") and p.suffix != ".csv" and p.stat().st_size < 8_000_000:
        cp(p, game / "d2rloader" / "plugins" / p.name)
if (plug / "d2r_vr_skill_icons").is_dir():
    shutil.copytree(plug / "d2r_vr_skill_icons", game / "d2rloader" / "plugins" / "d2r_vr_skill_icons")
for p in (real / "reshade-shaders" / "Shaders").glob("*"):
    if p.is_file() and p.suffix in (".fx", ".addonfx") and ".before" not in p.name and p.name != "D2R_DepthFog.fx.prev":
        cp(p, game / "reshade-shaders" / "Shaders" / p.name)
for p in (real / "reshade-shaders" / "Textures" / "D2R_Sky_ours").glob("*"):
    cp(p, game / "reshade-shaders" / "Textures" / "D2R_Sky_ours" / p.name)
plugdir = game / "d2rloader" / "plugins"
nfiles = sum(1 for _ in game.rglob("*") if _.is_file())
print(f"{nfiles} files, {sum(f.stat().st_size for f in game.rglob('*') if f.is_file()) / 1e6:.0f} MB; "
      f"backups copied: {len(list(plugdir.glob('*.before_*')))}")

# ------------------------------------------------------------------ 1. the packages
sky = game / "reshade-shaders" / "Textures" / "D2R_Sky_ours"
mods = home / "mods"
vr = mods / "d2r-vr"
vr.mkdir(parents=True)
shutil.copy2(here / "examples" / "d2r-vr" / "manifest.json", vr / "manifest.json")
pay = vr / "payload"
cp(plugdir / "d2rl-vrcam.dll", pay / "d2rloader" / "plugins" / "d2rl-vrcam.dll")
cp(game / "D2R_VR_Settings.exe", pay / "D2R_VR_Settings.exe")
cp(game / "D2R_VR_LICENSE.txt", pay / "D2R_VR_LICENSE.txt")
cp(game / "reshade-shaders" / "Shaders" / "D2R_DepthFog.fx", pay / "reshade-shaders" / "Shaders" / "D2R_DepthFog.fx")
for p in sky.glob("*"):
    cp(p, pay / ("sky_ours" if p.name.startswith("D2R_Ceiling") else "sky_common") / p.name)
cp(game / "FlatVR_DepthProvider.addon64", pay / "FlatVR_DepthProvider.addon64")
cp(game / "FlatVR_Keepalive.addonfx", pay / "FlatVR_Keepalive.addonfx")
cp(plugdir / "d2r_vr.ini", pay / "user" / "d2r_vr.ini")
cp(plugdir / "d2r_vr.default.ini", pay / "d2r_vr.default.ini")
cp(game / "ReShade.ini", pay / "user" / "ReShade.ini")
cp(game / "ReShadePreset.ini", pay / "user" / "ReShadePreset.ini")

pc = mods / "d2r-pcvr"
pc.mkdir(parents=True)
shutil.copy2(here / "examples" / "d2r-pcvr" / "manifest.json", pc / "manifest.json")
for rel, text in {"payload/d2rloader/plugins/d2rl-vrcam.dll": "PCVR PLUGIN (stand-in)", "payload/openxr_loader.dll": "OPENXR LOADER (stand-in)",
                  "payload/reshade-shaders/Shaders/D2R_DepthFog.fx": "// PCVR shader (stand-in)",
                  "payload/d2r_vr.recommended.ini": "[calibration]\nversion=1\n", "payload/ReShade.recommended.ini": "[GENERAL]\nPresetPath=.\\D2R_Frame_ReShade.ini\n",
                  "payload/D2R_Frame_ReShade.recommended.ini": "Techniques=D2R_DepthFog@D2R_DepthFog.fx\n",
                  "payload/user/d2r_vr.ini": "", "payload/user/ReShade.ini": "", "payload/user/D2R_Frame_ReShade.ini": ""}.items():
    (pc / rel).parent.mkdir(parents=True, exist_ok=True)
    (pc / rel).write_text(text, encoding="utf-8")
for p in sky.glob("*"):                       # his 25 pictures are byte for byte ours: use the real bytes
    if not p.name.startswith("D2R_Ceiling"):
        cp(p, pc / "payload" / "sky" / p.name)

print("\n=== 1. register")
check(run("register", str(vr / "manifest.json")).returncode == 0, "register d2r-vr")
check(run("register", str(pc / "manifest.json")).returncode == 0, "register d2r-pcvr")

# ------------------------------------------------------------------ 2. requirements against the real D2RLoader files
print("\n=== 2. the example manifest's D2RLoader hashes against the real files")
r = run("requirements", "d2r-vr")
check(r.returncode == 0 and "ok      d2rloader" in r.stdout, "D2RLoader.exe, D2RCore.dll and d2rloader.mpq have the hashes the manifest lists")
r = run("deps", "verify", str(real / "d2rloader.mpq"))
check(r.returncode == 23, "a file that is not the D2RLoader ZIP is refused by `deps verify` (exit 23)")

# ------------------------------------------------------------------ 3. import
print("\n=== 3. the copy is an old-style installation: verify warns, import takes it over")
S0 = snap(game)
r = run("verify")
check("UNMANAGED_MOD_FILES" in r.stdout and "d2r-vr" in r.stdout, "verify notices D2R VR's plugin without the launcher")
r = run("import", "d2r-vr")
check(r.returncode == 0 and "imported d2r-vr" in r.stdout, "import d2r-vr")
check(snap(game) == S0, "importing moved nothing in the game folder")
check(run("verify").returncode == 0, "verify is clean after the import")
backups = home / "games"
imported = list(backups.glob("*/imports/d2r-vr/backups/d2rloader/plugins/*.before_*"))
check(len(imported) == len(list(plugdir.glob("*.before_*"))) and imported, f"{len(imported)} *.before_* backups collected beside the launcher's own")
store_ini = next(iter(backups.glob("*/store/d2r-vr/d2rloader/plugins/d2r_vr.ini")), None)
check(store_ini is not None and sha(store_ini) == S0["d2rloader/plugins/d2r_vr.ini"], "the real d2r_vr.ini was captured, byte for byte")
run("status")

# ------------------------------------------------------------------ 4. the round trip
print("\n=== 4. d2r-vr -> d2r-pcvr -> d2r-vr")
r = run("activate", "d2r-pcvr", "--dry-run")
t = time.time()
r = run("activate", "d2r-pcvr")
check(r.returncode == 0, "switch to d2r-pcvr")
check((game / "openxr_loader.dll").exists() and not (game / "D2R_VR_Settings.exe").exists(), "his files are in, ours are out")
check(not (plugdir / "FlatVR_DepthProvider.addon64").exists() and not (game / "FlatVR_DepthProvider.addon64").exists(), "the FlatVR add-on is gone while his mod runs")
check((plugdir / "d2r_vr.ini").read_text() == "[calibration]\nversion=1\n", "his ini is his own defaults, not our tuned one")
unrelated = {k: v for k, v in S0.items() if k in ("D2R.exe", "D2RLoader.exe", "D2RCore.dll", "d2rloader.mpq", "ReShade64.dll", "d2rloader/config/d2rloader.toml", "data/.build.config")}
check(all(snap(game).get(k) == v for k, v in unrelated.items()), "D2R.exe, D2RLoader, ReShade64.dll, d2rloader.toml untouched")
check(run("verify").returncode == 0, "verify after the switch")
r = run("activate", "d2r-vr")
check(r.returncode == 0, "switch back to d2r-vr")
S1 = snap(game)
d = diff(S0, S1)
check(not d, "the game copy is byte for byte as before the round trip" + ("" if not d else f" ({len(d)} differences: {d[:5]})"))
check(run("verify").returncode == 0, "verify after the round trip")

# ------------------------------------------------------------------ 5. the real process probe
print("\n=== 5. a running D2R.exe: the launcher refuses")
fake = work / "fakegame" / "D2R.exe"
fake.parent.mkdir(parents=True)
shutil.copy2(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "PING.EXE", fake)
proc = subprocess.Popen([str(fake), "-n", "40", "127.0.0.1"], stdout=subprocess.DEVNULL)
time.sleep(0.7)
r = run("activate", "d2r-pcvr")
check(r.returncode == 10 and "E_GAME_RUNNING" in r.stderr, "activate refused while a process called D2R.exe runs (exit 10)")
check(snap(game) == S1, "and the game copy did not change")
proc.kill()
proc.wait()
time.sleep(0.3)
check(run("activate", "d2r-pcvr", "--dry-run", show=False).returncode == 0, "the dry run works again once it is gone")

# ------------------------------------------------------------------ 6. kill the launcher in the middle of a switch
print(f"\n=== 6. the launcher process is killed in the middle of a switch ({kills} times)")
random.seed(7)
S_vr = snap(game)
results = {"killed before anything changed": 0, "journal found, rolled back": 0, "journal found, rolled forward": 0, "finished before the kill": 0}
delays = sorted([0.0, 0.003, 0.006, 0.01, 0.02, 0.03] + [random.uniform(0.04, 0.95) for _ in range(kills - 6)])
S_pc = None
for i, delay in enumerate(delays):
    cmd = [exe, "--home", str(home), "--game", str(game), "activate", "d2r-pcvr"]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(delay)
    died = p.poll() is None
    p.kill()
    p.wait()
    journal = list(home.glob("games/*/journal.json"))
    r = run("recover", show=False)
    ok = r.returncode == 0 and not list(home.glob("games/*/journal.json"))
    now = snap(game)
    if now == S_vr:
        results["journal found, rolled back" if journal else "killed before anything changed"] += 1
        state = "d2r-vr"
    else:
        # completed (or rolled forward): must be a consistent d2r-pcvr
        v = run("verify", show=False)
        if v.returncode == 0 and (game / "openxr_loader.dll").exists():
            S_pc = S_pc or now
            ok = ok and now == S_pc
            results["journal found, rolled forward" if journal else "finished before the kill"] += 1
            state = "d2r-pcvr"
        else:
            ok = False
            state = "BROKEN"
    leftovers = [k for k in now if k.endswith(".vrml-tmp")]
    ok = ok and not leftovers
    if not ok:
        failures.append(f"kill #{i} (delay {delay:.3f} s): {state}, recover exit {r.returncode}, journal before: {bool(journal)}, tmp files: {leftovers[:3]}")
        print(f"  FAIL  kill #{i} at {delay:.3f} s: {state}")
    if state == "d2r-pcvr":                 # back to the start for the next round
        rr = run("activate", "d2r-vr", show=False)
        if rr.returncode != 0 or snap(game) != S_vr:
            failures.append(f"kill #{i}: could not return to the start state ({rr.stderr.strip()[:120]})")
            print(f"  FAIL  kill #{i}: the way back did not restore the start state")
            break
print(f"  {kills} kills: " + ", ".join(f"{v} x {k}" for k, v in results.items()))
check(snap(game) == S_vr, "after all the kills the game copy is exactly the start state")
check(run("verify", show=False).returncode == 0, "verify is clean at the end")

print(f"\n{'ALL PASSED' if not failures else str(len(failures)) + ' PROBLEM(S): ' + '; '.join(failures)}")
sys.exit(1 if failures else 0)
