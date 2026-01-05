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
LOWERCASE_STACK_BASE="${BASE_STACK_NAME,,}"
ENVIRONMENT="${2:-}"
shift 2 || true
STACK_NAME="${BASE_STACK_NAME}-${ENVIRONMENT}"


PROFILE="${AWS_PROFILE:-}"
REGION="${AWS_REGION:-}"
BUCKET="${CFN_ARTIFACT_BUCKET:-}"
TEMPLATE="root.yaml"
OUTDIR=".build"

# Lambdas live in ./lambda/<function-name>/
LAMBDA_ROOT="nested/lambda"


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

ACCOUNT_ID="$(aws "${AWS_ARGS[@]}" sts get-caller-identity --query Account --output text)"

# Keep a copy of the "base" args (may include --profile) for the assume-role call
AWS_BASE_ARGS=("${AWS_ARGS[@]}")

assume_role_if_needed() {
  local env="$1"
  local arn role_name role_arn creds

  arn="$(aws "${AWS_ARGS[@]}" sts get-caller-identity --query Arn --output text)"

  # If we're already an assumed role, nothing to do.
  if [[ "$arn" == *":assumed-role/"* ]]; then
    return 0
  fi

  # Choose deployer role by env
  if [[ "$env" == "prod" ]]; then
    role_name="yx-deployer-prod"
  else
    role_name="yx-deployer-devtest"
  fi

  role_arn="arn:aws:iam::${ACCOUNT_ID}:role/${role_name}"

  echo "==> Assuming role for deploy: ${role_arn}" >&2

  # Assume using the *base* identity (profile/user keys)
  creds="$(aws "${AWS_BASE_ARGS[@]}" sts assume-role \
    --role-arn "$role_arn" \
    --role-session-name "deploy-${STACK_NAME}-$(date +%s)" \
    --query 'Credentials.[AccessKeyId,SecretAccessKey,SessionToken]' \
    --output text)"

  export AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY AWS_SESSION_TOKEN
  AWS_ACCESS_KEY_ID="$(awk '{print $1}' <<<"$creds")"
  AWS_SECRET_ACCESS_KEY="$(awk '{print $2}' <<<"$creds")"
  AWS_SESSION_TOKEN="$(awk '{print $3}' <<<"$creds")"

  # IMPORTANT: once we export temp creds, stop using --profile so env creds take effect
  AWS_ARGS=()
  AWS_ARGS+=(--region "$REGION")

  arn="$(aws "${AWS_ARGS[@]}" sts get-caller-identity --query Arn --output text)"
  echo "==> Caller is now: $arn" >&2
}

assume_role_if_needed "$ENVIRONMENT"


if [[ ! -f "$TEMPLATE" ]]; then
  echo "Error: template not found: $TEMPLATE" >&2
  exit 2
fi

# Create a default artifacts bucket if none provided.
# Use account+region to keep it globally unique.
if [[ -z "$BUCKET" ]]; then
  BUCKET="cfn-artifacts-${ACCOUNT_ID}-${REGION}"
fi

ensure_bucket() {
  local bucket="$1"
  local err=""

  # The artifacts bucket must already exist (created by IAM/create.sh or manually).
  if aws "${AWS_ARGS[@]}" s3api head-bucket --bucket "$bucket" >/dev/null 2>&1; then
    return 0
  fi

  err="$(aws "${AWS_ARGS[@]}" s3api head-bucket --bucket "$bucket" 2>&1 || true)"

  if echo "$err" | grep -qiE 'NoSuchBucket|NotFound|404'; then
    echo "ERROR: Artifacts bucket s3://${bucket} does not exist." >&2
    echo "       Create it first (via IAM/create.sh or manually), then re-run this deploy." >&2
  elif echo "$err" | grep -qiE 'Forbidden|AccessDenied'; then
    echo "ERROR: Artifacts bucket s3://${bucket} exists but is not accessible with the current credentials." >&2
    echo "       Ensure you're assuming the deployer role and it has access to the bucket." >&2
  else
    echo "ERROR: Unable to verify access to artifacts bucket s3://${bucket}." >&2
  fi

  echo "Details: ${err}" >&2
  exit 1
}

zip_lambdas() {
  local root="$1"

  if [[ ! -d "$root" ]]; then
    echo "==> No ./$root directory found; skipping lambda zips."
    return 0
  fi

  if ! command -v zip >/dev/null 2>&1; then
    echo "Error: 'zip' is required but not found in PATH." >&2
    exit 2
  fi

  # Collect directories safely (so a non-zero from find doesn't abort under pipefail)
  local -a dirs=()
  while IFS= read -r -d '' d; do
    dirs+=("$d")
  done < <(find "$root" -mindepth 1 -maxdepth 1 -type d -print0 2>/dev/null || true)

  if [[ ${#dirs[@]} -eq 0 ]]; then
    echo "==> No subdirectories found under ./$root; skipping."
    return 0
  fi

  for dir in "${dirs[@]}"; do
    local name zip_path
    name="$(basename "$dir")"
    zip_path="${dir}/${name}.zip"

    echo "==> Zipping lambda: ${dir} -> ${zip_path}"
    rm -f "$zip_path"

    (
      cd "$dir"

      # Don't let strict mode kill the whole deploy if zip returns "nothing to do"
      set +e
      zip -qr "${name}.zip" . \
        -x "${name}.zip" \
        -x '__pycache__/*' \
        -x '*.pyc' \
        -x '.pytest_cache/*' \
        -x '.mypy_cache/*' \
        -x '.venv/*' \
        -x 'venv/*' \
        -x '.git/*' \
        -x 'node_modules/*'
      rc=$?
      set -e

      # zip exit 12 = "nothing to do" (often empty dir / all excluded)
      if [[ $rc -eq 12 ]]; then
        echo "==> Warning: nothing to zip in ${dir} (zip exit 12). Continuing."
        rm -f "${name}.zip"
      elif [[ $rc -ne 0 ]]; then
        echo "Error: zip failed in ${dir} (exit $rc)" >&2
        exit $rc
      fi
    )
  done
}


ensure_bucket "$BUCKET" "$REGION"

mkdir -p "$OUTDIR"

PACKAGED_TEMPLATE="${OUTDIR}/packaged.yaml"

# ---- NEW: build lambda zip artifacts ----
zip_lambdas "$LAMBDA_ROOT"

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
  --parameter-overrides Environment="$ENVIRONMENT" LowercaseStackName="$LOWERCASE_STACK_BASE" \
  --role-arn "$CFN_EXEC_ROLE_ARN" \
  --no-fail-on-empty-changeset

echo "==> Done. Artifacts bucket: s3://$BUCKET"
