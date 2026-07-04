#!/usr/bin/env python3
"""Compare CLI option names vs yaml_writer node keys per app unit.

A CLI option without a matching yaml key means `--dump-config`-style output
loses that option (f4_enable_occ shape). Only heuristic: names usually match.
"""
import re
import sys
from pathlib import Path

REPO = Path("/home/user/ocudu")

UNITS = {
    "du_low": ("apps/units/flexible_o_du/o_du_low/du_low_config_cli11_schema.cpp",
               "apps/units/flexible_o_du/o_du_low/du_low_config_yaml_writer.cpp"),
    "du_high": ("apps/units/flexible_o_du/o_du_high/du_high/du_high_config_cli11_schema.cpp",
                "apps/units/flexible_o_du/o_du_high/du_high/du_high_config_yaml_writer.cpp"),
}

OPT_RE = re.compile(r'"--([a-zA-Z0-9_]+)"')
KEY_RE = re.compile(r'(?:node|.*_node|.*_subnode)\s*\[\s*"([a-zA-Z0-9_]+)"\s*\]')
KEY_RE2 = re.compile(r'\[\s*"([a-zA-Z0-9_]+)"\s*\]')

for unit, (cli, yaml) in UNITS.items():
    cli_text = (REPO / cli).read_text()
    yaml_text = (REPO / yaml).read_text()
    opts = set(OPT_RE.findall(cli_text))
    keys = set(KEY_RE2.findall(yaml_text))
    missing = sorted(opts - keys)
    print(f"== {unit}: {len(opts)} CLI options, {len(keys)} yaml keys, {len(missing)} missing from yaml ==")
    for m in missing:
        print(f"   {m}")
