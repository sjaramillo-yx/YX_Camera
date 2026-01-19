import os
import boto3

ddb = boto3.client("dynamodb")
TABLE = os.environ["TABLE_NAME"]


def _n(x):
    # DynamoDB expects numbers as strings
    return {"N": str(x)}


def _s(x):
    return {"S": str(x)}


def _b(x: bool):
    return {"BOOL": bool(x)}


def _update_item(pk: str, sk: str, set_attrs: dict, remove_attrs: list):
    """Apply a partial update to a single DynamoDB item."""
    expr_parts = []
    ean = {}
    eav = {}

    # Build SET
    if set_attrs:
        set_expr = []
        for attr_name, av in set_attrs.items():
            name_token = f"#a{len(ean) + 1}"
            value_token = f":v{len(eav) + 1}"
            ean[name_token] = attr_name
            eav[value_token] = av
            set_expr.append(f"{name_token} = {value_token}")
        expr_parts.append("SET " + ", ".join(set_expr))

    # Build REMOVE
    if remove_attrs:
        rem_expr = []
        for attr_name in remove_attrs:
            name_token = f"#r{len(ean) + 1}"
            ean[name_token] = attr_name
            rem_expr.append(name_token)
        expr_parts.append("REMOVE " + ", ".join(rem_expr))

    if not expr_parts:
        return

    ddb.update_item(
        TableName=TABLE,
        Key={"PK": _s(pk), "SK": _s(sk)},
        UpdateExpression=" ".join(expr_parts),
        ExpressionAttributeNames=ean if ean else None,
        ExpressionAttributeValues=eav if eav else None,
    )


def lambda_handler(event, context):
    # Primary update: the row specified by PK/SK in the event
    pk = event["PK"]
    sk = event["SK"]

    set_attrs = {}
    remove_attrs = []

    # Fields that may be present
    if event.get("status") is not None:
        set_attrs["status"] = _s(event["status"])
    if event.get("updatedAt") is not None:
        set_attrs["updatedAt"] = _n(event["updatedAt"])
    if event.get("transactionId") is not None:
        set_attrs["transactionId"] = _s(event["transactionId"])
    if event.get("filename") is not None:
        set_attrs["filename"] = _s(event["filename"])
    if event.get("filesize") is not None:
        set_attrs["filesize"] = _s(event["filesize"])

    # Keep GSI1 current (if provided)
    if event.get("allRecordingsPk") is not None:
        set_attrs["allRecordingsPk"] = _s(event["allRecordingsPk"])
    if event.get("allRecordingsSk") is not None:
        set_attrs["allRecordingsSk"] = _s(event["allRecordingsSk"])

    # Ongoing sparse index membership:
    if event.get("ongoingRecordingsPk") is not None and event.get("ongoingRecordingsSk") is not None:
        set_attrs["ongoingRecordingsPk"] = _s(event["ongoingRecordingsPk"])
        set_attrs["ongoingRecordingsSk"] = _s(event["ongoingRecordingsSk"])
    else:
        # Remove the sparse index attributes to drop membership
        remove_attrs += ["ongoingRecordingsPk", "ongoingRecordingsSk"]

    # ONGOING fields
    if event.get("currentFPS") is not None and event.get("currentBitrate") is not None:
        set_attrs["currentFPS"] = _n(event["currentFPS"])
        set_attrs["currentBitrate"] = _n(event["currentBitrate"])
    else:
        remove_attrs += ["currentBitrate", "currentFPS"]
    
    # Recording length
    if event.get("lengthSec") is not None:
        set_attrs["lengthSec"] = _n(event["lengthSec"])

    # TARGET fields (persist once; keep forever unless explicitly changed)
    if event.get("targetFps") is not None:
        set_attrs["targetFps"] = _n(event["targetFps"])
    if event.get("targetBitrate") is not None:
        set_attrs["targetBitrate"] = _n(event["targetBitrate"])

    _update_item(pk, sk, set_attrs=set_attrs, remove_attrs=remove_attrs)

    return {"ok": True}