#!/usr/bin/env python3
"""The launcher manifest of bearstonem's D2R VR Unleashed (d2r-pcvr), made from HIS files.

    make_pcvr_vrmod_manifest.py --repo <clone of github.com/bearstonem/d2r-pcvr> [--ref origin/main]
                                [--out FILE | --out -] [--icon launcher_icon.png] [--for-kit]

Writes examples/d2rloader-mods/d2r-pcvr/vrmod-manifest.json (the launcher's built-in profile). --for-kit writes the file proposed for his installer
(kits/d2r-pcvr-for-review/vrmod-manifest.json): the same without `install_help`, which launchers before 0.3.5 do not know.

Nothing of his is run or imported and the clone is never changed: every file is read with `git show <ref>:<path>` and taken apart
with regular expressions. Each fact is looked for in the exact shape it has today; when a file no longer says it that way the script
stops ("his files changed") instead of guessing, so a new release of his is a re-read, not a silent drift.

What comes from where
  steamvr/mod_identity.py       MOD_ID, MOD_NAME (the D2RLoader mod folder), DISPLAY_NAME, TAGLINE, the folder layout
  vr/pcvr_identity.h            version, plugin id (-> <id>.log), author; must agree with the installer script
  installer/D2R_PCVR_Setup.iss  ModVersion, project and support URLs, where the uninstaller is, that it needs no admin rights
  steamvr/launch_windows.py     `-mod`, --frame-openxr-runtime= / --frame-vdxr, the Steam / Battle.net environment rules,
                                the Explorer hand-off (Steam only), the launch log, the developer flag (--frame-dev)
  steamvr/prepare_dev_scenarios.py   the name of the developer mod folder that flag stages (not the launcher's business, said in x-unverified)
  steam_config.py               the Steam app id the environment rule sets
  steamvr/game_install.py       the game build per store
  steamvr/install.py            the two accepted D2RLoader archive hashes, the archive URL, the processes that must be closed
  steamvr/evidence/...repack    the three D2RLoader file hashes (his own evidence file)
  docs/*.md, installer/README   log file names, the release tag pattern, the license name
  his review of the launcher    `support` (his Discord, in place of the Setup's AppSupportURL) and `install_help` (his words to players),
                                kept below as SUPPORT and INSTALL_HELP: they are in none of his files yet
The result is checked against schema/vrmod-manifest.1.schema.json (the draft-07 subset validator of tools/check_schema.py).
"""
import argparse
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
# From bearstonem's review of the launcher (0.3.4): the link on his card and the install text he wants players to read.
SUPPORT = "https://discord.gg/DmbFfJAnnm"
INSTALL_HELP = ("Install Unleashed through the shared launcher or download our Windows Setup from https://github.com/bearstonem/d2r-pcvr/releases. "
                "Both use the same Setup wizard. Close D2R and fully exit Steam before installing. Choose Full install for a new setup, "
                "or Update / repair to preserve existing settings and calibration. Setup handles the approved D2RLoader and ReShade versions "
                "automatically. Use offline characters only; Battle.net support is still experimental. The portable ZIP is also available "
                "for manual installation.")
LAUNCHER = TOOLS.parent
DEFAULT_OUT = LAUNCHER / "examples" / "d2rloader-mods" / "d2r-pcvr" / "vrmod-manifest.json"
SCHEMA = LAUNCHER / "schema" / "vrmod-manifest.1.schema.json"

FILES = {
    "identity": "steamvr/mod_identity.py",
    "launch": "steamvr/launch_windows.py",
    "dev": "steamvr/prepare_dev_scenarios.py",
    "game": "steamvr/game_install.py",
    "install": "steamvr/install.py",
    "steam": "steamvr/steam_config.py",
    "iss": "installer/D2R_PCVR_Setup.iss",
    "header": "vr/pcvr_identity.h",
    "license": "LICENSE",
    "evidence": "steamvr/evidence/2026-10-13-loader-archive-repack.json",
    "coexistence": "docs/MOD_COEXISTENCE.md",
    "workflow": "docs/AGENT_WORKFLOW.md",
    "installer_readme": "installer/README.md",
    "smoke": "installer/smoke_native_installer.ps1",
}
# Process and file names are lower case in his code; the launcher shows them the way Windows does.
PROCESS_NAMES = {"d2r.exe": "D2R.exe", "d2rloader.exe": "D2RLoader.exe"}
LOADER_FILES = {"D2RLoader.exe": "{game}/D2RLoader.exe", "D2RCore.dll": "{game}/D2RCore.dll", "d2rloader.mpq": "{game}/d2rloader.mpq"}
ENV_NAMES = {"steamoverlaygameid": "SteamOverlayGameId", "steamappid": "SteamAppId", "steamgameid": "SteamGameId"}


class Drift(Exception):
    """His files do not say what this script expects: read them again."""


def git(repo, *args):
    p = subprocess.run(["git", "-C", str(repo)] + list(args), capture_output=True)
    if p.returncode != 0:
        raise Drift("git " + " ".join(args) + " failed: " + p.stderr.decode("utf-8", "replace").strip())
    return p.stdout.decode("utf-8-sig")


def read_files(repo, ref):
    return {key: git(repo, "show", f"{ref}:{path}").replace("\r\n", "\n") for key, path in FILES.items()}


def find(pattern, text, what, flags=re.M):
    m = re.search(pattern, text, flags)
    if not m:
        raise Drift(f"{what}: not found in the shape this script knows ({pattern})")
    return m


def string_const(text, name, file):
    return find(rf"^{name} = '([^']*)'\s*$", text, f"{file}: {name}").group(1)


# ------------------------------------------------------------------ the facts

def facts_identity(t):
    f = "mod_identity.py"
    d = {name: string_const(t["identity"], name, f) for name in ("MOD_ID", "MOD_NAME", "DISPLAY_NAME", "TAGLINE")}
    # the layout the launcher relies on: <game>\mods\<MOD_NAME>\<MOD_NAME>.mpq\modinfo.json, plugin and ini in the mod
    find(r"^MOD_DIR = Path\('mods'\) / MOD_NAME\s*$", t["identity"], f + ": MOD_DIR")
    find(r"^MODINFO = MOD_DIR / f'\{MOD_NAME\}\.mpq/modinfo\.json'\s*$", t["identity"], f + ": MODINFO")
    find(r"^PLUGIN = MOD_DIR / 'd2rloader/plugins/d2rl-pcvr\.dll'\s*$", t["identity"], f + ": PLUGIN")
    find(r"^RESHADE_DIR = MOD_DIR / 'reshade'\s*$", t["identity"], f + ": RESHADE_DIR")
    return d


def facts_header(t):
    values = dict(re.findall(r"#define D2RPCVR_(\w+) (.+)", t["header"]))
    out = {}
    for key in ("MOD_ID", "PLUGIN_ID", "AUTHOR", "VERSION"):
        if key not in values or not re.fullmatch(r'"[^"]+"', values[key].strip()):
            raise Drift(f"vr/pcvr_identity.h: D2RPCVR_{key} is not a plain string")
        out[key] = values[key].strip().strip('"')
    return out


def facts_installer(t):
    s = t["iss"]
    d = {
        "version": find(r'^\s*#define ModVersion "([^"]+)"', s, "iss: ModVersion").group(1),
        "app_id": find(r"^AppId=(.+)$", s, "iss: AppId").group(1).strip(),
        "app_name": find(r"^AppName=(.+)$", s, "iss: AppName").group(1).strip(),
        "publisher": find(r"^AppPublisher=(.+)$", s, "iss: AppPublisher").group(1).strip(),
        "homepage": find(r"^AppPublisherURL=(\S+)", s, "iss: AppPublisherURL").group(1),
        "support": find(r"^AppSupportURL=(\S+)", s, "iss: AppSupportURL").group(1),
        "dir": find(r"^DefaultDirName=(.+)$", s, "iss: DefaultDirName").group(1).strip(),
        "privileges": find(r"^PrivilegesRequired=(\w+)", s, "iss: PrivilegesRequired").group(1),
    }
    if re.search(r"^UninstallFilesDir=", s, re.M):
        raise Drift("iss: UninstallFilesDir is set - the uninstaller is no longer in {app}")
    if "unins000.exe" not in t["smoke"]:
        raise Drift("installer/smoke_native_installer.ps1 no longer names unins000.exe")
    return d


def facts_launch(t, steam_app_id):
    s = t["launch"]
    f = "launch_windows.py"
    d = {
        "runtime_option": string_const(s, "RUNTIME_OPTION", f),
        "vdxr_option": string_const(s, "VDXR_OPTION", f),
        "vdxr_manifest": find(r"Path\(os\.environ\.get\('ProgramFiles', 'C:/Program Files'\)\) / '([^']+virtualdesktop-openxr\.json)'",
                              s, f + ": the VDXR manifest").group(1),
        "launch_log": find(r"Path\(cwd\) / '(d2rloader/logs/pcvr-launch\.log)'", s, f + ": the launch log").group(1),
        "dev_option": string_const(s, "DEV_OPTION", f),
    }
    # `-mod <MOD_NAME>`: both ways the loader is started (Steam: put in after the loader; Battle.net: the whole command)
    # since v0.3.18 the developer mode (--frame-dev) picks its own offline mod; the normal start is still MOD_NAME
    find(r"command\[index \+ 1:index \+ 1\] = \['-mod', (?:MOD if developer else )?MOD_NAME\]", s, f + ": -mod (Steam)")
    find(r"return \[str\(loader\), '-mod', (?:MOD if developer else )?MOD_NAME\], str\(game\)", s, f + ": -mod (Battle.net)")
    find(r"loader = ntpath\.join\(folder, 'D2RLoader\.exe'\)", s, f + ": the loader beside D2R.exe (Steam)")
    find(r"loader = game / 'D2RLoader\.exe'", s, f + ": the loader beside D2R.exe (Battle.net)")
    # environment: SteamOverlayGameId always removed; Battle.net: the Steam ids removed; Steam: removed, then both set to the app id
    find(r"if key\.lower\(\) == 'steamoverlaygameid':\s*\n\s*del environment\[key\]", s, f + ": SteamOverlayGameId removal")
    m = find(r"if battlenet_game\(arguments\) is not None:(?P<bn>.*?)\n    else:(?P<steam>.*?)\n    if VDXR_OPTION in arguments:",
             s, f + ": the Steam / Battle.net environment branches", re.S)
    removal = r"if key\.lower\(\) in \('steamappid', 'steamgameid'\):\s*\n\s*del environment\[key\]"
    if not re.search(removal, m.group("bn")) or "environment['SteamAppId']" in m.group("bn"):
        raise Drift(f + ": the Battle.net branch is no longer 'remove SteamAppId / SteamGameId'")
    if not re.search(removal, m.group("steam")):
        raise Drift(f + ": the Steam branch no longer removes SteamAppId / SteamGameId first")
    d["steam_app_id"] = find(r"environment\['SteamAppId'\] = environment\['SteamGameId'\] = '(\d+)'", m.group("steam"),
                             f + ": the Steam app id").group(1)
    if d["steam_app_id"] != steam_app_id:
        raise Drift(f"{f} sets {d['steam_app_id']} but steam_config.APP_ID is {steam_app_id}")
    # the Explorer hand-off: Windows, VR, and not Battle.net
    find(r"if os\.name == 'nt' and '--frame-vr' in arguments and battlenet_game\(arguments\) is None:\s*\n\s*return launch_from_desktop\(",
         s, f + ": the Explorer hand-off rule (Steam only)")
    # a runtime manifest that is missing is an error, never a fallback
    find(r"raise SystemExit\(f'OpenXR runtime manifest was not found: \{runtime\}'\)", s, f + ": missing runtime = error")
    find(r"environment\['XR_RUNTIME_JSON'\] = str\(runtime\.resolve\(\)\)", s, f + ": XR_RUNTIME_JSON")
    return d


def facts_dev(t, mod_name):
    """His developer mode (--frame-dev): a second D2RLoader mod folder of its own, staged on demand."""
    dev = string_const(t["dev"], "MOD", "prepare_dev_scenarios.py")
    if dev.lower() == mod_name.lower():
        raise Drift("prepare_dev_scenarios.py stages the developer mod under the normal mod's name")
    find(r"root = checked\(game / 'mods' / MOD\)", t["dev"], "prepare_dev_scenarios.py: the developer mod folder")
    return {"mod": dev}


def facts_games(t):
    s = t["game"]
    d = {"steam": string_const(s, "STEAM_VERSION", "game_install.py"), "battlenet": string_const(s, "BATTLENET_VERSION", "game_install.py")}
    experimental = bool(re.search(r"if detected == 'battlenet' and not experimental_battlenet:", s))
    return d, experimental


def facts_loader(t):
    s = t["install"]
    block = find(r"^LOADER_HASHES = \((.*?)\n\)", s, "install.py: LOADER_HASHES", re.M | re.S).group(1)
    archives = re.findall(r"'([0-9a-f]{64})'", block)
    if len(archives) < 1:
        raise Drift("install.py: LOADER_HASHES is empty")
    m = find(r"download_verified\('(https://d2rloader\.net/downloads/D2RLoader-(\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+)?)\.zip)'", s,
             "install.py: the D2RLoader archive URL")
    names = re.findall(r"'([^']+)'", find(r"if any\(name in \(([^)]*)\) for name in names\)", s, "install.py: the processes setup waits for").group(1))
    procs = []
    for n in names:
        if n not in PROCESS_NAMES:
            raise Drift(f"install.py waits for a process this script does not know: {n}")
        procs.append(PROCESS_NAMES[n])
    evidence = json.loads(t["evidence"])
    if set(evidence["archives"][k]["sha256"] for k in ("historical", "current")) != set(archives):
        raise Drift("the evidence file's archive hashes are not install.py's LOADER_HASHES")
    payload = {p["name"]: p["sha256"] for p in evidence["identical_payloads"]}
    files = []
    for name, path in LOADER_FILES.items():
        if name not in payload or not re.fullmatch(r"[0-9a-f]{64}", payload[name]):
            raise Drift(f"the evidence file has no SHA-256 for {name}")
        files.append({"path": path, "sha256_any_of": [payload[name]]})
    return {"archives": archives, "url": m.group(1), "version": m.group(2), "processes": procs, "files": files}


def facts_docs(t, identity, header):
    c = find(r"`mods/([^/`]+)/d2rloader/logs/(d2r-vr-frame\.log)`", t["coexistence"], "docs/MOD_COEXISTENCE.md: the runtime log")
    if c.group(1) != identity["MOD_NAME"]:
        raise Drift("docs/MOD_COEXISTENCE.md names the runtime log of another mod folder")
    find(r"`(d2rloader/logs/d2rloader\.log)`", t["workflow"], "docs/AGENT_WORKFLOW.md: d2rloader.log")
    find(r"`ReShade\.log`", t["workflow"], "docs/AGENT_WORKFLOW.md: ReShade.log")
    find(rf"mods/{re.escape(identity['MOD_NAME'])}/reshade", t["workflow"], "docs/AGENT_WORKFLOW.md: ReShade in the mod folder")
    tag = find(r"--release-tag v\d+\.\d+\.\d+(-[A-Za-z][A-Za-z-]*)", t["installer_readme"], "installer/README.md: the release tag shape").group(1)
    if not t["license"].startswith("MIT License"):
        raise Drift("LICENSE is not the MIT license any more")
    return {"frame_log": c.group(2), "tag_suffix": tag, "license": "MIT"}


# ------------------------------------------------------------------ the manifest

def build(repo, ref, icon, for_kit=False):
    commit = git(repo, "rev-parse", "--verify", ref + "^{commit}").strip()

    def tag_exists(tag):
        return subprocess.run(["git", "-C", str(repo), "rev-parse", "-q", "--verify", f"refs/tags/{tag}^{{commit}}"],
                              capture_output=True).returncode == 0
    return assemble(read_files(repo, ref), commit, tag_exists, icon, for_kit)


def assemble(t, commit, tag_exists, icon, for_kit=False):
    """The manifest from the texts of his files `t` (see FILES); tag_exists(name) says whether his repo has that release tag.
    for_kit: the copy for his installer, without `install_help` (launchers before 0.3.5 would call the manifest not valid)."""
    ident = facts_identity(t)
    head = facts_header(t)
    iss = facts_installer(t)
    steam_id = find(r"^APP_ID = '(\d+)'", t["steam"], "steam_config.py: APP_ID").group(1)
    launch = facts_launch(t, steam_id)
    dev = facts_dev(t, ident["MOD_NAME"])
    builds, battlenet_experimental = facts_games(t)
    loader = facts_loader(t)
    docs = facts_docs(t, ident, head)

    # the files must agree with each other
    if head["VERSION"] != iss["version"]:
        raise Drift(f"pcvr_identity.h says {head['VERSION']} but the installer script says {iss['version']}")
    if head["MOD_ID"] != ident["MOD_ID"]:
        raise Drift("pcvr_identity.h and mod_identity.py disagree on the mod id")
    if iss["app_name"] != ident["DISPLAY_NAME"]:
        raise Drift("the installer's AppName is not mod_identity.DISPLAY_NAME")
    if iss["privileges"].lower() != "lowest":
        raise Drift("the installer needs more than user rights: the manifest says needs_admin false")
    m = re.fullmatch(r"https://github\.com/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)", iss["homepage"])
    if not m:
        raise Drift("AppPublisherURL is not a github.com/owner/repo address")
    slug = m.group(1)
    if not re.fullmatch(r"\{localappdata\}\\[^{}]+", iss["dir"]):
        raise Drift("DefaultDirName is no longer {localappdata}\\<folder>")
    uninstall_exe = iss["dir"].replace("\\", "/") + "/unins000.exe"          # Inno's default name in {app}

    mod, version = ident["MOD_NAME"], iss["version"]
    tag_pattern = "v*" + docs["tag_suffix"]
    tag = f"v{version}{docs['tag_suffix']}"
    plugin_log = head["PLUGIN_ID"] + ".log"
    mod_logs = f"{{game}}/mods/{mod}/d2rloader/logs/"
    runtime = launch["runtime_option"]
    stores = ["steam", "battlenet"]

    option = {
        "id": "default",
        "label": "Play " + ident["DISPLAY_NAME"],
        "kind": "exec",
        "exe": "{game}/D2RLoader.exe",
        "args": ["-mod", mod],
        "cwd": "{game}",
        "via": "auto",
        "stores": stores,
        "runtimes": ["system", "steamvr", "vdxr", "file"],
        "env": [
            {"unset": [ENV_NAMES["steamoverlaygameid"]]},
            {"stores": ["steam"], "unset": [ENV_NAMES["steamappid"], ENV_NAMES["steamgameid"]],
             "set": {ENV_NAMES["steamappid"]: launch["steam_app_id"], ENV_NAMES["steamgameid"]: launch["steam_app_id"]}},
            {"stores": ["battlenet"], "unset": [ENV_NAMES["steamappid"], ENV_NAMES["steamgameid"]]},
        ],
        "x-note": (
            f"The same command his steamvr/launch_windows.py ends in: D2RLoader.exe -mod {mod}, started in the game folder. "
            f"Runtimes as in his wrapper: system = none of its flags (the active OpenXR runtime), steamvr = {runtime}<SteamVR>/steamxr_win64.json, "
            f"vdxr = {launch['vdxr_option']} ({launch['vdxr_manifest']} under Program Files), file = {runtime}<manifest>. "
            "The launcher hands the chosen one to the child as XR_RUNTIME_JSON, which is what his launch_environment sets. "
            "His wrapper hands a Steam start over to Explorer's desktop (Steam only, because Steam's overlay injects into the process chain "
            "of a launch option, and Battle.net is started directly). The launcher starts the game from its own desktop process, "
            "never from Steam's launch-option chain, so it does not use that hand-off."),
    }

    manifest = {
        "schema": "vrmod-manifest/1",
        "id": ident["MOD_ID"],
        "name": ident["DISPLAY_NAME"],
        "version": version,
        "publisher": head["AUTHOR"],
        "homepage": iss["homepage"],
        "support": SUPPORT,
        "license": docs["license"],
        "description": ident["TAGLINE"] + ".",
    }
    if not for_kit:
        manifest["install_help"] = INSTALL_HELP
    if icon:
        manifest["icon"] = icon
    manifest.update({
        "d2rloader_mod": {"name": mod},
        "game": {
            "id": "d2r",
            "builds": [builds["steam"], builds["battlenet"]],
            "stores": stores,
            "processes": loader["processes"],
            "x-builds-by-store": {"steam": builds["steam"],
                                  "battlenet": builds["battlenet"] + (" (experimental: offline testing)" if battlenet_experimental else "")},
        },
        "requires": [{
            "id": "d2rloader",
            "version": loader["version"],
            "provider": "launcher",
            "note": (f"File hashes of the {loader['version']} files unpacked from the reviewed ZIP, as his {FILES['evidence']} lists them. "
                     f"x-archive: the {len(loader['archives'])} archive hashes his install.py accepts."),
            "files": loader["files"],
            "x-archive": {"url": loader["url"], "sha256_any_of": loader["archives"]},
        }],
        "launch": {"options": [option]},
        "logs": [
            mod_logs + docs["frame_log"],
            mod_logs + plugin_log,
            "{game}/d2rloader/logs/" + plugin_log,
            "{game}/d2rloader/logs/d2rloader.log",
            "{game}/" + launch["launch_log"],
            f"{{game}}/mods/{mod}/reshade/ReShade.log",
        ],
        "update": {"kind": "github-release", "repo": slug, "tag": tag_pattern},
        "uninstall": {"exe": uninstall_exe, "needs_admin": False},
        "x-source": {
            "repo": iss["homepage"],
            "commit": commit,
            "release_tag": tag if tag_exists(tag) else None,
            "installer_app_id": iss["app_id"],
            "read_with": "git show <commit>:<path> only, none of his code was run",
            "files": [FILES[k] for k in FILES],
        },
        "x-unverified": unverified(icon, builds, battlenet_experimental, mod, plugin_log, uninstall_exe, iss, head, launch, dev),
    })
    return manifest


def unverified(icon, builds, experimental, mod, plugin_log, uninstall_exe, iss, head, launch, dev):
    u = {}
    if not icon:
        u["icon"] = ("left out because no Unleashed artwork exists. For a picture put a PNG or ICO next to this file and add "
                     "\"icon\": \"launcher_icon.png\". Without one the launcher shows a neutral placeholder.")
    u["game.builds"] = (f"a flat list. Steam {builds['steam']} is the build his releases are verified on, Battle.net {builds['battlenet']} is "
                        + ("experimental (his wrapper wants --experimental-battlenet). " if experimental else "supported. ")
                        + "The manifest cannot say which build belongs to which store (see x-builds-by-store).")
    u["launch.options[0].via"] = ("not run. His wrapper always goes through Explorer's desktop for Steam, the launcher does not "
                                  "(it starts from its own desktop process). Whether that is enough against Steam's overlay injection is untested.")
    u["launch.options[0]"] = ("his wrapper also passes on whatever else is in Steam's launch command after D2R.exe, and the Steam shortcut has the "
                              "OpenVR flag set for SteamVR. The launcher passes neither.")
    u["developer mode"] = (f"his wrapper's {launch['dev_option']} (Launch-Dev-SteamVR.cmd, steamvr/DEVELOPMENT_SCENARIOS.md) stages a second "
                           f"D2RLoader mod of its own, mods\\{dev['mod']}, and starts that one with an offline test character. The launcher has "
                           f"no such option. A mods\\{dev['mod']} folder, once it exists, is listed by the launcher as a plain D2RLoader mod "
                           f"and started with -mod {dev['mod']} alone (his docs say that route works too for a prepared environment), "
                           "without the environment rules above.")
    u["launch.options[0].runtimes"] = ("steamvr: the launcher finds SteamVR's steamxr_win64.json through Steam's libraries (his Battle.net shortcut "
                                       "gets the folder from setup). vdxr and system are as in his wrapper. Nothing was started with a headset.")
    u["logs"] = (f"{plugin_log} is D2RLoader's startup log of the plugin. Whether a mod's plugin writes it to mods\\{mod}\\d2rloader\\logs or to "
                 "the global d2rloader\\logs is not stated in his files, so both are listed. The same goes for ReShade.log: his docs say 'game "
                 f"root' for the old layout, with RESHADE_BASE_PATH_OVERRIDE it should be mods\\{mod}\\reshade. pcvr-launch.log is written "
                 "only by his Explorer hand-off, not when the launcher starts the game. A file that does not exist should just be skipped.")
    u["uninstall"] = (f"Inno's default unins000.exe in {{app}} = {iss['dir']} (his smoke test uses that name). Only the EXE installer has it, "
                      "not the ZIP / portable setup. /SILENT is left out on purpose: his uninstaller asks (ConfirmUninstall) and removes the "
                      "mod from every Steam and Battle.net copy it manages. PrivilegesRequired=lowest, so no admin rights.")
    u["publisher"] = (f"{head['AUTHOR']} is the plugin's author (D2RPCVR_AUTHOR), his installer script says '{iss['publisher']}'. "
                      "The license is his LICENSE: MIT, with BodyWalkVR's copyright line.")
    u["update"] = ("tag pattern from his tags and installer/README.md. Whether GitHub marks his releases as pre-releases (this decides whether "
                   "releases/latest finds them) is not in the repository.")
    u["settings_app"] = ("none, docs/MOD_COEXISTENCE.md says the legacy settings editor is not part of the native Windows package. "
                         f"Settings are mods\\{mod}\\d2rloader\\plugins\\d2r_pcvr.ini.")
    u["requires[0].files"] = ("the three hashes are the ones in his steamvr/evidence/2026-10-13-loader-archive-repack.json (identical_payloads), "
                              "not recomputed from the ZIP here. His install.py checks only the archive hashes.")
    u["everything"] = "read at the commit in x-source. No game, installer, headset or code of his was run."
    return u


# ------------------------------------------------------------------ the checks

def load_validator():
    spec = importlib.util.spec_from_file_location("check_schema", TOOLS / "check_schema.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_schema(manifest):
    cs = load_validator()
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    try:
        cs.validate(manifest, schema, schema)
    except cs.Invalid as e:
        raise Drift("the manifest breaks the schema: " + str(e))
    # what the contract (docs/MOD_MANIFEST.md) adds to the schema
    if "exclusive_group" in manifest or "files" in manifest:
        raise Drift("a manifest with d2rloader_mod has no exclusive_group and no files")
    if not manifest["launch"]["options"][0]["args"][:1] == ["-mod"]:
        raise Drift("the launch option must start D2RLoader with -mod")


def render(manifest):
    text = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    # CMake embeds this file as the launcher's built-in profile of the mod (src/builtin_mods.h.in, configure_file @ONLY): no '@' in its text.
    # A ';' is kept as it is by file(READ) and configure_file (his install text has one; tests/mods_test.cpp reads it back from the built-in profile).
    if "@" in text:
        raise Drift("the manifest text holds an '@' (CMake embeds this file as the built-in profile): reword the text that has it")
    return text


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo", required=True, type=Path, help="a clone of github.com/bearstonem/d2r-pcvr (only read with git show)")
    ap.add_argument("--ref", default="origin/main", help="branch, tag or commit to read (default origin/main)")
    ap.add_argument("--out", default=str(DEFAULT_OUT), help="where to write ('-' = stdout)")
    ap.add_argument("--icon", default="", help="name of a picture his installer puts next to the manifest (none by default: there is none)")
    ap.add_argument("--for-kit", action="store_true", help="the copy for his installer (kits/d2r-pcvr-for-review): without install_help")
    args = ap.parse_args(argv)
    try:
        manifest = build(args.repo, args.ref, args.icon, args.for_kit)
        check_schema(manifest)
        text = render(manifest)
    except Drift as e:
        print("his files changed (or cannot be read): " + str(e), file=sys.stderr)
        return 2
    if args.out == "-":
        sys.stdout.write(text)
    else:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print(f"{args.out}: {manifest['name']} {manifest['version']} from {manifest['x-source']['commit'][:12]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
