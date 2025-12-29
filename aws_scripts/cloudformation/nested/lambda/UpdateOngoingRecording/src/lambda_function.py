import os, boto3

ddb = boto3.client("dynamodb")
TABLE = os.environ["TABLE_NAME"]

def _n(x):
    return {"N": str(x)}

def _s(x):
    return {"S": str(x)}

def lambda_handler(event, context):
    pk = event["PK"]
    sk = event["SK"]

    status = event.get("status")
    updated_at = event.get("updatedAt")
    transactionId = event.get("transactionId")

    # Build dynamic UpdateExpression
    set_parts = []
    names = {"#st": "status"}
    values = {}

    def set_attr(name, value_av):
        placeholder = f":{name}"
        set_parts.append(f"{name} = {placeholder}")
        values[placeholder] = value_av

    # Always set these
    if status is not None:
        set_parts.append("#st = :st")
        values[":st"] = _s(status)
    if updated_at is not None:
        set_attr("updatedAt", _n(updated_at))
    if transactionId is not None:
        set_attr("transactionId", _s(transactionId))

    # Always keep GSI1 current
    if event.get("allRecordingsPk") is not None: set_attr("allRecordingsPk", _s(event["allRecordingsPk"]))
    if event.get("allRecordingsSk") is not None: set_attr("allRecordingsSk", _s(event["allRecordingsSk"]))

    # Ongoing sparse index membership:
    if event.get("ongoingRecordingsPk") is not None and event.get("ongoingRecordingsSk") is not None:
        set_attr("ongoingRecordingsPk", _s(event["ongoingRecordingsPk"]))
        set_attr("ongoingRecordingsSk", _s(event["ongoingRecordingsSk"]))
    else:
        remove_parts += ["ongoingRecordingsPk", "ongoingRecordingsSk"]

    # ONGOING fields
    if event.get("currentFPS") is not None:
        set_attr("currentFPS", _n(event["currentFPS"]))
    if event.get("currentBitrate") is not None:
        set_attr("currentBitrate", _n(event["currentBitrate"]))
    if event.get("lengthSec") is not None:
        set_attr("lengthSec", _n(event["lengthSec"]))

    update_expr = []
    if set_parts:
        update_expr.append("SET " + ", ".join(set_parts))

    ddb.update_item(
        TableName=TABLE,
        Key={"PK": _s(pk), "SK": _s(sk)},
        UpdateExpression=" ".join(update_expr),
        ExpressionAttributeNames=names,
        ExpressionAttributeValues=values,
    )

    return {"ok": True}
