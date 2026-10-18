#!/usr/bin/env python3
"""Makes the ZIP fixtures of tests/zip_test.cpp with Python's own zipfile (a writer that is not ours), into tests/fixtures/zip/.

    make_zip_fixtures.py [<output folder>]

Deterministic (fixed times, seeded bytes): running it again gives the same files. fixtures.json lists, for the good archives, every file
and the SHA-256 it must unpack to. The damaged ones are made by patching bytes of a good archive, so each has exactly one fault.
"""
import hashlib
import json
import random
import struct
import sys
import zipfile
from pathlib import Path

out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "fixtures" / "zip"
out.mkdir(parents=True, exist_ok=True)
for old in out.glob("*"):
    old.unlink()

rng = random.Random(20261018)
TIME = (2026, 10, 18, 12, 0, 0)

text = ("D2R VR launcher fixture line %05d - the quick brown fox jumps over the lazy dog\n" % 0).encode()
big_text = b"".join(("line %05d of a text that compresses well, with some words repeated: alpha beta gamma delta\n" % i).encode() for i in range(1200))
noise = bytes(rng.randrange(256) for _ in range(20000))             # incompressible: deflate stores it in raw blocks
mixed = b"".join((noise[i:i + 300] + b"ABCABCABC" * 40) for i in range(0, 20000, 300))
FILES = {
    "readme.txt": text,
    "empty.txt": b"",
    "big/text.txt": big_text,
    "big/noise.bin": noise,
    "x/y/z/deep/mixed.bin": mixed,
    "x/y/z/one-byte": b"Q",
}
DIRS = ["big/", "x/", "x/y/", "x/y/z/", "x/y/z/deep/", "empty-folder/"]


def zi(name, method, dir_=False):
    i = zipfile.ZipInfo(name, TIME)
    i.compress_type = method
    i.external_attr = (0o40755 << 16) | 0x10 if dir_ else (0o100644 << 16)
    i.create_system = 3
    return i


def write(name, method, level=None, files=FILES, dirs=DIRS, extra=None):
    p = out / name
    with zipfile.ZipFile(p, "w", method, compresslevel=level) as z:
        for d in dirs:
            z.writestr(zi(d, zipfile.ZIP_STORED, True), b"")
        for n, data in files.items():
            z.writestr(zi(n, method), data, compress_type=method, compresslevel=level)
        if extra:
            extra(z)
    return p


write("stored.zip", zipfile.ZIP_STORED)
write("deflate1.zip", zipfile.ZIP_DEFLATED, 1)
write("deflate6.zip", zipfile.ZIP_DEFLATED, 6)
write("deflate9.zip", zipfile.ZIP_DEFLATED, 9)
write("deflate0.zip", zipfile.ZIP_DEFLATED, 0)                       # deflate with "stored" blocks only

# one archive with a UTF-8 name (the language flag is set by zipfile for non-ASCII names)
with zipfile.ZipFile(out / "utf8_name.zip", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr(zi("caf\u00e9/\u00fc.txt", zipfile.ZIP_DEFLATED), b"umlaut")

# ---- unsafe names (python's writestr keeps the name exactly when it is set on the ZipInfo after the constructor)
def raw_named(fname, names, method=zipfile.ZIP_STORED):
    with zipfile.ZipFile(out / fname, "w") as z:
        z.writestr(zi("ok.txt", method), b"fine")
        for n in names:
            info = zi("placeholder", method)
            info.filename = n
            z.writestr(info, b"evil")


raw_named("zipslip.zip", ["../evil.txt"])
raw_named("zipslip_deep.zip", ["a/b/../../../evil.txt"])
raw_named("absolute.zip", ["/etc/evil.txt"])
raw_named("drive.zip", ["C:/evil.txt"])
raw_named("backslash.zip", ["..\\evil.txt"])
raw_named("stream.zip", ["file.txt:hidden"])
raw_named("device.zip", ["nul.txt"])
raw_named("dup.zip", ["ok.txt"])
raw_named("dup_case.zip", ["OK.TXT"])
raw_named("file_and_folder.zip", ["ok.txt/inside.txt"])

with zipfile.ZipFile(out / "symlink.zip", "w") as z:
    z.writestr(zi("ok.txt", zipfile.ZIP_STORED), b"fine")
    info = zi("link", zipfile.ZIP_STORED)
    info.external_attr = 0xA1FF0000                                   # a Unix symbolic link
    z.writestr(info, b"../../outside")

with zipfile.ZipFile(out / "bzip2.zip", "w") as z:
    z.writestr(zi("packed.txt", zipfile.ZIP_BZIP2), b"x" * 1000)


# ---- damaged archives: patch bytes of a good one
def read(name):
    return bytearray((out / name).read_bytes())


def central_records(b):
    eocd = b.rfind(b"PK\x05\x06")
    cd_off = struct.unpack_from("<I", b, eocd + 16)[0]
    n = struct.unpack_from("<H", b, eocd + 10)[0]
    pos, recs = cd_off, []
    for _ in range(n):
        nl, el, cl = struct.unpack_from("<HHH", b, pos + 28)
        name = bytes(b[pos + 46:pos + 46 + nl]).decode("utf-8")
        local = struct.unpack_from("<I", b, pos + 42)[0]
        recs.append((name, pos, local))
        pos += 46 + nl + el + cl
    return recs


def rec(b, name):
    return next(r for r in central_records(b) if r[0] == name)


good = read("deflate6.zip")

b = bytearray(good)                                                  # the CRC of one file is wrong (in the directory and the local header)
_, c, l = rec(b, "readme.txt")
struct.pack_into("<I", b, c + 16, struct.unpack_from("<I", b, c + 16)[0] ^ 0xDEADBEEF)
struct.pack_into("<I", b, l + 14, struct.unpack_from("<I", b, c + 16)[0])
(out / "bad_crc.zip").write_bytes(b)

b = bytearray(good)                                                  # the directory claims 2 GB for a small file
_, c, l = rec(b, "readme.txt")
struct.pack_into("<I", b, c + 24, 0x7FFFFFF0)
(out / "huge_claim.zip").write_bytes(b)

b = bytearray(good)                                                  # the directory claims less than the data holds (a "bomb" that lies small)
_, c, l = rec(b, "big/text.txt")
struct.pack_into("<I", b, c + 24, 100)
(out / "small_claim.zip").write_bytes(b)

b = bytearray(good)                                                  # compressed data of one file damaged in the middle
_, c, l = rec(b, "big/text.txt")
nl, el = struct.unpack_from("<HH", b, l + 26)
cs = struct.unpack_from("<I", b, c + 20)[0]
mid = l + 30 + nl + el + cs // 2
for i in range(mid, mid + 16):
    b[i] ^= 0x5A
(out / "damaged_data.zip").write_bytes(b)

b = bytearray(good)                                                  # cut off: the end of the central directory is gone
(out / "truncated.zip").write_bytes(bytes(b[: len(b) * 2 // 3]))

b = bytearray(good)                                                  # an entry is marked as encrypted
_, c, l = rec(b, "readme.txt")
struct.pack_into("<H", b, c + 8, struct.unpack_from("<H", b, c + 8)[0] | 1)
(out / "encrypted.zip").write_bytes(b)

b = bytearray(good)                                                  # the local header names another file than the directory does
_, c, l = rec(b, "readme.txt")
b[l + 30] = ord("R")
(out / "name_mismatch.zip").write_bytes(b)

(out / "not_a_zip.zip").write_bytes(b"this is not a zip archive at all, only text" * 10)
(out / "empty.zip").write_bytes(b"PK\x05\x06" + b"\x00" * 18)         # a valid zip with no entries

# ---- what the good archives must unpack to
def sha(d):
    return hashlib.sha256(d).hexdigest()


listing = {
    "files": {n: sha(d) for n, d in FILES.items()},
    "dirs": [d.rstrip("/") for d in DIRS],
    "good": ["stored.zip", "deflate0.zip", "deflate1.zip", "deflate6.zip", "deflate9.zip"],
}
(out / "fixtures.json").write_text(json.dumps(listing, indent=2), encoding="utf-8")
print("wrote", len(list(out.glob("*"))), "files to", out)
