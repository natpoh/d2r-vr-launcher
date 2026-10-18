#!/usr/bin/env python3
"""The install commands of the launcher against a LOCAL web server and a fake game folder: no internet, no real game, no real installer.

    install_smoke.py --cli vrmodlauncher.exe --fake-setup fake_setup.exe --work <empty work folder>

A Python http.server on 127.0.0.1 plays all three sources: a synthetic D2RLoader ZIP (its three files hashed and passed to the CLI as a test pin),
our version API (with and without a sha256), a GitHub-style releases list, and installers that are copies of fake_setup.exe (a stand-in Setup that
writes a marker and creates mods\\<Mod>\\<Mod>.mpq\\modinfo.json in the game folder). The CLI is told about the server by the test switches
VRML_ALLOW_HTTP_HOST / VRML_URL_* / --pin-file, which the launcher honours only together (production code never does plain http).

Scenarios: install loader (fresh, repair with backup, hash mismatch writes nothing, the player's toml kept, game running refused, protected folder,
bad archive names, dry run), redirects (on the list, off the list, too many), size cap, a cancelled and a truncated download, `releases`, install
authentic (sha enforced / missing / accepted / wrong / cancelled Setup / Setup that installed elsewhere / address off our host / size cap),
install unleashed (digest vs .sha256 mismatch refused, a good one, a file that is not what the digest says).
"""
import argparse
import hashlib
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
ap.add_argument("--cli", required=True)
ap.add_argument("--fake-setup", required=True)
ap.add_argument("--work", required=True)
a = ap.parse_args()
CLI = str(Path(a.cli).resolve())
FAKE_SETUP = Path(a.fake_setup).resolve()
WORK = Path(a.work).resolve()
if len(str(WORK)) > 80:   # the install writes <game>\.vrml-install-<stamp>\d2rloader\config\d2rloader.toml.vrml-tmp: a deep work folder (a build folder in a long path) would pass MAX_PATH
    import tempfile
    WORK = Path(tempfile.gettempdir()).resolve() / "vrml_install_smoke"
HERE = Path(__file__).resolve().parent.parent
failures = []


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
    return {p.relative_to(root).as_posix(): sha_file(p) for p in sorted(root.rglob("*")) if p.is_file()}


# ------------------------------------------------------------------ the server

LOCK = threading.Lock()
HITS = []                       # every path requested
STATIC = {}                     # path -> bytes
ROUTES = {}                     # path -> function(handler)
EVENTS = {}                     # things a handler wants the test to know


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


def redirect_to(target):
    def fn(h):
        h.reply(302, b"", {"Location": target})
    return fn


# ------------------------------------------------------------------ the fake game, the pin, the sources

rng = random.Random(20261018)
REAL = {"D2RLoader.exe": rng.randbytes(40000), "D2RCore.dll": rng.randbytes(30000), "d2rloader.mpq": rng.randbytes(300000)}
TOML_DEFAULT = b"[d2rloader.advanced]\n# shipped default\n"


def loader_zip(files=None, extra=None, level=6):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=level) as z:
        z.writestr(zipfile.ZipInfo("mods/"), b"")
        for n, d in (files or REAL).items():
            z.writestr(n, d)
        z.writestr("d2rloader/config/d2rloader.toml", TOML_DEFAULT)
        for n, d in (extra or {}).items():
            info = zipfile.ZipInfo("x")
            info.filename = n                                    # kept exactly as given (zipfile would clean a name given to writestr)
            z.writestr(info, d)
    return buf.getvalue()


GOOD_ZIP = loader_zip()


def write_pin(zip_bytes, name="pin.json", files=None):
    p = WORK / name
    p.write_text(json.dumps({
        "version": "9.9.9-test",
        "url": f"{BASE}/loader/L.zip",
        "file": "D2RLoader-test.zip",
        "sha256": [sha(zip_bytes)],
        "expected": [{"path": n, "sha256_any_of": [sha(d)]} for n, d in (files or REAL).items()],
    }), encoding="utf-8")
    return p


def make_game(name):
    game = WORK / name / "Diablo II Resurrected"            # a name with spaces, like the real one: the /DIR argument must survive it
    exe = b"MZ-fake-D2R.exe " + "3.3.93787".encode("utf-16-le")
    for rel, data in {"D2R.exe": exe, "Data/.build.config": b"x", "ReShade64.dll": b"reshade", "Saved/Settings.json": b"{}",
                      "d2rloader/plugins/other_plugin.dll": b"someone else's plugin"}.items():
        (game / rel).parent.mkdir(parents=True, exist_ok=True)
        (game / rel).write_bytes(data)
    return game, WORK / name / "home"


def installer(version, tag=b""):
    return FAKE_SETUP.read_bytes() + b"\0INSTALLER " + version.encode() + tag


def manifest_for(mod_id, version):
    base = json.loads((HERE / "examples" / "d2rloader-mods" / ("d2r-vr" if mod_id == "d2r-vr" else "d2r-pcvr") / "vrmod-manifest.json").read_text(encoding="utf-8"))
    base["version"] = version
    p = WORK / f"manifest_{mod_id}_{version}.json"
    p.write_text(json.dumps(base), encoding="utf-8")
    return p


def env_for(extra=None, loader_url=None):
    env = dict(os.environ)
    env["VRML_ALLOW_HTTP_HOST"] = f"127.0.0.1:{PORT}"
    env["VRML_URL_AUTHENTIC_API"] = f"{BASE}/api/version"
    env["VRML_URL_UNLEASHED_API"] = f"{BASE}/gh/releases"
    env["VRML_URL_LOADER"] = loader_url or f"{BASE}/loader/L.zip"
    env["VRML_TEST_NO_ELEVATE"] = "1"                       # the stand-in Setup must not raise a UAC prompt
    env["FAKE_SETUP_LOG"] = str(WORK / "fake_setup.log")
    env.update(extra or {})
    return env


def run(game, home, *args, env=None, show=True, timeout=90):
    cmd = [CLI, "--home", str(home), *([] if game is None else ["--game", str(game)]), *args]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env or env_for(), timeout=timeout)
    if show:
        print(f"\n$ vrmodlauncher {' '.join(str(x) for x in args)}   [exit {r.returncode}]")
        lines = (r.stdout + r.stderr).rstrip().splitlines()
        print("\n".join("    " + l for l in lines[:12]) + (f"\n    ... ({len(lines) - 12} more lines)" if len(lines) > 12 else ""))
    return r


def parts(root):
    return [str(p) for p in root.rglob("*") if p.name.endswith(".vrml-part") or p.name.endswith(".vrml-tmp") or p.name.startswith(".vrml-install-")] if root.exists() else []


def tasklist(name):
    r = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    return name.lower() in r.lower()


if tasklist("D2R.exe") or tasklist("D2RLoader.exe"):
    # the launcher's own "is the game running" check looks at the whole machine, so every install here would be refused: not a failure, a skip (ctest: 77)
    print("SKIPPED: D2R.exe or D2RLoader.exe is running on this machine; close the game and run again.")
    sys.exit(77)
if WORK.exists():
    shutil.rmtree(WORK)
WORK.mkdir(parents=True)
STATIC["/loader/L.zip"] = GOOD_ZIP

# ================================================================== 1. D2RLoader
print("\n=== 1. install loader")
game, home = make_game("loader")
pin = write_pin(GOOD_ZIP)
before = snap(game)
r = run(game, home, "install", "loader", "--dry-run", "--pin-file", str(pin))
check(r.returncode == 0 and f"{BASE}/loader/L.zip" in r.stdout and sha(GOOD_ZIP) in r.stdout, "dry run: says where it would download from and the expected hash (exit 0)")
check(hits("/loader/L.zip") == 0 and snap(game) == before and not (home / "deps").exists(), "dry run: nothing downloaded, nothing written")

r = run(game, home, "install", "loader", "--pin-file", str(pin))
check(r.returncode == 0 and hits("/loader/L.zip") == 1, "fresh install: exit 0, one download")
check(all((game / n).read_bytes() == d for n, d in REAL.items()), "...the three files are in the game folder, byte for byte")
check((game / "d2rloader/config/d2rloader.toml").read_bytes() == TOML_DEFAULT, "...d2rloader.toml was absent: the shipped default was written")
check((game / "mods").is_dir() and not parts(game) and not parts(home), "...mods\\ exists, no temporary file or folder is left")
check(all(snap(game).get(k) == v for k, v in before.items()), "...the game's own files are untouched")
j = json.loads(run(game, home, "mods", "--json", show=False).stdout)
check(j["d2rloader"]["present"] is True, "...`mods` sees D2RLoader.exe")

(game / "d2rloader/config/d2rloader.toml").write_text("[mine]\ndefault_mod = \"D2R-VR\"\n", encoding="utf-8")
after_first = snap(game)
r = run(game, home, "install", "loader", "--pin-file", str(pin))
check(r.returncode == 0 and "already installed" in r.stdout and hits("/loader/L.zip") == 1 and snap(game) == after_first, "second run: 'already installed', no download, nothing changes")

(game / "D2RCore.dll").write_bytes(b"damaged by something")
(game / "D2RLoader.exe").write_bytes(b"an old loader")
r = run(game, home, "install", "loader", "--pin-file", str(pin))
check(r.returncode == 0 and hits("/loader/L.zip") == 1, "repair: exit 0, the verified archive came from the cache (still one download)")
check((game / "D2RCore.dll").read_bytes() == REAL["D2RCore.dll"] and (game / "D2RLoader.exe").read_bytes() == REAL["D2RLoader.exe"], "...the damaged files are replaced")
check((game / "d2rloader/config/d2rloader.toml").read_text(encoding="utf-8").startswith("[mine]"), "...the player's d2rloader.toml is kept")
backs = sorted((home / "backups").glob("d2rloader-*"))
check(len(backs) == 1 and (backs[0] / "D2RCore.dll").read_bytes() == b"damaged by something" and (backs[0] / "D2RLoader.exe").read_bytes() == b"an old loader",
      "...the replaced files are in backups\\d2rloader-<time>\\")
check(not (backs[0] / "d2rloader.mpq").exists(), "...a file that was already right is not backed up")

# hash mismatch: the archive hash is the pinned one (so it is accepted) but a file inside is not the reviewed one
game, home = make_game("loader_tampered")
tampered = loader_zip({**REAL, "D2RCore.dll": b"TAMPERED" * 100})
STATIC["/loader/tampered.zip"] = tampered
(game / "d2rloader.mpq").write_bytes(b"mine")
before = snap(game)
r = run(game, home, "install", "loader", "--pin-file", str(write_pin(tampered, "pin_t.json", REAL)), env=env_for(loader_url=f"{BASE}/loader/tampered.zip"))
check(r.returncode == 23 and "E_HASH" in r.stderr, "an unpacked file that is not the reviewed one: exit 23")
check(snap(game) == before and not parts(game), "...nothing was written to the game folder, no temporary folder left")
# the archive itself is not the pinned one
r = run(game, home, "install", "loader", "--pin-file", str(pin), env=env_for(loader_url=f"{BASE}/loader/tampered.zip"))
check(r.returncode == 23 and snap(game) == before, "an archive with another hash than the pinned one: exit 23, nothing written")
check(not (home / "deps" / "d2rloader" / "D2RLoader-test.zip").exists(), "...and the bad download was deleted from the cache")

# an archive with a name it may not hold
for label, extra in (("a file outside the allowed names", {"evil.dll": b"x"}), ("a zip-slip name", {"../escape.txt": b"x"}), ("a backslash name", {"..\\escape.txt": b"x"})):
    game, home = make_game("loader_names")
    z = loader_zip(extra=extra)
    STATIC["/loader/names.zip"] = z
    before = snap(game)
    r = run(game, home, "install", "loader", "--pin-file", str(write_pin(z, "pin_n.json")), env=env_for(loader_url=f"{BASE}/loader/names.zip"), show=False)
    check(r.returncode == 26 and "E_ARCHIVE" in r.stderr and snap(game) == before and not (game.parent / "escape.txt").exists(), f"archive with {label}: exit 26, nothing written")

# the game is running
game, home = make_game("loader_running")
proc_dir = WORK / "proc"
proc_dir.mkdir()
shutil.copy2(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "PING.EXE", proc_dir / "D2R.exe")
proc = subprocess.Popen([str(proc_dir / "D2R.exe"), "-n", "60", "127.0.0.1"], stdout=subprocess.DEVNULL)
time.sleep(0.7)
n = hits("/loader/L.zip")
before = snap(game)
r = run(game, home, "install", "loader", "--pin-file", str(pin))
check(r.returncode == 10 and "E_GAME_RUNNING" in r.stderr and hits("/loader/L.zip") == n and snap(game) == before, "D2R.exe running: refused (exit 10), nothing downloaded or written")
proc.kill()
proc.wait()

# not a game folder
notgame = WORK / "notgame"
notgame.mkdir()
r = run(notgame, WORK / "home_ng", "install", "loader", "--pin-file", str(pin), show=False)
check(r.returncode == 25 and not any(notgame.iterdir()), "a folder without D2R.exe: exit 25")

# a protected game folder (a deny rule stands in for Program Files)
game, home = make_game("loader_protected")
user = os.environ.get("USERNAME", "")
before = snap(game)
denied = subprocess.run(["icacls", str(game), "/deny", f"{user}:(OI)(CI)(WD,AD)"], capture_output=True, text=True)
try:
    if denied.returncode != 0:
        print("    (icacls could not make a protected folder here: skipped)")
    else:
        r = run(game, home, "install", "loader", "--pin-file", str(pin))
        check(r.returncode == 20 and "administrator rights are needed" in r.stderr, "a protected game folder: exit 20 'administrator rights are needed for this folder'")
finally:
    subprocess.run(["icacls", str(game), "/remove:d", user], capture_output=True, text=True)
if denied.returncode == 0:
    check(snap(game) == before and not (game / "D2RLoader.exe").exists() and not parts(game), "...the game folder is as it was")

# ------------------------------------------------------------------ the wire
print("\n=== 2. the wire: redirects, caps, cancel, truncation")
game, home = make_game("wire")
STATIC["/loader/ok.zip"] = GOOD_ZIP
ROUTES["/redir/ok"] = redirect_to("/loader/ok.zip")
r = run(game, home, "install", "loader", "--no-run", "--pin-file", str(pin), env=env_for(loader_url=f"{BASE}/redir/ok"))
check(r.returncode == 0 and hits("/loader/ok.zip") >= 1, "a redirect to the same (allowed) host is followed")
cached = home / "deps" / "d2rloader" / "D2RLoader-test.zip"
check(cached.exists() and sha_file(cached) == sha(GOOD_ZIP), "...the cached archive is byte for byte what was served")
j = json.loads(run(game, home, "install", "loader", "--no-run", "--json", "--pin-file", str(pin), env=env_for(loader_url=f"{BASE}/redir/ok"), show=False).stdout)
check(j.get("sha256") == sha(GOOD_ZIP), "...and `--no-run --json` reports its SHA-256")

shutil.rmtree(home, ignore_errors=True)
ROUTES["/redir/off"] = redirect_to(f"http://localhost:{PORT}/loader/ok.zip")
n = hits("/loader/ok.zip")
r = run(game, home, "install", "loader", "--no-run", "--pin-file", str(pin), env=env_for(loader_url=f"{BASE}/redir/off"))
check(r.returncode == 20 and "localhost" in r.stderr and hits("/loader/ok.zip") == n, "a redirect to a host that is not on the list is refused, and never fetched")
check(not cached.exists() and not parts(home), "...no file was kept")

for i in range(8):
    ROUTES[f"/redir/loop{i}"] = redirect_to(f"/redir/loop{i + 1}")
ROUTES["/redir/loop8"] = redirect_to("/loader/ok.zip")
r = run(game, home, "install", "loader", "--no-run", "--pin-file", str(pin), env=env_for(loader_url=f"{BASE}/redir/loop0"))
check(r.returncode == 20 and "too many redirects" in r.stderr, "more than 5 redirects in a row are refused")
ROUTES["/redir/five"] = redirect_to("/redir/five1")
for i in range(1, 4):
    ROUTES[f"/redir/five{i}"] = redirect_to(f"/redir/five{i + 1}")
ROUTES["/redir/five4"] = redirect_to("/loader/ok.zip")             # five -> five1 -> five2 -> five3 -> five4 -> ok.zip: five redirects
r = run(game, home, "install", "loader", "--no-run", "--pin-file", str(pin), env=env_for(loader_url=f"{BASE}/redir/five"), show=False)
check(r.returncode == 0, "...exactly 5 are fine")

def claim_big(h):                                           # promises 400 MB, sends nothing
    h.send_response(200)
    h.send_header("Content-Length", str(400 << 20))
    h.send_header("Connection", "close")
    h.end_headers()
    time.sleep(0.5)
ROUTES["/big/claim"] = claim_big
shutil.rmtree(home, ignore_errors=True)
r = run(game, home, "install", "loader", "--no-run", "--pin-file", str(pin), env=env_for(loader_url=f"{BASE}/big/claim"))
check(r.returncode == 20 and "allows" in r.stderr and not parts(home), "a Content-Length over the size cap is refused before a byte is read")

def truncated(h):                                           # promises 100 000 bytes, sends 40 000, hangs up
    h.send_response(200)
    h.send_header("Content-Length", "100000")
    h.send_header("Connection", "close")
    h.end_headers()
    h.wfile.write(b"x" * 40000)
    h.wfile.flush()
ROUTES["/bad/truncated"] = truncated
r = run(game, home, "install", "loader", "--no-run", "--pin-file", str(pin), env=env_for(loader_url=f"{BASE}/bad/truncated"))
check(r.returncode == 20 and ("incomplete" in r.stderr or "broke off" in r.stderr) and not parts(home) and not (home / "deps" / "d2rloader" / "D2RLoader-test.zip").exists(),
      "a download that is cut short fails, and leaves no partial file")

r = run(game, home, "install", "loader", "--no-run", "--pin-file", str(pin), env=env_for(loader_url=f"{BASE}/no/such/file.zip"))
check(r.returncode == 20 and "404" in r.stderr, "HTTP 404: exit 20 with the status in the message")

def slow(h):                                                # 8 MB in 64 KB pieces, 25 ms apart
    total = 8_000_000
    h.send_response(200)
    h.send_header("Content-Length", str(total))
    h.send_header("Connection", "close")
    h.end_headers()
    sent = 0
    EVENTS["slow_sent"] = 0
    while sent < total:
        chunk = min(65536, total - sent)
        h.wfile.write(b"z" * chunk)
        h.wfile.flush()
        sent += chunk
        EVENTS["slow_sent"] = sent
        time.sleep(0.025)
    EVENTS["slow_done"] = True
ROUTES["/slow/L.zip"] = slow
EVENTS.clear()
shutil.rmtree(home, ignore_errors=True)
t0 = time.time()
r = run(game, home, "install", "loader", "--no-run", "--pin-file", str(pin), env=env_for({"VRML_TEST_CANCEL_AFTER": "300000"}, loader_url=f"{BASE}/slow/L.zip"))
check(r.returncode == 30 and "cancelled" in r.stderr, "a download cancelled half way: exit 30")
check(not parts(home) and not (home / "deps" / "d2rloader" / "D2RLoader-test.zip").exists(), "...the partial file is deleted")
time.sleep(1.0)
check(EVENTS.get("slow_sent", 0) < 8_000_000 and not EVENTS.get("slow_done"), "...and the connection was dropped (the server did not get to send it all)")

# ================================================================== 3. releases
print("\n=== 3. releases")
game, home = make_game("releases")
v168 = installer("0.168.0")
STATIC["/files/D2R_VR_Setup_v0.168.0.exe"] = v168
unleashed = installer("0.3.19", b"U")
STATIC["/gh/download/v0.3.19-windows-preview/D2R-PCVR-Setup-0.3.19.exe"] = unleashed


def authentic_answer(version="0.168", sha_hex=None, size=None, url=None, bare=False):
    d = {"version": version, "download_url": url or f"{BASE}/files/D2R_VR_Setup_v{version}.0.exe", "changelog": "test changelog"}
    if sha_hex:
        d["sha256"] = sha_hex
    if size:
        d["size"] = size
    return d


AUTH = {"answer": authentic_answer()}


def api_version(h):
    v = h.path.split("v=")[-1] if "v=" in h.path else ""
    if v == "0.168.0":
        h.reply(200, json.dumps({"version": "0.0.0", "download_url": "", "changelog": ""}).encode(), ctype="application/json")
    else:
        h.reply(200, json.dumps(AUTH["answer"]).encode(), ctype="application/json")
ROUTES["/api/version"] = api_version


def gh_list(version, exe, digest="same", sha_asset="same", sha_asset_present=True):
    tag = f"v{version}-windows-preview"
    name = f"D2R-PCVR-Setup-{version}.exe"
    base = f"{BASE}/gh/download/{tag}/"
    d = sha(exe) if digest == "same" else digest
    assets = [{"name": name, "size": len(exe), "state": "uploaded", "digest": ("sha256:" + d) if d else "", "browser_download_url": base + name}]
    if sha_asset_present:
        assets.append({"name": name + ".sha256", "size": 92, "state": "uploaded", "browser_download_url": base + name + ".sha256"})
        STATIC[f"/gh/download/{tag}/{name}.sha256"] = ((sha(exe) if sha_asset == "same" else sha_asset) + f"  {name}\n").encode()
    return json.dumps([{"tag_name": tag, "draft": False, "prerelease": True, "published_at": "2026-10-18T08:17:11Z", "body": "# D2R VR Unleashed\n\nnotes", "assets": assets}])


GH = {"list": gh_list("0.3.19", unleashed)}


def gh_releases(h):
    h.reply(200, GH["list"].encode(), ctype="application/json")
ROUTES["/gh/releases"] = gh_releases

r = run(game, home, "releases", "--json")
j = json.loads(r.stdout) if r.returncode == 0 else {}
mods = {m["id"]: m for m in j.get("mods", [])}
check(r.returncode == 0 and set(mods) == {"d2r-vr", "d2r-pcvr"}, "releases --json: both mods answered (exit 0)")
check(mods["d2r-vr"]["latest"]["version"] == "0.168.0" and mods["d2r-vr"]["latest"]["sha256"] == "" and mods["d2r-vr"]["can_install"], "...Authentic: 0.168.0, no published sha256, can be installed")
check(mods["d2r-pcvr"]["latest"]["version"] == "0.3.19" and mods["d2r-pcvr"]["latest"]["sha256"] == sha(unleashed), "...Unleashed: 0.3.19 with GitHub's digest, which the .sha256 file confirms")
check(mods["d2r-pcvr"]["latest"]["sha256_source"] == "digest + .sha256" and mods["d2r-pcvr"]["latest"]["prerelease"], "...checked against both, and marked a pre-release")
r = run(game, home, "releases")
check(r.returncode == 0 and "can be installed" in r.stdout and "none published" in r.stdout, "releases (text)")

# ================================================================== 4. install authentic
print("\n=== 4. install authentic")
setup_log = WORK / "fake_setup.log"
mf168 = manifest_for("d2r-vr", "0.168.0")
setup_env = {"FAKE_SETUP_GAME": "", "FAKE_SETUP_MOD": "D2R-VR", "FAKE_SETUP_MANIFEST": str(mf168)}


def auth_env(game, **extra):
    e = {**setup_env, "FAKE_SETUP_GAME": str(game), **extra}
    return env_for(e)


game, home = make_game("authentic")
AUTH["answer"] = authentic_answer()
before = snap(game)
n = hits("/files/D2R_VR_Setup_v0.168.0.exe")
r = run(game, home, "install", "authentic", "--dry-run", env=auth_env(game))
check(r.returncode == 27 and "NONE PUBLISHED" in r.stdout and f"{BASE}/files/D2R_VR_Setup_v0.168.0.exe" in r.stdout and "/DIR=" in r.stdout and "YES" in r.stdout,
      "dry run: where from, no checksum published (exit 27), administrator rights, the /DIR argument")
check(hits("/files/D2R_VR_Setup_v0.168.0.exe") == n and snap(game) == before, "...nothing downloaded or run")
r = run(game, home, "install", "authentic", env=auth_env(game))
check(r.returncode == 27 and "E_UNVERIFIED" in r.stderr and hits("/files/D2R_VR_Setup_v0.168.0.exe") == n and not setup_log.exists(), "no published sha256 and no --accept-unverified: refused (exit 27) before any download")

r = run(game, home, "install", "authentic", "--accept-unverified", env=auth_env(game))
check(r.returncode == 0 and sha(v168) in r.stdout and "no published checksum" in r.stdout, "--accept-unverified: installs, and shows the SHA-256 it computed")
log = setup_log.read_text(encoding="utf-8", errors="replace") if setup_log.exists() else ""
check(f"arg1=/DIR={game}" in log, "...the Setup received /DIR=\"<game folder>\" with its spaces intact")
j = json.loads(run(game, home, "mods", "--json", show=False).stdout)
m = {x["id"]: x for x in j["mods"]}
check(m["d2r-vr"]["installed"] and m["d2r-vr"]["version"] == "0.168.0" and m["d2r-vr"]["source"] == "manifest", "...afterwards `mods` shows D2R VR Authentic 0.168.0 installed")
r = run(game, home, "install", "authentic", env=auth_env(game))
check(r.returncode == 0 and "already the newest version" in r.stdout, "installed and nothing newer: 'already the newest version', nothing run")

# the sha256 is published and enforced
game, home = make_game("authentic_sha")
setup_log.unlink(missing_ok=True)
AUTH["answer"] = authentic_answer(sha_hex=sha(v168), size=len(v168))
r = run(game, home, "install", "authentic", env=auth_env(game))
check(r.returncode == 0 and "matches the published one" in r.stdout and "started with administrator rights" not in r.stdout, "a published sha256 + size matching the download: installed, 'matches the published one'")
check((game / "mods/D2R-VR/D2R-VR.mpq/modinfo.json").exists() and setup_log.exists(), "...the Setup ran and left the mod in the game folder")

game, home = make_game("authentic_wrong_sha")
setup_log.unlink(missing_ok=True)
AUTH["answer"] = authentic_answer(sha_hex=sha(b"something else"), size=len(v168))
r = run(game, home, "install", "authentic", env=auth_env(game))
check(r.returncode == 23 and "E_HASH" in r.stderr and sha(v168) in r.stderr, "a download that is not the announced sha256: exit 23, both hashes shown")
check(not setup_log.exists() and not list((home / "downloads").rglob("*.exe")), "...deleted, and the Setup never ran")

game, home = make_game("authentic_size")
AUTH["answer"] = authentic_answer(sha_hex=sha(v168), size=100)
r = run(game, home, "install", "authentic", env=auth_env(game))
check(r.returncode == 20 and "allows" in r.stderr and not setup_log.exists(), "a file bigger than the published size is refused by the size cap")

for label, url in (("a host that is not ours", "https://evil.example/static/D2R_VR_Setup_v0.168.0.exe"), ("plain http to another port", f"http://127.0.0.1:{PORT + 1}/files/x.exe"),
                   ("our host name with another host behind it", "https://bodywalkvr.com@evil.example/x.exe")):
    game, home = make_game("authentic_url")
    AUTH["answer"] = authentic_answer(sha_hex=sha(v168), url=url)
    n = len(HITS)
    r = run(game, home, "install", "authentic", env=auth_env(game), show=False)
    check(r.returncode == 29 and "E_UNTRUSTED" in r.stderr and not setup_log.exists(), f"a download address on {label}: refused (exit 29)")

# the Setup is cancelled / fails / installs elsewhere
AUTH["answer"] = authentic_answer(sha_hex=sha(v168), size=len(v168))
for label, extra, code, text in (("the player cancels the Setup (Inno exit 5)", {"FAKE_SETUP_EXIT": "5", "FAKE_SETUP_MODE": "nothing"}, 30, "cancelled"),
                                 ("the Setup fails (exit 4)", {"FAKE_SETUP_EXIT": "4", "FAKE_SETUP_MODE": "nothing"}, 28, "exit code 4"),
                                 ("the Setup ends well but installed elsewhere", {"FAKE_SETUP_MODE": "nothing"}, 11, "NOT installed")):
    game, home = make_game("authentic_setup")
    r = run(game, home, "install", "authentic", env=auth_env(game, **extra), show=False)
    j = json.loads(run(game, home, "mods", "--json", show=False).stdout)
    inst = {x["id"]: x for x in j["mods"]}["d2r-vr"]["installed"]
    check(r.returncode == code and text in r.stdout and not inst, f"{label}: exit {code}, the mod is reported as not installed")

# dry run does not download
game, home = make_game("authentic_dry")
AUTH["answer"] = authentic_answer(sha_hex=sha(v168), size=len(v168))
n = hits("/files/D2R_VR_Setup_v0.168.0.exe")
r = run(game, home, "install", "authentic", "--dry-run", env=auth_env(game))
check(r.returncode == 0 and sha(v168) in r.stdout and hits("/files/D2R_VR_Setup_v0.168.0.exe") == n, "dry run with a published sha256: exit 0, the expected hash shown, nothing downloaded")
r = run(game, home, "install", "authentic", "--no-run", env=auth_env(game))
check(r.returncode == 0 and "was not run" in r.stdout and any((home / "downloads").rglob("*.exe")) and not (game / "mods").exists(), "--no-run: downloaded and verified, Setup not run, game folder untouched")

# ================================================================== 5. install unleashed
print("\n=== 5. install unleashed")
mf319 = manifest_for("d2r-pcvr", "0.3.19")
pc_env = {"FAKE_SETUP_MOD": "D2R-PCVR", "FAKE_SETUP_MANIFEST": str(mf319)}
dl_path = "/gh/download/v0.3.19-windows-preview/D2R-PCVR-Setup-0.3.19.exe"

game, home = make_game("unleashed_mismatch")
GH["list"] = gh_list("0.3.19", unleashed, digest="same", sha_asset="b" * 64)
n = hits(dl_path)
r = run(game, home, "install", "unleashed", env=env_for({**pc_env, "FAKE_SETUP_GAME": str(game)}))
check(r.returncode == 23 and "disagree" in r.stderr and hits(dl_path) == n, "GitHub's digest and the .sha256 file disagree: refused (exit 23) before the download")

GH["list"] = gh_list("0.3.19", unleashed)
setup_log.unlink(missing_ok=True)
game, home = make_game("unleashed_ok")
r = run(game, home, "install", "unleashed", env=env_for({**pc_env, "FAKE_SETUP_GAME": str(game)}))
check(r.returncode == 0 and "matches the published one" in r.stdout, "a good release: installed with no flag (the digest is published)")
log = setup_log.read_text(encoding="utf-8", errors="replace")
check("started args=0" in log, "...his Setup was started with no arguments (it shows its own wizard)")
j = json.loads(run(game, home, "mods", "--json", show=False).stdout)
m = {x["id"]: x for x in j["mods"]}
check(m["d2r-pcvr"]["installed"] and m["d2r-pcvr"]["version"] == "0.3.19", "...afterwards `mods` shows D2R VR Unleashed 0.3.19 installed")
r = run(game, home, "releases", "--json", env=env_for({**pc_env, "FAKE_SETUP_GAME": str(game)}), show=False)
mm = {x["id"]: x for x in json.loads(r.stdout)["mods"]}
check(not mm["d2r-pcvr"]["update_available"] and mm["d2r-pcvr"]["installed_version"] == "0.3.19", "...and `releases` sees it as current")

GH["list"] = gh_list("0.3.19", unleashed, digest=sha(b"a different file"), sha_asset=sha(b"a different file"))
setup_log.unlink(missing_ok=True)
game, home = make_game("unleashed_badfile")
r = run(game, home, "install", "unleashed", env=env_for({**pc_env, "FAKE_SETUP_GAME": str(game)}))
check(r.returncode == 23 and "E_HASH" in r.stderr and not setup_log.exists() and not list((home / "downloads").rglob("*.exe")),
      "a file that is not what the digest says: exit 23, deleted, never run")

GH["list"] = gh_list("0.3.19", unleashed, digest="", sha_asset_present=False)
game, home = make_game("unleashed_nodigest")
r = run(game, home, "install", "unleashed", env=env_for({**pc_env, "FAKE_SETUP_GAME": str(game)}))
check(r.returncode == 27, "no digest and no .sha256 at all: exit 27 unless --accept-unverified")

# ================================================================== done
print(f"\n{'ALL PASSED' if not failures else str(len(failures)) + ' FAILED'}")
for f in failures:
    print("  FAIL:", f)
server.shutdown()
if not failures:
    shutil.rmtree(WORK, ignore_errors=True)
else:
    print("work folder kept:", WORK)
sys.exit(1 if failures else 0)
