\
# lambda_tools_stdlib_v6.py (stdlib only)
# - YAML subset parser (no pip deps) + ENV: substitution
# - Extracts function properties and SAM "Policies: - Statement: ..." into an IAM inline policy document

import json
import os
import re
import sys
from typing import Any, Dict, List, Union

ENV_TOKEN_RE = re.compile(r"ENV:([A-Z0-9_]+)")
BLOCK_TOKENS = {"|", "|-", ">", ">-"}
INLINE_MAP_RE = re.compile(r"^[^:]+:(\s|$)")

def _read_text(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()

def _find_placeholders_in_text(text: str) -> List[str]:
    return sorted(set(m.group(1) for m in ENV_TOKEN_RE.finditer(text)))

def _substitute_env_tokens_in_string(s: str) -> str:
    def repl(m: re.Match) -> str:
        key = m.group(1)
        val = os.environ.get(key)
        return val if val is not None else m.group(0)
    return ENV_TOKEN_RE.sub(repl, s)

def _walk_substitute(obj: Any) -> Any:
    if isinstance(obj, str):
        return _substitute_env_tokens_in_string(obj)
    if isinstance(obj, list):
        return [_walk_substitute(x) for x in obj]
    if isinstance(obj, dict):
        return {k: _walk_substitute(v) for k, v in obj.items()}
    return obj

def _strip_comment(line: str) -> str:
    out = []
    in_s = False
    in_d = False
    for ch in line:
        if ch == "'" and not in_d:
            in_s = not in_s
        elif ch == '"' and not in_s:
            in_d = not in_d
        if ch == "#" and not in_s and not in_d:
            break
        out.append(ch)
    return "".join(out).rstrip()

def _parse_scalar(token: str) -> Any:
    t = token.strip()
    if t.startswith("!"):
        parts = t.split(None, 1)
        t = parts[1] if len(parts) == 2 else ""
        t = t.strip()

    if (len(t) >= 2) and ((t[0] == t[-1] == "'") or (t[0] == t[-1] == '"')):
        return t[1:-1]

    low = t.lower()
    if low in ("true", "yes", "on"):
        return True
    if low in ("false", "no", "off"):
        return False
    if low in ("null", "~"):
        return None

    if re.fullmatch(r"-?\d+", t or ""):
        try:
            return int(t)
        except Exception:
            pass
    if re.fullmatch(r"-?\d+\.\d+", t or ""):
        try:
            return float(t)
        except Exception:
            pass

    if t == "{}":
        return {}
    if t == "[]":
        return []

    return t

class _Frame:
    def __init__(self, indent: int, container: Union[Dict[str, Any], List[Any]]):
        self.indent = indent
        self.container = container
        self.pending_key: Union[str, None] = None

def _read_block_scalar(lines: List[str], start_idx: int, parent_indent: int, token: str) -> (str, int):
    i = start_idx
    buf: List[str] = []
    block_indent: Union[int, None] = None

    while i < len(lines):
        raw0 = lines[i]
        if raw0.strip() == "":
            buf.append("")
            i += 1
            continue

        indent = len(raw0) - len(raw0.lstrip(" "))
        if indent <= parent_indent:
            break

        if block_indent is None:
            block_indent = indent

        if indent < block_indent:
            break

        buf.append(raw0[block_indent:])
        i += 1

    text = "\n".join(buf)
    if token.startswith(">"):
        parts = text.split("\n\n")
        folded = []
        for p in parts:
            folded.append(" ".join([ln for ln in p.split("\n") if ln != ""]))
        text = "\n\n".join(folded)

    return text, i

def parse_simple_yaml(text: str) -> Any:
    lines = text.splitlines()
    root: Union[Dict[str, Any], List[Any]] = {}
    stack: List[_Frame] = [_Frame(-1, root)]

    i = 0
    while i < len(lines):
        raw0 = lines[i]
        i += 1

        if not raw0.strip() or raw0.lstrip().startswith("#"):
            continue

        raw = _strip_comment(raw0)
        if not raw.strip():
            continue

        indent = len(raw) - len(raw.lstrip(" "))
        content = raw.lstrip(" ")

        while stack and indent < stack[-1].indent:
            stack.pop()
        if not stack:
            raise SystemExit("ERROR: YAML indentation error")

        while True:
            if (
                len(stack) >= 2
                and indent == stack[-1].indent
                and content.startswith("-")
                and isinstance(stack[-1].container, dict)
                and isinstance(stack[-2].container, list)
                and stack[-2].indent == indent
            ):
                stack.pop()
                continue
            break

        while True:
            top = stack[-1]
            if isinstance(top.container, dict) and top.pending_key is not None and indent > top.indent:
                next_is_list = content.startswith("-")
                key = top.pending_key
                child: Union[Dict[str, Any], List[Any]] = [] if next_is_list else {}
                top.container[key] = child
                top.pending_key = None
                stack.append(_Frame(indent, child))
                continue
            break

        top = stack[-1]

        if content.startswith("-"):
            if not isinstance(top.container, list):
                raise SystemExit("ERROR: YAML structure error: list item under non-list container")

            rest = content[1:].lstrip(" ")

            if rest == "":
                item: Dict[str, Any] = {}
                top.container.append(item)
                stack.append(_Frame(indent, item))
                continue

            if rest in BLOCK_TOKENS:
                block_text, new_i = _read_block_scalar(lines, i, indent, rest)
                top.container.append(block_text)
                i = new_i
                continue

            if INLINE_MAP_RE.match(rest):
                k, v = rest.split(":", 1)
                k = k.strip()
                v = v.strip()

                item: Dict[str, Any] = {}
                top.container.append(item)
                item_frame = _Frame(indent, item)
                stack.append(item_frame)

                if v == "":
                    item_frame.pending_key = k
                    continue

                if v in BLOCK_TOKENS:
                    block_text, new_i = _read_block_scalar(lines, i, indent, v)
                    item[k] = block_text
                    i = new_i
                    continue

                item[k] = _parse_scalar(v)
                continue

            top.container.append(_parse_scalar(rest))
            continue

        if isinstance(top.container, dict):
            if ":" not in content:
                raise SystemExit(f"ERROR: YAML line missing ':' => {content}")
            k, v = content.split(":", 1)
            k = k.strip()
            v = v.strip()

            if v == "":
                top.pending_key = k
                continue

            if v in BLOCK_TOKENS:
                block_text, new_i = _read_block_scalar(lines, i, indent, v)
                top.container[k] = block_text
                i = new_i
                continue

            top.container[k] = _parse_scalar(v)
            continue

        raise SystemExit("ERROR: YAML structure error: dict entry under list without '-'")

    return root

def _load_template(path: str) -> Dict[str, Any]:
    strict = os.environ.get("STRICT_PLACEHOLDERS", "true").lower() == "true"
    raw = _read_text(path)
    raw = _substitute_env_tokens_in_string(raw)
    doc = parse_simple_yaml(raw)

    if not isinstance(doc, dict):
        raise SystemExit(f"ERROR: {path} must parse to a YAML mapping/object at root")

    doc = _walk_substitute(doc)

    if strict:
        as_text = json.dumps(doc, ensure_ascii=False)
        missing = _find_placeholders_in_text(as_text)
        if missing:
            raise SystemExit(f"ERROR: Unresolved ENV placeholders: {missing}")

    return doc

def cmd_placeholders(template_path: str) -> int:
    text = _read_text(template_path)
    for name in _find_placeholders_in_text(text):
        print(name)
    return 0

def _get_resources(doc: Dict[str, Any]) -> Dict[str, Any]:
    res = doc.get("Resources")
    return res if isinstance(res, dict) else {}

def _extract_env(props: Dict[str, Any]) -> Dict[str, str]:
    env = props.get("Environment")
    if isinstance(env, dict):
        vars_ = env.get("Variables")
        if isinstance(vars_, dict):
            out: Dict[str, str] = {}
            for k, v in vars_.items():
                if isinstance(k, str):
                    out[k] = "" if v is None else str(v)
            return out
    return {}

def _extract_function_name(logical_id: str, props: Dict[str, Any]) -> str:
    fn = props.get("FunctionName")
    if isinstance(fn, str) and fn.strip():
        return fn.strip()
    return logical_id

def _extract_inline_policy_from_sam_policies(policies: Any) -> Union[Dict[str, Any], None]:
    # SAM "Policies" can be: string, list, dict. We only turn "Statement" blocks into an IAM policy document.
    stmts: List[Dict[str, Any]] = []

    if isinstance(policies, dict):
        # sometimes { Statement: [...] }
        st = policies.get("Statement")
        if isinstance(st, list):
            for s in st:
                if isinstance(s, dict):
                    stmts.append(s)

    if isinstance(policies, list):
        for item in policies:
            if isinstance(item, dict):
                st = item.get("Statement")
                if isinstance(st, list):
                    for s in st:
                        if isinstance(s, dict):
                            stmts.append(s)

    if not stmts:
        return None

    # Normalize keys slightly: ensure Effect/Action/Resource exist as expected (leave as-is if already valid)
    return {"Version": "2012-10-17", "Statement": stmts}

def _extract_common(props: Dict[str, Any]) -> Dict[str, Any]:
    out: Dict[str, Any] = {}

    runtime = props.get("Runtime")
    handler = props.get("Handler")
    role = props.get("Role")

    if isinstance(runtime, str):
        out["runtime"] = runtime
    if isinstance(handler, str):
        out["handler"] = handler
    if isinstance(role, str) and role.strip():
        out["role"] = role.strip()

    timeout = props.get("Timeout")
    if isinstance(timeout, int):
        out["timeout"] = timeout
    elif isinstance(timeout, str) and timeout.isdigit():
        out["timeout"] = int(timeout)

    mem = props.get("MemorySize")
    if isinstance(mem, int):
        out["memorySize"] = mem
    elif isinstance(mem, str) and mem.isdigit():
        out["memorySize"] = int(mem)

    desc = props.get("Description")
    if isinstance(desc, str):
        out["description"] = desc

    env_vars = _extract_env(props)
    if env_vars:
        out["environment"] = env_vars

    # If no explicit Role, try to derive inline policy from SAM Policies
    if "role" not in out:
        inline = _extract_inline_policy_from_sam_policies(props.get("Policies"))
        if inline:
            out["inlinePolicy"] = inline

    # RoleName is not a SAM Function property; we provide a recommended name for the bash script.
    # (Bash can override via LAMBDA_ROLE_PREFIX/SUFFIX)
    return out

def cmd_functions(template_path: str) -> int:
    doc = _load_template(template_path)
    resources = _get_resources(doc)

    for logical_id, r in resources.items():
        if not isinstance(r, dict):
            continue
        t = r.get("Type")
        props = r.get("Properties") if isinstance(r.get("Properties"), dict) else {}

        if t in ("AWS::Serverless::Function", "AWS::Lambda::Function"):
            fn: Dict[str, Any] = {"logicalId": logical_id}
            fn_name = _extract_function_name(logical_id, props)
            fn["functionName"] = fn_name
            fn.update(_extract_common(props))
            # if no role, suggest roleName
            if "role" not in fn:
                fn["roleName"] = f"{fn_name}-exec"
            print(json.dumps(fn, separators=(",", ":"), ensure_ascii=False))

    return 0

def main(argv: List[str]) -> int:
    if len(argv) < 3:
        print("Usage: python3 -m lambda_tools_stdlib_v6 <placeholders|functions> <template.yml>", file=sys.stderr)
        return 2
    cmd = argv[1]
    template = argv[2]
    if cmd == "placeholders":
        return cmd_placeholders(template)
    if cmd == "functions":
        return cmd_functions(template)
    print(f"Unknown command: {cmd}", file=sys.stderr)
    return 2

if __name__ == "__main__":
    raise SystemExit(main(sys.argv))