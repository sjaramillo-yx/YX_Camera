#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <stack-name> [additional aws cloudformation deploy args]" >&2
  exit 1
fi

STACK_NAME="$1"
shift || true

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

aws cloudformation deploy \
  --template-file "${SCRIPT_DIR}/all-in-one.yaml" \
  --stack-name "${STACK_NAME}" \
  --capabilities CAPABILITY_NAMED_IAM \
  "$@"
