#!/usr/bin/env python3
r"""The install view and the update badges of D2RVRLauncher.exe, for real, against a LOCAL web server and fake game folders: no internet, no real game, no
real installer, nothing in %LOCALAPPDATA%\VRModLauncher (every run gets --home <scratch>).

    install_view_smoke.py --exe D2RVRLauncher.exe --fake-setup fake_setup.exe --work <empty work folder> [--shots <folder for the screenshots>]

One Python http.server on 127.0.0.1 plays all the sources (a synthetic D2RLoader ZIP passed to the launcher as a test pin, our version API with and without a
sha256, a GitHub-style releases list, installers that are copies of fake_setup.exe, a slow one for the cancel tests). The launcher is told about the server by the
test switches VRML_ALLOW_HTTP_HOST / VRML_URL_* / --pin-file, which it honours only together.

What is checked
  0. the launcher's version: src/util.h, the exe's version resource and `vrmodlauncher version` agree (0.3.1)
  1. `--install-run` (what the view's Install does, with no window): every piece, the order, the E_UNVERIFIED refusal and --accept-unverified, a wrong hash,
     a cancel half way through a slow download, a running game, a Setup that fails / is cancelled / installs elsewhere, a folder that is not a game, and that the
     test aids refuse to run without the loopback switch
  2. the install view, driven with window messages: what is ticked and why, the Tab order, the question before a download without a published checksum (and
     its default button, No), a real install, a cancel in the middle of a download, the refusal while the game runs and without a game folder
  3. the main window: the update badge and the Install / Update button of a card, the quiet check (off by --no-update-check and by the setting, silent when
     the server is down), the "Check for updates when the launcher starts" box, the offer of the install view on a first run
  4. screenshots of the view in its states (checking, ready, after an install with a failure) at 100 and 150 % and the main window with badges
"""
import argparse
import hashlib
import importlib.util
import io
import json
import os
import random
import shutil
import subprocess
import sys
import threading
import time
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("--exe", required=True)
ap.add_argument("--fake-setup", required=True)
ap.add_argument("--work", required=True)
ap.add_argument("--shots")
a = ap.parse_args()
EXE = str(Path(a.exe).resolve())
FAKE_SETUP = Path(a.fake_setup).resolve()
WORK = Path(a.work).resolve()
if len(str(WORK)) > 80:   # the loader install writes deep paths inside the game folder: a deep build folder would pass MAX_PATH
    import tempfile
    WORK = Path(tempfile.gettempdir()).resolve() / "vrml_install_view_smoke"
HERE = Path(__file__).resolve().parent.parent
SHOTS = Path(a.shots).resolve() if a.shots else WORK / "shots"
failures = []

# the helpers of the window test of the main window (Png, the Win32 plumbing)
_spec = importlib.util.spec_from_file_location("ui_smoke", HERE / "tools" / "ui_smoke.py")
U = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(U)
import ctypes
from ctypes import wintypes

user32 = U.user32
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
user32.EnumChildWindows.argtypes = [wintypes.HWND, U.WNDENUMPROC, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.IsWindow.argtypes = [wintypes.HWND]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.GetDlgCtrlID.argtypes = [wintypes.HWND]
user32.GetNextDlgTabItem.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.BOOL]
user32.GetNextDlgTabItem.restype = wintypes.HWND
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageW.restype = wintypes.LPARAM
BM_CLICK, BM_GETCHECK, BM_SETCHECK, WM_CLOSE, DM_GETDEFID, PBM_GETPOS, WM_GETTEXT, WM_GETTEXTLENGTH = 0xF5, 0xF0, 0xF1, 0x10, 0x400, 0x408, 0x0D, 0x0E
IDYES, IDNO = 6, 7

# the control ids of gui_install_view.cpp and gui_window.cpp
V_ROWS, V_NOTICE, V_STEP, V_PROGRESS, V_LOG, V_CHECK, V_INSTALL, V_CLOSE, V_BOX = 210, 211, 212, 213, 214, 215, 216, 2, 1100       # V_BOX + 0 loader, 1 Authentic, 2 Unleashed
M_LIST, M_INSTALL_VIEW, M_UPDATE_CHECK, M_MESSAGE, M_LOG = 105, 119, 120, 117, 118
CARD_RADIO, CARD_LINK, CARD_INSTALL = 1000, 2000, 3000
VIEW_CLASS, MAIN_CLASS = "D2RVRLauncherInstallView", "D2RVRLauncherWindow"


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def sha_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def snap(root):
    return {p.relative_to(root).as_posix(): sha_file(p) for p in sorted(root.rglob("*")) if p.is_file()} if root.exists() else {}


# ------------------------------------------------------------------ the server

LOCK = threading.Lock()
HITS = []
STATIC = {}
ROUTES = {}
EVENTS = {}
STATE = {"api_delay": 0.0}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, code, body, headers=None, ctype="application/octet-stream"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        with LOCK:
            HITS.append(path)
        try:
            if path in ROUTES:
                ROUTES[path](self)
            elif path in STATIC:
                self.reply(200, STATIC[path])
            else:
                self.reply(404, b"not found")
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
            EVENTS["client_gone"] = path
        self.close_connection = True


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
PORT = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()
BASE = f"http://127.0.0.1:{PORT}"


def hits(path):
    with LOCK:
        return HITS.count(path)


def api_hits():
    return hits("/api/version") + hits("/gh/releases")


# ------------------------------------------------------------------ what the server serves

rng = random.Random(20261018)
REAL = {"D2RLoader.exe": rng.randbytes(40000), "D2RCore.dll": rng.randbytes(30000), "d2rloader.mpq": rng.randbytes(120000)}


def loader_zip(files=None):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(zipfile.ZipInfo("mods/"), b"")
        for n, d in (files or REAL).items():
            z.writestr(n, d)
        z.writestr("d2rloader/config/d2rloader.toml", b"[d2rloader.advanced]\n")
    return buf.getvalue()


GOOD_ZIP = loader_zip()
STATIC["/loader/L.zip"] = GOOD_ZIP


def installer(version, tag=b"", extra=0):
    return FAKE_SETUP.read_bytes() + b"\0INSTALLER " + version.encode() + tag + b"\0" * extra


V168 = installer("0.168.0")
V169_SLOW = installer("0.169.0", b"S", 6_000_000)
U319 = installer("0.3.19", b"U")
U320_SLOW = installer("0.3.20", b"S", 6_000_000)
STATIC["/files/D2R_VR_Setup_v0.168.0.exe"] = V168
STATIC["/gh/download/v0.3.19-windows-preview/D2R-PCVR-Setup-0.3.19.exe"] = U319


def slow_route(data, chunk=65536, pause=0.03, event="slow"):
    def fn(h):
        h.send_response(200)
        h.send_header("Content-Length", str(len(data)))
        h.send_header("Connection", "close")
        h.end_headers()
        sent = 0
        EVENTS[event + "_sent"] = 0
        EVENTS.pop(event + "_done", None)
        while sent < len(data):
            n = min(chunk, len(data) - sent)
            h.wfile.write(data[sent:sent + n])
            h.wfile.flush()
            sent += n
            EVENTS[event + "_sent"] = sent
            time.sleep(pause)
        EVENTS[event + "_done"] = True
    return fn


ROUTES["/files/D2R_VR_Setup_v0.169.0.exe"] = slow_route(V169_SLOW, event="slow_auth")
ROUTES["/gh/download/v0.3.20-windows-preview/D2R-PCVR-Setup-0.3.20.exe"] = slow_route(U320_SLOW, event="slow_gh")

AUTH = {}


def auth_answer(version="0.168", data=V168, name=None, published=False, sha_override=None):
    d = {"version": version, "download_url": f"{BASE}/files/{name or 'D2R_VR_Setup_v%s.0.exe' % version}", "changelog": "test changelog"}
    if published:
        d["sha256"] = sha_override or sha(data)
        d["size"] = len(data)
    return d


def set_auth(**kw):
    AUTH["answer"] = auth_answer(**kw)
    AUTH["newest"] = kw.get("version", "0.168") + ".0"


def api_version(h):
    time.sleep(STATE["api_delay"])
    v = h.path.split("v=")[-1] if "v=" in h.path else ""
    if v == AUTH["newest"]:
        h.reply(200, json.dumps({"version": "0.0.0", "download_url": "", "changelog": ""}).encode(), ctype="application/json")
    else:
        h.reply(200, json.dumps(AUTH["answer"]).encode(), ctype="application/json")


ROUTES["/api/version"] = api_version
GH = {}


def set_gh(version="0.3.19", data=U319, digest=True):
    tag = f"v{version}-windows-preview"
    name = f"D2R-PCVR-Setup-{version}.exe"
    base = f"{BASE}/gh/download/{tag}/"
    assets = [{"name": name, "size": len(data), "state": "uploaded", "digest": ("sha256:" + sha(data)) if digest else "", "browser_download_url": base + name}]
    if digest:
        assets.append({"name": name + ".sha256", "size": 92, "state": "uploaded", "browser_download_url": base + name + ".sha256"})
        STATIC[f"/gh/download/{tag}/{name}.sha256"] = (sha(data) + f"  {name}\n").encode()
    GH["list"] = json.dumps([{"tag_name": tag, "draft": False, "prerelease": True, "published_at": "2026-10-18T08:17:11Z", "body": "notes", "assets": assets}])


def gh_releases(h):
    time.sleep(STATE["api_delay"])
    h.reply(200, GH["list"].encode(), ctype="application/json")


ROUTES["/gh/releases"] = gh_releases


def manifest_for(mod_id, version):
    base = json.loads((HERE / "examples" / "d2rloader-mods" / ("d2r-vr" if mod_id == "d2r-vr" else "d2r-pcvr") / "vrmod-manifest.json").read_text(encoding="utf-8"))
    base["version"] = version
    p = WORK / "manifests" / f"{mod_id}_{version}.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(base), encoding="utf-8")
    return p


def write_pin(zip_bytes, name="pin.json"):
    p = WORK / name
    p.write_text(json.dumps({"version": "1.3.1-beta", "url": f"{BASE}/loader/L.zip", "file": "D2RLoader-test.zip", "sha256": [sha(zip_bytes)],
                             "expected": [{"path": n, "sha256_any_of": [sha(d)]} for n, d in REAL.items()]}), encoding="utf-8")
    return p


PIN = None          # set once WORK exists
SETUP_LOG = None


def reset_server():
    set_auth()
    set_gh()
    STATE["api_delay"] = 0.0


def set_created(path, days_ago):
    """Sets the creation time of a file or folder to `days_ago` days back: the launcher lists the two VR mods in the order they were installed, so a test says which was first
    (folders made one after the other can get the very same time, the clock is coarse)."""
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    k32.CreateFileW.restype = ctypes.c_void_p
    k32.SetFileTime.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    k32.CloseHandle.argtypes = [ctypes.c_void_p]
    h = k32.CreateFileW(str(path), 0x100, 7, None, 3, 0x02000000, None)       # FILE_WRITE_ATTRIBUTES, share all, OPEN_EXISTING, BACKUP_SEMANTICS (a folder too)
    if not h or h == ctypes.c_void_p(-1).value:
        return
    ft = int((time.time() - days_ago * 86400 + 11644473600) * 10000000)
    c = wintypes.FILETIME(ft & 0xFFFFFFFF, ft >> 32)
    k32.SetFileTime(h, ctypes.byref(c), None, None)
    k32.CloseHandle(h)


def make_game(name, loader=False, authentic=None, unleashed=None):
    """A fake Diablo II: Resurrected folder, in a folder with spaces like the real one. `authentic` / `unleashed`: the version of the mod installed in it."""
    game = WORK / name / "Diablo II Resurrected"
    exe = b"MZ-fake-D2R.exe " + "3.3.93787".encode("utf-16-le")
    for rel, data in {"D2R.exe": exe, "Data/.build.config": b"x", "Saved/Settings.json": b"{}"}.items():
        (game / rel).parent.mkdir(parents=True, exist_ok=True)
        (game / rel).write_bytes(data)
    if loader:
        for n, d in REAL.items():
            (game / n).write_bytes(d)
        (game / "mods").mkdir(exist_ok=True)
    for ver, mod_id, folder in ((authentic, "d2r-vr", "D2R-VR"), (unleashed, "d2r-pcvr", "D2R-PCVR")):
        if ver:
            mod = game / "mods" / folder
            (mod / f"{folder}.mpq").mkdir(parents=True, exist_ok=True)
            (mod / f"{folder}.mpq" / "modinfo.json").write_text('{"name":"%s"}' % folder, encoding="utf-8")
            shutil.copy(manifest_for(mod_id, ver), mod / "vrmod-manifest.json")
            days = 10 if mod_id == "d2r-vr" else 5                         # Authentic was installed first
            set_created(mod, days)
            set_created(mod / f"{folder}.mpq" / "modinfo.json", days)
    return game, WORK / name / "home"


def env_for(game, extra=None, loopback=True, setup_exit=None):
    env = dict(os.environ)
    for k in [k for k in env if k.startswith("VRML_") or k.startswith("FAKE_SETUP")]:
        del env[k]
    if loopback:
        env["VRML_ALLOW_HTTP_HOST"] = f"127.0.0.1:{PORT}"
        env["VRML_URL_AUTHENTIC_API"] = f"{BASE}/api/version"
        env["VRML_URL_UNLEASHED_API"] = f"{BASE}/gh/releases"
        env["VRML_URL_LOADER"] = f"{BASE}/loader/L.zip"
        env["VRML_TEST_NO_ELEVATE"] = "1"                       # the stand-in Setup must not raise a UAC prompt
    env["FAKE_SETUP_LOG"] = str(SETUP_LOG)
    env["FAKE_SETUP_GAME"] = str(game)
    env["FAKE_SETUP_MANIFEST_D2R_VR"] = str(manifest_for("d2r-vr", "0.168.0"))
    env["FAKE_SETUP_MANIFEST_D2R_PCVR"] = str(manifest_for("d2r-pcvr", "0.3.19"))
    env.update(extra or {})
    return env


def parts(root):
    return [str(p) for p in root.rglob("*") if p.name.endswith(".vrml-part") or p.name.endswith(".vrml-tmp") or p.name.startswith(".vrml-install-")] if root.exists() else []


def setup_runs():
    """The mods the stand-in Setup was started for, in order."""
    if not SETUP_LOG.exists():
        return []
    return [l.split("=", 1)[1].strip() for l in SETUP_LOG.read_text(encoding="utf-8", errors="replace").splitlines() if l.startswith("mod=")]


def setup_log_text():
    return SETUP_LOG.read_text(encoding="utf-8", errors="replace") if SETUP_LOG.exists() else ""


def local_appdata_snapshot():
    """What the real launcher folder holds (never written by these tests): compared before and after."""
    root = Path(os.environ.get("LOCALAPPDATA", "")) / "VRModLauncher"
    if not root.exists():
        return None
    return {p.relative_to(root).as_posix(): (p.stat().st_size, p.stat().st_mtime_ns) for p in sorted(root.rglob("*")) if p.is_file()}


# ------------------------------------------------------------------ the launcher: headless, and with a window

def run_headless(game, home, pieces, *extra, env=None, timeout=120, processes="-"):
    cmd = [EXE, "--game", str(game), "--home", str(home), "--install-run", pieces, "--pin-file", str(PIN), "--processes", processes, *extra]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env or env_for(game), timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def lines_of(out):
    """{piece id: (status, code)} from the lines `<id>  <status>[  <code>]` of --install-run."""
    d = {}
    for l in out.splitlines():
        parts_ = l.split()
        if len(parts_) >= 2 and parts_[0] in ("d2rloader", "d2r-vr", "d2r-pcvr"):
            d[parts_[0]] = (parts_[1], parts_[2] if len(parts_) > 2 else "")
    return d


PROCS = []


def top_windows(pid):
    found = []

    @U.WNDENUMPROC
    def each(h, _):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid:
            cls = ctypes.create_unicode_buffer(80)
            user32.GetClassNameW(h, cls, 80)
            found.append((h, cls.value))
        return True

    user32.EnumWindows(each, 0)
    return found


def wtext(h):
    n = user32.SendMessageW(h, WM_GETTEXTLENGTH, 0, 0)
    buf = ctypes.create_unicode_buffer(n + 2)
    user32.SendMessageW(h, WM_GETTEXT, n + 1, ctypes.cast(buf, ctypes.c_void_p).value)
    return buf.value


def dlg(h, ident):
    return user32.GetDlgItem(h, ident)


def children(h):
    out = []

    @U.WNDENUMPROC
    def each(c, _):
        cls = ctypes.create_unicode_buffer(80)
        user32.GetClassNameW(c, cls, 80)
        out.append((c, cls.value))
        return True

    user32.EnumChildWindows(h, each, 0)
    return out


def launcher_args(args, env):
    """The arguments of a run: the process list is the test's own (the real game may run on this PC), and with the loopback switch the test pin of D2RLoader."""
    out = [str(x) for x in args]
    if "--processes" not in out:
        out += ["--processes", "-"]
    if "VRML_ALLOW_HTTP_HOST" in env:
        out += ["--pin-file", str(PIN)]
    return out


def wait_for(cond, timeout=20, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if cond():
                return True
        except Exception:
            pass
        time.sleep(step)
    return False


class App:
    """D2RVRLauncher.exe with a window, driven from outside with window messages."""

    def __init__(self, args, env):
        self.proc = subprocess.Popen([EXE] + launcher_args(args, env), env=env)
        PROCS.append(self.proc)
        self.pid = self.proc.pid

    def window(self, cls, timeout=20):
        res = []
        def find():
            for h, c in top_windows(self.pid):
                if c == cls and user32.IsWindowVisible(h):
                    res[:] = [h]
                    return True
            return False
        return res[0] if wait_for(find, timeout) else None

    def dialogs(self):
        return [h for h, c in top_windows(self.pid) if c == "#32770" and user32.IsWindowVisible(h)]

    def wait_dialog(self, timeout=15):
        res = []
        def find():
            d = self.dialogs()
            res[:] = d[:1]
            return bool(d)
        return res[0] if wait_for(find, timeout) else None

    def alive(self):
        return self.proc.poll() is None

    def close(self, timeout=15):
        for h, c in top_windows(self.pid):
            if c in (MAIN_CLASS, VIEW_CLASS):
                user32.PostMessageW(h, WM_CLOSE, 0, 0)
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            return None

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


def press(h):
    user32.PostMessageW(h, BM_CLICK, 0, 0)         # posted: a click that opens a message box must not block this thread


def box(view, i):
    return dlg(dlg(view, V_ROWS), V_BOX + i)


def ticked(view):
    return [user32.SendMessageW(box(view, i), BM_GETCHECK, 0, 0) == 1 for i in range(3)]


def log_of(view):
    return wtext(dlg(view, V_LOG))


def set_box(view, i, on):
    if (user32.SendMessageW(box(view, i), BM_GETCHECK, 0, 0) == 1) != on:
        user32.SendMessageW(box(view, i), BM_CLICK, 0, 0)       # a check box: it does not block


def view_ready(view, timeout=30):
    """Both servers have answered (or failed): the log has a line for each mod."""
    return wait_for(lambda: "D2R VR Authentic:" in log_of(view) and "D2R VR Unleashed:" in log_of(view), timeout)


def run_done(view, timeout=60):
    return wait_for(lambda: wtext(dlg(view, V_CLOSE)) == "Close" and ("Done:" in wtext(dlg(view, V_NOTICE)) or "Finished:" in wtext(dlg(view, V_NOTICE))), timeout)


def view_tab_order(view):
    ids, h = [], None
    for _ in range(30):
        h = user32.GetNextDlgTabItem(view, h, False)
        if not h:
            break
        i = user32.GetDlgCtrlID(h)
        if i in ids:
            break
        ids.append(i)
    return ids


def shot(args, name, env, timeout=90):
    out = SHOTS / (name + ".png")
    if out.exists():
        out.unlink()
    r = subprocess.run([EXE] + launcher_args(args, env) + ["--screenshot", str(out)], env=env, capture_output=True, text=True, timeout=timeout)
    return r.returncode, out


def is_ok_green(r, g, b):
    return abs(r - 14) < 25 and abs(g - 112) < 30 and abs(b - 14) < 25


def is_error_red(r, g, b):
    return abs(r - 190) < 25 and abs(g - 30) < 30 and abs(b - 20) < 30


def is_amber(r, g, b):
    return abs(r - 150) < 25 and abs(g - 86) < 25 and abs(b - 0) < 30


def count_in(png, pred, box_=None):
    return png.count(pred, box_)


# ================================================================== main
def main():
    global PIN, SETUP_LOG
    if WORK.exists():
        shutil.rmtree(WORK)
    WORK.mkdir(parents=True)
    SHOTS.mkdir(parents=True, exist_ok=True)
    PIN = write_pin(GOOD_ZIP)
    SETUP_LOG = WORK / "fake_setup.log"
    reset_server()
    real_before = local_appdata_snapshot()
    try:
        part_0_version()
        part_1_headless()
        part_2_view()
        part_3_main_window()
        part_4_screenshots()
    finally:
        for p in PROCS:
            if p.poll() is None:
                p.kill()
    check(local_appdata_snapshot() == real_before, "nothing was written to the real %LOCALAPPDATA%\\VRModLauncher (every run had its own --home)")
    print(f"\n{'ALL PASSED' if not failures else str(len(failures)) + ' FAILED'}")
    for f in failures:
        print("  FAIL:", f)
    server.shutdown()
    if not failures:
        shutil.rmtree(WORK / "tmpwork", ignore_errors=True)
    else:
        print("work folder kept:", WORK)
    return 1 if failures else 0


# ------------------------------------------------------------------ 0. the version
def part_0_version():
    print("\n=== 0. the version (kLauncherVersion in src/util.h = the exe's version resource = `vrmodlauncher version`)")
    import re
    want = re.search(r'kLauncherVersion = "([^"]+)"', (HERE / "src" / "util.h").read_text(encoding="utf-8")).group(1)
    got = subprocess.run(["powershell", "-NoProfile", "-Command", f"(Get-Item '{EXE}').VersionInfo.FileVersion"], capture_output=True, text=True).stdout.strip()
    check(want == "0.3.1", f"the launcher is 0.3.1 ({want})")
    check(got == want, f"the exe's version resource follows it ({got!r})")
    cli = Path(EXE).with_name("vrmodlauncher.exe")
    if cli.exists():
        out = subprocess.run([str(cli), "version"], capture_output=True, text=True).stdout
        check(out.startswith("vrmodlauncher " + want), f"`vrmodlauncher version` says it ({out.strip()!r})")


# ------------------------------------------------------------------ 1. --install-run
def part_1_headless():
    print("\n=== 1. --install-run: what the view's Install does, without a window")
    reset_server()

    game, home = make_game("h_loader")
    before = snap(game)
    code, out = run_headless(game, home, "loader")
    check(code == 0 and lines_of(out).get("d2rloader") == ("installed", ""), f"loader: installed, exit 0 ({code}) {out.strip().splitlines()[:2]}")
    check(all((game / n).read_bytes() == d for n, d in REAL.items()) and hits("/loader/L.zip") == 1, "...the three files are in the game folder, one download")
    check((game / "mods").is_dir() and not parts(game) and not parts(home), "...no temporary file or folder is left")
    check(all(snap(game).get(k) == v for k, v in before.items()), "...the game's own files are untouched")
    code, out = run_headless(game, home, "loader")
    check(code == 0 and lines_of(out).get("d2rloader") == ("already-there", "") and hits("/loader/L.zip") == 1, "loader again: already there, no second download")

    # Authentic without a published checksum
    game, home = make_game("h_authentic", loader=True)
    SETUP_LOG.unlink(missing_ok=True)
    n = hits("/files/D2R_VR_Setup_v0.168.0.exe")
    before = snap(game)
    code, out = run_headless(game, home, "authentic")
    check(code == 27 and lines_of(out).get("d2r-vr") == ("failed", "E_UNVERIFIED"), f"authentic, no published checksum, no --accept-unverified: failed E_UNVERIFIED, exit 27 ({code})")
    check(hits("/files/D2R_VR_Setup_v0.168.0.exe") == n and not setup_runs() and snap(game) == before, "...nothing was downloaded or run, the game folder is as it was")
    code, out = run_headless(game, home, "authentic", "--accept-unverified")
    check(code == 0 and lines_of(out).get("d2r-vr") == ("installed", "") and sha(V168) in out and "no published checksum" in out, "--accept-unverified: installed, and the SHA-256 it computed is shown")
    check(f"arg1=/DIR={game}" in setup_log_text(), "...the Setup received /DIR=\"<game folder>\" with its spaces intact")
    check((game / "mods/D2R-VR/vrmod-manifest.json").is_file() and (game / "mods/D2R-VR/D2R-VR.mpq/modinfo.json").is_file(), "...D2R VR Authentic is in the game folder")
    code, out = run_headless(game, home, "authentic", "--accept-unverified")
    check(code == 0 and lines_of(out).get("d2r-vr") == ("already-there", "") and len(setup_runs()) == 1, "again: already the newest version, the Setup is not run a second time")

    # everything, in the right order
    game, home = make_game("h_all")
    SETUP_LOG.unlink(missing_ok=True)
    code, out = run_headless(game, home, "unleashed,authentic,loader", "--accept-unverified")
    st = lines_of(out)
    check(code == 0 and st == {"d2rloader": ("installed", ""), "d2r-vr": ("installed", ""), "d2r-pcvr": ("installed", "")}, f"all three: installed, exit 0 ({code}) {st}")
    check(setup_runs() == ["D2R-VR", "D2R-PCVR"], f"...D2RLoader first, then Authentic, then Unleashed, whatever order they were asked for ({setup_runs()})")
    check([l.split()[0] for l in out.splitlines() if l.split() and l.split()[0] in ("d2rloader", "d2r-vr", "d2r-pcvr")] == ["d2rloader", "d2r-vr", "d2r-pcvr"], "...and the lines come in that order")
    log = setup_log_text()
    check("started args=1" in log and "started args=0" in log, "...Authentic's Setup got the /DIR argument, Unleashed's none (it shows its own wizard)")

    # a wrong hash
    game, home = make_game("h_wronghash", loader=True)
    SETUP_LOG.unlink(missing_ok=True)
    set_auth(published=True, sha_override=sha(b"something else"))
    code, out = run_headless(game, home, "authentic")
    check(code == 23 and lines_of(out).get("d2r-vr") == ("failed", "E_HASH"), f"a download that is not the announced sha256: failed E_HASH, exit 23 ({code})")
    check(not setup_runs() and not list((home / "downloads").rglob("*.exe")), "...the file was deleted and the Setup never ran")
    set_auth(published=True)
    game, home = make_game("h_publishedok", loader=True)
    code, out = run_headless(game, home, "authentic")
    check(code == 0 and "matches the published one" in out, "a published sha256 that matches: installed without --accept-unverified")
    set_auth()

    # a cancel half way through a slow download, the rest is not started
    game, home = make_game("h_cancel", loader=True)
    SETUP_LOG.unlink(missing_ok=True)
    set_auth(version="0.169", data=V169_SLOW, name="D2R_VR_Setup_v0.169.0.exe", published=True)
    set_gh()
    EVENTS.clear()
    code, out = run_headless(game, home, "authentic,unleashed", env=env_for(game, {"VRML_TEST_CANCEL_AFTER": "400000"}))
    st = lines_of(out)
    check(code == 30 and st.get("d2r-vr") == ("cancelled", "E_CANCELLED") and st.get("d2r-pcvr", ("", ""))[0] == "not-run", f"a download cancelled half way: cancelled, exit 30, the next piece not run ({code}) {st}")
    check(not parts(home) and not list((home / "downloads").rglob("*.exe")) and not setup_runs(), "...the partial file is deleted and no Setup ran")
    time.sleep(1.0)
    check(EVENTS.get("slow_auth_sent", 0) < len(V169_SLOW) and not EVENTS.get("slow_auth_done"), "...and the connection was dropped (the server did not get to send it all)")
    set_auth()

    # the game runs
    game, home = make_game("h_running", loader=True)
    SETUP_LOG.unlink(missing_ok=True)
    n = hits("/files/D2R_VR_Setup_v0.168.0.exe")
    before = snap(game)
    code, out = run_headless(game, home, "authentic,unleashed", "--accept-unverified", processes="D2R.exe")
    st = lines_of(out)
    check(code == 10 and st.get("d2r-vr") == ("failed", "E_GAME_RUNNING") and st.get("d2r-pcvr", ("", ""))[0] == "not-run", f"D2R.exe running: refused, exit 10, the rest not run ({code})")
    check(hits("/files/D2R_VR_Setup_v0.168.0.exe") == n and not setup_runs() and snap(game) == before, "...nothing downloaded, run or written")

    # not a game folder
    notgame = WORK / "notgame"
    notgame.mkdir()
    code, out = run_headless(notgame, WORK / "h_ng_home", "loader")
    check(code == 25 and not any(notgame.iterdir()), f"a folder without D2R.exe: exit 25 ({code})")

    # the Setup's own results
    for label, extra, want_code, want in (("the Setup fails (Inno exit 4)", {"FAKE_SETUP_EXIT": "4", "FAKE_SETUP_MODE": "nothing"}, 28, ("failed", "E_SETUP")),
                                           ("the player cancels the Setup (exit 5)", {"FAKE_SETUP_EXIT": "5", "FAKE_SETUP_MODE": "nothing"}, 30, ("cancelled", "E_CANCELLED")),
                                           ("the Setup ends well but installed elsewhere", {"FAKE_SETUP_MODE": "nothing"}, 11, ("failed", "E_MOD_NOT_INSTALLED"))):
        game, home = make_game("h_setup", loader=True)
        code, out = run_headless(game, home, "authentic", "--accept-unverified", env=env_for(game, extra))
        check(code == want_code and lines_of(out).get("d2r-vr") == want, f"{label}: {want[1]}, exit {want_code} ({code})")
        check(not (game / "mods/D2R-VR").exists(), "...and it is reported as not installed")
    # one piece failing does not stop the next one
    game, home = make_game("h_continue", loader=True)
    SETUP_LOG.unlink(missing_ok=True)
    code, out = run_headless(game, home, "authentic,unleashed", "--accept-unverified", env=env_for(game, {"FAKE_SETUP_EXIT_D2R_VR": "4", "FAKE_SETUP_MODE_D2R_VR": "nothing"}))
    st = lines_of(out)
    check(code == 28 and st.get("d2r-vr") == ("failed", "E_SETUP") and st.get("d2r-pcvr") == ("installed", ""), f"Authentic's Setup fails, Unleashed is still installed; the exit code is the first failure's (28) ({code})")

    # an unknown piece and the test aids without the loopback switch
    game, home = make_game("h_usage", loader=True)
    r = subprocess.run([EXE, "--game", str(game), "--home", str(home), "--install-run", "bogus"], capture_output=True, text=True, env=env_for(game), timeout=60)
    check(r.returncode == 2 and "bogus" in r.stdout + r.stderr, f"an unknown piece in the list: exit 2 ({r.returncode})")
    env_off = env_for(game, loopback=False)
    for args in (["--install-run", "loader"], ["--accept-unverified"], ["--pin-file", str(PIN)]):
        r = subprocess.run([EXE, "--game", str(game), "--home", str(home)] + args, capture_output=True, text=True, env=env_off, timeout=60)
        check(r.returncode == 2 and "VRML_ALLOW_HTTP_HOST" in r.stdout + r.stderr, f"{args[0]} without the loopback switch: refused, exit 2 ({r.returncode})")
    check(not home.exists() and not parts(game), "...and nothing was written")


# ------------------------------------------------------------------ 2. the install view
def part_2_view():
    print("\n=== 2. the install view, driven with window messages")
    reset_server()
    game, home = make_game("v_fresh")
    n_files = hits("/files/D2R_VR_Setup_v0.168.0.exe")
    SETUP_LOG.unlink(missing_ok=True)
    app = App(["--game", game, "--home", home, "--install-view", "--no-update-check"], env_for(game))
    try:
        view = app.window(VIEW_CLASS)
        check(view is not None, "--install-view: the view opens right after start")
        if not view:
            return
        check(view_ready(view), "the newest versions arrive (the log says what each server answered)")
        log = log_of(view)
        check("D2R VR Authentic: newest is 0.168.0, no published checksum" in log and "D2R VR Unleashed: newest is 0.3.19, checksum digest + .sha256" in log, "...Authentic: no published checksum; Unleashed: GitHub's digest and the .sha256 file")
        check(ticked(view) == [True, True, True], f"missing pieces are ticked: D2RLoader, Authentic, Unleashed ({ticked(view)})")
        check(wtext(dlg(view, V_NOTICE)).startswith("Ready to install: D2RLoader, D2R VR Authentic 0.168.0, D2R VR Unleashed 0.3.19"), f"the notice says what will happen ({wtext(dlg(view, V_NOTICE))[:90]!r})")
        check(user32.IsWindowEnabled(dlg(view, V_INSTALL)) and wtext(dlg(view, V_CLOSE)) == "Close" and wtext(dlg(view, V_INSTALL)).replace("&", "") == "Install selected", "Install selected is on, the other button says Close")
        check(view_tab_order(view) == [1100, 1101, 1102, V_CHECK, V_INSTALL, V_CLOSE, V_LOG], f"Tab visits the three boxes, Check again, Install selected, Close, the log ({view_tab_order(view)})")

        # the tick rules
        set_box(view, 1, False)
        set_box(view, 2, False)
        check(wait_for(lambda: ticked(view) == [False, False, False], 3), f"unticking both mods lets D2RLoader go (it was never touched) ({ticked(view)})")
        check(not user32.IsWindowEnabled(dlg(view, V_INSTALL)) and "Tick what you want to install" in wtext(dlg(view, V_NOTICE)), "...and nothing can be started: the notice says why")
        set_box(view, 1, True)
        check(wait_for(lambda: ticked(view) == [True, True, False], 3), f"ticking Authentic ticks D2RLoader, which is missing, too ({ticked(view)})")
        set_box(view, 0, False)
        check(ticked(view) == [False, True, False], f"the player may untick D2RLoader himself ({ticked(view)})")
        set_box(view, 2, True)
        check(ticked(view) == [True, True, True], f"ticking another mod while D2RLoader is missing ticks it again ({ticked(view)})")
        set_box(view, 2, False)

        # the question before a download without a published checksum
        press(dlg(view, V_INSTALL))
        d = app.wait_dialog()
        check(d is not None, "Install selected with Authentic ticked: a question appears before anything is downloaded")
        if d:
            text = " ".join(wtext(c) for c, cls in children(d) if cls == "Static")
            check("does not publish a checksum" in text and "bodywalkvr.com" in text and "probably fine" in text and "SHA-256" in text, "...it says what is not verifiable and why it is probably fine")
            check((user32.SendMessageW(d, DM_GETDEFID, 0, 0) & 0xFFFF) == IDNO, "...the default button is No")
            press(dlg(d, IDNO))
            check(wait_for(lambda: not app.dialogs(), 5), "...No closes it")
        check(wait_for(lambda: "Nothing was downloaded" in wtext(dlg(view, V_NOTICE)), 5) and hits("/files/D2R_VR_Setup_v0.168.0.exe") == n_files and not setup_runs(),
              "No: nothing was downloaded, nothing ran, and the notice says so")
        check("Not installed: a download without a published checksum was not accepted" in log_of(view), "...and the log")

        # Yes: a real install, D2RLoader first
        SETUP_LOG.unlink(missing_ok=True)
        press(dlg(view, V_INSTALL))
        d = app.wait_dialog()
        if d:
            press(dlg(d, IDYES))
        check(run_done(view, 90), f"after Yes the install runs to the end ({wtext(dlg(view, V_NOTICE))[:80]!r})")
        check("Done: 2 installed" in wtext(dlg(view, V_NOTICE)), f"...the summary in words: {wtext(dlg(view, V_NOTICE))!r}")
        check(all((game / n).read_bytes() == d for n, d in REAL.items()), "D2RLoader's three files are in the game folder")
        check((game / "mods/D2R-VR/vrmod-manifest.json").is_file() and setup_runs() == ["D2R-VR"], "D2R VR Authentic is installed through its Setup")
        log = log_of(view)
        check(sha(V168) in log and "no published checksum to compare with" in log, "the SHA-256 of the download is in the log")
        check("D2RLoader: installed" in log and "D2R VR Authentic: installed 0.168.0" in log, "the log summarises each piece")
        check(user32.SendMessageW(dlg(view, V_PROGRESS), PBM_GETPOS, 0, 0) == 1000, "the progress bar is full")
        check(user32.IsWindowEnabled(dlg(view, V_CHECK)) and wtext(dlg(view, V_CLOSE)) == "Close", "Check again and Close are on again")
        press(dlg(view, V_CLOSE))
        check(wait_for(lambda: not app.window(VIEW_CLASS, 0.2), 5), "Close closes the view")
        check(app.alive(), "...the main window stays")
        s = (home / "settings.json").read_text(encoding="utf-8").replace('":"', '": "') if (home / "settings.json").exists() else ""
        check('"install_view_offered": true' in s and '"game": ""' in s, "having seen the view counts as the first-run offer (remembered; the folder given with --game is not)")
    finally:
        app.close()

    # a view over a game with the pieces installed: outdated is ticked, current is not
    print("  -- Authentic 0.167.0 and Unleashed 0.3.19 installed")
    reset_server()
    game, home = make_game("v_update", loader=True, authentic="0.167.0", unleashed="0.3.19")
    app = App(["--game", game, "--home", home, "--install-view", "--no-update-check"], env_for(game))
    try:
        view = app.window(VIEW_CLASS)
        check(view is not None and view_ready(view), "the view opens and hears the servers")
        if view:
            check(wait_for(lambda: ticked(view) == [False, True, False], 5), f"the outdated Authentic is ticked, the current Unleashed and the reviewed-files D2RLoader are not ({ticked(view)})")
            check("D2R VR Unleashed: you have the newest version" in log_of(view), "the log: Unleashed is current")
            check(wtext(dlg(view, V_NOTICE)).startswith("Ready to install: D2R VR Authentic 0.168.0"), "the notice names only what is ticked")
            set_box(view, 2, True)
            check(user32.IsWindowEnabled(dlg(view, V_INSTALL)), "ticking a current mod is allowed (it installs it again)")
    finally:
        app.close()

    print("  -- the card's Install button: only that piece ticked")
    game, home = make_game("v_tick")
    app = App(["--game", game, "--home", home, "--install-view", "--install-tick", "unleashed", "--no-update-check"], env_for(game))
    try:
        view = app.window(VIEW_CLASS)
        check(view is not None and view_ready(view) and ticked(view) == [True, False, True], f"--install-tick unleashed: Unleashed ticked, D2RLoader with it (missing), Authentic not ({ticked(view) if view else None})")
    finally:
        app.close()

    print("  -- a cancel in the middle of a download")
    reset_server()
    set_gh("0.3.20", U320_SLOW)
    game, home = make_game("v_cancel", loader=True)
    SETUP_LOG.unlink(missing_ok=True)
    EVENTS.clear()
    app = App(["--game", game, "--home", home, "--install-view", "--install-tick", "unleashed", "--no-update-check"], env_for(game))
    try:
        view = app.window(VIEW_CLASS)
        check(view is not None and view_ready(view), "the view opens")
        if view:
            check(ticked(view) == [False, False, True], f"only Unleashed is ticked ({ticked(view)})")
            press(dlg(view, V_INSTALL))
            check(wait_for(lambda: wtext(dlg(view, V_CLOSE)) == "Cancel", 10), "Install selected: the Close button becomes Cancel")
            check(wait_for(lambda: "downloading" in wtext(dlg(view, V_STEP)), 10), f"the step text says what is happening ({wtext(dlg(view, V_STEP))!r})")
            check(not user32.IsWindowEnabled(dlg(view, V_INSTALL)) and not user32.IsWindowEnabled(dlg(view, V_CHECK)), "...Install selected and Check again are off while it runs")
            user32.SendMessageW(box(view, 1), BM_CLICK, 0, 0)
            check(ticked(view) == [False, False, True], f"...a click on a box changes nothing ({ticked(view)}; still running: {wtext(dlg(view, V_CLOSE))!r}; log: {log_of(view)[-300:]!r})")
            check(wait_for(lambda: user32.SendMessageW(dlg(view, V_PROGRESS), PBM_GETPOS, 0, 0) > 0, 10), "the progress bar moves")
            user32.PostMessageW(view, WM_CLOSE, 0, 0)
            time.sleep(0.5)
            check(app.window(VIEW_CLASS, 0.2) is not None, "closing the window while it runs is refused (Cancel first)")
            press(dlg(view, V_CLOSE))
            check(wait_for(lambda: wtext(dlg(view, V_CLOSE)) in ("Cancelling...", "Close"), 5), "Cancel: the button says Cancelling...")
            check(run_done(view, 30), f"the install ends ({wtext(dlg(view, V_NOTICE))!r})")
            check("Cancel requested" in log_of(view) and "cancelled" in log_of(view).lower(), "the log says it was cancelled")
            check(not (game / "mods/D2R-PCVR").exists() and not setup_runs() and not parts(home) and not list((home / "downloads").rglob("*.exe")), "no Setup ran, the partial download is deleted, nothing was installed")
            time.sleep(1.0)
            check(not EVENTS.get("slow_gh_done"), "the connection was dropped before the server sent it all")
    finally:
        app.close()

    print("  -- the game runs / no game folder")
    reset_server()
    game, home = make_game("v_running", loader=True)
    app = App(["--game", game, "--home", home, "--install-view", "--no-update-check", "--processes", "D2R.exe"], env_for(game))
    try:
        view = app.window(VIEW_CLASS)
        check(view is not None and view_ready(view), "the view opens while the game runs")
        if view:
            check(wait_for(lambda: not user32.IsWindowEnabled(dlg(view, V_INSTALL)), 5) and "game is running" in wtext(dlg(view, V_NOTICE)), f"Install selected is off and the notice says the game is running ({wtext(dlg(view, V_NOTICE))[:70]!r})")
    finally:
        app.close()
    empty = WORK / "v_nogame"
    empty.mkdir()
    app = App(["--game", empty, "--home", WORK / "v_nogame_home", "--install-view", "--no-update-check"], env_for(empty))
    try:
        view = app.window(VIEW_CLASS)
        check(view is not None, "the view opens over a folder that is not a game")
        if view:
            check(wait_for(lambda: "Pick the folder" in wtext(dlg(view, V_NOTICE)), 5), f"the notice says to pick the game folder ({wtext(dlg(view, V_NOTICE))[:60]!r})")
            check(not user32.IsWindowEnabled(dlg(view, V_INSTALL)) and not user32.IsWindowEnabled(box(view, 0)) and not user32.IsWindowEnabled(dlg(view, V_CHECK)), "...the boxes, Check again and Install selected are off")
            check(not any(empty.iterdir()), "...and nothing was written to the folder")
    finally:
        app.close()

    print("  -- a server that is down is said in words")
    game, home = make_game("v_down", loader=True)
    dead = env_for(game, {"VRML_URL_AUTHENTIC_API": "http://127.0.0.1:1/api/version", "VRML_URL_UNLEASHED_API": "http://127.0.0.1:1/gh/releases"})
    app = App(["--game", game, "--home", home, "--install-view", "--no-update-check"], dead)
    try:
        view = app.window(VIEW_CLASS)
        check(view is not None and view_ready(view), "the view opens and does not freeze when the servers cannot be reached")
        if view:
            check("could not find out the newest version" in log_of(view), "the log says in words that the newest version could not be found out")
            check(ticked(view) == [False, False, False], "...nothing is ticked by itself")
            set_box(view, 1, True)
            check(not user32.IsWindowEnabled(dlg(view, V_INSTALL)) and "could not be found out" in wtext(dlg(view, V_NOTICE)) and "Check again" in wtext(dlg(view, V_NOTICE)),
                  f"ticking what could not be checked: refused, and the notice says why ({wtext(dlg(view, V_NOTICE))[:90]!r})")
    finally:
        app.close()


# ------------------------------------------------------------------ 3. the main window
def part_3_main_window():
    print("\n=== 3. the main window: update badges, the quiet check, the first-run offer")
    reset_server()
    set_auth(version="0.168")
    set_gh("0.3.19", U319)
    game, home = make_game("m_update", loader=True, authentic="0.167.0", unleashed="0.3.17")
    base_hits = api_hits()
    app = App(["--game", game, "--home", home], env_for(game))
    try:
        main = app.window(MAIN_CLASS)
        check(main is not None, "the window opens")
        if not main:
            return
        lst = dlg(main, M_LIST)
        check(wait_for(lambda: dlg(lst, CARD_INSTALL) and wtext(dlg(lst, CARD_INSTALL)) == "Update", 15), "the quiet check at start: the Authentic card gets an Update button")
        check(wtext(dlg(lst, CARD_INSTALL + 1)) == "Update", "...and the Unleashed card too")
        check(api_hits() - base_hits == 2, f"one request per source ({api_hits() - base_hits})")
        check("D2R VR Authentic: Update available: 0.167.0 -> 0.168.0" in wtext(dlg(main, M_LOG)) and "D2R VR Unleashed: Update available: 0.3.17 -> 0.3.19" in wtext(dlg(main, M_LOG)), "...the log has the badges' words")
        check("Website" in wtext(dlg(lst, CARD_LINK)), "each card keeps a small Website link")
        ids = []
        h = None
        for _ in range(30):
            h = user32.GetNextDlgTabItem(main, h, False)
            if not h:
                break
            i = user32.GetDlgCtrlID(h)
            if i in ids:
                break
            ids.append(i)
        check(ids == [101, 102, 1000, 3000, 3001, 4000, 4001, 2000, 2001, 107, 111, 110, 120, 119, 114, 116, 118],
              f"Tab order of the main window: game folder, Browse, the card, both Install / Update buttons, both Uninstall buttons, both Website links, runtime, Play, the two boxes, Install / update mods, Logs, About, the log ({ids})")
        # the card's Update button opens the view with that piece ticked
        press(dlg(lst, CARD_INSTALL + 1))
        view = app.window(VIEW_CLASS)
        check(view is not None, "the Update button of the Unleashed card opens the install view")
        if view:
            check(view_ready(view) and ticked(view) == [False, False, True], f"...with only that piece ticked ({ticked(view)})")
            press(dlg(view, V_CLOSE))
            check(wait_for(lambda: not app.window(VIEW_CLASS, 0.2), 5), "Close returns to the main window")
        press(dlg(main, M_INSTALL_VIEW))
        view = app.window(VIEW_CLASS)
        check(view is not None and view_ready(view), "the button 'Install / update mods...' opens the view too")
        if view:
            check(wait_for(lambda: ticked(view) == [False, True, True], 5), f"...with what is outdated ticked ({ticked(view)})")
            press(dlg(view, V_CLOSE))
            wait_for(lambda: not app.window(VIEW_CLASS, 0.2), 5)
    finally:
        app.close()

    print("  -- the quiet check can be switched off")
    for label, args, setting in (("--no-update-check", ["--no-update-check"], None), ("the setting 'Check for updates when the launcher starts' off", [], False)):
        game, home = make_game("m_off", loader=True, authentic="0.167.0", unleashed="0.3.17")
        if setting is not None:
            home.mkdir(parents=True, exist_ok=True)
            (home / "settings.json").write_text(json.dumps({"schema": "vrmod-launcher-settings/1", "game": "", "last_mod": "", "close_on_start": False, "check_updates_on_start": setting, "mods": {}}), encoding="utf-8")
        n = api_hits()
        app = App(["--game", game, "--home", home] + args, env_for(game))
        try:
            main = app.window(MAIN_CLASS)
            time.sleep(3.0)
            lst = dlg(main, M_LIST) if main else None
            check(main is not None and api_hits() == n, f"{label}: the servers are not asked ({api_hits() - n} requests)")
            check(lst and not dlg(lst, CARD_INSTALL), "...and no card has an Update button")
            if setting is False:
                check(not user32.SendMessageW(dlg(main, M_UPDATE_CHECK), BM_GETCHECK, 0, 0), "the box 'Check for updates when the launcher starts' is unticked")
                user32.SendMessageW(dlg(main, M_UPDATE_CHECK), BM_CLICK, 0, 0)
                check(wait_for(lambda: '"check_updates_on_start": true' in (home / "settings.json").read_text(encoding="utf-8").replace('":"', '": "'), 5), "ticking it is remembered in settings.json at once")
        finally:
            app.close()

    print("  -- a server that is down: quiet")
    game, home = make_game("m_down", loader=True, authentic="0.167.0", unleashed="0.3.17")
    app = App(["--game", game, "--home", home], env_for(game, {"VRML_URL_AUTHENTIC_API": "http://127.0.0.1:1/api/version", "VRML_URL_UNLEASHED_API": "http://127.0.0.1:1/gh/releases"}))
    try:
        main = app.window(MAIN_CLASS)
        time.sleep(3.0)
        check(main is not None and app.alive(), "the window is fine when the servers cannot be reached")
        if main:
            lst = dlg(main, M_LIST)
            log_text = wtext(dlg(main, M_LOG))
            check(not dlg(lst, CARD_INSTALL) and "warning" not in log_text.lower() and "error" not in log_text.lower(), f"...no button, no warning, no error: silent on failure ({log_text[-200:]!r})")
            check(wtext(dlg(main, M_MESSAGE)) == "Choose a mod and press Play.", f"...the message line is the usual one ({wtext(dlg(main, M_MESSAGE))!r})")
            check("update check" in ((home / "logs" / "launcher.log").read_text(encoding="utf-8") if (home / "logs" / "launcher.log").exists() else ""), "...the support log (launcher.log) has the reason")
    finally:
        app.close()

    print("  -- the first run: no VR mod installed, the folder found, not given")
    reset_server()
    game, home = make_game("m_first", loader=True)
    home.mkdir(parents=True, exist_ok=True)
    (home / "settings.json").write_text(json.dumps({"schema": "vrmod-launcher-settings/1", "game": str(game).replace("\\", "/"), "last_mod": "", "close_on_start": False, "mods": {}}), encoding="utf-8")
    app = App(["--home", home, "--no-search", "--no-update-check"], env_for(game))
    try:
        d = app.wait_dialog(20)
        check(d is not None, "the window offers the install view")
        if d:
            text = " ".join(wtext(c) for c, cls in children(d) if cls == "Static")
            check("Neither D2R VR Authentic nor D2R VR Unleashed is installed" in text and "Install / update mods" in text, "...it says why and where to find it later")
            press(dlg(d, IDNO))
            check(wait_for(lambda: not app.dialogs(), 5), "...No closes it")
            time.sleep(0.5)
            check(app.window(VIEW_CLASS, 0.3) is None, "...and the view does not open")
            s = (home / "settings.json").read_text(encoding="utf-8").replace('":"', '": "')
            check('"install_view_offered": true' in s, "it was offered once: remembered in settings.json")
    finally:
        app.close()
    app = App(["--home", home, "--no-search", "--no-update-check"], env_for(game))
    try:
        main = app.window(MAIN_CLASS)
        time.sleep(2.0)
        check(main is not None and not app.dialogs(), "the next start does not offer it again")
    finally:
        app.close()
    game, home = make_game("m_first_yes", loader=True)
    home.mkdir(parents=True, exist_ok=True)
    (home / "settings.json").write_text(json.dumps({"schema": "vrmod-launcher-settings/1", "game": str(game).replace("\\", "/"), "last_mod": "", "close_on_start": False, "mods": {}}), encoding="utf-8")
    app = App(["--home", home, "--no-search", "--no-update-check"], env_for(game))
    try:
        d = app.wait_dialog(20)
        if d:
            press(dlg(d, IDYES))
        check(d is not None and app.window(VIEW_CLASS, 15) is not None, "Yes opens the install view")
    finally:
        app.close()
    game, home = make_game("m_first_given", loader=True)
    app = App(["--game", game, "--home", home, "--no-update-check"], env_for(game))
    try:
        main = app.window(MAIN_CLASS)
        time.sleep(2.0)
        check(main is not None and not app.dialogs(), "a folder given with --game is never offered anything")
    finally:
        app.close()


# ------------------------------------------------------------------ 4. screenshots
def part_4_screenshots():
    print("\n=== 4. screenshots")
    reset_server()
    set_auth()
    set_gh()
    env_fresh = lambda g: env_for(g)

    game, home = make_game("s_checking")
    STATE["api_delay"] = 3.0
    t0 = time.time()
    code, out = shot(["--game", game, "--home", home, "--install-view", "--shot-now", "--no-update-check"], "view_checking", env_fresh(game))
    STATE["api_delay"] = 0.0
    check(code == 0 and out.is_file() and time.time() - t0 < 2.9, f"--install-view --shot-now: the 'checking...' state is shot at the first paint (exit {code}, {time.time() - t0:.1f} s)")
    if out.is_file():
        png = U.Png(out)
        check(500 <= png.w <= 1300 and 560 <= png.h <= 1400, f"...a plausible size ({png.w}x{png.h})")
        check(png.count(is_ok_green) < 50, "...nothing is green yet")

    game, home = make_game("s_ready")
    code, out = shot(["--game", game, "--home", home, "--install-view", "--no-update-check"], "view_ready", env_fresh(game))
    check(code == 0 and out.is_file(), f"--install-view --screenshot: the view after the checks (exit {code})")
    if out.is_file():
        png = U.Png(out)
        check(png.count(is_amber) > 100, "...the amber notes and 'no checksum published' are there")
        check(png.count(lambda r, g, b: abs(r - 15) < 30 and abs(g - 108) < 30 and abs(b - 189) < 30) > 300, "...the ticked cards have the accent blue border")
    check(not (game / "D2RLoader.exe").exists() and not (home / "settings.json").exists(), "the screenshot installed and wrote nothing")

    empty = WORK / "s_nogame"
    empty.mkdir()
    code, out = shot(["--game", empty, "--home", WORK / "s_nogame_home", "--install-view", "--no-update-check"], "view_no_game", env_fresh(empty))
    check(code == 0 and out.is_file(), f"the view over a folder that is not a game (exit {code})")

    # after a real install with one Setup that fails
    game, home = make_game("s_result")
    SETUP_LOG.unlink(missing_ok=True)
    code, out = shot(["--game", game, "--home", home, "--install-view", "--install-now", "--accept-unverified", "--no-update-check"], "view_result",
                     env_for(game, {"FAKE_SETUP_EXIT_D2R_VR": "4", "FAKE_SETUP_MODE_D2R_VR": "nothing"}))
    check(code == 0 and out.is_file(), f"--install-now: the view presses Install by itself and is shot at the end (exit {code})")
    if out.is_file():
        png = U.Png(out)
        check(png.count(is_ok_green) > 150, "...green marks: D2RLoader and Unleashed installed")
        check(png.count(is_error_red) > 80, "...a red mark: Authentic's Setup failed")
    check(all((game / n).read_bytes() == d for n, d in REAL.items()) and (game / "mods/D2R-PCVR/vrmod-manifest.json").is_file() and not (game / "mods/D2R-VR").exists(),
          "...D2RLoader and Unleashed are installed, Authentic is not")

    # 125 / 150 / 200 %
    sizes = {}
    for dpi in (96, 144, 192):
        game, home = make_game(f"s_dpi{dpi}", loader=True, authentic="0.167.0")
        code, out = shot(["--game", game, "--home", home, "--install-view", "--no-update-check", "--dpi", dpi], f"view_{dpi}dpi", env_fresh(game))
        check(code == 0 and out.is_file(), f"{dpi} dpi: exit {code}")
        if out.is_file():
            sizes[dpi] = U.Png(out).w
    if 96 in sizes and 192 in sizes:
        check(abs(sizes[192] - 2 * sizes[96]) <= 24, f"192 dpi is twice as wide as 96 ({sizes[192]} vs {sizes[96]})")

    # the main window with badges
    reset_server()
    set_auth()
    set_gh()
    game, home = make_game("s_badges", loader=True, authentic="0.167.0", unleashed="0.3.17")
    code, out = shot(["--game", game, "--home", home], "main_badges", env_for(game))
    check(code == 0 and out.is_file(), f"the main window waits for the quiet check before the screenshot (exit {code})")
    if out.is_file():
        png = U.Png(out)
        check(png.count(is_amber) > 150, "...the amber 'Update available: ...' lines are there")
    code, out = shot(["--game", game, "--home", home, "--dpi", "144"], "main_badges_150pct", env_for(game))
    check(code == 0 and out.is_file(), f"150 % (exit {code})")
    # a screenshot without the test server asks nobody (tests never use the internet)
    n = api_hits()
    game2, home2 = make_game("s_noasking", loader=True, authentic="0.167.0")
    code, out = shot(["--game", game2, "--home", home2], "main_no_asking", env_for(game2, loopback=False))
    log2 = home2 / "logs" / "launcher.log"
    check(code == 0 and api_hits() == n and "update check" not in (log2.read_text(encoding="utf-8") if log2.exists() else ""),
          "a screenshot run without the loopback switch does not ask the internet (no quiet check was even started)")
    print("screenshots:", SHOTS)


if __name__ == "__main__":
    sys.exit(main())
