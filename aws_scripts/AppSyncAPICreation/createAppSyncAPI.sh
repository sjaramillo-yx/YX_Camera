#!/usr/bin/env bash
set -euo pipefail

# ------------------------------------------------------------------------------
# Deploy an AppSync API (schema + DynamoDB datasource + JS resolvers/functions)
# No external dependencies (no jq, no pip).
#
# Expected layout (default SCRIPT_DIR=./appsync):
#   appsync/
#     schema.graphql
#     resolvers/
#       getCamera.js
#       listCameraStatuses.js
#       listRecordings.js
#     functions/
#       getCameraInfo.js
#       getCameraStatus.js
#
# You can also place the .js files directly under SCRIPT_DIR; the script will find them.
# ------------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}" )" && pwd)"

PROFILE="${BOOTSTRAP_PROFILE:-${PROFILE:-default}}"
REGION="$(aws configure get region --profile "$PROFILE" 2>/dev/null || true)"

API_NAME="${APPSYNC_API_NAME:-${API_NAME:-yx-cam-api}}"
AUTH_TYPE="${APPSYNC_AUTH_TYPE:-${AUTH_TYPE:-AWS_IAM}}"   # API_KEY | AWS_IAM | AMAZON_COGNITO_USER_POOLS | OPENID_CONNECT | AWS_LAMBDA
API_KEY_EXPIRES_DAYS="${APPSYNC_API_KEY_EXPIRES_DAYS:-${API_KEY_EXPIRES_DAYS:-30}}"

TABLE_NAME="${DYNAMO_TABLE_NAME:-${TABLE_NAME:-yx-cam-data}}"
DATASOURCE_NAME="${APPSYNC_DATASOURCE_NAME:-${DATASOURCE_NAME:-Dynamo}}"
ROLE_NAME="${APPSYNC_ROLE_NAME:-${ROLE_NAME:-appsync-${API_NAME:-api}-dynamo}}"
ROLE_POLICY_NAME="${APPSYNC_ROLE_POLICY_NAME:-${ROLE_POLICY_NAME:-appsync-dynamo-access}}"

PYTOOLS="${PYTOOLS:-${SCRIPT_DIR}/appsync_tools.py}"

ACCOUNT_ID="$(aws --profile "$PROFILE" sts get-caller-identity --query Account --output text)"
echo "Profile=$PROFILE Region=$REGION Account=$ACCOUNT_ID"

die() { echo "ERROR: $*" >&2; exit 1; }

need() {
  local v="$1"
  [[ -n "${!v:-}" ]] || die "$v is required"
}

aws_cli() {
  aws --profile "${PROFILE}" --region "${REGION}" --no-cli-pager "$@"
}

find_file() {
  local rel="$1"
  local p1="${SCRIPT_DIR}/${rel}"
  local p2="${SCRIPT_DIR}/$(basename "${rel}")"
  if [[ -f "$p1" ]]; then echo "$p1"; return 0; fi
  if [[ -f "$p2" ]]; then echo "$p2"; return 0; fi
  return 1
}

py() {
  python3 "$PYTOOLS" "$@"
}

# --- Placeholder substitution (ENV:VARNAME) with both env vars + script vars ---
# We substitute into a temp dir so we always deploy the expanded version.
TMP_DIR="$(mktemp -d)"
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

substitute_to_tmp() {
  local src="$1" dst="$2"

  # Discover placeholders first
  mapfile -t keys < <(py list-placeholders "$src" || true)

  # Build an env-overrides blob from *shell variables* (not just exported ones)
  # so lines like TABLE_NAME="foo" at the top of this script are still picked up.
  local overrides_json
  overrides_json="$(py vars-to-json "${keys[@]:-}")"

  py substitute-file \
    --in "$src" \
    --out "$dst" \
    --vars-json "$overrides_json"
}

# --- Validation ---
need API_NAME
need TABLE_NAME

SCHEMA_PATH="$(find_file "schema.graphql" || true)"
[[ -n "${SCHEMA_PATH}" ]] || die "schema.graphql not found under SCRIPT_DIR=${SCRIPT_DIR}"

# Copy/substitute schema
SCHEMA_TMP="${TMP_DIR}/schema.graphql"
substitute_to_tmp "$SCHEMA_PATH" "$SCHEMA_TMP"

# Basic sanity: schema must include Query root
if ! grep -qE '^\s*type\s+Query\s*\{' "$SCHEMA_TMP"; then
  die "schema.graphql does not define 'type Query { ... }' after substitution"
fi

# --- Create or get API ---
API_ID="$(aws_cli appsync list-graphql-apis --query "graphqlApis[?name=='${API_NAME}'].apiId | [0]" --output text || true)"
if [[ -z "$API_ID" || "$API_ID" == "None" ]]; then
  echo "Creating AppSync API: ${API_NAME} (auth=${AUTH_TYPE})"
  API_ID="$(aws_cli appsync create-graphql-api \
    --name "${API_NAME}" \
    --authentication-type "${AUTH_TYPE}" \
    --query "graphqlApi.apiId" \
    --output text)"
else
  echo "Using existing AppSync API: ${API_NAME} (apiId=${API_ID})"
  # Keep auth type consistent if you want (optional):
  aws_cli appsync update-graphql-api --api-id "${API_ID}" --name "${API_NAME}" --authentication-type "${AUTH_TYPE}" >/dev/null
fi

# --- Ensure API key if using API_KEY auth ---
if [[ "${AUTH_TYPE}" == "API_KEY" ]]; then
  # Create a key only if there isn't any active one
  ACTIVE_KEY_ID="$(aws_cli appsync list-api-keys --api-id "${API_ID}" \
    --query "apiKeys[?expires>=$(date +%s)].id | [0]" --output text || true)"
  if [[ -z "$ACTIVE_KEY_ID" || "$ACTIVE_KEY_ID" == "None" ]]; then
    # Convert days -> seconds -> epoch
    EXPIRES_EPOCH="$(( $(date +%s) + (API_KEY_EXPIRES_DAYS * 86400) ))"
    echo "Creating API key (expires in ${API_KEY_EXPIRES_DAYS} days)"
    aws_cli appsync create-api-key --api-id "${API_ID}" --expires "${EXPIRES_EPOCH}" >/dev/null
  fi
fi

# --- Upload schema (async) ---
echo "Uploading schema..."
aws_cli appsync start-schema-creation --api-id "${API_ID}" --definition "fileb://${SCHEMA_TMP}" >/dev/null

echo "Waiting for schema creation to finish..."
for _ in {1..60}; do
  STATUS="$(aws_cli appsync get-schema-creation-status --api-id "${API_ID}" --query status --output text)"
  case "$STATUS" in
    SUCCESS|ACTIVE) break ;;
    FAILED) die "Schema creation FAILED. Check AppSync console / CloudWatch logs." ;;
    *) sleep 2 ;;
  esac
done
echo "Schema status: ${STATUS}"

# --- Ensure IAM role for DynamoDB datasource ---
# Trust policy: appsync.amazonaws.com
TRUST_TMP="${TMP_DIR}/trust.json"
cat > "$TRUST_TMP" <<'JSON'
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Principal": { "Service": "appsync.amazonaws.com" },
      "Action": "sts:AssumeRole"
    }
  ]
}
JSON

# Inline policy: least-ish privilege for typical DynamoDB resolvers
# (Uses wildcard account to avoid embedding account numbers.)
POLICY_TMP="${TMP_DIR}/role-policy.json"
cat > "$POLICY_TMP" <<JSON
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "AppSyncDynamoAccess",
      "Effect": "Allow",
      "Action": [
        "dynamodb:GetItem",
        "dynamodb:PutItem",
        "dynamodb:UpdateItem",
        "dynamodb:DeleteItem",
        "dynamodb:Query",
        "dynamodb:Scan",
        "dynamodb:BatchGetItem",
        "dynamodb:BatchWriteItem"
      ],
      "Resource": [
        "arn:aws:dynamodb:${REGION}:*:table/${TABLE_NAME}",
        "arn:aws:dynamodb:${REGION}:*:table/${TABLE_NAME}/*"
      ]
    }
  ]
}
JSON

ROLE_ARN="$(aws_cli iam get-role --role-name "${ROLE_NAME}" --query "Role.Arn" --output text 2>/dev/null || true)"
if [[ -z "${ROLE_ARN}" || "${ROLE_ARN}" == "None" ]]; then
  echo "Creating IAM role for AppSync datasource: ${ROLE_NAME}"
  ROLE_ARN="$(aws_cli iam create-role \
    --role-name "${ROLE_NAME}" \
    --assume-role-policy-document "file://${TRUST_TMP}" \
    --query "Role.Arn" --output text)"
else
  echo "Using existing IAM role: ${ROLE_NAME}"
  aws_cli iam update-assume-role-policy --role-name "${ROLE_NAME}" --policy-document "file://${TRUST_TMP}" >/dev/null
fi

# Put/refresh inline policy
aws_cli iam put-role-policy \
  --role-name "${ROLE_NAME}" \
  --policy-name "${ROLE_POLICY_NAME}" \
  --policy-document "file://${POLICY_TMP}" >/dev/null

# --- Ensure DynamoDB datasource ---
DS_ARN="$(aws_cli appsync get-data-source --api-id "${API_ID}" --name "${DATASOURCE_NAME}" \
  --query "dataSource.dataSourceArn" --output text 2>/dev/null || true)"

if [[ -z "${DS_ARN}" || "${DS_ARN}" == "None" ]]; then
  echo "Creating AppSync datasource: ${DATASOURCE_NAME} -> DynamoDB(${TABLE_NAME})"
  aws_cli appsync create-data-source \
    --api-id "${API_ID}" \
    --name "${DATASOURCE_NAME}" \
    --type "AMAZON_DYNAMODB" \
    --service-role-arn "${ROLE_ARN}" \
    --dynamodb-config "tableName=${TABLE_NAME},awsRegion=${REGION},useCallerCredentials=false" >/dev/null
else
  echo "Updating AppSync datasource: ${DATASOURCE_NAME}"
  aws_cli appsync update-data-source \
    --api-id "${API_ID}" \
    --name "${DATASOURCE_NAME}" \
    --type "AMAZON_DYNAMODB" \
    --service-role-arn "${ROLE_ARN}" \
    --dynamodb-config "tableName=${TABLE_NAME},awsRegion=${REGION},useCallerCredentials=false" >/dev/null
fi

RUNTIME_ARG="name=APPSYNC_JS,runtimeVersion=1.0.0"

# --- Functions (for pipeline resolver Query.getCamera) ---
ensure_function() {
  local func_name="$1" file_rel="$2"
  local src
  src="$(find_file "functions/${file_rel}" || find_file "${file_rel}" || true)"
  [[ -n "$src" ]] || die "Function file not found: ${file_rel}"

  local dst="${TMP_DIR}/${func_name}.js"
  substitute_to_tmp "$src" "$dst"

  local func_id
  func_id="$(aws_cli appsync list-functions --api-id "${API_ID}" \
    --query "functions[?name=='${func_name}'].functionId | [0]" --output text || true)"

  if [[ -z "$func_id" || "$func_id" == "None" ]]; then
    func_id="$(aws_cli appsync create-function \
      --api-id "${API_ID}" \
      --name "${func_name}" \
      --data-source-name "${DATASOURCE_NAME}" \
      --runtime "${RUNTIME_ARG}" \
      --code "file://${dst}" \
      --query "functionConfiguration.functionId" --output text)"
  else
    aws_cli appsync update-function \
      --api-id "${API_ID}" \
      --function-id "${func_id}" \
      --name "${func_name}" \
      --data-source-name "${DATASOURCE_NAME}" \
      --runtime "${RUNTIME_ARG}" \
      --code "file://${dst}" >/dev/null
  fi

  echo "${func_id}"
}

FUNC_INFO_ID="$(ensure_function "getCameraInfo" "getCameraInfo.js")"
FUNC_STATUS_ID="$(ensure_function "getCameraStatus" "getCameraStatus.js")"

# --- Resolvers ---
ensure_resolver() {
  local type_name="$1" field_name="$2" kind="$3" file_rel="$4" pipeline_funcs_csv="${5:-}"

  local src
  src="$(find_file "resolvers/${file_rel}" || find_file "${file_rel}" || true)"
  [[ -n "$src" ]] || die "Resolver file not found: ${file_rel}"

  local dst="${TMP_DIR}/${type_name}.${field_name}.js"
  substitute_to_tmp "$src" "$dst"

  # Does it exist?
  if aws_cli appsync get-resolver --api-id "${API_ID}" --type-name "${type_name}" --field-name "${field_name}" >/dev/null 2>&1; then
    echo "Updating resolver: ${type_name}.${field_name} (${kind})"
    if [[ "${kind}" == "PIPELINE" ]]; then
      aws_cli appsync update-resolver \
        --api-id "${API_ID}" \
        --type-name "${type_name}" \
        --field-name "${field_name}" \
        --kind "PIPELINE" \
        --pipeline-config "functions=${pipeline_funcs_csv}" \
        --runtime "${RUNTIME_ARG}" \
        --code "file://${dst}" >/dev/null
    else
      aws_cli appsync update-resolver \
        --api-id "${API_ID}" \
        --type-name "${type_name}" \
        --field-name "${field_name}" \
        --kind "UNIT" \
        --data-source-name "${DATASOURCE_NAME}" \
        --runtime "${RUNTIME_ARG}" \
        --code "file://${dst}" >/dev/null
    fi
  else
    echo "Creating resolver: ${type_name}.${field_name} (${kind})"
    if [[ "${kind}" == "PIPELINE" ]]; then
      aws_cli appsync create-resolver \
        --api-id "${API_ID}" \
        --type-name "${type_name}" \
        --field-name "${field_name}" \
        --kind "PIPELINE" \
        --pipeline-config "functions=${pipeline_funcs_csv}" \
        --runtime "${RUNTIME_ARG}" \
        --code "file://${dst}" >/dev/null
    else
      aws_cli appsync create-resolver \
        --api-id "${API_ID}" \
        --type-name "${type_name}" \
        --field-name "${field_name}" \
        --data-source-name "${DATASOURCE_NAME}" \
        --kind "UNIT" \
        --runtime "${RUNTIME_ARG}" \
        --code "file://${dst}" >/dev/null
    fi
  fi
}

# Based on your schema/query names:
ensure_resolver "Query" "getCamera" "PIPELINE" "getCamera.js" "${FUNC_INFO_ID},${FUNC_STATUS_ID}"
ensure_resolver "Query" "listCameraStatuses" "UNIT" "listCameraStatuses.js"
ensure_resolver "Query" "listRecordings" "UNIT" "listRecordings.js"

echo ""
echo "AppSync deployed:"
echo "  API_NAME=${API_NAME}"
echo "  API_ID=${API_ID}"
echo "  REGION=${REGION}"
