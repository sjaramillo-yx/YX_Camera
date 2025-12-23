#!/usr/bin/env bash
# Get the region
PROFILE="${BOOTSTRAP_PROFILE:-"default"}"
REGION="$(aws configure get region --profile "$PROFILE")"
REGION="${REGION:-sa-east-1}"

# Build the bucket na,e
ACCOUNT_ID="$(aws sts get-caller-identity --query Account --output text --profile "$PROFILE" --region "$REGION")"
BUCKET="yx-${ACCOUNT_ID}-cam-data"

echo "Using profile=$PROFILE region=$REGION"
echo "Creating bucket with name "$BUCKET""

# Create the bucket
aws s3api create-bucket \
    --bucket "$BUCKET" \
    --profile "$PROFILE" \
    --region "$REGION" \
    --create-bucket-configuration LocationConstraint="$REGION" \
    --output text

aws s3api put-object --bucket "$BUCKET" --key "videos/"   --profile "$PROFILE" --region "$REGION" > /dev/null
aws s3api put-object --bucket "$BUCKET" --key "logs/"     --profile "$PROFILE" --region "$REGION" > /dev/null
aws s3api put-object --bucket "$BUCKET" --key "firmware/" --profile "$PROFILE" --region "$REGION" > /dev/null

# Allow versioning for OTA updates
aws s3api put-bucket-versioning \
  --bucket "$BUCKET" \
  --versioning-configuration Status=Enabled \
  --profile "$PROFILE" \
  --region "$REGION"
