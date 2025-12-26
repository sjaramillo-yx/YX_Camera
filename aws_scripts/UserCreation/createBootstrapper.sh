#! /usr/bin/env bash
# --------
# Variables (edit as you like)
# --------

BOOTSTRAP_PROFILE="bootstrapper"
POLICY_NAME="FullBootstrap"
AWS_REGION="sa-east-1"

# Where this script is stored
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Files
CREDS_FILE="${SCRIPT_DIR}/${BOOTSTRAP_PROFILE}Keys.json"
POLICY_DOC_FILE="${SCRIPT_DIR}/${POLICY_NAME}.json"

# ---- Helper: delete user if it exists ----
if aws iam get-user --user-name "${BOOTSTRAP_PROFILE}" >/dev/null 2>&1; then
  echo "User '${BOOTSTRAP_PROFILE}' already exists, deleting it..."

  # Detach managed policies
  for arn in $(aws iam list-attached-user-policies  --user-name "${BOOTSTRAP_PROFILE}" \
                                                    --query 'AttachedPolicies[].PolicyArn' \
                                                    --output text); do
    aws iam detach-user-policy --user-name "${BOOTSTRAP_PROFILE}" --policy-arn "${arn}"
  done

  # Delete inline policies
  for policy in $(aws iam list-user-policies  --user-name "${BOOTSTRAP_PROFILE}" \
                                              --query 'PolicyNames[]' \
                                              --output text); do
    aws iam delete-user-policy --user-name "${BOOTSTRAP_PROFILE}" --policy-name "${policy}"
  done

  # Delete access keys
  for key in $(aws iam list-access-keys --user-name "${BOOTSTRAP_PROFILE}" \
                                        --query 'AccessKeyMetadata[].AccessKeyId' \
                                        --output text); do
    aws iam delete-access-key --user-name "${BOOTSTRAP_PROFILE}" --access-key-id "${key}"
  done

  # Finally delete the user
  aws iam delete-user --user-name "${BOOTSTRAP_PROFILE}"
  echo "User '${BOOTSTRAP_PROFILE}' deleted"
fi


# ---- Helper: delete existing customer-managed policy with same name ----
EXISTING_POLICY_ARN="$(aws iam list-policies  --scope Local \
                                              --query "Policies[?PolicyName=='${POLICY_NAME}'].Arn | [0]" \
                                              --output text)"

if [[ "${EXISTING_POLICY_ARN}" != "None" && -n "${EXISTING_POLICY_ARN}" ]]; then
  echo "Policy '${POLICY_NAME}' already exists, deleting it."

  # Detach from users
  for u in $(aws iam list-entities-for-policy --policy-arn "${EXISTING_POLICY_ARN}" \
                                              --query 'PolicyUsers[].UserName' \
                                              --output text); do
    aws iam detach-user-policy --user-name "${u}" --policy-arn "${EXISTING_POLICY_ARN}"
  done
  # Detach from roles
  for r in $(aws iam list-entities-for-policy --policy-arn "${EXISTING_POLICY_ARN}" \
                                              --query 'PolicyRoles[].RoleName' \
                                              --output text); do
    aws iam detach-role-policy --role-name "${r}" --policy-arn "${EXISTING_POLICY_ARN}"
  done
  # Detach from groups
  for g in $(aws iam list-entities-for-policy --policy-arn "${EXISTING_POLICY_ARN}" \
                                              --query 'PolicyGroups[].GroupName' \
                                              --output text); do
    aws iam detach-group-policy --group-name "${g}" --policy-arn "${EXISTING_POLICY_ARN}"
  done

  # Delete non-default versions first
  for v in $(aws iam list-policy-versions --policy-arn "${EXISTING_POLICY_ARN}" \
                                          --query "Versions[?IsDefaultVersion==\`false\`].VersionId" \
                                          --output text); do
    aws iam delete-policy-version --policy-arn "${EXISTING_POLICY_ARN}" --version-id "${v}"
  done

  aws iam delete-policy --policy-arn "${EXISTING_POLICY_ARN}"
  echo "Policy '${POLICY_NAME}' deleted."
fi

# --------
# 1) Create the IAM user
# --------
aws iam create-user --user-name "${BOOTSTRAP_PROFILE}"

# --------
# 2) Create the customer-managed policy
# --------
POLICY_ARN="$(aws iam create-policy --policy-name "${POLICY_NAME}" \
                                    --policy-document "file://${POLICY_DOC_FILE}" \
                                    --query 'Policy.Arn' \
                                    --output text)"

echo "Created policy with ARN: ${POLICY_ARN}"

# --------
# 3) Attach policy to the bootstrap user
# --------
aws iam attach-user-policy  --user-name "${BOOTSTRAP_PROFILE}" \
                            --policy-arn "${POLICY_ARN}"

# --------
# 4) Create access keys for the bootstrap user (SAVE THE SECRET ONCE)
# --------
aws iam create-access-key --user-name "${BOOTSTRAP_PROFILE}" --output json > "${CREDS_FILE}"

# --------
# 5) Activate the new user
# --------
ACCESS_KEY_ID="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["AccessKey"]["AccessKeyId"])' "${CREDS_FILE}")"
SECRET_ACCESS_KEY="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["AccessKey"]["SecretAccessKey"])' "${CREDS_FILE}")"

aws configure set aws_access_key_id     "${ACCESS_KEY_ID}"     --profile "${BOOTSTRAP_PROFILE}"
aws configure set aws_secret_access_key "${SECRET_ACCESS_KEY}" --profile "${BOOTSTRAP_PROFILE}"
aws configure set region                "${AWS_REGION}"        --profile "${BOOTSTRAP_PROFILE}"

echo "Validating bootstrapper profile '${BOOTSTRAP_PROFILE}'..."
aws sts get-caller-identity --query Account --output text --profile "$BOOTSTRAP_PROFILE" --region "$AWS_REGION"
export BOOTSTRAP_PROFILE
