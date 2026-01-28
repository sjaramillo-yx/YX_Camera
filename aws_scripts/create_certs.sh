#!/usr/bin/env bash
set -euo pipefail

# Usage: ./script.sh <StackName> <Environment>
if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <StackName> <Environment>"
  exit 1
fi

StackName="$1"
Environment="$2"
Region="us-east-1"
FullStackName="${StackName}-${Environment}"

OUT="./AWSCertificates"
mkdir -p "$OUT"

echo "Creating device certificate"

DEVICE_CERT_ARN="$(
  aws iot create-keys-and-certificate \
    --set-as-active \
    --certificate-pem-outfile "$OUT/device.cert.pem" \
    --private-key-outfile     "$OUT/device.private.key" \
    --public-key-outfile      "$OUT/device.public.key" \
    --query 'certificateArn' \
    --output text
)"

curl -o "${OUT}/AmazonRootCA1.pem" https://www.amazontrust.com/repository/AmazonRootCA1.pem

echo "Attaching policy ${StackName}-provisioningPolicy-${Environment}"

aws iot attach-policy \
  --policy-name "${StackName}-provisioningPolicy-${Environment}" \
  --target "$DEVICE_CERT_ARN"

echo "Getting CodeSigningCertificateArn from stack ${FullStackName}"

CODESIGN_CERT_ARN="$(
  aws cloudformation describe-stacks \
    --stack-name "${FullStackName}" \
    --region "$Region" \
    --query "Stacks[0].Outputs[?OutputKey=='CodeSigningCertificateArn'].OutputValue | [0]" \
    --output text
)"

if [[ -z "${CODESIGN_CERT_ARN}" || "${CODESIGN_CERT_ARN}" == "None" ]]; then
  echo "ERROR: Output 'CodeSigningCertificateArn' not found on stack ${FullStackName} in region ${Region}"
  exit 2
fi

echo "Extracting OTA code-signing public key to $OUT/ota_sign.public.key"

aws acm get-certificate --certificate-arn "$CODESIGN_CERT_ARN" --region "$Region" \
  --query Certificate --output text \
| openssl x509 -pubkey -noout \
> "$OUT/ota_sign.public.key"

echo "Getting IoT Core Data-ATS endpoint via describe-endpoint"

IOT_ENDPOINT="$(
  aws iot describe-endpoint \
    --endpoint-type iot:Data-ATS \
    --region "$Region" \
    --query endpointAddress \
    --output text
)"

echo "$IOT_ENDPOINT" | tee "$OUT/iot_endpoint.txt"

echo "Done."
