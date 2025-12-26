#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROFILE="${BOOTSTRAP_PROFILE:-"default"}"
REGION="$(aws configure get region --profile "$PROFILE")"
REGION="${REGION:-sa-east-1}"


CAMERA_POLICY_NAME="${CAMERA_POLICY_NAME:-cameraPolicy}"
PROVISIONING_POLICY_NAME="${PROVISIONING_POLICY_NAME:-provisioningPolicy}"
TEMPLATE_NAME="${TEMPLATE_NAME:-cameraTemplate}"

CAMERA_POLICY_FILE="${CAMERA_POLICY_FILE:-${SCRIPT_DIR}/cameraPolicy.json}"
PROVISIONING_POLICY_FILE="${PROVISIONING_POLICY_FILE:-${SCRIPT_DIR}/provisioningPolicy.json}"
TEMPLATE_FILE="${TEMPLATE_FILE:-${SCRIPT_DIR}/cameraTemplate.json}"

ROLE_NAME="yx-IoTProvisioningRole"
ROLE_FILE="${SCRIPT_DIR}/provisionerRole.json"
AWSIOT_THINGS_REG_POLICY_ARN="arn:aws:iam::aws:policy/service-role/AWSIoTThingsRegistration"

require_file() {
  local f="$1"
  [[ -f "$f" ]] || { echo "ERROR: missing file: $f"; exit 1; }
}

ensure_room_for_policy_version() {
  # IoT policies can have max 5 versions; delete oldest non-default if we already have 4 non-defaults
  local name="$1"
  local non_default_count oldest_non_default

  non_default_count="$(aws --profile "${PROFILE}" iot list-policy-versions \
    --policy-name "$name" \
    --query 'length(policyVersions[?isDefaultVersion==`false`])' \
    --output text)"

  if [[ "$non_default_count" -ge 4 ]]; then
    oldest_non_default="$(aws --profile "${PROFILE}" iot list-policy-versions \
      --policy-name "$name" \
      --query 'sort_by(policyVersions[?isDefaultVersion==`false`], &versionId)[0].versionId' \
      --output text)"
    if [[ -n "$oldest_non_default" && "$oldest_non_default" != "None" ]]; then
      aws --profile "${PROFILE}" iot delete-policy-version --policy-name "$name" --policy-version-id "$oldest_non_default"
    fi
  fi
}

upsert_iot_policy() {
  local name="$1"
  local file="$2"

  if aws --profile "${PROFILE}" iot get-policy --policy-name "$name" >/dev/null 2>&1; then
    echo "Updating IoT policy (new version): $name"
    ensure_room_for_policy_version "$name"
    aws --profile "${PROFILE}" iot create-policy-version \
      --policy-name "$name" \
      --policy-document "file://$file" \
      --set-as-default >/dev/null
  else
    echo "Creating IoT policy: $name"
    aws --profile "${PROFILE}" iot create-policy \
      --policy-name "$name" \
      --policy-document "file://$file" >/dev/null
  fi
}

ensure_room_for_template_version() {
  # Provisioning templates can have max 5 versions; delete an older non-default if we're at the limit
  local template_name="$1"
  local non_default_count oldest_non_default

  non_default_count="$(aws --profile "${PROFILE}" iot list-provisioning-template-versions \
    --template-name "$template_name" \
    --query 'length(versions[?isDefaultVersion==`false`])' \
    --output text)"

  if [[ "$non_default_count" -ge 4 ]]; then
    oldest_non_default="$(aws --profile "${PROFILE}" iot list-provisioning-template-versions \
      --template-name "$template_name" \
      --query 'sort_by(versions[?isDefaultVersion==`false`], &versionId)[0].versionId' \
      --output text)"

    if [[ -n "$oldest_non_default" && "$oldest_non_default" != "None" ]]; then
      aws --profile "${PROFILE}" iot delete-provisioning-template-version \
        --template-name "$template_name" \
        --version-id "$oldest_non_default"
    fi
  fi
}

upsert_provisioning_template() {
  local name="$1"
  local file="$2"

  if aws --profile "${PROFILE}" iot describe-provisioning-template --template-name "$name" >/dev/null 2>&1; then
    echo "Updating provisioning template (new version): $name"
    ensure_room_for_template_version "$name"
    aws --profile "${PROFILE}" iot create-provisioning-template-version \
      --template-name "$name" \
      --template-body "file://$file" \
      --set-as-default >/dev/null
  else
    echo "Creating provisioning template: $name"
    aws --profile "${PROFILE}" iot create-provisioning-template \
      --template-name "$name" \
      --provisioning-role-arn "$PROVISIONING_ROLE_ARN" \
      --template-body "file://$file" \
      --enabled \
      --type FLEET_PROVISIONING >/dev/null
  fi
}

# ---- Validate inputs ----
require_file "$CAMERA_POLICY_FILE"
require_file "$PROVISIONING_POLICY_FILE"
require_file "$TEMPLATE_FILE"

# ---- Run ----
echo "Using profile=$PROFILE region=$REGION"
aws --profile "${PROFILE}" sts get-caller-identity >/dev/null

# ---- Create or update the role ----
if aws --profile "${PROFILE}" iam get-role --role-name "$ROLE_NAME" >/dev/null 2>&1; then
  echo "Role exists, updating trust policy: $ROLE_NAME"
  aws --profile "${PROFILE}" iam update-assume-role-policy \
    --role-name "$ROLE_NAME" \
    --policy-document "file://$ROLE_FILE" >/dev/null
else
  echo "Creating role: $ROLE_NAME"
  aws --profile "${PROFILE}" iam create-role \
    --role-name "$ROLE_NAME" \
    --assume-role-policy-document "file://$ROLE_FILE" \
    --description "AWS IoT fleet provisioning role" >/dev/null
fi

# ---- Attach AWSIoTThingsRegistration if not already attached ----
if aws --profile "${PROFILE}" iam list-attached-role-policies --role-name "$ROLE_NAME" \
  --query "AttachedPolicies[?PolicyArn=='$AWSIOT_THINGS_REG_POLICY_ARN'] | length(@)" \
  --output text | grep -qx "0"; then
  echo "Attaching managed policy: AWSIoTThingsRegistration"
  aws --profile "${PROFILE}" iam attach-role-policy \
    --role-name "$ROLE_NAME" \
    --policy-arn "$AWSIOT_THINGS_REG_POLICY_ARN"
else
  echo "Managed policy already attached: AWSIoTThingsRegistration"
fi

PROVISIONING_ROLE_ARN="$(aws --profile "${PROFILE}" iam get-role --role-name "$ROLE_NAME" --query 'Role.Arn' --output text)"
echo "Provisioning role ARN: $PROVISIONING_ROLE_ARN"

upsert_iot_policy "$CAMERA_POLICY_NAME" "$CAMERA_POLICY_FILE"
upsert_iot_policy "$PROVISIONING_POLICY_NAME" "$PROVISIONING_POLICY_FILE"
upsert_provisioning_template "$TEMPLATE_NAME" "$TEMPLATE_FILE"

OUT_DIR="${OUT_DIR:-$SCRIPT_DIR/certs}"
mkdir -p "$OUT_DIR"
chmod 700 "$OUT_DIR"

CERT_PEM="$OUT_DIR/provisioning.cert.pem"
PUB_KEY="$OUT_DIR/provisioning.public.key"
PRIV_KEY="$OUT_DIR/provisioning.private.key"
META_JSON="$OUT_DIR/provisioning.cert.meta.json"

# 1) Create cert + keys (and activate it)
aws iot create-keys-and-certificate \
  --set-as-active \
  --certificate-pem-outfile "$CERT_PEM" \
  --public-key-outfile "$PUB_KEY" \
  --private-key-outfile "$PRIV_KEY" \
  --query '{certificateArn:certificateArn, certificateId:certificateId}' \
  --output json \
  --profile "$PROFILE" \
  --region "$REGION" | tee "$META_JSON" >/dev/null

CERT_ARN="$(python3 -c 'import json; print(json.load(open("'"$META_JSON"'"))["certificateArn"])')"
CERT_ID="$(python3 -c 'import json; print(json.load(open("'"$META_JSON"'"))["certificateId"])')"

echo "Created cert:"
echo "  ARN: $CERT_ARN"
echo "  ID : $CERT_ID"

# 2) Attach the provisioning policy to the certificate
aws iot attach-policy \
  --policy-name "$PROVISIONING_POLICY_NAME" \
  --target "$CERT_ARN" \
  --profile "$PROFILE" \
  --region "$REGION"

echo "Attached policy '$PROVISIONING_POLICY_NAME' to certificate."

echo "Done."
echo "Policies: $CAMERA_POLICY_NAME, $PROVISIONING_POLICY_NAME"
echo "Template: $TEMPLATE_NAME"

