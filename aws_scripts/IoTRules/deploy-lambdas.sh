#!/usr/bin/env bash
set -euo pipefail

# Stdlib-only Lambda deployer (no pip deps).
#
# Directory structure:
#   lambdas/<lambdaName>/template.yml
#   lambdas/<lambdaName>/src/...
#
# Placeholders:
#   Use ENV:VARNAME anywhere in template.yml; values come from:
#   - exported environment variables, OR
#   - bash variables defined in this script (even if not exported)
#
# Roles:
#   If a function does NOT specify Properties.Role (common for SAM templates that use Properties.Policies),
#   this script will create/ensure an execution role and pass it to the Lambda Create/Update call.
#   Role naming:
#     - default: "<FunctionName>-exec"
#     - override prefix/suffix via LAMBDA_ROLE_PREFIX / LAMBDA_ROLE_SUFFIX
#   It also attaches AWS managed policy AWSLambdaBasicExecutionRole and puts an inline policy built
#   from any "Policies: - Statement: ..." blocks in the SAM template.

PROFILE="${BOOTSTRAP_PROFILE:-${PROFILE:-default}}"
REGION="${REGION:-$(aws configure get region --profile "$PROFILE" 2>/dev/null || echo "us-east-1")}"
STRICT_PLACEHOLDERS="${STRICT_PLACEHOLDERS:-true}"

LAMBDA_ROLE_PREFIX="${LAMBDA_ROLE_PREFIX:-}"
LAMBDA_ROLE_SUFFIX="${LAMBDA_ROLE_SUFFIX:--exec}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAMBDAS_DIR="${LAMBDAS_DIR:-$SCRIPT_DIR/lambdas}"
PYTHONPATH="${SCRIPT_DIR}:${PYTHONPATH:-}"

require() { command -v "$1" >/dev/null 2>&1 || { echo "Missing dependency: $1"; exit 1; }; }
require aws
require python3

[[ -d "$LAMBDAS_DIR" ]] || { echo "ERROR: Lambdas directory not found: $LAMBDAS_DIR"; exit 1; }

ACCOUNT_ID="$(aws --profile "$PROFILE" sts get-caller-identity --query Account --output text)"
echo "Deploying Lambdas from: $LAMBDAS_DIR"
echo "Profile=$PROFILE Region=$REGION Account=$ACCOUNT_ID"
echo

py_for_template() {
  local template="$1"; shift
  local -a cmd=(python3 -m lambda_tools)

  local vars
  vars="$(PYTHONPATH="$PYTHONPATH" "${cmd[@]}" placeholders "$template")"

  local -a env_pairs=()
  local v
  while IFS= read -r v; do
    [[ -z "$v" ]] && continue
    if [[ "${!v+x}" == "x" ]]; then
      env_pairs+=("$v=${!v}")
    fi
  done <<<"$vars"

  env_pairs+=("STRICT_PLACEHOLDERS=$STRICT_PLACEHOLDERS")

  PYTHONPATH="$PYTHONPATH" env "${env_pairs[@]}" "${cmd[@]}" "$@" "$template"
}

zip_src() {
  local src_dir="$1"
  local out_zip="$2"

  rm -f "$out_zip"
  (cd "$src_dir" && python3 - <<'PY' "$out_zip"
import os, sys, zipfile

out_zip = sys.argv[1]
with zipfile.ZipFile(out_zip, "w", compression=zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk("."):
        dirs[:] = [d for d in dirs if d not in ("__pycache__", ".pytest_cache", ".mypy_cache", ".venv", "node_modules")]
        for f in files:
            if f in (".DS_Store",):
                continue
            if f.endswith((".pyc", ".pyo")):
                continue
            p = os.path.join(root, f)
            arc = p[2:] if p.startswith("./") else p
            z.write(p, arcname=arc)
print(out_zip)
PY
  )
}

lambda_exists() {
  local fn="$1"
  aws --profile "$PROFILE" --region "$REGION" lambda get-function --function-name "$fn" >/dev/null 2>&1
}

iam_role_exists() {
  local role_name="$1"
  aws --profile "$PROFILE" iam get-role --role-name "$role_name" >/dev/null 2>&1
}

ensure_lambda_role() {
  # Args:
  # 1 role_name
  # 2 inline_policy_json (or empty)
  local role_name="$1"
  local inline_policy_json="${2:-}"

  # Trust policy
  local trust_json
  trust_json="$(python3 - <<'PY'
import json
print(json.dumps({
  "Version":"2012-10-17",
  "Statement":[{
    "Effect":"Allow",
    "Principal":{"Service":"lambda.amazonaws.com"},
    "Action":"sts:AssumeRole"
  }]
}, separators=(",",":")))
PY
)"

  if ! iam_role_exists "$role_name"; then
    echo "Creating execution role: $role_name"
    aws --profile "$PROFILE" iam create-role \
      --role-name "$role_name" \
      --assume-role-policy-document "$trust_json" \
      >/dev/null
  fi

  # Ensure trust policy (idempotent)
  aws --profile "$PROFILE" iam update-assume-role-policy \
    --role-name "$role_name" \
    --policy-document "$trust_json" \
    >/dev/null

  # Attach AWS managed basic execution policy (logs)
  local basic_arn="arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole"
  aws --profile "$PROFILE" iam attach-role-policy \
    --role-name "$role_name" \
    --policy-arn "$basic_arn" \
    >/dev/null || true

  # Put inline policy if provided
  if [[ -n "$inline_policy_json" && "$inline_policy_json" != "null" ]]; then
    local policy_name="${role_name}-inline"
    aws --profile "$PROFILE" iam put-role-policy \
      --role-name "$role_name" \
      --policy-name "$policy_name" \
      --policy-document "$inline_policy_json" \
      >/dev/null
  fi

  aws --profile "$PROFILE" iam get-role --role-name "$role_name" --query 'Role.Arn' --output text
}

deploy_one_function() {
  local fn_name="$1"
  local runtime="$2"
  local handler="$3"
  local role_arn="$4"
  local timeout="$5"
  local memory="$6"
  local env_json="$7"
  local zip_path="$8"
  local desc="$9"

  if lambda_exists "$fn_name"; then
    echo "Updating Lambda: $fn_name"
    aws --profile "$PROFILE" --region "$REGION" lambda update-function-code \
      --function-name "$fn_name" \
      --zip-file "fileb://${zip_path}" \
      >/dev/null

    local -a args=(--function-name "$fn_name")
    [[ -n "$runtime" && "$runtime" != "null" ]] && args+=(--runtime "$runtime")
    [[ -n "$handler" && "$handler" != "null" ]] && args+=(--handler "$handler")
    [[ -n "$role_arn" && "$role_arn" != "null" ]] && args+=(--role "$role_arn")
    [[ -n "$timeout" && "$timeout" != "null" ]] && args+=(--timeout "$timeout")
    [[ -n "$memory" && "$memory" != "null" ]] && args+=(--memory-size "$memory")
    [[ -n "$desc" && "$desc" != "null" ]] && args+=(--description "$desc")
    [[ -n "$env_json" && "$env_json" != "null" ]] && args+=(--environment "$env_json")

    aws --profile "$PROFILE" --region "$REGION" lambda update-function-configuration \
      "${args[@]}" >/dev/null
  else
    echo "Creating Lambda: $fn_name"
    local -a args=(
      --function-name "$fn_name"
      --runtime "$runtime"
      --handler "$handler"
      --role "$role_arn"
      --zip-file "fileb://${zip_path}"
      --publish
    )
    [[ -n "$timeout" && "$timeout" != "null" ]] && args+=(--timeout "$timeout")
    [[ -n "$memory" && "$memory" != "null" ]] && args+=(--memory-size "$memory")
    [[ -n "$desc" && "$desc" != "null" ]] && args+=(--description "$desc")
    [[ -n "$env_json" && "$env_json" != "null" ]] && args+=(--environment "$env_json")

    aws --profile "$PROFILE" --region "$REGION" lambda create-function "${args[@]}" >/dev/null
  fi

  echo "Deployed: $fn_name"
}

process_lambda_dir() {
  local dir="$1"
  local template="${dir}/template.yml"
  local src="${dir}/src"

  [[ -f "$template" ]] || { echo "Skipping (no template.yml): $dir"; return 0; }
  [[ -d "$src" ]] || { echo "ERROR: Missing src/ directory: $dir"; exit 1; }

  # One JSON object per line describing each function (with extracted policy statements if present)
  local funcs
  funcs="$(py_for_template "$template" functions)"

  if [[ -z "$funcs" ]]; then
    echo "ERROR: No functions found in $template"
    exit 1
  fi

  local zip_path
  zip_path="$(mktemp -t "lambda-src-XXXXXX.zip")"
  zip_src "$src" "$zip_path" >/dev/null

  while IFS= read -r line; do
    [[ -z "$line" ]] && continue

    # Parse fields from the json line
    local fn_name runtime handler role_arn timeout memory env_json desc role_name inline_policy
    fn_name="$(python3 - <<'PY' "$line"
import json, sys
o=json.loads(sys.argv[1]); print(o.get("functionName",""))
PY
)"
    runtime="$(python3 - <<'PY' "$line"
import json, sys
o=json.loads(sys.argv[1]); print(o.get("runtime",""))
PY
)"
    handler="$(python3 - <<'PY' "$line"
import json, sys
o=json.loads(sys.argv[1]); print(o.get("handler",""))
PY
)"
    role_arn="$(python3 - <<'PY' "$line"
import json, sys
o=json.loads(sys.argv[1]); print(o.get("role",""))
PY
)"
    timeout="$(python3 - <<'PY' "$line"
import json, sys
o=json.loads(sys.argv[1]); print(o.get("timeout",""))
PY
)"
    memory="$(python3 - <<'PY' "$line"
import json, sys
o=json.loads(sys.argv[1]); print(o.get("memorySize",""))
PY
)"
    env_json="$(python3 - <<'PY' "$line"
import json, sys
o=json.loads(sys.argv[1]); env=o.get("environment")
print(json.dumps({"Variables": env}, separators=(",",":")) if isinstance(env, dict) and env else "")
PY
)"
    desc="$(python3 - <<'PY' "$line"
import json, sys
o=json.loads(sys.argv[1]); print(o.get("description",""))
PY
)"
    role_name="$(python3 - <<'PY' "$line"
import json, sys
o=json.loads(sys.argv[1]); print(o.get("roleName",""))
PY
)"
    inline_policy="$(python3 - <<'PY' "$line"
import json, sys
o=json.loads(sys.argv[1]); p=o.get("inlinePolicy")
print(json.dumps(p, separators=(",",":")) if isinstance(p, dict) and p else "")
PY
)"

    if [[ -z "$fn_name" ]]; then
      echo "ERROR: functionName missing for a function in $template"
      exit 1
    fi

    # If role missing, create/ensure it using extracted policy statements (SAM Policies) if available
    if [[ -z "$role_arn" || "$role_arn" == "null" ]]; then
      if [[ -z "$role_name" ]]; then
        role_name="${LAMBDA_ROLE_PREFIX}${fn_name}${LAMBDA_ROLE_SUFFIX}"
      fi
      echo "Role not specified in template; ensuring role '$role_name' for $fn_name"
      role_arn="$(ensure_lambda_role "$role_name" "$inline_policy")"
    fi

    if [[ -z "$runtime" || -z "$handler" || -z "$role_arn" ]]; then
      echo "ERROR: runtime/handler/role missing for $fn_name in $template"
      echo "  runtime='${runtime}' handler='${handler}' role='${role_arn}'"
      exit 1
    fi

    deploy_one_function "$fn_name" "$runtime" "$handler" "$role_arn" "$timeout" "$memory" "$env_json" "$zip_path" "$desc"
  done <<<"$funcs"

  rm -f "$zip_path"
  echo "Done directory: $dir"
  echo "----"
}

while IFS= read -r -d '' d; do
  process_lambda_dir "$d"
done < <(find "$LAMBDAS_DIR" -mindepth 1 -maxdepth 1 -type d -print0 | sort -z)

echo "All lambdas deployed."