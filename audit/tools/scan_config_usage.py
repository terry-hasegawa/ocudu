#!/usr/bin/env python3
"""Scan config struct fields and classify where each field name is referenced.

Heuristic dead-knob detector for OCUDU config plumbing:
  declaration -> cli11 schema -> yaml writer -> validator -> translator -> lib consumption
Fields that appear in CLI/yaml but have no translator/lib consumption are flagged.
"""
import re
import subprocess
import sys
import json
from collections import defaultdict

REPO = "/home/user/ocudu"

# (header, struct filter or None for all structs in file)
TARGETS = [
    "apps/units/flexible_o_du/o_du_low/du_low_config.h",
    "apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h",
    "include/ocudu/phy/upper/upper_phy_factories.h",
]

FIELD_RE = re.compile(
    r"^\s*(?!return|if|for|while|else|using|namespace|template|static_assert|typedef|public|private|struct|class|enum|#)"
    r"(?P<type>(?:[\w:]+(?:<[^;{}]*>)?[\s\*&]+|(?:std::)?(?:optional|vector|array|map)<[^;{}]*>\s+))"
    r"(?P<name>[a-zA-Z_]\w*)\s*(?:=\s*[^;]+|\{[^;]*\})?;\s*(?://.*)?$"
)
STRUCT_RE = re.compile(r"^\s*struct\s+(\w+)\b")


def extract_fields(path):
    fields = []  # (struct, name, line)
    struct_stack = []
    brace_depth = 0
    pending_struct = None
    in_func_depth = None
    with open(f"{REPO}/{path}") as f:
        for lineno, line in enumerate(f, 1):
            m = STRUCT_RE.match(line)
            if m:
                pending_struct = m.group(1)
            opens = line.count("{")
            closes = line.count("}")
            if pending_struct and opens:
                struct_stack.append((pending_struct, brace_depth))
                pending_struct = None
            # crude function-body skip: constructor bodies like `du_low_unit_expert_threads_config() {`
            if re.search(r"\)\s*(?:const)?\s*\{", line) and in_func_depth is None:
                in_func_depth = brace_depth + opens - closes
            brace_depth += opens - closes
            if in_func_depth is not None and brace_depth <= in_func_depth:
                in_func_depth = None
                continue
            if in_func_depth is not None:
                continue
            if not struct_stack:
                continue
            fm = FIELD_RE.match(line)
            if fm and "(" not in fm.group("name"):
                t = fm.group("type").strip()
                if t in ("return", "delete"):
                    continue
                fields.append((struct_stack[-1][0], fm.group("name"), lineno, t))
            while struct_stack and brace_depth <= struct_stack[-1][1]:
                struct_stack.pop()
    return fields


def classify(path_):
    p = path_
    if "cli11_schema" in p:
        return "cli11"
    if "yaml_writer" in p:
        return "yaml"
    if "config_validator" in p or "validator" in p:
        return "validator"
    if "translator" in p:
        return "translator"
    if p.startswith("tests/"):
        return "tests"
    if p.startswith("apps/"):
        return "apps_other"
    if p.startswith(("lib/", "include/")):
        return "lib"
    return "other"


def grep_word(word):
    out = subprocess.run(
        ["rg", "-w", "--no-heading", "-c", word,
         "apps", "lib", "include", "tests"],
        cwd=REPO, capture_output=True, text=True)
    hits = defaultdict(int)
    files = defaultdict(list)
    for line in out.stdout.splitlines():
        f, _, cnt = line.rpartition(":")
        cat = classify(f)
        hits[cat] += int(cnt)
        files[cat].append(f"{f}:{cnt}")
    return hits, files


def main():
    results = []
    for header in TARGETS:
        for struct, name, lineno, typ in extract_fields(header):
            hits, files = grep_word(name)
            decl = f"{header}:{lineno}"
            # consumption = lib hits beyond declaration if header is in include/
            lib_hits = hits.get("lib", 0)
            if header.startswith("include/"):
                lib_hits -= 1  # its own declaration
            suspicious = ""
            if hits.get("cli11", 0) > 0 and hits.get("translator", 0) == 0 and lib_hits <= 0:
                suspicious = "DEAD?"
            elif header.startswith("include/") and lib_hits <= 0:
                suspicious = "LIB-UNUSED?"
            results.append({
                "header": header, "struct": struct, "field": name,
                "decl": decl, "type": typ,
                "cli11": hits.get("cli11", 0), "yaml": hits.get("yaml", 0),
                "validator": hits.get("validator", 0),
                "translator": hits.get("translator", 0),
                "apps_other": hits.get("apps_other", 0),
                "lib": max(lib_hits, 0) if header.startswith("include/") else hits.get("lib", 0),
                "tests": hits.get("tests", 0),
                "flag": suspicious,
                "files": {k: v for k, v in files.items()},
            })
    with open(sys.argv[1] if len(sys.argv) > 1 else "/dev/stdout", "w") as out:
        json.dump(results, out, indent=1)
    # print summary of flagged
    print(f"{'struct':40s} {'field':45s} cli yaml val tr apps lib tests flag")
    for r in results:
        if r["flag"]:
            print(f"{r['struct'][:40]:40s} {r['field'][:45]:45s} "
                  f"{r['cli11']:3d} {r['yaml']:4d} {r['validator']:3d} {r['translator']:2d} "
                  f"{r['apps_other']:4d} {r['lib']:3d} {r['tests']:5d} {r['flag']}")


if __name__ == "__main__":
    main()
