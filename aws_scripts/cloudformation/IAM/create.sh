#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  ./create.sh <stack-base> [options]

Required:
  <stack-base>           Base stack name (e.g. "foo")

Options:
  --profile <profile>  AWS CLI profile (default: AWS_PROFILE if set)
  --region <region>      AWS region (default: sa-east-1)
  --artifact-bucket <b>  S3 bucket for CloudFormation artifacts (default: cfn-artifacts-${ACCOUNT_ID}-${REGION})
  --user-policy <file>   JSON IAM policy to attach inline to created user(s)
                         (default: policies/yxcam-user-policy.json)
  --dev-user <name>      IAM user for developers (or set DEV_USER env var)
  --pm-user <name>       OPTIONAL: IAM user for product manager (enables prod deployer role/policy)
  --no-access-keys       Do not create access keys for users
  --reset-console-login  Reset existing console login password(s) and force password reset
  -h, --help             Show this help

Examples:
  DEV_USER=dev.simon ./create.sh foo
  ./create.sh foo --dev-user dev.simon --pm-user pm.maria
EOF
}

# -----------------------------
# Args
# -----------------------------
REGION="sa-east-1"
PROFILE="${AWS_PROFILE:-}"
ARTIFACT_BUCKET="${CFN_ARTIFACT_BUCKET:-}"
USER_POLICY_FILE="${USER_POLICY_FILE:-}"
STACK_BASE=""
DEV_USER="${DEV_USER:-}"
PM_USER="${PM_USER:-}"
CREATE_ACCESS_KEYS="true"
RESET_CONSOLE_LOGIN="false"

if [[ $# -lt 1 ]]; then
  usage
  exit 1
fi

STACK_BASE="$1"
shift
LOWERCASE_STACK_BASE="${STACK_BASE,,}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile)
      PROFILE="$2"; shift 2 ;;
    --region)
      REGION="$2"; shift 2 ;;
    --artifact-bucket)
      ARTIFACT_BUCKET="$2"; shift 2 ;;
    --user-policy)
      USER_POLICY_FILE="$2"; shift 2 ;;
    --dev-user)
      DEV_USER="$2"; shift 2 ;;
    --pm-user)
      PM_USER="$2"; shift 2 ;;
    --no-access-keys)
      CREATE_ACCESS_KEYS="false"; shift ;;
    --reset-console-login)
      RESET_CONSOLE_LOGIN="true"; shift ;;
    -h|--help)
      usage; exit 0 ;;
    *)
      echo "Unknown option: $1"
      usage
      exit 1 ;;
  esac
done

if [[ -z "${DEV_USER}" ]]; then
  echo "ERROR: DEV_USER is required (set DEV_USER env var or pass --dev-user)."
  exit 1
fi

ACCOUNT_ID="$(aws sts get-caller-identity --query Account --output text)"

# Default artifacts bucket naming matches deploy.sh
if [[ -z "${ARTIFACT_BUCKET}" ]]; then
  ARTIFACT_BUCKET="cfn-artifacts-${ACCOUNT_ID}-${REGION}"
fi
export ACCOUNT_ID REGION ARTIFACT_BUCKET STACK_BASE DEV_USER PM_USER PROFILE

# Common AWS CLI args (used for region/profile aware calls like S3)
AWS_ARGS=()
if [[ -n "${PROFILE}" ]]; then
  AWS_ARGS+=(--profile "${PROFILE}")
fi
AWS_ARGS+=(--region "${REGION}")

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RENDER_DIR="${ROOT_DIR}/.rendered"
mkdir -p "${RENDER_DIR}"

# Default user policy (can be overridden via USER_POLICY_FILE env var or --user-policy)
if [[ -z "${USER_POLICY_FILE}" ]]; then
  USER_POLICY_FILE="${ROOT_DIR}/policies/yxcam-user-policy.json"
fi

if [[ ! -f "${USER_POLICY_FILE}" ]]; then
  echo "ERROR: User policy file not found: ${USER_POLICY_FILE}" >&2
  echo "       Create it at policies/yxcam-user-policy.json or pass --user-policy <path>." >&2
  exit 1
fi

log() { echo "==> $*" >&2; }

generate_temp_password() {
  # Generates a strong temp password that includes upper/lower/digit/symbol.
  python - <<'PY'
import secrets, string
upper = string.ascii_uppercase
lower = string.ascii_lowercase
digits = string.digits
symbols = "!@#$%^&*()-_=+[]{}.,?"
# ensure at least one from each class
pw = [
  secrets.choice(upper),
  secrets.choice(lower),
  secrets.choice(digits),
  secrets.choice(symbols),
]
alphabet = upper + lower + digits + symbols
pw += [secrets.choice(alphabet) for _ in range(20)]
secrets.SystemRandom().shuffle(pw)
print("".join(pw))
PY
}

ensure_console_login() {
  local user_name="$1"
  local pw_file="${RENDER_DIR}/${user_name}.console-password.txt"

  if aws iam get-login-profile --user-name "${user_name}" >/dev/null 2>&1; then
    if [[ "${RESET_CONSOLE_LOGIN}" != "true" ]]; then
      log "Console login profile exists for ${user_name} (leaving unchanged; pass --reset-console-login to reset)"
      return 0
    fi

    log "Resetting console login password for ${user_name} (forcing password reset)"
    local tmp_pw
    tmp_pw="$(generate_temp_password)"
    aws iam update-login-profile       --user-name "${user_name}"       --password "${tmp_pw}"       --password-reset-required >/dev/null
    umask 177
    printf "%s\n" "${tmp_pw}" > "${pw_file}"
    log "Saved TEMP console password to: ${pw_file} (chmod 600)"
  else
    log "Creating console login profile for ${user_name} (forcing password reset)"
    local tmp_pw
    tmp_pw="$(generate_temp_password)"
    aws iam create-login-profile       --user-name "${user_name}"       --password "${tmp_pw}"       --password-reset-required >/dev/null
    umask 177
    printf "%s\n" "${tmp_pw}" > "${pw_file}"
    log "Saved TEMP console password to: ${pw_file} (chmod 600)"
  fi
}


render_json() {
  local in_file="$1"
  local out_file="$2"
  local user_name="${3:-}"
  local env_name="${4:-}"

  # Using | delimiter to avoid escaping / in ARNs
  sed \
    -e "s|\${ACCOUNT_ID}|${ACCOUNT_ID}|g" \
    -e "s|\${REGION}|${REGION}|g" \
    -e "s|\${ARTIFACT_BUCKET}|${ARTIFACT_BUCKET}|g" \
    -e "s|\${STACK_BASE}|${STACK_BASE}|g" \
    -e "s|\${LOWERCASE_STACK_BASE}|${LOWERCASE_STACK_BASE}|g" \
    -e "s|\${DEV_USER}|${DEV_USER}|g" \
    -e "s|\${PM_USER}|${PM_USER}|g" \
    -e "s|\${USER_NAME}|${user_name}|g" \
    -e "s|\${ENV}|${env_name}|g" \
    "${in_file}" > "${out_file}"

  python -m json.tool "${out_file}" >/dev/null
}

policy_arn_by_name() {
  local name="$1"
  aws iam list-policies --scope Local \
    --query "Policies[?PolicyName=='${name}'].Arn | [0]" --output text
}

require_arn() {
  local arn="$1"
  if [[ ! "$arn" =~ ^arn:aws:iam::[0-9]{12}:policy/.+ ]]; then
    echo "ERROR: Not a valid policy ARN: '$arn'" >&2
    exit 1
  fi
}

ensure_room_for_policy_version() {
  # Managed policies can have max 5 versions. If there are already 4 non-default versions,
  # delete the oldest non-default version so we can create a new default.
  local policy_arn="$1"
  local policy_name="$2"

  local non_default_count
  non_default_count="$(aws iam list-policy-versions \
    --policy-arn "${policy_arn}" \
    --query 'length(Versions[?IsDefaultVersion==`false`])' \
    --output text)"

  if [[ "${non_default_count}" -ge 4 ]]; then
    local oldest_non_default
    oldest_non_default="$(aws iam list-policy-versions \
      --policy-arn "${policy_arn}" \
      --query 'sort_by(Versions[?IsDefaultVersion==`false`], &CreateDate)[0].VersionId' \
      --output text)"
    if [[ -n "${oldest_non_default}" && "${oldest_non_default}" != "None" ]]; then
      log "Deleting oldest non-default version ${oldest_non_default} for ${policy_name}"
      aws iam delete-policy-version --policy-arn "${policy_arn}" --version-id "${oldest_non_default}" >/dev/null
    fi
  fi
}

ensure_managed_policy() {
  local policy_name="$1"
  local policy_src="$2"
  local user_name="${3:-}"
  local env_name="${4:-}"

  # Always render so updates are possible even when the policy already exists
  local rendered="${RENDER_DIR}/${policy_name}.json"
  render_json "${policy_src}" "${rendered}" "${user_name}" "${env_name}"

  local existing_arn
  existing_arn="$(policy_arn_by_name "${policy_name}")"

  if [[ "${existing_arn}" != "None" && -n "${existing_arn}" ]]; then
    require_arn "${existing_arn}"
    log "Managed policy exists: ${policy_name} (updating default version)"

    ensure_room_for_policy_version "${existing_arn}" "${policy_name}"

    aws iam create-policy-version \
      --policy-arn "${existing_arn}" \
      --policy-document "file://${rendered}" \
      --set-as-default >/dev/null

    echo "${existing_arn}"
    return
  fi

  log "Creating managed policy: ${policy_name}"
  local created_arn
  created_arn="$(aws iam create-policy \
    --policy-name "${policy_name}" \
    --policy-document "file://${rendered}" \
    --query 'Policy.Arn' --output text)"

  require_arn "${created_arn}"
  echo "${created_arn}"
}

ensure_role() {
  local role_name="$1"
  local trust_src="$2"
  local user_name="${3:-}"

  local rendered="${RENDER_DIR}/trust-${role_name}.json"
  render_json "${trust_src}" "${rendered}" "${user_name}" ""

  if aws iam get-role --role-name "${role_name}" >/dev/null 2>&1; then
    log "Role exists: ${role_name} (updating trust policy)"
    aws iam update-assume-role-policy \
      --role-name "${role_name}" \
      --policy-document "file://${rendered}" >/dev/null
  else
    log "Creating role: ${role_name}"
    aws iam create-role \
      --role-name "${role_name}" \
      --assume-role-policy-document "file://${rendered}" >/dev/null
  fi
}

ensure_attach_role_policy() {
  local role_name="$1"
  local policy_arn="$2"


  require_arn "${policy_arn}"

  if aws iam list-attached-role-policies --role-name "${role_name}" \
      --query "AttachedPolicies[?PolicyArn=='${policy_arn}'] | length(@)" \
      --output text | grep -q '^1$'; then
    log "Policy already attached to role: ${role_name}"
    return
  fi

  log "Attaching policy to role: ${role_name}"
  aws iam attach-role-policy --role-name "${role_name}" --policy-arn "${policy_arn}" >/dev/null
}

ensure_user() {
  local user_name="$1"

  if aws iam get-user --user-name "${user_name}" >/dev/null 2>&1; then
    log "User exists: ${user_name}"
    return 0
  fi

  log "Creating user: ${user_name}"
  aws iam create-user --user-name "${user_name}" >/dev/null

  # IAM is eventually consistent. Wait until the user is readable before continuing
  local attempt=1
  local max_attempts=30
  local sleep_s=1
  while ! aws iam get-user --user-name "${user_name}" >/dev/null 2>&1; do
    if (( attempt >= max_attempts )); then
      echo "ERROR: IAM user '${user_name}' was created but is not readable after ${max_attempts} attempts." >&2
      exit 1
    fi
    log "Waiting for IAM user '${user_name}' to become available... (attempt ${attempt}/${max_attempts})"
    sleep "${sleep_s}"
    attempt=$((attempt+1))
    # small backoff, cap at 5s
    if (( sleep_s < 5 )); then sleep_s=$((sleep_s+1)); fi
  done
}

put_inline_user_policy() {
  local user_name="$1"
  local policy_name="$2"
  local policy_src="$3"

  local rendered="${RENDER_DIR}/inline-${user_name}-${policy_name}.json"
  render_json "${policy_src}" "${rendered}" "" ""

  log "Putting inline policy on user: ${user_name} (${policy_name})"
  aws iam put-user-policy \
    --user-name "${user_name}" \
    --policy-name "${policy_name}" \
    --policy-document "file://${rendered}" >/dev/null
}

maybe_create_access_key() {
  local user_name="$1"
  if [[ "${CREATE_ACCESS_KEYS}" != "true" ]]; then
    log "Skipping access keys for ${user_name} (--no-access-keys)"
    return
  fi

  local key_count
  key_count="$(aws iam list-access-keys --user-name "${user_name}" --query 'length(AccessKeyMetadata)' --output text)"

  if [[ "${key_count}" -ge 1 ]]; then
    log "User ${user_name} already has ${key_count} access key(s). Skipping create-access-key."
    return
  fi

  log "Creating access key for ${user_name} (SAVE OUTPUT!)"
  aws iam create-access-key --user-name "${user_name}"
}

# -----------------------------
# Main
# -----------------------------
log "Account: ${ACCOUNT_ID}"
log "Profile: ${PROFILE:-"(default)"}"
log "Region:  ${REGION}"
log "Stack base: ${STACK_BASE}"
log "Dev user: ${DEV_USER}"
if [[ -n "${PM_USER}" ]]; then
  log "PM user:  ${PM_USER} (prod deploy enabled)"
else
  log "PM user:  (not set) (prod deploy disabled)"
fi

ensure_bucket() {
  local bucket="$1"
  local region="$2"

  # If bucket exists and we can access it, this succeeds.
  if aws "${AWS_ARGS[@]}" s3api head-bucket --bucket "$bucket" >/dev/null 2>&1; then
    return 0
  fi

  # If the bucket exists but we don't have access, head-bucket returns 403.
  # Creating would then fail with a confusing error, so surface a clearer message.
  if aws "${AWS_ARGS[@]}" s3api head-bucket --bucket "$bucket" 2>&1 | grep -q 'Forbidden'; then
    echo "ERROR: Bucket s3://${bucket} exists but you don't have access to it (head-bucket Forbidden)." >&2
    exit 1
  fi

  echo "==> Creating artifacts bucket: s3://$bucket (region: $region)"

  if [[ "$region" == "us-east-1" ]]; then
    aws "${AWS_ARGS[@]}" s3api create-bucket --bucket "$bucket" >/dev/null
  else
    aws "${AWS_ARGS[@]}" s3api create-bucket \
      --bucket "$bucket" \
      --create-bucket-configuration "LocationConstraint=$region" >/dev/null
  fi

  # Helpful defaults
  aws "${AWS_ARGS[@]}" s3api put-bucket-versioning \
    --bucket "$bucket" \
    --versioning-configuration Status=Enabled >/dev/null

  aws "${AWS_ARGS[@]}" s3api put-public-access-block \
    --bucket "$bucket" \
    --public-access-block-configuration \
      BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true >/dev/null

  aws "${AWS_ARGS[@]}" s3api put-bucket-encryption \
    --bucket "$bucket" \
    --server-side-encryption-configuration '{
      "Rules":[{"ApplyServerSideEncryptionByDefault":{"SSEAlgorithm":"AES256"}}]
    }' >/dev/null
}

# 1) Execution roles (always create: dev/test/prod)
ensure_role "cfn-exec-dev"  "${ROOT_DIR}/roles/cfn-trust.json"
ensure_role "cfn-exec-test" "${ROOT_DIR}/roles/cfn-trust.json"
ensure_role "cfn-exec-prod" "${ROOT_DIR}/roles/cfn-trust.json"

# 2) Execution policies (always create: dev/test/prod)
cfn_exec_policy_arn_dev="$(ensure_managed_policy "cfn-exec-dev-policy"  "${ROOT_DIR}/policies/cfn-exec-policy.json" "" "dev")"
cfn_exec_policy_arn_test="$(ensure_managed_policy "cfn-exec-test-policy" "${ROOT_DIR}/policies/cfn-exec-policy.json" "" "test")"
cfn_exec_policy_arn_prod="$(ensure_managed_policy "cfn-exec-prod-policy" "${ROOT_DIR}/policies/cfn-exec-policy.json" "" "prod")"

ensure_attach_role_policy "cfn-exec-dev"  "${cfn_exec_policy_arn_dev}"
ensure_attach_role_policy "cfn-exec-test" "${cfn_exec_policy_arn_test}"
ensure_attach_role_policy "cfn-exec-prod" "${cfn_exec_policy_arn_prod}"

# 3) Dev/Test deployer role + policy (always)
ensure_role "yx-deployer-devtest" "${ROOT_DIR}/roles/deployer-trust-account.json"
deployer_devtest_policy_arn="$(ensure_managed_policy "yx-deploy-devtest-policy" "${ROOT_DIR}/policies/deployer-devtest-policy.json")"
ensure_attach_role_policy "yx-deployer-devtest" "${deployer_devtest_policy_arn}"

# 3b) IoT Core dev/test permissions (CLI + Console)
iot_devtest_policy_arn="$(ensure_managed_policy "yx-iot-devtest-policy" "${ROOT_DIR}/policies/iot-devtest-policy.json")"
ensure_attach_role_policy "yx-deployer-devtest" "${iot_devtest_policy_arn}"
ddb_devtest_policy_arn="$(ensure_managed_policy "yx-ddb-devtest-policy" "${ROOT_DIR}/policies/ddb-devtest-policy.json")"
ensure_attach_role_policy "yx-deployer-devtest" "${ddb_devtest_policy_arn}"
s3_devtest_policy_arn="$(ensure_managed_policy "yx-s3-devtest-policy" "${ROOT_DIR}/policies/s3-devtest-policy.json")"
ensure_attach_role_policy "yx-deployer-devtest" "${s3_devtest_policy_arn}"

# 4) Dev user (always)
ensure_user "${DEV_USER}"
ensure_console_login "${DEV_USER}"
if [[ -n "${USER_POLICY_FILE}" ]]; then
  put_inline_user_policy "${DEV_USER}" "YxCamUserPermissions" "${USER_POLICY_FILE}"
fi
put_inline_user_policy "${DEV_USER}" "AssumeYxDeployerDevTest" "${ROOT_DIR}/policies/assume-deployer-devtest.json"
ensure_role "yx-deployer-devtest" "${ROOT_DIR}/roles/deployer-trust-user.json" "${DEV_USER}"
maybe_create_access_key "${DEV_USER}"

# 5) Prod deployer role + PM user (optional)
if [[ -n "${PM_USER}" ]]; then
  ensure_role "yx-deployer-prod" "${ROOT_DIR}/roles/deployer-trust-account.json"
  deployer_prod_policy_arn="$(ensure_managed_policy "yx-deploy-prod-policy" "${ROOT_DIR}/policies/deployer-prod-policy.json")"
  ensure_attach_role_policy "yx-deployer-prod" "${deployer_prod_policy_arn}"

  ensure_user "${PM_USER}"
  ensure_console_login "${PM_USER}"
  if [[ -n "${USER_POLICY_FILE}" ]]; then
    put_inline_user_policy "${PM_USER}" "YxCamUserPermissions" "${USER_POLICY_FILE}"
  fi
  put_inline_user_policy "${PM_USER}" "AssumeYxDeployerProd" "${ROOT_DIR}/policies/assume-deployer-prod.json"
  ensure_role "yx-deployer-prod" "${ROOT_DIR}/roles/deployer-trust-user.json" "${PM_USER}"
  maybe_create_access_key "${PM_USER}"
else
  log "Skipping prod deployer role + PM user (no --pm-user provided)."
fi

# 6) Create the CloudFormation artifact bucket 
ensure_bucket "${ARTIFACT_BUCKET}" "${REGION}"

log "Done."