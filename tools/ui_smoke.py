#!/usr/bin/env python3
"""Starts D2RVRLauncher.exe for real against a FAKE game folder and checks what it does.

    ui_smoke.py <D2RVRLauncher.exe> <work dir>

Everything happens in <work dir> (it is emptied first): a fake game (D2R.exe holding the build as UTF-16 text, D2RLoader.exe, two mods and a third
plain one), a launcher home of its own. Nothing of the real game folder, the real launcher folder (%LOCALAPPDATA%\\VRModLauncher) or a real D2R is
touched; the window is started with --game / --home, and a stand-in "D2R.exe" (a copy of PING.EXE) plays the running game.

What is checked
  * --screenshot: exit code 0, a PNG of a plausible size with the header's red crystal, the window's grey, the accent blue of the selected card
    (the screenshots stay in <work dir>/shots for looking at)
  * --play --dry-run for both mods: the plan names D2R_VR_Settings.exe --start / D2RLoader.exe -mod D2R-PCVR, settings.json is not written
  * a first start with an empty home, a mod that is not installed (E_MOD_NOT_INSTALLED, 11), D2RLoader.exe missing (E_LOADER_MISSING, 11),
    a running game (E_GAME_RUNNING, 10), a folder that is not a game (E_GAME, 25), no game at all
  * 125 / 150 / 200 % (--dpi): the window and its picture grow with the DPI
Refuses to run while a real D2R.exe or D2RLoader.exe is running (the checks would see it).
"""
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

FAILS = []
RUN_TIMEOUT = 90
EXTRA = ["--no-update-check", "--skip-first-start-check"]   # arguments for every run: no quiet look at the internet (tests never use it), Play does not depend on this PC's
                                                            # Saved Games (Settings.json); --processes - when a real game runs, see main


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        FAILS.append(what)


# ---------------------------------------------------------------- a fake game

def png_bytes(w, h, pixel):
    """A small valid PNG (RGB, 8 bit) - what a mod's launcher_icon.png is in the fake game."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw += bytes(pixel(x, y))

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))


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


def make_game(root, source_dir, authentic=True, unleashed=True, third=True, loader=True, long_names=False):
    """A fake Diablo II: Resurrected folder."""
    root.mkdir(parents=True, exist_ok=True)
    write(root / "D2R.exe", b"MZ fake D2R.exe " + "3.3.93787".encode("utf-16-le") + b"\0\0")
    write(root / "Data" / ".build.config", "fake")
    if loader:
        write(root / "D2RLoader.exe", "fake D2RLoader")
    write(root / "d2rloader" / "logs" / "d2rloader.log", "[x] D2RLoader v1.3.1-beta (Release, Release) started.\n[x] [BUILD] D2R version: 3.3.93787.\n")
    if authentic:
        write(root / "D2R_VR_Settings.exe", "fake settings app")
        write(root / "unins000.exe", "fake uninstaller")
        mod = root / "mods" / "D2R-VR"
        write(mod / "D2R-VR.mpq" / "modinfo.json", '{ "name": "D2R-VR", "savepath": "../", "description": "x" }')
        manifest = (source_dir / "examples" / "d2rloader-mods" / "d2r-vr" / "vrmod-manifest.json").read_text(encoding="utf-8")
        write(mod / "vrmod-manifest.json", manifest)
        write(mod / "launcher_icon.png", png_bytes(64, 64, lambda x, y: (200, 40 + x * 2, 40 + y * 2)))
        write(mod / "d2rloader" / "logs" / "d2r-vr-vrcam.log", "a log line\n")
    if unleashed:
        write(root / "mods" / "D2R-PCVR" / "D2R-PCVR.mpq" / "modinfo.json", '{ "name": "D2R-PCVR", "savepath": "../", "description": "x" }')
    for folder, days in (("D2R-VR", 10), ("D2R-PCVR", 5)):             # Authentic was installed first (the list is in install order)
        if (root / "mods" / folder).is_dir():
            set_created(root / "mods" / folder, days)
            set_created(root / "mods" / folder / (folder + ".mpq") / "modinfo.json", days)
    if third:
        name = "A Very Long Mod Folder Name That Will Not Fit On One Line Of The Card" if long_names else "Plain Third Mod"
        write(root / "mods" / name / (name + ".mpq") / "modinfo.json", '{ "name": "%s", "savepath": "../" }' % name)
        write(root / "mods" / "Another Mod" / "Another Mod.mpq" / "modinfo.json", '{ "name": "Another Mod", "savepath": "../" }')
    if long_names:
        # a mod whose vrmod-manifest.json is damaged: listed as a plain mod, with the reason as its note
        write(root / "mods" / "Broken Manifest" / "Broken Manifest.mpq" / "modinfo.json", '{ "name": "Broken Manifest", "savepath": "../" }')
        write(root / "mods" / "Broken Manifest" / "vrmod-manifest.json", "{ this is not json")


# ---------------------------------------------------------------- PNG

class Png:
    def __init__(self, path):
        data = Path(path).read_bytes()
        assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
        pos, idat = 8, b""
        while pos < len(data):
            n, kind = struct.unpack(">I4s", data[pos:pos + 8])
            body = data[pos + 8:pos + 8 + n]
            if kind == b"IHDR":
                self.w, self.h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
                assert depth == 8 and ctype in (2, 6) and interlace == 0, "unsupported PNG"
                self.bpp = 3 if ctype == 2 else 4
            elif kind == b"IDAT":
                idat += body
            pos += 12 + n
        raw = zlib.decompress(idat)
        stride = self.w * self.bpp
        rows, prev = [], bytearray(stride)
        for y in range(self.h):
            f = raw[y * (stride + 1)]
            line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
            for i in range(stride):
                a = line[i - self.bpp] if i >= self.bpp else 0
                b = prev[i]
                c = prev[i - self.bpp] if i >= self.bpp else 0
                if f == 1:
                    line[i] = (line[i] + a) & 255
                elif f == 2:
                    line[i] = (line[i] + b) & 255
                elif f == 3:
                    line[i] = (line[i] + ((a + b) >> 1)) & 255
                elif f == 4:
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                    line[i] = (line[i] + pr) & 255
            rows.append(line)
            prev = line
        self.rows = rows

    def px(self, x, y):
        r = self.rows[y]
        i = x * self.bpp
        return r[i], r[i + 1], r[i + 2]

    def count(self, pred, box=None):
        x0, y0, x1, y1 = box or (0, 0, self.w, self.h)
        n = 0
        for y in range(y0, min(y1, self.h)):
            for x in range(x0, min(x1, self.w)):
                if pred(*self.px(x, y)):
                    n += 1
        return n

    def colors(self):
        s = set()
        step = max(1, self.w // 160)
        for y in range(0, self.h, 2):
            for x in range(0, self.w, step):
                s.add(self.px(x, y))
        return len(s)


def is_red(r, g, b):
    return r > 90 and r > 2 * g and r > 2 * b


def is_accent(r, g, b):
    return abs(r - 15) < 30 and abs(g - 108) < 30 and abs(b - 189) < 30


def is_bg(r, g, b):
    return (r, g, b) == (243, 243, 243)


# ---------------------------------------------------------------- running the exe

def run(exe, args, timeout=RUN_TIMEOUT):
    p = subprocess.run([str(exe)] + EXTRA + [str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def shot(exe, name, shots, game, home, *extra):
    out = shots / (name + ".png")
    if out.exists():
        out.unlink()
    t = time.time()
    code, so, se = run(exe, ["--game", game, "--home", home, "--screenshot", out] + list(extra))
    return code, out, time.time() - t


# ---------------------------------------------------------------- driving the window (Win32 messages)

if sys.platform == "win32":
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.SendMessageW.restype = wintypes.LPARAM
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.GetDlgItem.restype = wintypes.HWND
    user32.IsWindowEnabled.argtypes = [wintypes.HWND]
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

WM_GETTEXT, WM_GETTEXTLENGTH, WM_CLOSE, WM_COMMAND, WM_KEYDOWN, WM_KEYUP = 0x0D, 0x0E, 0x10, 0x111, 0x100, 0x101
BM_CLICK, BM_GETCHECK, CB_GETCURSEL, CB_SETCURSEL, CB_GETLBTEXT, CB_GETLBTEXTLEN, CBN_SELCHANGE = 0xF5, 0xF0, 0x147, 0x14E, 0x148, 0x149, 1
# the control ids of gui_window.cpp (its enum starts at 100); the cards' radio buttons are 1000 + n, their links 2000 + n, children of the list
ID = dict(game_label=100, game_combo=101, game_browse=102, game_status=103, mods_label=104, mod_list=105, rt_label=106, rt_combo=107,
          rt_browse=108, rt_detail=109, close_check=110, play=111, play_reason=112, settings=113, logs=114, uninstall=115, about=116,
          message=117, log=118, install_view=119, update_check=120)


class Gui:
    """The launcher window, running, driven from outside with window messages (what a keyboard and a mouse would do)."""

    def __init__(self, exe, args, env=None):
        self.proc = subprocess.Popen([str(exe)] + EXTRA + [str(a) for a in args], env=env)
        self.hwnd = None
        deadline = time.time() + 20
        while time.time() < deadline and not self.hwnd:
            self.hwnd = self._find()
            if not self.hwnd:
                time.sleep(0.1)

    def _find(self):
        found = []

        @WNDENUMPROC
        def each(h, _):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value == self.proc.pid:
                cls = ctypes.create_unicode_buffer(64)
                user32.GetClassNameW(h, cls, 64)
                if cls.value == "D2RVRLauncherWindow":
                    found.append(h)
            return True

        user32.EnumWindows(each, 0)
        return found[0] if found else None

    def child(self, ident, parent=None):
        return user32.GetDlgItem(parent or self.hwnd, ID[ident] if isinstance(ident, str) else ident)

    def text(self, ident, parent=None):
        h = self.child(ident, parent)
        n = user32.SendMessageW(h, WM_GETTEXTLENGTH, 0, 0)
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.SendMessageW(h, WM_GETTEXT, n + 1, ctypes.cast(buf, ctypes.c_void_p).value)
        return buf.value

    def enabled(self, ident):
        return bool(user32.IsWindowEnabled(self.child(ident)))

    def checked(self, ident):
        return user32.SendMessageW(self.child(ident), BM_GETCHECK, 0, 0) == 1

    def post_click(self, ident, parent=None):
        """A click that opens a message box: posted, so this thread is not stuck in the box."""
        user32.PostMessageW(self.child(ident, parent), BM_CLICK, 0, 0)

    def click(self, ident, parent=None):
        user32.SendMessageW(self.child(ident, parent), BM_CLICK, 0, 0)

    def radio(self, n):
        return self.child(1000 + n, self.child("mod_list"))

    def click_radio(self, n):
        user32.SendMessageW(self.radio(n), BM_CLICK, 0, 0)

    def radio_checked(self, n):
        return user32.SendMessageW(self.radio(n), BM_GETCHECK, 0, 0) == 1

    def wait_for(self, cond, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            if cond():
                return True
            if self.proc.poll() is not None:
                return cond()
            time.sleep(0.1)
        return False

    def wait_ready(self):
        """The game line has its final text: the hashing of D2RLoader's files is over."""
        return self.wait_for(lambda: "checking" not in self.text("game_status"))

    def press_enter(self, ident="game_combo"):
        h = self.child(ident)
        user32.PostMessageW(h, WM_KEYDOWN, 0x0D, 0)
        user32.PostMessageW(h, WM_KEYUP, 0x0D, 0)

    def combo_select(self, ident, index):
        h = self.child(ident)
        user32.SendMessageW(h, CB_SETCURSEL, index, 0)
        user32.SendMessageW(self.hwnd, WM_COMMAND, (CBN_SELCHANGE << 16) | ID[ident], h)

    def combo_index(self, ident):
        return ctypes.c_int(user32.SendMessageW(self.child(ident), CB_GETCURSEL, 0, 0)).value

    def combo_text(self, ident, index):
        h = self.child(ident)
        n = user32.SendMessageW(h, CB_GETLBTEXTLEN, index, 0)
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.SendMessageW(h, CB_GETLBTEXT, index, ctypes.cast(buf, ctypes.c_void_p).value)
        return buf.value

    def tab_order(self, parent="main"):
        """The control ids Tab goes through, in order, in the window (or in the list): the enabled controls with the tab stop style."""
        user32.GetNextDlgTabItem.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.BOOL]
        user32.GetNextDlgTabItem.restype = wintypes.HWND
        user32.GetDlgCtrlID.argtypes = [wintypes.HWND]
        top = self.hwnd if parent == "main" else self.child(parent)
        ids, h = [], None
        for _ in range(60):
            h = user32.GetNextDlgTabItem(top, h, False)
            if not h:
                break
            ident = user32.GetDlgCtrlID(h)
            if ident in ids:
                break
            ids.append(ident)
        return ids

    def scroll_info(self, ident="mod_list"):
        """(range, page, position) of the list's vertical scroll bar."""
        class SCROLLINFO(ctypes.Structure):
            _fields_ = [("cbSize", wintypes.UINT), ("fMask", wintypes.UINT), ("nMin", ctypes.c_int), ("nMax", ctypes.c_int),
                        ("nPage", wintypes.UINT), ("nPos", ctypes.c_int), ("nTrackPos", ctypes.c_int)]

        si = SCROLLINFO(ctypes.sizeof(SCROLLINFO), 0x17)          # SIF_ALL
        user32.GetScrollInfo(self.child(ident), 1, ctypes.byref(si))      # SB_VERT
        return si.nMax - si.nMin + 1, si.nPage, si.nPos

    def rect(self, hwnd):
        r = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        return r.left, r.top, r.right, r.bottom

    def resize_client(self, w, h):
        r = wintypes.RECT(0, 0, w, h)
        user32.AdjustWindowRectEx(ctypes.byref(r), 0x00CF0000 | 0x02000000, False, 0)       # WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN
        user32.SetWindowPos(self.hwnd, None, 0, 0, r.right - r.left, r.bottom - r.top, 0x0002 | 0x0004 | 0x0010)    # NOMOVE | NOZORDER | NOACTIVATE

    def dialogs(self):
        """The message boxes this process has open (class #32770), as window handles."""
        found = []

        @WNDENUMPROC
        def each(h, _):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value == self.proc.pid and user32.IsWindowVisible(h):
                cls = ctypes.create_unicode_buffer(32)
                user32.GetClassNameW(h, cls, 32)
                if cls.value == "#32770":
                    found.append(h)
            return True

        user32.EnumWindows(each, 0)
        return found

    def wait_dialog(self, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            d = self.dialogs()
            if d:
                return d[0]
            time.sleep(0.1)
        return None

    def dialog_title(self, h):
        n = user32.SendMessageW(h, WM_GETTEXTLENGTH, 0, 0)
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.SendMessageW(h, WM_GETTEXT, n + 1, ctypes.cast(buf, ctypes.c_void_p).value)
        return buf.value

    def dialog_text(self, h):
        """The text of a message box: its static control (id 0xFFFF)."""
        s = user32.GetDlgItem(h, 0xFFFF)
        return self.dialog_title(s) if s else ""

    def answer(self, h, ident):
        user32.PostMessageW(h, WM_COMMAND, ident, 0)       # IDOK 1, IDNO 7

    def close(self, timeout=15):
        user32.PostMessageW(self.hwnd, WM_CLOSE, 0, 0)
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return None

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


def make_standin_mod(root):
    """A D2RLoader mod whose Play starts a copy of PING.EXE (a few seconds, no window) and whose Settings runs it too: the real start path,
    with nothing of Diablo or D2RLoader in the process list."""
    mod = root / "mods" / "StandIn"
    write(mod / "StandIn.mpq" / "modinfo.json", '{ "name": "StandIn", "savepath": "../" }')
    manifest = {
        "schema": "vrmod-manifest/1", "id": "standin", "name": "Stand-in Mod", "version": "1.0.0",
        "description": "A test mod: Play starts a copy of ping.exe.",
        "game": {"id": "d2r"},
        "d2rloader_mod": {"name": "StandIn"},
        "launch": {"options": [{"id": "default", "label": "Play", "kind": "exec", "exe": "{game}/standin.exe", "args": ["-n", "5", "127.0.0.1"],
                                "cwd": "{game}", "via": "direct", "runtimes": ["system", "steamvr", "vdxr", "file"]}]},
        "settings_app": {"exe": "{game}/standin.exe", "args": ["-n", "1", "127.0.0.1"]},
    }
    import json
    write(mod / "vrmod-manifest.json", json.dumps(manifest, indent=2))
    shutil.copy(r"C:\Windows\System32\PING.EXE", root / "standin.exe")


def tasklist_has(name):
    p = subprocess.run(["tasklist", "/FI", "IMAGENAME eq " + name, "/NH"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return name.lower() in p.stdout.decode("utf-8", "replace").lower()


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    exe = Path(sys.argv[1]).resolve()
    work = Path(sys.argv[2]).resolve()
    source_dir = Path(__file__).resolve().parent.parent
    if not exe.is_file():
        print("no such exe:", exe)
        return 2
    # A real game running (somebody is playing) must neither be disturbed nor change the answers: then the launcher is given a process list of
    # the test's own (--processes) and the stand-in game is a name on that list instead of a process.
    real_game = [n for n in ("D2R.exe", "D2RLoader.exe") if tasklist_has(n)]
    if real_game:
        EXTRA.extend(["--processes", "-"])
        print("note: %s is running on this PC; the launcher gets its own process list (--processes) for these checks" % ", ".join(real_game))

    if work.exists():
        shutil.rmtree(work)
    shots = work / "shots"
    shots.mkdir(parents=True)
    game = work / "game"
    make_game(game, source_dir)
    home = work / "home"
    home.mkdir()

    print("the window (screenshot)")
    code, out, secs = shot(exe, "main", shots, game, home)
    check(code == 0, "--screenshot exits 0 (got %s, %.1f s)" % (code, secs))
    check(out.is_file() and out.stat().st_size > 15000, "the PNG is there and not tiny (%s bytes)" % (out.stat().st_size if out.is_file() else 0))
    if out.is_file():
        png = Png(out)
        check(500 <= png.w <= 2600 and 560 <= png.h <= 2000, "the size is plausible (%dx%d)" % (png.w, png.h))
        check(png.colors() > 120, "many colours: pictures and text were drawn (%d)" % png.colors())
        check(png.count(is_red, (0, 0, png.w, png.h // 6)) > 100, "the header's red crystal is there")
        check(png.count(is_bg) > png.w * png.h // 5, "the window's grey is there")
        check(png.count(is_accent) > 200, "the accent blue of the selected card and Play is there")
    check(not (home / "settings.json").exists(), "a screenshot writes no settings.json")

    print("play, dry run")
    code, so, se = run(exe, ["--game", game, "--home", home, "--select", "d2r-vr", "--play", "--dry-run"])
    check(code == 0, "Authentic: exit 0 (got %s) %s" % (code, so.strip().splitlines()[-2:] if code else ""))
    gm = str(game).replace("\\", "/")
    check("launch d2r-vr" in so and "D2R_VR_Settings.exe" in so, "Authentic: the plan runs D2R_VR_Settings.exe")
    check("arg   --start" in so, "Authentic: with --start")
    check("warning:" in so, "Authentic: D2RLoader's files are not the reviewed ones (warning)")
    last = home / "logs" / "gui_last.txt"
    check(last.is_file() and "D2R_VR_Settings.exe" in last.read_text(encoding="utf-8"), "the outcome is also in logs/gui_last.txt")
    code, so, se = run(exe, ["--game", game, "--home", home, "--select", "d2r-pcvr", "--play", "--dry-run"])
    check(code == 0, "Unleashed: exit 0 (got %s)" % code)
    check("launch d2r-pcvr" in so and "D2RLoader.exe" in so, "Unleashed: the plan runs D2RLoader.exe")
    check("arg   -mod" in so and "arg   D2R-PCVR" in so, "Unleashed: with -mod D2R-PCVR")
    check("SteamAppId=2536520" in so, "Unleashed: the Steam edition's ids are set")
    check(not (home / "settings.json").exists(), "a dry run writes no settings.json")
    code, so, se = run(exe, ["--game", game, "--home", home, "--select", "nope", "--play", "--dry-run"])
    check(code == 3 and "E_NOT_FOUND" in so, "an unknown --select: E_NOT_FOUND, exit 3 (got %s)" % code)

    print("first run, empty home")
    fresh = work / "home_first_run"
    code, out, secs = shot(exe, "first_run", shots, game, fresh)
    check(code == 0 and out.is_file(), "the first run opens (exit %s)" % code)
    check(not (fresh / "settings.json").exists(), "and remembers nothing yet")

    print("a mod that is not installed")
    game2 = work / "game_no_unleashed"
    make_game(game2, source_dir, unleashed=False)
    code, so, se = run(exe, ["--game", game2, "--home", home, "--select", "d2r-pcvr", "--play", "--dry-run"])
    check(code == 11 and "E_MOD_NOT_INSTALLED" in so, "Play is refused: E_MOD_NOT_INSTALLED, exit 11 (got %s)" % code)
    code, out, secs = shot(exe, "not_installed", shots, game2, home, "--select", "d2r-pcvr")
    check(code == 0 and out.is_file(), "the window shows it (exit %s)" % code)

    print("D2RLoader missing")
    game3 = work / "game_no_loader"
    make_game(game3, source_dir, loader=False)
    code, so, se = run(exe, ["--game", game3, "--home", home, "--select", "d2r-vr", "--play", "--dry-run"])
    check(code == 11 and "E_LOADER_MISSING" in so, "Play is refused: E_LOADER_MISSING, exit 11 (got %s)" % code)
    code, out, secs = shot(exe, "no_loader", shots, game3, home)
    check(code == 0 and out.is_file(), "the window shows it (exit %s)" % code)

    print("a folder that is not a game, and no game at all")
    empty = work / "not_a_game"
    empty.mkdir()
    code, so, se = run(exe, ["--game", empty, "--home", home, "--play", "--dry-run"])
    check(code == 25 and "E_GAME" in so, "--play: E_GAME, exit 25 (got %s)" % code)
    code, out, secs = shot(exe, "not_a_game", shots, empty, home)
    check(code == 0 and out.is_file(), "the window says so (exit %s)" % code)
    code, so, se = run(exe, ["--home", work / "home_nogame", "--no-search", "--play", "--dry-run"])
    check(code == 25, "--no-search and nothing remembered: no game, exit 25 (got %s)" % code)
    out = shots / "no_game.png"
    code, so, se = run(exe, ["--home", work / "home_nogame", "--no-search", "--screenshot", out])
    check(code == 0 and out.is_file(), "the window with no game at all offers Browse (exit %s)" % code)

    print("many and long mod names")
    game4 = work / "game_long"
    make_game(game4, source_dir, long_names=True)
    code, out, secs = shot(exe, "long_names", shots, game4, home)
    check(code == 0 and out.is_file(), "five mods, one with a very long name (exit %s)" % code)

    print("sizes")
    for name, size in (("size_min", "560x600"), ("size_short", "640x640"), ("size_tall", "700x1000"), ("size_wide", "1100x800")):
        code, out, secs = shot(exe, name, shots, game4, home, "--size", size)
        ok = code == 0 and out.is_file()
        check(ok, "%s: exit %s" % (size, code))
        if ok:
            png = Png(out)
            check(png.count(is_red, (0, 0, png.w, png.h // 6)) > 100 and png.count(is_accent) > 200, "%s: header and Play are drawn (%dx%d)" % (size, png.w, png.h))

    print("DPI")
    sizes = {}
    for dpi in (96, 120, 144, 192):
        code, out, secs = shot(exe, "dpi_%d" % dpi, shots, game, home, "--dpi", dpi)
        check(code == 0 and out.is_file(), "%d dpi: exit %s" % (dpi, code))
        if out.is_file():
            png = Png(out)
            sizes[dpi] = png.w
            check(png.count(is_red, (0, 0, png.w, png.h // 5)) > 60 * dpi * dpi // 9216, "%d dpi: the crystal is drawn at that size" % dpi)
    if 96 in sizes and 192 in sizes:
        check(abs(sizes[192] - 2 * sizes[96]) <= 24, "192 dpi is twice as wide as 96 (%s vs %s)" % (sizes[192], sizes[96]))

    print("driving the window: choosing a mod, a runtime")
    drive_home = work / "home_drive"
    gui = Gui(exe, ["--game", game2, "--home", drive_home])
    try:
        check(gui.hwnd is not None, "the window opens")
        if gui.hwnd:
            check(gui.wait_ready(), "D2RLoader's files are checked in the background, then the game line is final")
            check(gui.text("message") == "Choose a mod and press Play.", "the first message (%r)" % gui.text("message"))
            check(gui.enabled("play") and gui.text("play_reason") == "", "Play is on for the preselected Authentic")
            check(gui.radio_checked(0) and not gui.radio_checked(1), "Authentic's radio button is the checked one")
            check(gui.tab_order() == [101, 102, 1000, 3001, 4000, 2000, 2001, 107, 111, 113, 110, 120, 119, 114, 116, 118],
                  "Tab visits game folder, Browse, the checked card, the Install button of the card that is not installed, the Uninstall button of the installed one, the Website links, runtime, Play, the mod's Settings, the two checkboxes, Install / update mods, Logs, About, the log (%s)" % gui.tab_order())
            check(gui.tab_order("mod_list") == [1000, 3001, 4000, 2000, 2001], "the list gives Tab the checked radio button (the arrow keys move between the cards), the Install button and the Website links (%s)" % gui.tab_order("mod_list"))
            check(gui.text("install_view").replace("&", "") == "Install / update mods..." and gui.enabled("install_view"), "the button 'Install / update mods...' is there and on")
            # the mod's settings button: named for the mod, as big as Play, right after it on the same row; the two checkboxes are below that row
            check(gui.text("settings").replace("&", "") == "D2R VR Authentic Settings" and gui.enabled("settings"), "the mod's settings button says 'D2R VR Authentic Settings' and is on (%r)" % gui.text("settings"))
            pr, sr = gui.rect(gui.child("play")), gui.rect(gui.child("settings"))
            check(pr[3] - pr[1] == sr[3] - sr[1] and pr[1] == sr[1] and 0 < sr[0] - pr[2] <= 24 and sr[2] - sr[0] >= pr[2] - pr[0],
                  "...as high as Play, on its row, right after it, not narrower (Play %s, Settings %s)" % (pr, sr))
            cr, ur, ir = gui.rect(gui.child("close_check")), gui.rect(gui.child("update_check")), gui.rect(gui.child("install_view"))
            check(cr[1] >= pr[3] and ur[1] >= pr[3] and ir[1] > max(cr[3], ur[3]) - 1, "...the two checkboxes are under the Play row, above the row of small buttons (%s %s %s)" % (cr, ur, ir))
            check(not [h for h in (gui.rect(gui.child("logs")), gui.rect(gui.child("about"))) if h[1] != ir[1]] and not gui.child("uninstall"), "...Logs and About stay on the bottom row; Settings and the old bottom-row Uninstall are not there")
            check(gui.text(3001, gui.child("mod_list")) == "Install" and "Website" in gui.text(2000, gui.child("mod_list")), "the card that is not installed says Install, the cards keep a Website link")
            gui.click_radio(1)
            check(gui.wait_for(lambda: not gui.enabled("play")), "choosing Unleashed (not installed in this folder) turns Play off")
            check("not installed" in gui.text("play_reason"), "and says why (%r)" % gui.text("play_reason"))
            check(gui.radio_checked(1) and not gui.radio_checked(0), "the radio buttons follow")
            check(not gui.enabled("settings") and gui.text("settings").replace("&", "") == "Settings", "a mod with no settings app: the big button is a greyed 'Settings' (%r)" % gui.text("settings"))
            gui.click_radio(0)
            check(gui.wait_for(lambda: gui.enabled("play")) and gui.text("play_reason") == "", "back to Authentic: Play is on again")
            for idx in (1, 2):                         # SteamVR, Virtual Desktop: whichever of them this PC does not have cannot be chosen
                label = gui.combo_text("rt_combo", idx)
                gui.combo_select("rt_combo", idx)
                gui.wait_for(lambda: False, 0.4)
                if label.endswith("(not installed)"):
                    check(gui.combo_index("rt_combo") == 0 and "not installed" in gui.text("message"), "%r is not installed: the choice stays on the default and says so" % label)
                else:
                    check(gui.combo_index("rt_combo") == idx, "%r can be chosen" % label)
                    gui.combo_select("rt_combo", 0)
            check(gui.close() == 0, "the window closes with exit code 0")
    finally:
        gui.kill()

    print("the list of mods scrolls when it does not fit, and not otherwise")
    gui = Gui(exe, ["--game", game4, "--home", work / "home_scroll", "--size", "560x600", "--processes", "-"])
    try:
        check(gui.hwnd is not None, "the window opens at the smallest size")
        if gui.hwnd:
            gui.wait_ready()
            total, page, pos = gui.scroll_info()
            check(total > page and pos == 0, "five cards do not fit: the list has a scroll bar (%d of %d)" % (page, total))
            user32.SendMessageW(gui.child("mod_list"), 0x115, 7, 0)             # WM_VSCROLL, SB_BOTTOM
            gui.wait_for(lambda: gui.scroll_info()[2] > 0, 3)
            total, page, pos = gui.scroll_info()
            check(pos > 0 and pos + page >= total - 2, "scrolled to the bottom (%d + %d of %d)" % (pos, page, total))
            lst, last = gui.rect(gui.child("mod_list")), gui.rect(gui.radio(4))
            check(lst[1] <= last[1] and last[3] <= lst[3], "the last card's radio button is inside the list's view now")
            gui.resize_client(700, 1100)
            gui.wait_for(lambda: gui.scroll_info()[0] <= gui.scroll_info()[1], 5)
            total, page, pos = gui.scroll_info()
            check(total <= page and pos == 0, "in a tall window everything fits and the scroll bar is gone (%d of %d)" % (page, total))
            first, last = gui.rect(gui.radio(0)), gui.rect(gui.radio(4))
            lst = gui.rect(gui.child("mod_list"))
            check(lst[1] <= first[1] and last[3] <= lst[3], "all five radio buttons are inside the list")
            check(gui.close() == 0, "closes with exit code 0")
    finally:
        gui.kill()

    print("Play from the window: dry run, then Enter")
    gui = Gui(exe, ["--game", game, "--home", work / "home_dry", "--select", "d2r-pcvr", "--dry-run", "--processes", "-"])
    try:
        check(gui.hwnd is not None, "the window opens (title says dry run)")
        if gui.hwnd:
            gui.wait_ready()
            check(gui.radio_checked(1), "--select d2r-pcvr: Unleashed is the checked card")
            gui.press_enter()
            check(gui.wait_for(lambda: gui.text("message").startswith("Dry run:")), "Enter plays: %r" % gui.text("message"))
            log = gui.text("log")
            check("D2RLoader.exe" in log and "-mod" in log and "D2R-PCVR" in log, "the plan is in the log")
            check(gui.close() == 0, "closes with exit code 0")
            check(not (work / "home_dry" / "settings.json").exists(), "a dry run remembers nothing")
    finally:
        gui.kill()


    print("the cards' Uninstall buttons: Run, Explain, none, refusals")
    la_empty = work / "la_empty"
    la_full = work / "la_full"
    (la_empty / "Programs").mkdir(parents=True, exist_ok=True)
    (la_full / "Programs" / "D2R PCVR").mkdir(parents=True, exist_ok=True)
    (la_full / "Programs" / "D2R PCVR" / "unins000.exe").write_bytes(b"fake uninstaller of D2R VR Unleashed")
    game_u = work / "game_uninstall"
    make_game(game_u, source_dir)
    before = sorted((str(p.relative_to(game_u)), p.stat().st_size) for p in game_u.rglob("*") if p.is_file())

    def env_with(la):
        e = dict(os.environ)
        e["LOCALAPPDATA"] = str(la)
        return e

    # Unleashed has no uninstaller on this PC (a ZIP install): the button explains, nothing is run
    gui = Gui(exe, ["--game", game_u, "--home", work / "home_uninst1", "--processes", "-"], env=env_with(la_empty))
    try:
        check(gui.hwnd is not None, "the window opens (Authentic and Unleashed installed, no uninstaller of Unleashed)")
        if gui.hwnd:
            gui.wait_ready()
            log_text = gui.text("log")
            check("D2R VR Authentic 0.169.0 (installed, manifest)" in log_text, "the log names the version found in Authentic's own manifest (0.169.0)")
            check("D2R VR Unleashed, version not known (installed)" in log_text,
                  "...and says the version of Unleashed is not known: its Setup writes no manifest and the built-in profile's version is not what is installed (the cards say 'Version not known')")
            lst = gui.child("mod_list")
            ba, bu = gui.child(4000, lst), gui.child(4001, lst)
            check(ba and bu and gui.text(4000, lst) == "Uninstall" and gui.text(4001, lst) == "Uninstall", "both installed cards have an Uninstall button")
            check(not gui.child(4002, lst) and not gui.child(4003, lst), "the other (unknown) D2RLoader mods have none: the launcher never removes what it did not install")
            ra, rb = gui.rect(ba), gui.rect(gui.child(1000, lst))
            check(ra[2] - ra[0] > 40 and ra[1] > rb[1], "...the button sits in the card, under its status")
            gui.post_click(4001, lst)
            d = gui.wait_dialog()
            check(d is not None and "Uninstall D2R VR Unleashed" in gui.dialog_title(d), "Unleashed's button opens a message")
            txt = gui.dialog_text(d) if d else ""
            check("no uninstaller" in txt and "delete the folder" in txt and "never deletes a mod's folder" in txt and "D2R-PCVR" in txt, "...it says there is no uninstaller and how to remove the mod by hand (%r)" % txt[:90])
            if d:
                user32.PostMessageW(d, WM_CLOSE, 0, 0)       # an OK-only message box ends with its close button
            check(gui.wait_for(lambda: not gui.dialogs(), 5), "the message closes (still open: %r)" % [gui.dialog_title(h) + "|" + gui.dialog_text(h)[:40] for h in gui.dialogs()])
            check(gui.close() == 0, "closes with exit code 0")
    finally:
        gui.kill()
    after = sorted((str(p.relative_to(game_u)), p.stat().st_size) for p in game_u.rglob("*") if p.is_file())
    check(before == after, "nothing in the game folder changed (the launcher deletes no mod folder)")

    # Unleashed's own uninstaller is there: a question that names the mod; No runs nothing
    gui = Gui(exe, ["--game", game_u, "--home", work / "home_uninst2", "--processes", "-"], env=env_with(la_full))
    try:
        check(gui.hwnd is not None, "the window opens (Unleashed's uninstaller is in its usual place)")
        if gui.hwnd:
            gui.wait_ready()
            lst = gui.child("mod_list")
            gui.post_click(4001, lst)
            d = gui.wait_dialog()
            title = gui.dialog_title(d) if d else ""
            txt = gui.dialog_text(d) if d else ""
            check(d is not None and title == "Uninstall D2R VR Unleashed" and txt.startswith("Uninstall D2R VR Unleashed?") and "D2R PCVR" in txt and "unins000.exe" in txt and "its own questions" in txt,
                  "the question names the mod and its uninstaller (%r)" % txt[:70])
            if d:
                gui.answer(d, 7)
            check(gui.wait_for(lambda: not gui.dialogs()), "No closes it")
            check("Running the uninstaller" not in gui.text("log"), "...and nothing was run")
            gui.post_click(4000, lst)
            d = gui.wait_dialog()
            check(d is not None and gui.dialog_title(d) == "Uninstall D2R VR Authentic" and "unins000.exe" in gui.dialog_text(d), "Authentic's button asks about its own uninstaller")
            if d:
                gui.answer(d, 7)
            check(gui.close() == 0, "closes with exit code 0")
    finally:
        gui.kill()

    # the game runs: refused, no question
    gui = Gui(exe, ["--game", game_u, "--home", work / "home_uninst3", "--processes", "D2R.exe"], env=env_with(la_full))
    try:
        check(gui.hwnd is not None, "the window opens while the game runs")
        if gui.hwnd:
            gui.wait_ready()
            lst = gui.child("mod_list")
            gui.post_click(4000, lst)
            check(gui.wait_for(lambda: "The game is running" in gui.text("message") and "Save & Exit" in gui.text("message")), "Uninstall is refused while the game runs: %r" % gui.text("message")[:80])
            time.sleep(0.5)
            check(not gui.dialogs(), "...with no question")
            check(gui.close() == 0, "closes with exit code 0")
    finally:
        gui.kill()

    print("Play from the window: a real start (a stand-in mod that runs ping.exe)")
    game5 = work / "game_standin"
    make_game(game5, source_dir)
    make_standin_mod(game5)
    home5 = work / "home_standin"
    try:
        gui = Gui(exe, ["--game", game5, "--home", home5, "--select", "standin", "--processes", "-"])
        try:
            check(gui.hwnd is not None, "the window opens")
            if gui.hwnd:
                gui.wait_ready()
                check(gui.radio_checked(4), "--select standin: the stand-in card is the checked one")
                check(gui.enabled("settings") and gui.enabled("logs") is False and not gui.child(4004), "Settings is on, Logs is off and the other mod has no Uninstall button (the mod has neither)")
                gui.click("settings")
                check(gui.wait_for(lambda: "Opened the settings" in gui.text("log")), "Settings starts the mod's settings app")
                gui.click("play")
                check(gui.wait_for(lambda: gui.text("message").startswith("Started Stand-in Mod 1.0.0")), "Play starts it: %r" % gui.text("message")[:70])
                check(gui.wait_for(lambda: tasklist_has("standin.exe"), 6), "the stand-in process is running")
                check(not gui.enabled("play") and gui.text("play_reason") == "Starting the game...", "Play is off while the game starts (%r)" % gui.text("play_reason"))
                s = (home5 / "settings.json").read_text(encoding="utf-8") if (home5 / "settings.json").exists() else ""
                check('"last_mod": "standin"' in s.replace('":"', '": "'), "settings.json remembers the mod")
                check('"game": ""' in s.replace('":"', '": "'), "but not the game folder (it was given with --game)")
                check("started standin" in (home5 / "logs" / "launcher.log").read_text(encoding="utf-8"), "launcher.log says it was started")
                check(gui.close() == 0, "closes with exit code 0")
        finally:
            gui.kill()

        print("Play from the window: close the launcher when the game starts")
        home6 = work / "home_close"
        gui = Gui(exe, ["--game", game5, "--home", home6, "--select", "standin", "--processes", "-"])
        try:
            check(gui.hwnd is not None, "the window opens")
            if gui.hwnd:
                gui.wait_ready()
                gui.click("close_check")
                check(gui.checked("close_check"), "the box is ticked")
                s = (home6 / "settings.json").read_text(encoding="utf-8") if (home6 / "settings.json").exists() else ""
                check('"close_on_start": true' in s.replace('":"', '": "'), "and remembered at once")
                gui.click("play")
                try:
                    code = gui.proc.wait(15)
                except subprocess.TimeoutExpired:
                    code = None
                check(code == 0, "the window closed itself after the start (exit %s)" % code)
        finally:
            gui.kill()
    finally:
        subprocess.run(["taskkill", "/F", "/IM", "standin.exe"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    print("the game is running")
    proc = None
    running_args = []
    if real_game:
        running_args = ["--processes", "D2R.exe"]      # a name on the test's own list; the real process list is not read
    else:
        # a process really named D2R.exe: a copy of PING.EXE, which sleeps for a minute
        stand_in = work / "proc" / "D2R.exe"
        stand_in.parent.mkdir()
        shutil.copy(r"C:\Windows\System32\PING.EXE", stand_in)
        proc = subprocess.Popen([str(stand_in), "-n", "60", "127.0.0.1"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if proc:
            for _ in range(40):
                if tasklist_has("D2R.exe"):
                    break
                time.sleep(0.25)
            check(tasklist_has("D2R.exe"), "the stand-in D2R.exe is running")
        code, so, se = run(exe, ["--game", game, "--home", home, "--select", "d2r-vr", "--play", "--dry-run"] + running_args)
        check(code == 10 and "E_GAME_RUNNING" in so, "Play is refused: E_GAME_RUNNING, exit 10 (got %s)" % code)
        code, out, secs = shot(exe, "game_running", shots, game, home, *running_args)
        check(code == 0 and out.is_file(), "the window shows it (exit %s)" % code)
    finally:
        if proc:
            proc.kill()
            proc.wait()

    print()
    print("screenshots: %s" % shots)
    if FAILS:
        print("%d check(s) failed" % len(FAILS))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
