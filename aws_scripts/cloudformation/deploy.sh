#!/usr/bin/env bash
set -euo pipefail

# deploy.sh
# Deploys your CloudFormation templates in dependency order and wires Outputs -> Parameters.
#
# Usage:
#   chmod +x deploy.sh
#   ./deploy.sh <env-or-storage-stack-name>
#
# Examples:
#   ./deploy.sh dev
#   ./deploy.sh storage-and-data-dev
#
# Optional env vars:
#   AWS_PROFILE=yourprofile
#   AWS_REGION=us-east-1
#   ENV=dev
#   TABLE_NAME=yx-cam-data
#   API_NAME=yx-cam-api
#   AUTH_TYPE=AWS_IAM

ENV="${ENV:-dev}"

# If provided, the first positional argument controls stack naming.
#
# Behavior:
# - If you pass something like "dev" (alphanumeric only), stacks will be named
#   "<template>-dev".
# - If you pass a full storage stack name (anything containing a dash, e.g.
#   "foo-bar-dev" or "storage-and-data-dev"), that exact value will be used as
#   the storage stack name, and the suffix ("-dev") will be reused for the other
#   stacks.
ARG1="${1:-}"

if [[ -n "$ARG1" ]]; then
  if [[ "$ARG1" =~ ^[A-Za-z0-9]+$ ]]; then
    # Treat as an env/suffix (simple token like "dev", "prod").
    ENV="$ARG1"
    STACK_SUFFIX="-$ENV"
    STACK_STORAGE="yx-vuelta-cam${STACK_SUFFIX}"
  else
    # Treat as the full storage stack name (e.g., "foo-bar-dev").
    STACK_STORAGE="$ARG1"
    # Reuse the last dash-delimited segment as the env suffix for other stacks.
    # Examples:
    #   foo-bar-dev           -> ENV=dev,  STACK_SUFFIX="-dev"
    #   storage-and-data-dev  -> ENV=dev,  STACK_SUFFIX="-dev"
    #   mystack               -> (won't land here; handled above)
    ENV="${ARG1##*-}"
    STACK_SUFFIX="-$ENV"
  fi
else
  STACK_SUFFIX="-$ENV"
  STACK_STORAGE="storage-and-data${STACK_SUFFIX}"
fi
TABLE_NAME="${TABLE_NAME:-yx-cam-data}"
API_NAME="${API_NAME:-yx-cam-api}"
AUTH_TYPE="${AUTH_TYPE:-AWS_IAM}"

STACK_IOT_LAMBDA="iot-lambda-and-rules${STACK_SUFFIX}"
STACK_APPSYNC="appsync${STACK_SUFFIX}"
STACK_IOT_CORE="iot-core${STACK_SUFFIX}"
STACK_BOOTSTRAP="bootstrap-user${STACK_SUFFIX}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}" )" && pwd)"

TEMPLATE_STORAGE="$SCRIPT_DIR/storage-and-data.yaml"
TEMPLATE_IOT_LAMBDA="$SCRIPT_DIR/iot-lambda-and-rules.yaml"
TEMPLATE_APPSYNC="$SCRIPT_DIR/appsync.yaml"
TEMPLATE_IOT_CORE="$SCRIPT_DIR/iot-core.yaml"
TEMPLATE_BOOTSTRAP="$SCRIPT_DIR/bootstrap-user.yaml"

CAPS=(--capabilities CAPABILITY_NAMED_IAM)

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "Missing required command: $1" >&2; exit 1; }
}

log() { echo -e "\n==> $*\n"; }

need_cmd aws

# Helpful context
log "Using AWS_PROFILE=${AWS_PROFILE:-<default>} AWS_REGION=${AWS_REGION:-<default>} ENV=$ENV"
aws sts get-caller-identity >/dev/null

validate() {
  local f="$1"
  log "Validating template: $f"
  aws cloudformation validate-template --template-body "file://$f" >/dev/null
  echo "OK: $f"
}

deploy_stack() {
  local stack="$1"
  local template="$2"
  shift 2
  local params=("$@")

  log "Deploying stack: $stack (template: $template)"
  if ((${#params[@]} > 0)); then
    aws cloudformation deploy \
      --stack-name "$stack" \
      --template-file "$template" \
      "${CAPS[@]}" \
      --parameter-overrides "${params[@]}"
  else
    aws cloudformation deploy \
      --stack-name "$stack" \
      --template-file "$template" \
      "${CAPS[@]}"
  fi

  log "Stack status: $stack"
  aws cloudformation describe-stacks \
    --stack-name "$stack" \
    --query "Stacks[0].StackStatus" \
    --output text
}

get_output() {
  local stack="$1"
  local key="$2"
  aws cloudformation describe-stacks \
    --stack-name "$stack" \
    --query "Stacks[0].Outputs[?OutputKey=='$key'].OutputValue | [0]" \
    --output text
}

# ---- Validate all templates first (fast fail) ----
validate "$TEMPLATE_STORAGE"
validate "$TEMPLATE_IOT_LAMBDA"
validate "$TEMPLATE_APPSYNC"
validate "$TEMPLATE_IOT_CORE"
validate "$TEMPLATE_BOOTSTRAP"

# ---- Deploy in dependency order ----

# 1) Base storage/data
deploy_stack "$STACK_STORAGE" "$TEMPLATE_STORAGE" \
  "TableName=$TABLE_NAME"

# 2) Wire IoT Rule Role ARN into lambda/rules stack
log "Fetching Output IotRuleRoleArn from $STACK_STORAGE"
IOT_RULE_ROLE_ARN="$(get_output "$STACK_STORAGE" "IotRuleRoleArn")"
if [[ -z "$IOT_RULE_ROLE_ARN" || "$IOT_RULE_ROLE_ARN" == "None" ]]; then
  echo "ERROR: Could not read OutputKey=IotRuleRoleArn from stack $STACK_STORAGE" >&2
  echo "Check Outputs in the storage stack:" >&2
  aws cloudformation describe-stacks --stack-name "$STACK_STORAGE" \
    --query "Stacks[0].Outputs" --output table >&2
  exit 1
fi
echo "IotRuleRoleArn=$IOT_RULE_ROLE_ARN"

deploy_stack "$STACK_IOT_LAMBDA" "$TEMPLATE_IOT_LAMBDA" \
  "TableName=$TABLE_NAME" \
  "IotRuleRoleArn=$IOT_RULE_ROLE_ARN"

# 3) AppSync
deploy_stack "$STACK_APPSYNC" "$TEMPLATE_APPSYNC" \
  "TableName=$TABLE_NAME" \
  "ApiName=$API_NAME" \
  "AuthType=$AUTH_TYPE"

# 4) IoT core (policies/provisioning)
deploy_stack "$STACK_IOT_CORE" "$TEMPLATE_IOT_CORE"

# 5) Bootstrap user (optional). Disabled by default because it may output credentials.
if [[ "${DEPLOY_BOOTSTRAP_USER:-false}" == "true" ]]; then
  deploy_stack "$STACK_BOOTSTRAP" "$TEMPLATE_BOOTSTRAP"
else
  log "Skipping $STACK_BOOTSTRAP (set DEPLOY_BOOTSTRAP_USER=true to deploy bootstrap-user.yaml)"
fi

# ---- Print useful outputs ----
log "Outputs summary"

echo "Storage stack outputs:"
aws cloudformation describe-stacks --stack-name "$STACK_STORAGE" \
  --query "Stacks[0].Outputs" --output table || true

echo
echo "IoT lambda/rules stack outputs:"
aws cloudformation describe-stacks --stack-name "$STACK_IOT_LAMBDA" \
  --query "Stacks[0].Outputs" --output table || true

echo
echo "AppSync stack outputs:"
aws cloudformation describe-stacks --stack-name "$STACK_APPSYNC" \
  --query "Stacks[0].Outputs" --output table || true

echo
echo "IoT core stack outputs:"
aws cloudformation describe-stacks --stack-name "$STACK_IOT_CORE" \
  --query "Stacks[0].Outputs" --output table || true

log "Done."
echo "To delete everything (careful!):"
echo "  aws cloudformation delete-stack --stack-name $STACK_BOOTSTRAP"
echo "  aws cloudformation delete-stack --stack-name $STACK_IOT_CORE"
echo "  aws cloudformation delete-stack --stack-name $STACK_APPSYNC"
echo "  aws cloudformation delete-stack --stack-name $STACK_IOT_LAMBDA"
echo "  aws cloudformation delete-stack --stack-name $STACK_STORAGE"