#!/usr/bin/env python3
"""For each config header field, count references in its designated consumer files.
Report fields with zero consumer references (dead-knob candidates, collision-proof)."""
import re
import sys
sys.path.insert(0, "/tmp/claude-0/-home-user-ocudu/4798bf42-48be-5d00-a039-c883fdd7e1b6/scratchpad")
from scan_config_usage import extract_fields, REPO

CASES = [
    # (header, [consumer files], label)
    ("apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h",
     ["apps/units/flexible_o_du/o_du_high/du_high/du_high_config_translators.cpp"],
     "du_high fields not referenced by du_high translators"),
    ("include/ocudu/scheduler/config/scheduler_expert_config.h",
     None,  # None -> grep whole lib/scheduler
     "scheduler_expert_config fields not referenced under lib/scheduler"),
    ("include/ocudu/du/du_cell_config.h",
     None,
     "du_cell_config fields not referenced under lib/",
     ),
    ("include/ocudu/scheduler/config/pucch_resource_builder_params.h",
     ["lib/scheduler/config/pucch_resource_generator.cpp",
      "lib/du/du_high/du_manager/ran_resource_management/du_pucch_resource_manager.cpp"],
     "pucch_resource_builder_params fields not referenced by generator/manager"),
    ("include/ocudu/scheduler/config/srs_builder_params.h",
     None,
     "srs_builder_params fields not referenced under lib/"),
]

import subprocess

def count_in_files(word, files):
    total = 0
    for f in files:
        try:
            text = open(f"{REPO}/{f}").read()
        except FileNotFoundError:
            continue
        total += len(re.findall(rf"\b{re.escape(word)}\b", text))
    return total

def count_in_lib(word, subdir="lib"):
    out = subprocess.run(["rg", "-w", "-c", word, subdir], cwd=REPO,
                         capture_output=True, text=True)
    return sum(int(l.rpartition(":")[2]) for l in out.stdout.splitlines())

for case in CASES:
    header, consumers, label = case[0], case[1], case[2]
    print(f"\n===== {label} ({header}) =====")
    try:
        fields = extract_fields(header)
    except FileNotFoundError:
        print("   header not found, skipping")
        continue
    for struct, name, lineno, typ in fields:
        if consumers is not None:
            n = count_in_files(name, consumers)
        else:
            n = count_in_lib(name)
            # subtract declaration if header under include/ shadows lib scan? headers are include/, not lib/ -> no
        if n == 0:
            print(f"   {struct}.{name}  ({header.split('/')[-1]}:{lineno})")
