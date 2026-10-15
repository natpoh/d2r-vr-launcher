#!/usr/bin/env python3
"""Keeps schema/vrmod-manifest.1.schema.json and the launcher's C++ manifest validator in step.

    check_schema.py <vr_launcher dir> <manifest_test executable>

Every manifest under examples/ and tests/fixtures/manifests/ is checked twice:
  * by a small JSON Schema (draft-07 subset) validator written here - no packages needed;
  * by the C++ validator (`manifest_test --validate FILE...`).
By file name: ok_*.json and the examples must pass both; bad_*.json must fail both; semantic_*.json break a rule that only the C++
validator can check (duplicate dst, folder/file mismatch...) and must fail there (the schema is not asked).
"""
import json
import re
import subprocess
import sys
from pathlib import Path


class Invalid(Exception):
    pass


def type_ok(v, t):
    if t == "object":
        return isinstance(v, dict)
    if t == "array":
        return isinstance(v, list)
    if t == "string":
        return isinstance(v, str)
    if t == "boolean":
        return isinstance(v, bool)
    if t == "integer":
        return isinstance(v, int) and not isinstance(v, bool)
    if t == "number":
        return isinstance(v, (int, float)) and not isinstance(v, bool)
    if t == "null":
        return v is None
    raise ValueError("type " + t)


def validate(v, s, root, path="/"):
    """Raises Invalid(message) on the first problem."""
    if "$ref" in s:
        node = root
        for part in s["$ref"].lstrip("#/").split("/"):
            node = node[part]
        return validate(v, node, root, path)
    if "const" in s and v != s["const"]:
        raise Invalid(f"{path}: must be {s['const']!r}")
    if "enum" in s and v not in s["enum"]:
        raise Invalid(f"{path}: {v!r} is not one of {s['enum']}")
    if "type" in s:
        types = s["type"] if isinstance(s["type"], list) else [s["type"]]
        if not any(type_ok(v, t) for t in types):
            raise Invalid(f"{path}: wrong type, wanted {types}")
    if isinstance(v, str):
        if "minLength" in s and len(v) < s["minLength"]:
            raise Invalid(f"{path}: too short")
        if "pattern" in s and not re.search(s["pattern"], v):
            raise Invalid(f"{path}: {v!r} does not match {s['pattern']}")
    if isinstance(v, list):
        if "minItems" in s and len(v) < s["minItems"]:
            raise Invalid(f"{path}: too few items")
        if "items" in s:
            for i, e in enumerate(v):
                validate(e, s["items"], root, f"{path}{i}/")
    if isinstance(v, dict):
        for k in s.get("required", []):
            if k not in v:
                raise Invalid(f"{path}: missing {k!r}")
        props = s.get("properties", {})
        pats = s.get("patternProperties", {})
        for k, e in v.items():
            if k in props:
                validate(e, props[k], root, f"{path}{k}/")
                continue
            hit = [p for p in pats if re.search(p, k)]
            if hit:
                for p in hit:
                    validate(e, pats[p], root, f"{path}{k}/")
                continue
            ap = s.get("additionalProperties", True)
            if ap is False:
                raise Invalid(f"{path}: unknown field {k!r}")
            if isinstance(ap, dict):
                validate(e, ap, root, f"{path}{k}/")
    for sub in s.get("allOf", []):
        validate(v, sub, root, path)
    if "not" in s:
        try:
            validate(v, s["not"], root, path)
        except Invalid:
            pass
        else:
            raise Invalid(f"{path}: matches a 'not' rule")
    if "if" in s:
        try:
            validate(v, s["if"], root, path)
            cond = True
        except Invalid:
            cond = False
        branch = s.get("then") if cond else s.get("else")
        if branch:
            validate(v, branch, root, path)


def main():
    base = Path(sys.argv[1])
    exe = sys.argv[2]
    schema = json.loads((base / "schema" / "vrmod-manifest.1.schema.json").read_text(encoding="utf-8"))
    files = sorted(base.glob("examples/*/manifest.json")) + sorted(base.glob("tests/fixtures/manifests/*.json"))
    if not files:
        print("no manifests found")
        return 1
    out = subprocess.run([exe, "--validate"] + [str(f) for f in files], capture_output=True, text=True)
    cpp = {}
    for line in out.stdout.splitlines():
        verdict, _, name = line.partition(" ")
        cpp[name.strip()] = verdict
    problems = []
    for f in files:
        name = f.name if f.parent.name == "manifests" else f"{f.parent.name}/manifest.json"
        kind = "ok" if f.name == "manifest.json" or f.name.startswith("ok_") else ("bad" if f.name.startswith("bad_") else "semantic")
        try:
            validate(json.loads(f.read_text(encoding="utf-8")), schema, schema)
            by_schema = True
            why = ""
        except Invalid as e:
            by_schema = False
            why = str(e)
        by_cpp = cpp.get(str(f)) == "OK"
        if kind == "ok" and not (by_schema and by_cpp):
            problems.append(f"{name}: should pass both (schema: {by_schema} {why}; C++: {cpp.get(str(f))})")
        elif kind == "bad" and (by_schema or by_cpp):
            problems.append(f"{name}: should fail both (schema accepts: {by_schema}; C++ accepts: {by_cpp})")
        elif kind == "semantic" and by_cpp:
            problems.append(f"{name}: the C++ validator should reject it")
        print(f"  {name:48s} schema:{'ok ' if by_schema else 'BAD'}  c++:{'ok ' if by_cpp else 'BAD'}  ({kind})")
    if problems:
        print("\nPARITY PROBLEMS:")
        for p in problems:
            print("  " + p)
        return 1
    print(f"\n{len(files)} manifests: the schema and the C++ validator agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
