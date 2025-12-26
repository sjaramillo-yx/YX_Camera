#!/usr/bin/env bash
set -euo pipefail

# deploy.sh
# Deploys all CloudFormation resources in a single stack and wires parameters.
#
# Usage:
#   chmod +x deploy.sh
#   ./deploy.sh <stack-name>
#
# Optional env vars:
#   AWS_PROFILE=yourprofile
#   AWS_REGION=us-east-1
#   TABLE_NAME=yx-cam-data
#   API_NAME=yx-cam-api
#   AUTH_TYPE=AWS_IAM
#   LAMBDA_MEMORY=256
#   LAMBDA_TIMEOUT=10
#   DEPLOY_BOOTSTRAP_USER=false

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <stack-name>" >&2
  exit 1
fi

STACK_NAME="$1"
TABLE_NAME="${TABLE_NAME:-yx-cam-data}"
API_NAME="${API_NAME:-yx-cam-api}"
AUTH_TYPE="${AUTH_TYPE:-AWS_IAM}"
LAMBDA_MEMORY="${LAMBDA_MEMORY:-256}"
LAMBDA_TIMEOUT="${LAMBDA_TIMEOUT:-10}"
DEPLOY_BOOTSTRAP_USER="${DEPLOY_BOOTSTRAP_USER:-false}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}" )" && pwd)"
TEMPLATE_MAIN="$SCRIPT_DIR/all-resources.yaml"
CAPS=(--capabilities CAPABILITY_NAMED_IAM)

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "Missing required command: $1" >&2; exit 1; }
}

log() { echo -e "\n==> $*\n"; }

need_cmd aws

log "Using AWS_PROFILE=${AWS_PROFILE:-<default>} AWS_REGION=${AWS_REGION:-<default>} STACK_NAME=$STACK_NAME"
aws sts get-caller-identity >/dev/null

validate() {
  local f="$1"
  log "Validating template: $f"
  aws cloudformation validate-template --template-body "file://$f" >/dev/null
  echo "OK: $f"
}

validate "$TEMPLATE_MAIN"

PARAMS=(
  "TableName=${TABLE_NAME}"
  "ApiName=${API_NAME}"
  "AuthType=${AUTH_TYPE}"
  "LambdaMemory=${LAMBDA_MEMORY}"
  "LambdaTimeout=${LAMBDA_TIMEOUT}"
  "DeployBootstrapUser=${DEPLOY_BOOTSTRAP_USER}"
)

log "Deploying stack: $STACK_NAME (template: $TEMPLATE_MAIN)"
aws cloudformation deploy \
  --stack-name "$STACK_NAME" \
  --template-file "$TEMPLATE_MAIN" \
  "${CAPS[@]}" \
  --parameter-overrides "${PARAMS[@]}"

log "Stack status: $STACK_NAME"
aws cloudformation describe-stacks \
  --stack-name "$STACK_NAME" \
  --query "Stacks[0].StackStatus" \
  --output text

log "Outputs summary"
aws cloudformation describe-stacks --stack-name "$STACK_NAME" \
  --query "Stacks[0].Outputs" --output table || true

log "Done."
