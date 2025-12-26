#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}" )" && pwd)"
PROFILE="${BOOTSTRAP_PROFILE:-default}"
REGION="$(aws configure get region --profile "$PROFILE" 2>/dev/null || true)"
REGION="${REGION:-sa-east-1}"

RULES_DIR="${RULES_DIR:-$SCRIPT_DIR/iot-rules}"

# Optional: first argument can be the Lambda function name/alias/ARN used for
# resolving ENV:ONGOING_REC_LAMBDA_ARN in rule JSON files.
ONGOING_REC_LAMBDA_NAME_ARG="${1:-}"

# --- Role name -> role ARN (used for placeholder substitution) ---
ROLE_NAME="${ROLE_NAME:-iot-rule-dynamo-yx-cam-data}"
ROLE_ARN="$(aws --profile "$PROFILE" iam get-role \
  --role-name "$ROLE_NAME" \
  --query 'Role.Arn' --output text)"

# Export some commonly-used env vars for placeholder substitution.
# In your rule JSON, you can write:  "roleArn": "ENV:DYNAMO_ROLE_ARN"  (or ENV:IOT_RULE_ROLE_ARN)
export DYNAMO_ROLE_ARN="${DYNAMO_ROLE_ARN:-$ROLE_ARN}"
export IOT_RULE_ROLE_ARN="${IOT_RULE_ROLE_ARN:-$ROLE_ARN}"


# --- Lambda name -> function ARN (used for placeholder substitution) ---
# If your rule JSON contains: "functionArn": "ENV:ONGOING_REC_LAMBDA_ARN"
# set ONGOING_REC_LAMBDA_NAME to a Lambda function name/alias (or set ONGOING_REC_LAMBDA_ARN
# directly), and this script will export ONGOING_REC_LAMBDA_ARN for iot_rules_tools.py to substitute.
ONGOING_REC_LAMBDA_NAME="${ONGOING_REC_LAMBDA_NAME:-${ONGOING_REC_LAMBDA_NAME_ARG:-${LAMBDA_NAME:-UpdateOngoingRecording}}}"

if [[ -n "${ONGOING_REC_LAMBDA_ARN:-}" ]]; then
  export ONGOING_REC_LAMBDA_ARN
elif [[ -n "$ONGOING_REC_LAMBDA_NAME" ]]; then
  if [[ "$ONGOING_REC_LAMBDA_NAME" == arn:aws:lambda:* ]]; then
    export ONGOING_REC_LAMBDA_ARN="$ONGOING_REC_LAMBDA_NAME"
  else
    ONGOING_REC_LAMBDA_ARN="$(aws --profile "$PROFILE" --region "$REGION" lambda get-function-configuration \
      --function-name "$ONGOING_REC_LAMBDA_NAME" \
      --query 'FunctionArn' --output text 2>/dev/null || true)"
    if [[ -z "$ONGOING_REC_LAMBDA_ARN" || "$ONGOING_REC_LAMBDA_ARN" == "None" ]]; then
      echo "ERROR: Could not resolve Lambda ARN for ONGOING_REC_LAMBDA_NAME=$ONGOING_REC_LAMBDA_NAME" >&2
      exit 1
    fi
    export ONGOING_REC_LAMBDA_ARN
  fi
fi

# If true, fail a rule deployment if any ENV:PLACEHOLDER remains unresolved.
STRICT_PLACEHOLDERS="${STRICT_PLACEHOLDERS:-true}"

require() { command -v "$1" >/dev/null 2>&1 || { echo "Missing dependency: $1"; exit 1; }; }
require aws
require python3

[[ -d "$RULES_DIR" ]] || { echo "ERROR: Rules directory not found: $RULES_DIR"; exit 1; }

# Make the helper module importable
export PYTHONPATH="$SCRIPT_DIR${PYTHONPATH:+:$PYTHONPATH}"

# Helper module must live next to this script
[[ -f "$SCRIPT_DIR/iot_rules_tools.py" ]] || {
  echo "ERROR: Missing helper: $SCRIPT_DIR/iot_rules_tools.py";
  echo "Make sure createRules.sh and iot_rules_tools.py are in the same directory.";
  exit 1;
}

ACCOUNT_ID="$(aws --profile "$PROFILE" sts get-caller-identity --query Account --output text)"
IOT_RULE_ARN_PREFIX="arn:aws:iot:${REGION}:${ACCOUNT_ID}:rule"

sanitize_sid() {
  local s="$1"
  s="$(echo "$s" | tr -cd 'A-Za-z0-9_-')"
  echo "${s:0:100}"
}

rule_exists() {
  local rule_name="$1"
  aws --profile "$PROFILE" --region "$REGION" iot get-topic-rule --rule-name "$rule_name" >/dev/null 2>&1
}

upsert_rule() {
  local rule_name="$1"
  local payload="$2"

  if rule_exists "$rule_name"; then
    echo "Updating rule: $rule_name"
    aws --profile "$PROFILE" --region "$REGION" iot replace-topic-rule \
      --rule-name "$rule_name" \
      --topic-rule-payload "$payload" \
      >/dev/null
  else
    echo "Creating rule: $rule_name"
    aws --profile "$PROFILE" --region "$REGION" iot create-topic-rule \
      --rule-name "$rule_name" \
      --topic-rule-payload "$payload" \
      >/dev/null
  fi
}

set_rule_state() {
  local rule_name="$1"
  local disabled="$2"

  if [[ "$disabled" == "true" ]]; then
    aws --profile "$PROFILE" --region "$REGION" iot disable-topic-rule --rule-name "$rule_name" >/dev/null || true
    echo "Disabled: $rule_name"
  else
    aws --profile "$PROFILE" --region "$REGION" iot enable-topic-rule --rule-name "$rule_name" >/dev/null || true
    echo "Enabled: $rule_name"
  fi
}

# Read Lambda policy JSON from stdin; return success if Sid exists
lambda_policy_has_sid() {
  local sid="$1"
  python3 - "$sid" <<'PY'
import json, sys
sid = sys.argv[1]
text = sys.stdin.read().strip()
if not text:
    sys.exit(1)
try:
    doc = json.loads(text)
except Exception:
    sys.exit(1)
stmts = doc.get("Statement", [])
if not isinstance(stmts, list):
    sys.exit(1)
for s in stmts:
    if isinstance(s, dict) and s.get("Sid") == sid:
        sys.exit(0)
sys.exit(1)
PY
}

ensure_lambda_permission_for_rule() {
  local function_arn="$1"
  local rule_name="$2"
  local rule_arn="${IOT_RULE_ARN_PREFIX}/${rule_name}"
  local sid
  sid="$(sanitize_sid "iot-rule-${rule_name}")"

  local policy
  policy="$(aws --profile "$PROFILE" lambda get-policy --function-name "$function_arn" --query Policy --output text 2>/dev/null || true)"

  if echo "$policy" | lambda_policy_has_sid "$sid"; then
    echo "Lambda permission already present: $function_arn (Sid=$sid)"
    return 0
  fi

  echo "Adding Lambda permission: $function_arn (Sid=$sid, SourceArn=$rule_arn)"
  set +e
  local out
  out="$(aws --profile "$PROFILE" lambda add-permission \
    --function-name "$function_arn" \
    --statement-id "$sid" \
    --action "lambda:InvokeFunction" \
    --principal "iot.amazonaws.com" \
    --source-arn "$rule_arn" 2>&1)"
  local rc=$?
  set -e

  if [[ $rc -ne 0 ]]; then
    if echo "$out" | grep -q "ResourceConflictException"; then
      echo "Lambda permission already existed (conflict ignored): $function_arn (Sid=$sid)"
      return 0
    fi
    echo "ERROR adding lambda permission for $function_arn:"
    echo "$out"
    return $rc
  fi
}

py_flags=()
if [[ "$STRICT_PLACEHOLDERS" == "true" ]]; then
  py_flags=(--substitute-env --strict)
else
  py_flags=(--substitute-env)
fi

process_rule_file() {
  local file="$1"

  local rule_name payload disabled
  rule_name="$(python3 -m iot_rules_tools rule-name "$file")"
  if [[ -z "$rule_name" || "$rule_name" == "null" ]]; then
    rule_name="$(basename "$file" .json)"
  fi

  payload="$(python3 -m iot_rules_tools payload "$file" "${py_flags[@]}")"
  disabled="$(python3 -m iot_rules_tools rule-disabled "$file" "${py_flags[@]}")"

  upsert_rule "$rule_name" "$payload"
  set_rule_state "$rule_name" "$disabled"

  while IFS= read -r fn; do
    [[ -n "$fn" ]] || continue
    ensure_lambda_permission_for_rule "$fn" "$rule_name"
  done < <(python3 -m iot_rules_tools lambda-arns "$file" "${py_flags[@]}")

  echo "Done: $rule_name ($file)"
  echo "----"
}

echo "Deploying IoT rules from: $RULES_DIR"
echo "Profile=$PROFILE Region=$REGION Account=$ACCOUNT_ID"
echo "ROLE_NAME=$ROLE_NAME"
echo "Resolved role ARN: $ROLE_ARN"
echo "Placeholder envs: DYNAMO_ROLE_ARN=$DYNAMO_ROLE_ARN IOT_RULE_ROLE_ARN=$IOT_RULE_ROLE_ARN ONGOING_REC_LAMBDA_ARN=${ONGOING_REC_LAMBDA_ARN:-} DYNAMO_TABLE_NAME=${DYNAMO_TABLE_NAME}"

echo

mapfile -d '' files < <(find "$RULES_DIR" -maxdepth 1 -type f -name '*.json' -print0 | sort -z)
if [[ ${#files[@]} -eq 0 ]]; then
  echo "No *.json files found in $RULES_DIR"
  exit 0
fi

for f in "${files[@]}"; do
  process_rule_file "$f"
done

echo "All rules deployed."