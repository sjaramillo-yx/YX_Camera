#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}" )" && pwd)"
PROFILE="${BOOTSTRAP_PROFILE:-default}"
REGION="$(aws configure get region --profile "$PROFILE" 2>/dev/null || true)"
REGION="${REGION:-sa-east-1}"

# --- Inputs ---
# DynamoDB table spec (same format you already use)
TABLE_SPEC="${TABLE_SPEC:-${SCRIPT_DIR}/yx-cam-data.json}"
# Optional override (otherwise we use TableName from the JSON)
DYNAMO_TABLE_NAME="${DYNAMO_TABLE_NAME:-}"

# IoT Rule -> DynamoDB action role (created/updated by this script)
ROLE_NAME="${ROLE_NAME:-iot-rule-dynamo-${DYNAMO_TABLE_NAME:-writer}}"  # updated below once table name is known
ROLE_TRUST_POLICY="${ROLE_TRUST_POLICY:-${SCRIPT_DIR}/dynamoRole.json}"
ROLE_INLINE_POLICY_NAME="${ROLE_INLINE_POLICY_NAME:-DynamoWrite}"

require_file() {
  local f="$1"
  [[ -f "$f" ]] || { echo "ERROR: missing file: $f"; exit 1; }
}

require_file "$TABLE_SPEC"
require_file "$ROLE_TRUST_POLICY"

echo "Using profile=$PROFILE region=$REGION"
aws --profile "$PROFILE" sts get-caller-identity >/dev/null

# Read TableName from spec
SPEC_DYNAMO_TABLE_NAME="$(python3 - <<'PY' "$TABLE_SPEC"
import json, sys
path = sys.argv[1]
with open(path, 'r', encoding='utf-8') as f:
    spec = json.load(f)
name = spec.get('TableName')
if not isinstance(name, str) or not name:
    raise SystemExit('ERROR: TABLE_SPEC JSON must include a non-empty string TableName')
print(name)
PY
)"

EFFECTIVE_DYNAMO_TABLE_NAME="${DYNAMO_TABLE_NAME:-$SPEC_DYNAMO_TABLE_NAME}"

# Now that we know the table name, default the role name if not explicitly set
if [[ -z "${ROLE_NAME:-}" || "${ROLE_NAME}" == "iot-rule-dynamo-${DYNAMO_TABLE_NAME:-writer}" ]]; then
  ROLE_NAME="iot-rule-dynamo-${EFFECTIVE_DYNAMO_TABLE_NAME}"
fi

echo "Table spec: $TABLE_SPEC"
echo "Effective table: $EFFECTIVE_DYNAMO_TABLE_NAME"
echo "Role: $ROLE_NAME"

# --- Create / ensure table ---
if aws --profile "$PROFILE" --region "$REGION" dynamodb describe-table \
  --table-name "$EFFECTIVE_DYNAMO_TABLE_NAME" >/dev/null 2>&1; then
  echo "Table exists: $EFFECTIVE_DYNAMO_TABLE_NAME (skipping create)"
else
  tmp_json="$(mktemp)"
  trap 'rm -f "$tmp_json"' EXIT

  # Write effective spec to temp file (override TableName if needed)
  python3 - <<'PY' "$TABLE_SPEC" "$EFFECTIVE_DYNAMO_TABLE_NAME" "$tmp_json"
import json, sys
src, DYNAMO_TABLE_NAME, dst = sys.argv[1], sys.argv[2], sys.argv[3]
with open(src, 'r', encoding='utf-8') as f:
    spec = json.load(f)
spec['TableName'] = DYNAMO_TABLE_NAME
with open(dst, 'w', encoding='utf-8') as f:
    json.dump(spec, f, indent=2)
    f.write('\n')
PY

  echo "Creating DynamoDB table: $EFFECTIVE_DYNAMO_TABLE_NAME"
  aws --profile "$PROFILE" --region "$REGION" dynamodb create-table \
    --cli-input-json "file://$tmp_json" >/dev/null

  echo "Waiting for table to become ACTIVE..."
  while true; do
    status="$(aws --profile "$PROFILE" --region "$REGION" dynamodb describe-table \
      --table-name "$EFFECTIVE_DYNAMO_TABLE_NAME" \
      --query 'Table.TableStatus' --output text 2>/dev/null || true)"
    [[ "$status" == "ACTIVE" ]] && break
    sleep 2
  done

  echo "Table ACTIVE: $EFFECTIVE_DYNAMO_TABLE_NAME"
fi

# Fetch table ARN (used to scope the role policy)
TABLE_ARN="$(aws --profile "$PROFILE" --region "$REGION" dynamodb describe-table \
  --table-name "$EFFECTIVE_DYNAMO_TABLE_NAME" \
  --query 'Table.TableArn' --output text)"

# --- Create / ensure role for IoT Rule DynamoDB action ---
# 1) Create role if missing
if aws --profile "$PROFILE" iam get-role --role-name "$ROLE_NAME" >/dev/null 2>&1; then
  echo "Role exists: $ROLE_NAME"
  echo "Ensuring trust policy is up-to-date..."
  aws --profile "$PROFILE" iam update-assume-role-policy \
    --role-name "$ROLE_NAME" \
    --policy-document "file://$ROLE_TRUST_POLICY" >/dev/null
else
  echo "Creating role: $ROLE_NAME"
  aws --profile "$PROFILE" iam create-role \
    --role-name "$ROLE_NAME" \
    --assume-role-policy-document "file://$ROLE_TRUST_POLICY" \
    --description "IoT Rule action role to write to DynamoDB table ${EFFECTIVE_DYNAMO_TABLE_NAME}" \
    >/dev/null
fi

# 2) Put/update an inline policy scoped to this table (and its GSIs)
POLICY_DOC="${SCRIPT_DIR}/dynamoPolicy.json"

echo "Attaching inline policy '$ROLE_INLINE_POLICY_NAME' to role '$ROLE_NAME' (scoped to table ARN)"
aws --profile "$PROFILE" iam put-role-policy \
  --role-name "$ROLE_NAME" \
  --policy-name "$ROLE_INLINE_POLICY_NAME" \
  --policy-document "file://$POLICY_DOC" >/dev/null

echo "Done."
echo "Table: $EFFECTIVE_DYNAMO_TABLE_NAME"
echo "Table ARN: $TABLE_ARN"
echo "Role: $ROLE_NAME"

# Helpful output for wiring IoT Rules
ROLE_ARN="$(aws --profile "$PROFILE" iam get-role --role-name "$ROLE_NAME" --query 'Role.Arn' --output text)"
echo "Role ARN: $ROLE_ARN"

export DYNAMO_TABLE_NAME