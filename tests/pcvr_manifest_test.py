#!/usr/bin/env python3
"""The manifest of D2R VR Unleashed is what bearstonem's files say. Plain python, no pytest.

    python tests/pcvr_manifest_test.py --clone <clone of github.com/bearstonem/d2r-pcvr> [--manifest-test <manifest_test exe>] [--require-all]

--clone (or PCVR_CLONE) is only ever READ, with `git show`: nothing of his is run, imported or changed. --manifest-test (or
VRML_MANIFEST_TEST; else a build*/ folder next to this one is searched) is the launcher's C++ validator, `manifest_test --validate FILE...`.
Without a clone the test prints SKIPPED and passes; --require-all turns every skip into a failure.

It checks, at the commit the committed example says it was made from (x-source.commit):
  1. tools/make_pcvr_vrmod_manifest.py writes exactly examples/d2rloader-mods/d2r-pcvr/vrmod-manifest.json and exactly the kit's copy;
  2. that file passes the JSON Schema (tools/check_schema.py's validator) and `manifest_test --validate`;
  3. the values in it are the ones in his files (read here again, with other, simpler patterns than the generator's);
  4. the generator STOPS when a file of his stops saying what it expects (changed copies of his texts, in memory);
  5. kits/d2r-pcvr-for-review/install-patch-proposal.diff applies without fuzz to copies of his files at that commit (GNU patch, in a
     temporary folder), the patched Python still parses, and the manifest it adds is the kit's;
  6. the launcher he would carry (part 2 of the diff) is what docs/INSTALLER_CONTRACT.md says: registry values, version rule (word for word the
     launcher's own), silent switches, exit code ignored, never uninstalled, the pinned name and hash, nothing of his changed or removed;
  7. the patched build_native_installer.ps1 parses (PowerShell's parser, nothing run) and the patched D2R_PCVR_Setup.iss compiles with a dummy
     payload, with and without the launcher defines, and not with half of them (Inno Setup's ISCC.exe, --iscc or ISCC; skipped without it);
and says, without failing, when origin/main has moved on.
"""
import argparse
import ast
import difflib
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
LAUNCHER = HERE.parent
TOOLS = LAUNCHER / "tools"
EXAMPLE = LAUNCHER / "examples" / "d2rloader-mods" / "d2r-pcvr" / "vrmod-manifest.json"
KIT = LAUNCHER / "kits" / "d2r-pcvr-for-review"
SCHEMA = LAUNCHER / "schema" / "vrmod-manifest.1.schema.json"


class Skip(Exception):
    pass


def norm(text):
    return text.replace("\r\n", "\n")          # a Windows checkout may have turned LF into CRLF


def read(path):
    return norm(Path(path).read_text(encoding="utf-8"))


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def git_show(c, path):
    p = subprocess.run(["git", "-C", str(c.clone), "show", f"{c.commit}:{path}"], capture_output=True)
    assert p.returncode == 0, f"git show {path}: {p.stderr.decode('utf-8', 'replace').strip()}"
    return p.stdout


def shown(c, path):
    return norm(git_show(c, path).decode("utf-8-sig"))


class Ctx:
    pass


# ------------------------------------------------------------------ the tests

def test_the_generator_writes_the_committed_example_and_the_kit_copy(c):
    out = c.tmp / "generated.json"
    r = subprocess.run([sys.executable, str(TOOLS / "make_pcvr_vrmod_manifest.py"), "--repo", str(c.clone), "--ref", c.commit, "--out", str(out)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    got = read(out)
    assert got == read(EXAMPLE), "examples/d2rloader-mods/d2r-pcvr/vrmod-manifest.json is not what the generator makes: run it again"
    assert got == read(KIT / "vrmod-manifest.json"), "the kit's vrmod-manifest.json is not the example"


def test_the_manifest_passes_the_schema_and_the_cpp_validator(c):
    cs = load(TOOLS / "check_schema.py", "check_schema")
    schema = json.loads(read(SCHEMA))
    for f in (EXAMPLE, KIT / "vrmod-manifest.json"):
        cs.validate(json.loads(read(f)), schema, schema)
    if not c.exe:
        raise Skip("no manifest_test executable (--manifest-test / VRML_MANIFEST_TEST)")
    r = subprocess.run([str(c.exe), "--validate", str(EXAMPLE), str(KIT / "vrmod-manifest.json")], capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    assert r.returncode == 0 and len(lines) == 2 and all(l.startswith("OK ") for l in lines), r.stdout + r.stderr


def test_the_values_are_the_ones_in_his_files(c):
    m = json.loads(read(EXAMPLE))
    ident = shown(c, "steamvr/mod_identity.py")
    mod = re.search(r"^MOD_NAME = '([^']+)'", ident, re.M).group(1)
    assert m["d2rloader_mod"]["name"] == mod == "D2R-PCVR"
    assert m["launch"]["options"][0]["args"] == ["-mod", mod]
    assert m["id"] == re.search(r"^MOD_ID = '([^']+)'", ident, re.M).group(1)
    assert m["name"] == re.search(r"^DISPLAY_NAME = '([^']+)'", ident, re.M).group(1)
    assert m["description"] == re.search(r"^TAGLINE = '([^']+)'", ident, re.M).group(1) + "."
    iss = shown(c, "installer/D2R_PCVR_Setup.iss")
    assert m["version"] == re.search(r'ModVersion "([^"]+)"', iss).group(1)
    assert m["homepage"] == re.search(r"^AppPublisherURL=(\S+)", iss, re.M).group(1)
    assert m["support"] == re.search(r"^AppSupportURL=(\S+)", iss, re.M).group(1)
    assert m["update"] == {"kind": "github-release", "repo": "bearstonem/d2r-pcvr", "tag": "v*-windows-preview"}
    assert m["uninstall"]["exe"] == "{localappdata}/Programs/D2R PCVR/unins000.exe" and "args" not in m["uninstall"]
    assert re.search(r"^DefaultDirName=\{localappdata\}\\Programs\\D2R PCVR$", iss, re.M) and "PrivilegesRequired=lowest" in iss
    assert m["uninstall"]["needs_admin"] is False
    games = shown(c, "steamvr/game_install.py")
    assert m["game"]["builds"] == [re.search(r"^STEAM_VERSION = '([^']+)'", games, re.M).group(1),
                                   re.search(r"^BATTLENET_VERSION = '([^']+)'", games, re.M).group(1)]
    install = shown(c, "steamvr/install.py")
    archives = re.findall(r"'([0-9a-f]{64})'", re.search(r"LOADER_HASHES = \((.*?)\n\)", install, re.S).group(1))
    req = m["requires"][0]
    assert req["x-archive"]["sha256_any_of"] == archives and len(archives) == 2
    evidence = json.loads(shown(c, "steamvr/evidence/2026-10-13-loader-archive-repack.json"))
    by_name = {p["name"].lower(): p["sha256"] for p in evidence["identical_payloads"]}
    assert {Path(f["path"]).name.lower(): f["sha256_any_of"][0] for f in req["files"]} == \
        {k: by_name[k] for k in ("d2rloader.exe", "d2rcore.dll", "d2rloader.mpq")}
    # the three hashes are also the ones the contract and the other example (D2R VR) name
    other = json.loads(read(EXAMPLE.parent.parent / "d2r-vr" / "vrmod-manifest.json"))
    assert other["requires"][0]["files"] == req["files"], "the two examples disagree about D2RLoader 1.3.1-beta"
    launch = shown(c, "steamvr/launch_windows.py")
    steam_id = re.search(r"= environment\['SteamGameId'\] = '(\d+)'", launch).group(1)
    env = m["launch"]["options"][0]["env"]
    assert env[0] == {"unset": ["SteamOverlayGameId"]}
    assert env[1] == {"stores": ["steam"], "unset": ["SteamAppId", "SteamGameId"], "set": {"SteamAppId": steam_id, "SteamGameId": steam_id}}
    assert env[2] == {"stores": ["battlenet"], "unset": ["SteamAppId", "SteamGameId"]} and len(env) == 3
    assert "pcvr-launch.log" in launch and "--frame-vdxr" in launch and "--frame-openxr-runtime=" in launch
    assert any(l.endswith("/d2rloader/logs/pcvr-launch.log") for l in m["logs"]) and all(l.startswith("{game}/") for l in m["logs"])
    assert any(l.endswith("/d2r-pcvr-vrcam.log") for l in m["logs"]), "the plugin id's log"
    # the contract (docs/MOD_MANIFEST.md): a D2RLoader mod needs no files and no exclusive group
    assert "files" not in m and "exclusive_group" not in m
    assert "icon" not in m and not (KIT / "launcher_icon.png").exists(), "no icon art exists: none may be named"
    assert m["x-unverified"] and re.fullmatch(r"[0-9a-f]{40}", m["x-source"]["commit"])
    dev = shown(c, "steamvr/prepare_dev_scenarios.py")
    assert "developer mode" in m["x-unverified"] and re.search(r"^MOD = '(pcvr-dev)'", dev, re.M).group(1) in m["x-unverified"]["developer mode"]
    assert "--frame-dev" in launch and m["launch"]["options"][0]["args"] == ["-mod", mod], "the launcher's start is the normal one, never the developer mod"
    text = read(EXAMPLE)
    assert ";" not in text and "@" not in text, "CMake embeds this file as the built-in profile (src/builtin_mods.h.in)"
    assert len(text.encode("utf-8")) < 16000, "a C++ string literal of the built-in profile must stay under MSVC's 16380 bytes"


def test_the_generator_stops_when_his_files_change(c):
    gen = load(TOOLS / "make_pcvr_vrmod_manifest.py", "make_pcvr_vrmod_manifest")
    t = gen.read_files(c.clone, c.commit)

    def make(texts):
        return gen.assemble(texts, c.commit, lambda tag: True, "")

    assert make(t) == json.loads(read(EXAMPLE))
    first_archive = re.search(r"'([0-9a-f]{64})'", t["install"]).group(1)
    version = re.search(r'ModVersion "([^"]+)"', t["iss"]).group(1)
    cases = [
        ("launch", "'-mod', MOD", "'-m', MOD"),                                                    # the loader is no longer started with -mod
        ("launch", "else MOD_NAME]", "else 'other']"),                                             # the normal start is no longer the normal mod
        ("launch", "DEV_OPTION = '--frame-dev'", "DEVELOPER_OPTION = '--frame-dev'"),              # the developer flag moved
        ("dev", "MOD = 'pcvr-dev'", "MOD = 'D2R-PCVR'"),                                           # the developer mod would be the normal one
        ("dev", "root = checked(game / 'mods' / MOD)", "root = checked(game / 'mods' / 'x')"),     # the developer mod is no longer its own folder
        ("launch", "= '2536520'", "= '25365'"),                                                    # another Steam app id than steam_config's
        ("launch", "battlenet_game(arguments) is None", "battlenet_game(arguments) is not None"),  # the Explorer hand-off is not Steam-only
        ("launch", "if key.lower() == 'steamoverlaygameid':", "if key.lower() == 'steamoverlay':"),
        ("iss", f'ModVersion "{version}"', 'ModVersion "9.9.9"'),                                  # installer and plugin version disagree
        ("iss", "PrivilegesRequired=lowest", "PrivilegesRequired=admin"),
        ("iss", "AppPublisherURL=https://github.com/", "AppPublisherURL=https://example.org/"),
        ("install", "'d2rloader.exe')", "'d2rloader.exe', 'other.exe')"),                          # a process the manifest does not know
        ("install", first_archive, "0" * 64),                                                      # install.py and the evidence file disagree
        ("evidence", '"name": "d2rloader.mpq"', '"name": "d2rloader2.mpq"'),                       # no hash for a file the manifest pins
        ("identity", "MOD_DIR = Path('mods') / MOD_NAME", "MOD_DIR = Path('plugins') / MOD_NAME"),  # the mod folder moved
        ("license", "MIT License", "Apache License"),
    ]
    for key, old, new in cases:
        assert old in t[key], f"test case is stale: {key} no longer has {old!r}"
        try:
            make({**t, key: t[key].replace(old, new)})
        except gen.Drift:
            continue
        raise AssertionError(f"the generator did not notice: {key}: {old!r} -> {new!r}")


PATCHED = ["docs/RELEASING.md", "installer/D2R_PCVR_Setup.iss", "installer/README.md", "installer/build_native_installer.ps1",
           "installer/stage_native_installer.py", "installer/stage_native_installer_test.py", "steamvr/install.py", "steamvr/install_test.py",
           "steamvr/mod_identity_test.py", "steamvr/package.py"]


def patched_tree(c):
    """Copies of his files at the commit with the kit's diff applied by GNU patch (no fuzz, no offset), in the test's temporary folder."""
    if getattr(c, "tree", None):
        return c.tree
    patch = shutil.which("patch")
    if not patch:
        raise Skip("no GNU patch on PATH")
    diff = read(KIT / "install-patch-proposal.diff")
    names = re.findall(r"^--- a/(\S+)$", diff, re.M)
    assert sorted(names) == PATCHED, names
    tree = c.tmp / "tree"
    for n in names:
        target = tree / n
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(git_show(c, n))
    (c.tmp / "p.diff").write_bytes(diff.encode("utf-8"))
    r = subprocess.run([patch, "-p1", "--fuzz=0", "-i", str(c.tmp / "p.diff")], cwd=tree, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "offset" not in r.stdout and "fuzz" not in r.stdout.lower(), "the hunks no longer sit where they were written: " + r.stdout
    c.tree = tree
    return tree


def only_inserts(c, tree, name):
    """True when the diff adds lines to the file and changes or removes none of his."""
    before = git_show(c, name).decode("utf-8").splitlines()
    after = (tree / name).read_text(encoding="utf-8").splitlines()
    ops = difflib.SequenceMatcher(None, before, after, autojunk=False).get_opcodes()
    return all(op[0] in ("equal", "insert") for op in ops)


def test_the_patch_applies_and_adds_the_kits_manifest(c):
    tree = patched_tree(c)
    names = PATCHED
    for n in names:
        if n.endswith(".py"):
            ast.parse((tree / n).read_text(encoding="utf-8"), n)           # parsed, never executed
    assert (tree / "vrmod-manifest.json").read_text(encoding="utf-8") == read(KIT / "vrmod-manifest.json"), \
        "the new file in the diff is not the kit's vrmod-manifest.json: rebuild the diff"
    install = (tree / "steamvr/install.py").read_text(encoding="utf-8")
    assert "payload[mod_identity.MOD_DIR / 'vrmod-manifest.json'] = (ROOT / 'vrmod-manifest.json').read_bytes()" in install
    for n in ("steamvr/package.py", "installer/stage_native_installer.py"):
        assert "'vrmod-manifest.json'" in (tree / n).read_text(encoding="utf-8"), n
    assert "`vrmod-manifest.json`" in (tree / "docs/RELEASING.md").read_text(encoding="utf-8")
    # install.py only gains lines: what his installer already does (loader, icons, potions, the Arcane sky since 0.3.18) is left as it is
    before = git_show(c, "steamvr/install.py").decode("utf-8").splitlines()
    after = install.splitlines()
    ops = [op for op in difflib.SequenceMatcher(None, before, after, autojunk=False).get_opcodes() if op[0] != "equal"]
    assert [op[0] for op in ops] == ["insert"] and ops[0][4] - ops[0][3] == 3, ops
    assert json.loads((tree / "vrmod-manifest.json").read_text(encoding="utf-8"))["version"] == \
        re.search(r'ModVersion "([^"]+)"', shown(c, "installer/D2R_PCVR_Setup.iss")).group(1)


def pascal_function(text, name):
    """The text of one Pascal function / procedure of an Inno [Code] section, from its header to its closing `end;` at column 0."""
    m = re.search(rf"^(?:function|procedure) {name}\b.*?^end;", text, re.M | re.S)
    assert m, f"{name} not found"
    return re.sub(r"\s+", " ", re.sub(r"//[^\n]*", "", m.group(0))).strip()


def test_the_carried_launcher_is_what_the_launchers_contract_says(c):
    tree = patched_tree(c)
    contract = read(LAUNCHER / "docs" / "INSTALLER_CONTRACT.md")
    key = re.search(r"`HKCU\\(Software\\D2R VR Launcher)`", contract).group(1)
    value_names = re.search(r"string values `(\w+)` and `(\w+)`", contract).groups()
    program = re.search(r"`<InstallDir>\\(\w+\.exe)`", contract).group(1)
    switches = re.search(r"`D2R_VR_Launcher_Setup_<version>\.exe (/VERYSILENT[^`]*)`", contract).group(1)
    assert switches == "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TASKS=" and value_names == ("InstallDir", "Version")
    # the launcher's own Setup writes exactly what the contract says a mod's Setup reads, under the name the build pins
    own = read(LAUNCHER / "installer" / "D2R_VR_Launcher_Setup.iss")
    assert f'Subkey: "{key}"' in own and all(f'ValueName: "{n}"' in own for n in value_names) and f'{{app}}\\{program}' in own
    assert "OutputBaseFilename=D2R_VR_Launcher_Setup_{#LauncherVersion}" in own
    iss = read(tree / "installer/D2R_PCVR_Setup.iss")
    # 1. detect: both values and the program, in the original user's hive
    for name in value_names:
        assert f"RegQueryStringValue(HKCU, '{key}', '{name}'," in iss, name
    assert f"FileExists(AddBackslash(Dir) + '{program}')" in iss
    # 2. compare: the launcher's own dotted-number rule, word for word
    assert pascal_function(iss, "LauncherVersionNewer").replace("LauncherVersionNewer", "VersionNewer") == pascal_function(own, "VersionNewer")
    assert "LauncherVersionNewer('{#LauncherVersion}', Installed)" in iss
    # 3. install silently, as the original user, exit code logged and ignored, never blocking, never uninstalling
    assert f"ExecAsOriginalUser(SetupFile, '{switches}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode)" in iss
    block = iss[iss.index("function LauncherVersionNewer"):iss.index("procedure CurStepChanged")]
    assert block.count("(ignored)") == 3 and "except" in block and "RaiseException" not in block and "Abort" not in block
    assert "UninstallRun" not in iss and "unins" not in block.lower(), "this Setup must never uninstall the launcher"
    assert "CompareText(GetSHA256OfFile(SetupFile), '{#LauncherSetupSha256}')" in iss, "the pinned hash is checked again before the file is run"
    # nothing changes unless the build gives the three defines, and nothing of his is changed or removed
    assert iss.count("#ifdef LauncherSetupFile") == 3 and only_inserts(c, tree, "installer/D2R_PCVR_Setup.iss")
    assert re.search(r"#ifdef LauncherSetupFile\n    InstallCarriedLauncher;\n#endif\n    BackendBusy := True;", iss)
    # the pin: the build script and the staging script accept this one name and this one hash, and the launcher's real name matches both
    published = "D2R_VR_Launcher_Setup_0.2.0.exe"
    stage = read(tree / "installer/stage_native_installer.py")
    pattern = re.search(r"LAUNCHER_SETUP = re\.compile\(r'([^']+)'\)", stage).group(1)
    assert re.fullmatch(pattern, published) and re.fullmatch(pattern, "D2R_VR_Launcher_Setup_1.2.3.4.exe")
    assert not re.fullmatch(pattern, "Setup.exe") and not re.fullmatch(pattern, "D2R_VR_Launcher_Setup_0.2.0.exe.bak")
    assert "hashlib.sha256(data).hexdigest() != sha256" in stage and "hashes['launcher/' + name] = launcher_sha256" in stage
    ps = read(tree / "installer/build_native_installer.ps1")
    ps_pattern = re.search(r"-notmatch '\^(D2R_VR_Launcher_Setup_[^']+)\$'", ps).group(1)
    assert re.fullmatch(ps_pattern, published) and not re.fullmatch(ps_pattern, "Setup.exe")
    assert "-ne $LauncherSetupSha256.ToLowerInvariant()" in ps and "[string]$LauncherSetup," in ps and "[string]$LauncherSetupSha256" in ps
    assert not any("Mandatory" in l for l in ps.splitlines() if "LauncherSetup" in l), "carrying the launcher stays optional"
    assert only_inserts(c, tree, "installer/stage_native_installer_test.py") and only_inserts(c, tree, "installer/README.md")
    # the same switches are what the kit's README tells him
    assert switches in read(KIT / "README.md") and switches in read(tree / "installer/README.md")


def find_tool(env, candidates):
    for name in (os.environ.get(env, ""), *candidates):
        if name and Path(name).is_file():
            return Path(name)
    return None


def test_the_patched_powershell_parses_and_the_patched_inno_script_compiles(c):
    tree = patched_tree(c)
    ps = shutil.which("pwsh") or shutil.which("powershell")
    if ps:
        script = tree / "installer/build_native_installer.ps1"
        code = ("$e=$null;$t=$null;[void][System.Management.Automation.Language.Parser]::ParseFile($env:PARSE_TARGET,[ref]$t,[ref]$e);"
                "if($e.Count){$e|ForEach-Object{$_.ToString()};exit 1}")      # parsed, never run
        r = subprocess.run([ps, "-NoProfile", "-NonInteractive", "-Command", code], capture_output=True, text=True,
                           env={**os.environ, "PARSE_TARGET": str(script)})
        assert r.returncode == 0, r.stdout + r.stderr
    iscc = c.iscc or find_tool("ISCC", [r"C:\Program Files (x86)\Inno Setup 7\ISCC.exe", r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
                                         shutil.which("ISCC.exe") or ""])
    if not iscc:
        raise Skip("no ISCC.exe (--iscc / ISCC): the Inno script was not compiled" + ("" if ps else "; no PowerShell either"))
    payload = c.tmp / "dummy_payload"
    (payload / "launcher").mkdir(parents=True)
    (payload / "steamvr").mkdir()
    (payload / "LICENSE").write_text("fixture license\n")
    (payload / "steamvr/setup_native.py").write_text("# fixture\n")
    carried = payload / "launcher/D2R_VR_Launcher_Setup_0.2.0.exe"
    carried.write_bytes(b"carried launcher setup fixture")
    pin = hashlib.sha256(carried.read_bytes()).hexdigest()
    script = tree / "installer/D2R_PCVR_Setup.iss"

    def compile_with(out, *defines):
        return subprocess.run([str(iscc), "/Q", f"/DPayloadDir={payload}", f"/DInstallerOutput={c.tmp / out}", *defines, str(script)],
                              capture_output=True, text=True)

    plain = compile_with("out_plain")
    assert plain.returncode == 0 and list((c.tmp / "out_plain").glob("D2R-PCVR-Setup-*.exe")), plain.stdout + plain.stderr
    carrying = compile_with("out_carrying", "/DLauncherSetupFile=D2R_VR_Launcher_Setup_0.2.0.exe", "/DLauncherVersion=0.2.0",
                            f"/DLauncherSetupSha256={pin}")
    assert carrying.returncode == 0 and list((c.tmp / "out_carrying").glob("D2R-PCVR-Setup-*.exe")), carrying.stdout + carrying.stderr
    incomplete = compile_with("out_incomplete", "/DLauncherSetupFile=D2R_VR_Launcher_Setup_0.2.0.exe")
    assert incomplete.returncode != 0, "a half-given launcher must not compile"


# ------------------------------------------------------------------ the runner

TESTS = [test_the_generator_writes_the_committed_example_and_the_kit_copy,
         test_the_manifest_passes_the_schema_and_the_cpp_validator,
         test_the_values_are_the_ones_in_his_files,
         test_the_generator_stops_when_his_files_change,
         test_the_patch_applies_and_adds_the_kits_manifest,
         test_the_carried_launcher_is_what_the_launchers_contract_says,
         test_the_patched_powershell_parses_and_the_patched_inno_script_compiles]


def find_validator():
    for pattern in ("build*/**/manifest_test.exe", "build*/**/manifest_test"):
        for p in sorted(LAUNCHER.glob(pattern)):
            if p.is_file():
                return p
    return None


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--clone", default=os.environ.get("PCVR_CLONE", ""))
    ap.add_argument("--manifest-test", default=os.environ.get("VRML_MANIFEST_TEST", ""))
    ap.add_argument("--iscc", default="", help="Inno Setup's ISCC.exe (or ISCC in the environment) to compile the patched installer script with a dummy payload")
    ap.add_argument("--require-all", action="store_true")
    args = ap.parse_args(argv)
    if not args.clone:
        print("SKIPPED: no clone of bearstonem/d2r-pcvr (--clone DIR or PCVR_CLONE)")
        return 1 if args.require_all else 0
    c = Ctx()
    c.clone = Path(args.clone)
    c.exe = Path(args.manifest_test) if args.manifest_test else find_validator()
    c.iscc = Path(args.iscc) if args.iscc else None
    c.tree = None
    c.commit = json.loads(read(EXAMPLE))["x-source"]["commit"]
    have = subprocess.run(["git", "-C", str(c.clone), "cat-file", "-e", c.commit + "^{commit}"], capture_output=True)
    if have.returncode != 0:
        print(f"SKIPPED: the clone has no commit {c.commit[:12]} (fetch it, or regenerate the example from this clone)")
        return 1 if args.require_all else 0
    failed = skipped = 0
    with tempfile.TemporaryDirectory(prefix="pcvr_manifest_test_") as tmp:
        c.tmp = Path(tmp)
        for test in TESTS:
            try:
                test(c)
                print("ok    " + test.__name__)
            except Skip as e:
                skipped += 1
                print(f"SKIP  {test.__name__}: {e}")
            except Exception as e:                                    # a failed assert or anything unexpected
                failed += 1
                print(f"FAIL  {test.__name__}: {e}")
                if not isinstance(e, AssertionError):
                    traceback.print_exc()
    head = subprocess.run(["git", "-C", str(c.clone), "rev-parse", "--verify", "origin/main^{commit}"], capture_output=True, text=True)
    if head.returncode == 0 and head.stdout.strip() != c.commit:
        print(f"note: origin/main of the clone is {head.stdout.strip()[:12]}, the example was made from {c.commit[:12]}: "
              "run tools/make_pcvr_vrmod_manifest.py again when you want it to follow")
    print(f"{len(TESTS) - failed - skipped} ok, {skipped} skipped, {failed} failed")
    return 1 if failed or (skipped and args.require_all) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
