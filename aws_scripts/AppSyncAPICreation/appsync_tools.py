#!/usr/bin/env python3
"""
No-deps helpers for deploy-appsync.sh
- Placeholder substitution: ENV:VARNAME
- Use BOTH environment vars and shell variables (passed via vars-to-json)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Set

PLACEHOLDER_RE = re.compile(r"ENV:([A-Z0-9_]+)")

def list_placeholders(text: str) -> List[str]:
    keys: Set[str] = set()
    for m in PLACEHOLDER_RE.finditer(text):
        keys.add(m.group(1))
    return sorted(keys)

def substitute(text: str, vars_map: Dict[str, str]) -> str:
    def repl(m: re.Match[str]) -> str:
        key = m.group(1)
        if key in vars_map and vars_map[key] != "":
            return vars_map[key]
        env = os.environ.get(key)
        if env is not None and env != "":
            return env
        # leave as-is if missing (lets AWS error with something meaningful)
        return m.group(0)

    return PLACEHOLDER_RE.sub(repl, text)

def cmd_list_placeholders(args: argparse.Namespace) -> int:
    p = Path(args.path)
    text = p.read_text(encoding="utf-8")
    for k in list_placeholders(text):
        print(k)
    return 0

def cmd_vars_to_json(args: argparse.Namespace) -> int:
    """
    Build a JSON dict of placeholder->value using current process env *and* locals.
    In bash, we call this after list-placeholders and pass the keys; bash variables
    are visible via os.environ only if exported, so deploy script sets values by
    invoking this helper while the vars are still in bash and passing them as args:
      python appsync_tools.py vars-to-json KEY1 KEY2 ...
    This tool reads the process env; for non-exported bash vars, the deploy script
    calls us with KEYs and we return a dict of KEY -> os.environ.get(KEY,"") and
    relies on substitute-file to prefer this dict when provided.
    """
    keys: List[str] = args.keys or []
    out: Dict[str, str] = {}
    for k in keys:
        out[k] = os.environ.get(k, "")
    print(json.dumps(out, separators=(",", ":")))
    return 0

def cmd_substitute_file(args: argparse.Namespace) -> int:
    src = Path(args.in_path)
    dst = Path(args.out_path)

    text = src.read_text(encoding="utf-8")

    vars_map: Dict[str, str] = {}
    if args.vars_json:
        try:
            vars_map = json.loads(args.vars_json)
            if not isinstance(vars_map, dict):
                raise ValueError("vars-json must be a JSON object")
            # stringify values
            vars_map = {str(k): "" if v is None else str(v) for k, v in vars_map.items()}
        except Exception as e:
            print(f"ERROR: vars-json parse failed: {e}", file=sys.stderr)
            return 2

    out = substitute(text, vars_map)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(out, encoding="utf-8")
    return 0

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)

    p_lp = sub.add_parser("list-placeholders", help="Print ENV: placeholders found in a file")
    p_lp.add_argument("path")
    p_lp.set_defaults(func=cmd_list_placeholders)

    p_v = sub.add_parser("vars-to-json", help="Return a JSON dict of KEY->value (from current env)")
    p_v.add_argument("keys", nargs="*")
    p_v.set_defaults(func=cmd_vars_to_json)

    p_sf = sub.add_parser("substitute-file", help="Substitute ENV: placeholders in a file")
    p_sf.add_argument("--in", dest="in_path", required=True)
    p_sf.add_argument("--out", dest="out_path", required=True)
    p_sf.add_argument("--vars-json", dest="vars_json", default="")
    p_sf.set_defaults(func=cmd_substitute_file)

    return p

def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return int(args.func(args))

if __name__ == "__main__":
    raise SystemExit(main())
