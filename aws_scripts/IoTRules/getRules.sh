set -euo pipefail
AWS_PAGER=""
REGION="sa-east-1"      # <-- change
PROFILE="default"       # <-- change

mkdir -p iot-rules

aws --profile "$PROFILE" --region "$REGION" iot list-topic-rules \
  --query 'rules[].ruleName' --output text \
| tr '\t' '\n' \
| while IFS= read -r r; do
    [ -z "$r" ] && continue
    echo "Exporting $r"
    if ! aws --profile "$PROFILE" --region "$REGION" iot get-topic-rule \
        --rule-name "$r" --output json > "iot-rules/${r}.full.json"; then
      echo "FAILED to export $r" >&2
      continue
    fi

    # Extract payload into a separate file (will error if missing)
    aws --profile "$PROFILE" --region "$REGION" iot get-topic-rule \
      --rule-name "$r" --query 'topicRulePayload' --output json \
      > "iot-rules/${r}.payload.json"
  done
