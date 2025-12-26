#!/usr/bin/env python3
"""Utilities for deploying AWS IoT Topic Rules from JSON files.

Supported input shapes:
- {"ruleName": "...", "topicRulePayload": {...}}
- {"ruleName": "...", "payload": {...}}
- {"ruleArn": "...", "rule": {...}}  (aws iot get-topic-rule output)
- {...}  (payload-only)

Placeholder substitution:
- Any string value exactly equal to "ENV:VARNAME" is replaced by os.environ["VARNAME"]
  when set and non-empty.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Any, Dict, Iterable, List, Tuple


ALLOWED_TOPIC_RULE_PAYLOAD_KEYS = {
    "sql",
    "description",
    "actions",
    "ruleDisabled",
    "awsIotSqlVersion",
    "errorAction",
}


def _load_json(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def get_rule_name(data: Any) -> str:
    """Return rule name if present, otherwise empty string."""
    if not isinstance(data, dict):
        return ""

    rule = data.get("rule")
    if isinstance(rule, dict) and isinstance(rule.get("ruleName"), str):
        return rule["ruleName"]

    if isinstance(data.get("ruleName"), str):
        return data["ruleName"]

    if isinstance(data.get("RuleName"), str):
        return data["RuleName"]

    return ""


def extract_payload(data: Any) -> Dict[str, Any]:
    """Extract an IoT TopicRulePayload object from one of the supported shapes."""
    payload: Any

    if isinstance(data, dict):
        if isinstance(data.get("topicRulePayload"), dict):
            payload = data["topicRulePayload"]
        elif isinstance(data.get("payload"), dict):
            payload = data["payload"]
        elif isinstance(data.get("rule"), dict):
            payload = data["rule"]
        else:
            payload = data
    else:
        payload = data

    if not isinstance(payload, dict):
        raise ValueError("payload must be a JSON object")

    # If payload came from get-topic-rule's "rule" object, it contains extra keys.
    clean = {k: payload[k] for k in payload.keys() if k in ALLOWED_TOPIC_RULE_PAYLOAD_KEYS}

    if clean:
        return clean

    # Otherwise, try to treat it as already-a-payload (but remove common invalid keys)
    clean = dict(payload)
    for k in ("ruleName", "createdAt", "ruleArn"):
        clean.pop(k, None)
    return clean


def validate_payload(payload: Dict[str, Any], source: str = "payload") -> None:
    if not isinstance(payload, dict):
        raise ValueError(f"{source} must be a JSON object")
    if "sql" not in payload or "actions" not in payload or not isinstance(payload.get("actions"), list):
        raise ValueError(f"{source} must contain 'sql' and 'actions' (array)")


def _walk(obj: Any) -> Iterable[Tuple[List[str], Any]]:
    """Yield (path, value) for every node in a JSON-like structure."""
    if isinstance(obj, dict):
        for k, v in obj.items():
            for p, vv in _walk(v):
                yield [str(k), *p], vv
            yield [str(k)], v
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            for p, vv in _walk(v):
                yield [str(i), *p], vv
            yield [str(i)], v


def substitute_env_placeholders(obj: Any, strict: bool = True) -> Tuple[Any, List[str]]:
    """Replace strings equal to 'ENV:VARNAME' with env var values.

    Returns: (new_obj, unresolved_paths)
    """

    def repl(x: Any) -> Any:
        if isinstance(x, str) and x.startswith("ENV:"):
            var = x[4:]
            val = os.environ.get(var, "")
            if val:
                return val
            return x
        if isinstance(x, dict):
            return {k: repl(v) for k, v in x.items()}
        if isinstance(x, list):
            return [repl(v) for v in x]
        return x

    new_obj = repl(obj)

    unresolved: List[str] = []
    for path, value in _walk(new_obj):
        if isinstance(value, str) and value.startswith("ENV:"):
            unresolved.append(".".join(path))

    if strict and unresolved:
        return new_obj, unresolved

    return new_obj, unresolved


def get_rule_disabled(payload: Dict[str, Any]) -> bool:
    return bool(payload.get("ruleDisabled", False))


def get_lambda_arns(payload: Any) -> List[str]:
    arns: List[str] = []

    def walk(x: Any) -> None:
        if isinstance(x, dict):
            lam = x.get("lambda")
            if isinstance(lam, dict):
                fn = lam.get("functionArn")
                if isinstance(fn, str) and fn:
                    arns.append(fn)
            for v in x.values():
                walk(v)
        elif isinstance(x, list):
            for v in x:
                walk(v)

    walk(payload)

    # De-dup preserving order
    seen = set()
    out: List[str] = []
    for a in arns:
        if a not in seen:
            seen.add(a)
            out.append(a)
    return out


def cmd_rule_name(args: argparse.Namespace) -> int:
    data = _load_json(args.file)
    print(get_rule_name(data))
    return 0


def cmd_payload(args: argparse.Namespace) -> int:
    data = _load_json(args.file)
    payload = extract_payload(data)

    if args.substitute_env:
        payload, unresolved = substitute_env_placeholders(payload, strict=args.strict)
        if args.strict and unresolved:
            print(
                f"ERROR: unresolved ENV: placeholders in {args.file}:\n" +
                "\n".join(f"  - {p}" for p in unresolved),
                file=sys.stderr,
            )
            return 2

    validate_payload(payload, source=args.file)
    print(json.dumps(payload, separators=(",", ":"), ensure_ascii=False))
    return 0


def cmd_validate(args: argparse.Namespace) -> int:
    data = _load_json(args.file)
    payload = extract_payload(data)
    validate_payload(payload, source=args.file)
    return 0


def cmd_rule_disabled(args: argparse.Namespace) -> int:
    data = _load_json(args.file)
    payload = extract_payload(data)
    if args.substitute_env:
        payload, unresolved = substitute_env_placeholders(payload, strict=args.strict)
        if args.strict and unresolved:
            print(
                f"ERROR: unresolved ENV: placeholders in {args.file}:\n" +
                "\n".join(f"  - {p}" for p in unresolved),
                file=sys.stderr,
            )
            return 2
    print("true" if get_rule_disabled(payload) else "false")
    return 0


def cmd_lambda_arns(args: argparse.Namespace) -> int:
    data = _load_json(args.file)
    payload = extract_payload(data)
    if args.substitute_env:
        payload, unresolved = substitute_env_placeholders(payload, strict=args.strict)
        if args.strict and unresolved:
            print(
                f"ERROR: unresolved ENV: placeholders in {args.file}:\n" +
                "\n".join(f"  - {p}" for p in unresolved),
                file=sys.stderr,
            )
            return 2
    for arn in get_lambda_arns(payload):
        print(arn)
    return 0


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(description="IoT rule JSON helper")
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("rule-name", help="Print rule name if present")
    sp.add_argument("file")
    sp.set_defaults(func=cmd_rule_name)

    sp = sub.add_parser("payload", help="Print compact TopicRulePayload JSON")
    sp.add_argument("file")
    sp.add_argument("--substitute-env", action="store_true", help="Replace ENV:VARNAME placeholders")
    sp.add_argument("--strict", action="store_true", help="Fail if any ENV: placeholders remain")
    sp.set_defaults(func=cmd_payload)

    sp = sub.add_parser("validate", help="Validate that file contains a payload with sql/actions")
    sp.add_argument("file")
    sp.set_defaults(func=cmd_validate)

    sp = sub.add_parser("rule-disabled", help="Print true/false for ruleDisabled")
    sp.add_argument("file")
    sp.add_argument("--substitute-env", action="store_true")
    sp.add_argument("--strict", action="store_true")
    sp.set_defaults(func=cmd_rule_disabled)

    sp = sub.add_parser("lambda-arns", help="Print Lambda functionArns found in payload")
    sp.add_argument("file")
    sp.add_argument("--substitute-env", action="store_true")
    sp.add_argument("--strict", action="store_true")
    sp.set_defaults(func=cmd_lambda_arns)

    args = p.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))