#!/usr/bin/env python3
r"""Runs the launcher's own Setup (installer/D2R_VR_Launcher_Setup.iss) and its uninstaller for real, silently, in a scratch folder.

    python installer/tests/launcher_setup_test.py <folder with D2RVRLauncher.exe and vrmodlauncher.exe> <scratch folder> [ISCC.exe]

It is a per-user installer: the test checks that it needs no elevation, puts the files and the Start menu shortcut and the HKCU value where they belong, can be
run again over itself, and that the uninstaller takes all of it away. The desktop shortcut (a task) is not asked for here, to leave the real desktop alone.
Nothing touches a game folder, a mod or %LOCALAPPDATA%\VRModLauncher.
"""
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

build = Path(sys.argv[1]).resolve()
work = Path(sys.argv[2]).resolve()
iscc = sys.argv[3] if len(sys.argv) > 3 else r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
here = Path(__file__).resolve().parent.parent
failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def reg_value(name):
    r = subprocess.run(["reg", "query", r"HKCU\Software\D2R VR Launcher", "/v", name], capture_output=True, text=True)
    if r.returncode != 0:
        return None
    for line in r.stdout.splitlines():
        if name in line:
            return line.split("REG_SZ", 1)[1].strip()
    return None


def wait_log_closed(log, secs=90):
    t0 = time.time()
    while time.time() - t0 < secs:
        if log.exists() and "Log closed." in log.read_text(encoding="utf-8", errors="replace"):
            return True
        time.sleep(0.2)
    return False


if work.exists():
    shutil.rmtree(work)
work.mkdir(parents=True)
app, out = work / "app", work / "out"
shortcut = Path(os.environ["APPDATA"]) / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "D2R VR Launcher.lnk"

r = subprocess.run([iscc, "/Q", f"/DBuildDir={build}", "/DLauncherVersion=9.8.7", f"/O{out}", str(here / "D2R_VR_Launcher_Setup.iss")], capture_output=True, text=True)
print((r.stdout + r.stderr).strip()[-800:])
setup = out / "D2R_VR_Launcher_Setup_9.8.7.exe"
check(r.returncode == 0 and setup.exists(), "Setup compiles")
if not setup.exists():
    sys.exit(1)

print("\n=== the finish page of the script")
iss = (here / "D2R_VR_Launcher_Setup.iss").read_text(encoding="utf-8")
runs = [l for l in iss.splitlines() if l.startswith("Filename:") and "D2RVRLauncher.exe" in l]
choose = [l for l in runs if "--install-view" in l]
plain = [l for l in runs if "--install-view" not in l]
check(len(choose) == 1 and 'Parameters: "--install-view"' in choose[0] and 'Description: "Choose the mods to install"' in choose[0], "the finish page has 'Choose the mods to install' (D2RVRLauncher.exe --install-view)")
check(len(choose) == 1 and all(f in choose[0].split("Flags:")[1].split() for f in ("postinstall", "nowait", "skipifsilent")) and "unchecked" not in choose[0].split("Flags:")[1], "...postinstall nowait skipifsilent, ticked by default")
check(len(plain) == 1 and 'Description: "Open D2R VR Launcher"' in plain[0] and "skipifsilent" in plain[0] and "postinstall" in plain[0], "'Open D2R VR Launcher' is still there next to it (unticked, so that one launcher opens by default)")

print("\n=== install")
log = work / "install.log"
r = subprocess.run([str(setup), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/TASKS=", f"/LOG={log}", f"/DIR={app}"], capture_output=True, text=True, timeout=300)
text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
check(r.returncode == 0, f"Setup runs (exit {r.returncode})")
check("administrative install mode: no" in text.lower() and "hkey_current_user" in text.lower(), "...without administrator rights (no UAC prompt): per-user install mode, HKCU")
check((app / "D2RVRLauncher.exe").is_file() and (app / "vrmodlauncher.exe").is_file(), "...the two programs are in")
check((app / "LICENSE.txt").is_file(), "...and the licence")
check(shortcut.exists(), "...the Start menu shortcut is there")
check(reg_value("InstallDir") is not None and Path(reg_value("InstallDir")).resolve() == app, "...HKCU/Software/D2R VR Launcher/InstallDir names the folder")
check(reg_value("Version") == "9.8.7", "...and Version")
started = subprocess.run(["powershell", "-NoProfile", "-Command",
                          f"@(Get-Process -Name D2RVRLauncher -ErrorAction SilentlyContinue | Where-Object {{ $_.Path -like '{app}\\*' }}).Count"], capture_output=True, text=True).stdout.strip()
check(started == "0", f"...a silent run starts no launcher (skipifsilent; {started} running from the install folder)")
files_before = sorted(p.relative_to(app).as_posix() for p in app.rglob("*") if p.is_file())

print("\n=== over itself")
log2 = work / "install2.log"
r = subprocess.run([str(setup), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/TASKS=", f"/LOG={log2}", f"/DIR={app}"], capture_output=True, text=True, timeout=300)
files_after = sorted(p.relative_to(app).as_posix() for p in app.rglob("*") if p.is_file())
check(r.returncode == 0 and files_after == files_before, "a second run changes nothing but replaces the files")

print("\n=== a silent run of an older / newer Setup (the installers that carry it run it so)")


def compile_version(v):
    r = subprocess.run([iscc, "/Q", f"/DBuildDir={build}", f"/DLauncherVersion={v}", f"/O{out}", str(here / "D2R_VR_Launcher_Setup.iss")], capture_output=True, text=True)
    return out / f"D2R_VR_Launcher_Setup_{v}.exe" if r.returncode == 0 else None


older, newer = compile_version("9.8.6"), compile_version("9.9.0")
check(older is not None and newer is not None, "an older and a newer Setup compile")
before = {p.relative_to(app).as_posix(): p.stat().st_size for p in app.rglob("*") if p.is_file()}
r = subprocess.run([str(older), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/TASKS=", f"/LOG={work / 'older.log'}", f"/DIR={app}"], capture_output=True, text=True, timeout=300)
check(r.returncode != 0 and reg_value("Version") == "9.8.7", f"a silent 9.8.6 does not downgrade 9.8.7 (exit {r.returncode}, Version {reg_value('Version')})")
after = {p.relative_to(app).as_posix(): p.stat().st_size for p in app.rglob("*") if p.is_file()}
check(after == before, "...and changes no file")
r = subprocess.run([str(newer), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/TASKS=", f"/LOG={work / 'newer.log'}", f"/DIR={app}"], capture_output=True, text=True, timeout=300)
check(r.returncode == 0 and reg_value("Version") == "9.9.0", f"a silent 9.9.0 upgrades (exit {r.returncode}, Version {reg_value('Version')})")

print("\n=== uninstall")
ulog = work / "uninstall.log"
r = subprocess.run([str(app / "unins000.exe"), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", f"/LOG={ulog}"], capture_output=True, text=True, timeout=300)
check(wait_log_closed(ulog), "the uninstaller finishes")
left = [p.relative_to(app).as_posix() for p in app.rglob("*") if p.is_file()] if app.exists() else []
check(not left, f"the program folder is empty ({left})")
check(not shortcut.exists(), "the Start menu shortcut is gone")
check(reg_value("InstallDir") is None, "the registry value is gone")
utext = ulog.read_text(encoding="utf-8", errors="replace") if ulog.exists() else ""
check("VRModLauncher" not in utext, "the launcher's own data folder was not touched")

print(f"\n{'ALL PASSED' if not failures else str(len(failures)) + ' FAILED'}")
for f in failures:
    print("  FAIL:", f)
if not failures:
    shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if failures else 0)
