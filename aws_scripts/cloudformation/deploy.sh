#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  ./deploy.sh <stack-name> <environment> [options]

Args:
  stack-name     CloudFormation stack name to deploy/update
  environment    dev | test | prod

Options:
  -p, --profile  AWS CLI profile (default: AWS_PROFILE if set)
  -r, --region   AWS region (default: AWS_REGION or aws configure get region)
  -b, --bucket   S3 bucket for packaging artifacts (default: auto-generated)
  -t, --template Root template file (default: root.yaml)
  -o, --outdir   Output directory for packaged template (default: ./.build)
EOF
}

BASE_STACK_NAME="${1:-}"
ENVIRONMENT="${2:-}"
shift 2 || true
STACK_NAME="${BASE_STACK_NAME}-${ENVIRONMENT}"


PROFILE="${AWS_PROFILE:-}"
REGION="${AWS_REGION:-}"
BUCKET="${CFN_ARTIFACT_BUCKET:-}"
TEMPLATE="root.yaml"
OUTDIR=".build"


while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--profile) PROFILE="${2:-}"; shift 2;;
    -r|--region)  REGION="${2:-}"; shift 2;;
    -b|--bucket)  BUCKET="${2:-}"; shift 2;;
    -t|--template) TEMPLATE="${2:-}"; shift 2;;
    -o|--outdir) OUTDIR="${2:-}"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage; exit 2;;
  esac
done

if [[ -z "$STACK_NAME" || -z "$ENVIRONMENT" ]]; then
  echo "Error: <stack-name> and <environment> are required." >&2
  usage
  exit 2
fi

case "$ENVIRONMENT" in
  dev|test|prod) ;;
  *) echo "Error: environment must be one of: dev, test, prod" >&2; exit 2;;
esac

AWS_ARGS=()
[[ -n "$PROFILE" ]] && AWS_ARGS+=(--profile "$PROFILE")

# Region: prefer env/arg, else pull from config
if [[ -z "$REGION" ]]; then
  REGION="$(aws "${AWS_ARGS[@]}" configure get region 2>/dev/null || true)"
fi
if [[ -z "$REGION" ]]; then
  echo "Error: region not set. Use -r/--region or set AWS_REGION (or configure a default region)." >&2
  exit 2
fi
AWS_ARGS+=(--region "$REGION")

if [[ ! -f "$TEMPLATE" ]]; then
  echo "Error: template not found: $TEMPLATE" >&2
  exit 2
fi

# Create a default artifacts bucket if none provided.
# Use account+region to keep it globally unique.
if [[ -z "$BUCKET" ]]; then
  ACCOUNT_ID="$(aws "${AWS_ARGS[@]}" sts get-caller-identity --query Account --output text)"
  BUCKET="cfn-artifacts-${ACCOUNT_ID}-${REGION}"
fi

ensure_bucket() {
  local bucket="$1"
  local region="$2"

  # If bucket exists and we can access it, this succeeds.
  if aws "${AWS_ARGS[@]}" s3api head-bucket --bucket "$bucket" >/dev/null 2>&1; then
    return 0
  fi

  echo "==> Creating artifacts bucket: s3://$bucket (region: $region)"

  if [[ "$region" == "us-east-1" ]]; then
    aws "${AWS_ARGS[@]}" s3api create-bucket --bucket "$bucket" >/dev/null
  else
    aws "${AWS_ARGS[@]}" s3api create-bucket \
      --bucket "$bucket" \
      --create-bucket-configuration "LocationConstraint=$region" >/dev/null
  fi

  # Optional but helpful defaults
  aws "${AWS_ARGS[@]}" s3api put-bucket-versioning \
    --bucket "$bucket" \
    --versioning-configuration Status=Enabled >/dev/null

  aws "${AWS_ARGS[@]}" s3api put-public-access-block \
    --bucket "$bucket" \
    --public-access-block-configuration \
      BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true >/dev/null
}

ensure_bucket "$BUCKET" "$REGION"

mkdir -p "$OUTDIR"
PACKAGED_TEMPLATE="${OUTDIR}/packaged.yaml"

echo "==> Packaging $TEMPLATE -> $PACKAGED_TEMPLATE"
aws "${AWS_ARGS[@]}" cloudformation package \
  --template-file "$TEMPLATE" \
  --s3-bucket "$BUCKET" \
  --output-template-file "$PACKAGED_TEMPLATE"

echo "==> Deploying stack: $STACK_NAME (env=$ENVIRONMENT)"

if [[ "$ENVIRONMENT" == "dev" ]]; then
  CFN_EXEC_ROLE_ARN="arn:aws:iam::${ACCOUNT_ID}:role/cfn-exec-dev"
elif [[ "$ENVIRONMENT" == "test" ]]; then
  CFN_EXEC_ROLE_ARN="arn:aws:iam::${ACCOUNT_ID}:role/cfn-exec-test"
elif [[ "$ENVIRONMENT" == "prod" ]]; then
  CFN_EXEC_ROLE_ARN="arn:aws:iam::${ACCOUNT_ID}:role/cfn-exec-prod"
fi
echo "==> Role ARN: ${CFN_EXEC_ROLE_ARN}"

aws "${AWS_ARGS[@]}" cloudformation deploy \
  --stack-name "$STACK_NAME" \
  --template-file "$PACKAGED_TEMPLATE" \
  --capabilities CAPABILITY_NAMED_IAM \
  --parameter-overrides Environment="$ENVIRONMENT" \
  --role-arn "$CFN_EXEC_ROLE_ARN" \
  --no-fail-on-empty-changeset

echo "==> Done. Artifacts bucket: s3://$BUCKET"
