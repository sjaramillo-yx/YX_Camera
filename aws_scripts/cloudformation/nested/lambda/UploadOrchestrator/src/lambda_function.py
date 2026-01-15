# Upload Orchestrator Lambda (dynamic part sizing from DDB filesize)
# - Computes part_size & total_parts using filesize found in the camera's item collection
# - Looks for size on META first; else tries SK=Rec#<transactionId> with common attribute names
# - Hardened against stale MPUs (recreate if NoSuchUpload), consistent reads, IoT Data endpoint resolution

import os, json, time, math, logging, boto3
from botocore.exceptions import ClientError
from boto3.dynamodb.types import TypeDeserializer
from decimal import Decimal

log = logging.getLogger()
if not log.handlers:
    logging.basicConfig(level=logging.INFO)
log.setLevel(os.environ.get("LOG_LEVEL", "INFO"))

TABLE = os.environ["TABLE_NAME"]
BUCKET = os.environ["UPLOAD_BUCKET"]
TOPIC_PREFIX = os.environ.get("TOPIC_PREFIX", "yx")
PRESIGN_TTL_SECS = int(os.environ.get("PRESIGN_TTL_SECS", "900"))

# Part-sizing policy (tunable via env)
PART_MIN_MIB     = int(os.environ.get("PART_MIN_MIB", "5"))       # S3 minimum (except last part)
PART_TARGET_MIB  = int(os.environ.get("PART_TARGET_MIB", "8"))    # Aim around this when possible
PART_MAX_PARTS   = int(os.environ.get("PART_MAX_PARTS", "10000")) # S3 absolute limit
PART_MAX_MIB     = int(os.environ.get("PART_MAX_MIB", "512"))     # keep parts reasonable

MiB = 1024 * 1024

ddb = boto3.client("dynamodb")
s3  = boto3.client("s3")
_iot = None
_d  = TypeDeserializer()

def _s(x): return {"S": str(x)}
def _n(x): return {"N": str(int(x))}

def _iot_data():
    global _iot
    if _iot is not None:
        return _iot
    ep = os.getenv("IOT_DATA_ENDPOINT")
    if not ep:
        ep = boto3.client("iot").describe_endpoint(endpointType="iot:Data-ATS")["endpointAddress"]
    if not ep.startswith("https://"): ep = f"https://{ep}"
    _iot = boto3.client("iot-data", endpoint_url=ep)
    log.info("Using IoT Data endpoint: %s", ep)
    return _iot

def publish_cmd(thing, upload_id, payload):
    topic = f"{TOPIC_PREFIX}/uploads/{thing}/{upload_id}/commands"
    _iot_data().publish(topic=topic, qos=1, payload=json.dumps(payload).encode("utf-8"))
    log.info("Published %s to %s", payload.get("op"), topic)

def ddb_to_plain(item): return {k: _d.deserialize(v) for k, v in item.items()} if item else None

def get_meta(pk, sk):
    r = ddb.get_item(TableName=TABLE, Key={"PK": _s(pk), "SK": _s(sk)}, ConsistentRead=True)
    return ddb_to_plain(r.get("Item"))

def part_exists(pk, upid, n):
    resp = ddb.get_item(
        TableName=TABLE,
        Key={"PK": _s(pk), "SK": _s(f"Up#{upid}#Part#{n}")},
        ConsistentRead=True,
    )
    return "Item" in resp

def claim_issue_part(pk, sk, part_n):
    """
    Idempotent claim to issue a presigned URL for part_n.
    Only one caller wins; others skip.
    Allows re-issue if RESEND_PART_MS has elapsed since lastIssuedAt.
    """
    now_ms = int(time.time() * 1000)
    resend_ms = int(os.environ.get("RESEND_PART_MS", "120000"))
    threshold_ms = now_ms - resend_ms
    cond = (
        "(attribute_not_exists(lastIssuedPart) OR lastIssuedPart < :n) "
        "OR (lastIssuedPart = :n AND (attribute_not_exists(lastIssuedAt) OR lastIssuedAt < :threshold))"
    )
    try:
        ddb.update_item(
            TableName=TABLE,
            Key={"PK": _s(pk), "SK": _s(sk)},
            UpdateExpression=("SET lastIssuedPart = :n, lastIssuedAt = :now, "
                              "updatedAt = :now"),
            ConditionExpression=cond,
            ExpressionAttributeValues={
                ":n": _n(part_n),
                ":now": _n(now_ms),
                ":threshold": _n(threshold_ms),
            },
        )
        return True
    except ClientError as e:
        if e.response["Error"].get("Code") == "ConditionalCheckFailedException":
            return False
        raise

def get_recording_size_bytes(pk, recording_id):
    """Try multiple places/attribute names to find the file size (bytes)."""
    # 1) META sometimes stores the size; caller can pass META for a quick check
    # (We leave META check to the caller to avoid double reads.)
    # 2) Try Recording item: SK=Rec#<recording_id>
    sk = f"Rec#{recording_id}"
    log.debug("Getting item with PK=%s and SK=%s",_s(pk), _s(sk))
    r = ddb.get_item(TableName=TABLE, Key={"PK": _s(pk), "SK": _s(sk)}, ConsistentRead=True)
    item = ddb_to_plain(r.get("Item"))
    if not item: 
        log.error("No item with SK \"Rec#%s\"", recording_id)
        return None
    v = item.get("filesize")
    if isinstance(v, Decimal):
        v = int(v)  
    if isinstance(v, int) and v > 0:
        return v
    # handle string-encoded numbers
    if isinstance(v, str):
        try:
            iv = int(v)
            if iv > 0: return iv
        except: 
            pass
    return None

def choose_part_layout(file_size_bytes):
    """Return (part_size_bytes, total_parts) based on policy & constraints."""
    if not file_size_bytes or file_size_bytes <= 0:
        ps = max(PART_MIN_MIB, 1) * MiB
        return ps, 1
    # Start from target size, clamp to min/max
    target = PART_TARGET_MIB * MiB
    min_ps = max(PART_MIN_MIB, 5) * MiB  # ensure >=5 MiB
    max_ps = max(PART_MAX_MIB, PART_TARGET_MIB) * MiB
    ps = max(min_ps, min(target, max_ps))
    parts = math.ceil(file_size_bytes / ps)
    if parts > PART_MAX_PARTS:
        # Increase part size to satisfy max parts constraint
        ps = math.ceil(file_size_bytes / PART_MAX_PARTS)
        # round up to nearest MiB
        ps = int(math.ceil(ps / MiB) * MiB)
        ps = max(ps, min_ps)
        parts = math.ceil(file_size_bytes / ps)
    if parts < 1:
        parts = 1
    return int(ps), int(parts)

def lambda_handler(event, _ctx):
    log.info("Event: %s", json.dumps(event))
    thing  = event["thingName"]
    upid   = event["uploadId"]
    pk, sk = event["PK"], event["SK"]
    op     = (event.get("op") or event.get("deviceStatus") or "").lower()
    now_ms = int(event.get("receivedAt") or time.time()*1000)
    rec_id = event.get("transactionId") or event.get("recording_id")

    log.info("transactionId is %s", rec_id)
    log.info("TABLE=%s", TABLE)
    log.info("boto3 region=%s", boto3.session.Session().region_name)
    log.info("caller=%s", boto3.client("sts").get_caller_identity())

    # waiting_init -> create MPU + META + ACTIVE (idempotent)
    if op == "waiting_init":
        key = f"videos/{thing}/{rec_id}.bin"
        meta = get_meta(pk, sk)

        # Determine filesize
        file_size = None
        # Check META first if present
        if meta:
            for k in ("sizeBytes","filesize","fileSize","file_size","bytes","lengthBytes"):
                v = meta.get(k)
                if isinstance(v, int) and v > 0:
                    file_size = v; break
                if isinstance(v, str):
                    try:
                        iv = int(v); 
                        if iv > 0: file_size = iv; break
                    except: pass
        if file_size is None and rec_id:
            file_size = get_recording_size_bytes(pk, rec_id)
        if file_size:
            log.info("Filesize detected: %d bytes", file_size)

        if meta and meta.get("s3UploadId"):
            log.info("META exists; will (re)send init_info (mpu=...%s)", meta["s3UploadId"][-10:])
            # Optionally refresh part layout if META lacks it
            part_size = meta.get("partSize")
            total_parts = meta.get("totalParts")
            if (not part_size or not total_parts) and file_size:
                ps, tp = choose_part_layout(file_size)
                ddb.update_item(TableName=TABLE, Key={"PK":_s(pk),"SK":_s(sk)},
                    UpdateExpression="SET partSize=:ps, totalParts=:tp, updatedAt=:u",
                    ExpressionAttributeValues={":ps":_n(ps),":tp":_n(tp),":u":_n(now_ms)})
                part_size, total_parts = ps, tp
        else:
            log.info("MPU:create bucket=%s key=%s", BUCKET, key)
            s3_id = s3.create_multipart_upload(Bucket=BUCKET, Key=key)["UploadId"]
            log.info("MPU:created uploadId=...%s", s3_id[-10:])
            # Compute part layout
            if file_size:
                ps, tp = choose_part_layout(file_size)
            else:
                # Fall back to provided hints or defaults
                ps = int(event.get("part_size") or 5*MiB)
                tp = int(event.get("total_parts") or 1)
            ddb.transact_write_items(TransactItems=[
                {"Put": {"TableName": TABLE, "Item": {
                    "PK": _s(pk), "SK": _s(sk),
                    "uploadId": _s(upid), "transactionId": _s(rec_id),
                    "thingName": _s(thing),
                    "s3UploadId": _s(s3_id),
                    "status": _s("initialized"),
                    "startedAt": _n(now_ms), "updatedAt": _n(now_ms),
                    "partSize": _n(ps), "totalParts": _n(tp),
                    "nextPart": _n(1), "partsDone": _n(0),
                    # store discovered filesize if we have it
                    **({"sizeBytes": _n(file_size)} if file_size else {})
                }, "ConditionExpression": "attribute_not_exists(PK) AND attribute_not_exists(SK)"} },
                {"Put": {"TableName": TABLE, "Item": {
                    "PK": _s(pk), "SK": _s("Up#ACTIVE"),
                    "uploadId": _s(upid), "since": _n(now_ms)
                } } }
            ])
            part_size, total_parts = ps, tp

        # Always (re)publish init_info with computed values
        if not file_size:
            log.info("Filesize unknown; using part_size=%s total_parts=%s", part_size, total_parts)
        publish_cmd(thing, upid, {
            "op":"init_info","bucket":BUCKET,"key":key,
            "part_size": int(part_size), "total_parts": int(total_parts)
        })
        ddb.update_item(TableName=TABLE, Key={"PK":_s(pk),"SK":_s(sk)},
            UpdateExpression="SET #st=:st,lastCommand=:lc,updatedAt=:u",
            ExpressionAttributeNames={"#st":"status"},
            ExpressionAttributeValues={":st":_s("sending_parts"),
                ":lc":{"M":{"op":_s("init_info")}}, ":u":_n(now_ms)})
        return {"ok": True}

    # ready_for_parts -> verify MPU exists, presign next
    if op == "ready_for_parts":
        key = f"videos/{thing}/{rec_id}.bin"
        meta = get_meta(pk, sk)
        if not meta or not meta.get("s3UploadId"):
            log.warning("META/s3UploadId missing; request re-init (thing=%s upid=%s)", thing, upid)
            publish_cmd(thing, upid, {"op":"init_info_needed"})
            return {"retry": True}

        s3_id = meta["s3UploadId"]
        try:
            s3.list_parts(Bucket=BUCKET, Key=key, UploadId=s3_id, MaxParts=1)
        except ClientError as e:
            code = e.response["Error"].get("Code", "")
            if code in ("NoSuchUpload","NoSuchEntity"):
                log.warning("MPU missing; recreating (thing=%s upid=%s old=...%s)", thing, upid, s3_id[-10:])
                new_id = s3.create_multipart_upload(Bucket=BUCKET, Key=key)["UploadId"]
                log.info("MPU:recreated uploadId=...%s", new_id[-10:])
                # If META has filesize but no layout, compute now
                file_size = None
                for k in ("sizeBytes","filesize","fileSize","file_size","bytes","lengthBytes"):
                    v = meta.get(k)
                    if isinstance(v, int) and v > 0: file_size = v; break
                if file_size is None and (event.get("transactionId") or event.get("recording_id")):
                    rid = event.get("transactionId") or event.get("recording_id")
                    file_size = get_recording_size_bytes(pk, rid)
                if file_size:
                    ps, tp = choose_part_layout(file_size)
                    ddb.update_item(TableName=TABLE, Key={"PK":_s(pk),"SK":_s(sk)},
                        UpdateExpression="SET s3UploadId=:u, partSize=:ps, totalParts=:tp, nextPart=:n, partsDone=:z, updatedAt=:t",
                        ExpressionAttributeValues={":u":_s(new_id), ":ps":_n(ps), ":tp":_n(tp),
                                                   ":n":_n(1), ":z":_n(0), ":t":_n(int(time.time()*1000))})
                    publish_cmd(thing, upid, {"op":"init_info","bucket":BUCKET,"key":key,
                                              "part_size": ps, "total_parts": tp})
                    return {"recreated": True}
                else:
                    ddb.update_item(TableName=TABLE, Key={"PK":_s(pk),"SK":_s(sk)},
                        UpdateExpression="SET s3UploadId=:u, nextPart=:n, partsDone=:z, updatedAt=:t",
                        ExpressionAttributeValues={":u":_s(new_id), ":n":_n(1), ":z":_n(0), ":t":_n(int(time.time()*1000))})
                    publish_cmd(thing, upid, {"op":"init_info","bucket":BUCKET,"key":key,
                                              "part_size": int(meta.get('partSize', 5*MiB)),
                                              "total_parts": int(meta.get('totalParts', 1))})
                    return {"recreated": True}
            else:
                raise

        next_part = int(meta.get("nextPart", 1))
        if part_exists(pk, upid, next_part):
            log.info("Part %d already exists; skip presign", next_part)
            return {"ok": True, "skipped": next_part}
        if not claim_issue_part(pk, sk, next_part):
            log.info("Dedup: part %d already issued recently; skip", next_part)
            return {"ok": True, "dedup": next_part}
        log.info("MPU:presign part=%d uploadId=...%s key=%s", next_part, s3_id[-10:], key)
        url = s3.generate_presigned_url("upload_part",
               Params={"Bucket":BUCKET,"Key":key,"UploadId":s3_id,"PartNumber":next_part},
               ExpiresIn=PRESIGN_TTL_SECS)
        publish_cmd(thing, upid, {"op":"part","part_number":next_part,"url":url})
        ddb.update_item(TableName=TABLE, Key={"PK":_s(pk),"SK":_s(sk)},
            UpdateExpression="SET lastCommand=:lc, updatedAt=:u",
            ExpressionAttributeValues={":lc":{"M":{"op":_s("part"),"part_number":_n(next_part)}},":u":_n(now_ms)})
        return {"ok": True}

    # part_uploaded -> persist part + bump counters
    if op == "part_uploaded" and ("partNumber" in event or "part_number" in event) and ("etag" in event):
        n = int(event.get("partNumber") or event.get("part_number"))
        etag = event["etag"]
        log.info("PART_UPLOADED: part=%d etag=%s", n, etag)
        # 1) Atomically persist the part and bump counters
        ddb.transact_write_items(TransactItems=[
            {"Put": {"TableName":TABLE,
                "Item":{"PK":_s(pk),"SK":_s(f"Up#{upid}#Part#{n}"),
                        "uploadId":_s(upid),"partNumber":_n(n),"etag":_s(etag),"uploadedAt":_n(now_ms)}}},
            {"Update":{"TableName":TABLE,"Key":{"PK":_s(pk),"SK":_s(sk)},
                "UpdateExpression":"SET partsDone = if_not_exists(partsDone,:z)+:one, nextPart = :np, updatedAt=:u",
                "ExpressionAttributeValues":{":z":_n(0),":one":_n(1),":np":_n(n+1),":u":_n(now_ms)}}}
        ])
        # 2) Fetch META to decide whether to send the next part
        meta_after = get_meta(pk, sk)
        if not meta_after or not meta_after.get("s3UploadId"):
            log.warning("META missing after part persist; asking device to re-init")
            publish_cmd(thing, upid, {"op":"init_info_needed"})
            return {"ok": True, "next": "reinit"}
        total_parts = int(meta_after.get("totalParts", 1))
        next_part   = int(meta_after.get("nextPart", n+1))
        key         = f"videos/{thing}/{rec_id}.bin"
        s3_id       = meta_after["s3UploadId"]
        log.info("PART_STATE: partsDone=%s nextPart=%s totalParts=%s", meta_after.get("partsDone"), next_part, total_parts)
        # 3) If more parts remain, presign and send next URL; otherwise wait for all_parts_uploaded
        if next_part <= total_parts:
            if part_exists(pk, upid, next_part):
                log.info("Part %d already exists after persist; skip presign", next_part)
                return {"ok": True, "skipped": next_part}
            if not claim_issue_part(pk, sk, next_part):
                log.info("Dedup: part %d already issued recently (post-upload); skip", next_part)
                return {"ok": True, "dedup": next_part}
            url = s3.generate_presigned_url("upload_part",
                   Params={"Bucket":BUCKET,"Key":key,"UploadId":s3_id,"PartNumber":next_part},
                   ExpiresIn=PRESIGN_TTL_SECS)
            publish_cmd(thing, upid, {"op":"part","part_number":next_part,"url":url})
            ddb.update_item(TableName=TABLE, Key={"PK":_s(pk),"SK":_s(sk)},
                UpdateExpression="SET lastCommand=:lc, updatedAt=:u",
                ExpressionAttributeValues={":lc":{"M":{"op":_s("part"),"part_number":_n(next_part)}},":u":_n(now_ms)})
            return {"ok": True, "next": next_part}
        else:
            log.info("All parts uploaded according to counters; waiting for device to publish all_parts_uploaded")
            return {"ok": True, "next": "await_complete"}

    # all_parts_uploaded -> complete MPU
    if op == "all_parts_uploaded":
        meta = get_meta(pk, sk)
        if not meta or not meta.get("s3UploadId"):
            log.error("META missing; cannot complete MPU (thing=%s upid=%s)", thing, upid)
            return {"ignored": True, "reason": "meta_missing"}
        s3_id  = meta["s3UploadId"]
        key = f"videos/{thing}/{rec_id}.bin"
        parts  = event.get("parts") or []
        ddb.update_item(TableName=TABLE, Key={"PK":_s(pk),"SK":_s(sk)},
                        UpdateExpression="SET #st=:st, updatedAt=:u",
                        ExpressionAttributeNames={"#st":"status"},
                        ExpressionAttributeValues={":st":_s("completing"),":u":_n(now_ms)})
        formatted = [{"PartNumber": int(p["part_number"]), "ETag": p["etag"].strip('"')}
                                                                        for p in parts
        ]
        formatted.sort(key=lambda x: x["PartNumber"])
        s3.complete_multipart_upload(Bucket=BUCKET, Key=key, UploadId=s3_id,
                                     MultipartUpload={"Parts": formatted})
        ddb.update_item(TableName=TABLE, Key={"PK":_s(pk),"SK":_s(sk)},
                        UpdateExpression="SET #st=:st, completedAt=:c REMOVE lastCommand",
                        ExpressionAttributeNames={"#st":"status"},
                        ExpressionAttributeValues={":st":_s("done"),":c":_n(now_ms)})
        ddb.delete_item(TableName=TABLE, Key={"PK":_s(pk), "SK":_s("Up#ACTIVE")})
        return {"ok": True}

    # error -> mark + abort MPU if known
    if op == "error":
        meta = get_meta(pk, sk)
        if meta and meta.get("s3UploadId"):
            key = f"videos/{thing}/{rec_id}.bin"
            s3_id = meta["s3UploadId"]
            try:
                s3.abort_multipart_upload(Bucket=BUCKET, Key=key, UploadId=s3_id)
            except ClientError as e:
                log.warning("AbortMultipartUpload failed (maybe already gone): %s", e)
        ddb.update_item(TableName=TABLE, Key={"PK":_s(pk),"SK":_s(sk)},
          UpdateExpression="SET #st=:st, lastErrorCode=:ec, lastErrorMessage=:em, updatedAt=:u",
          ExpressionAttributeNames={"#st":"status"},
          ExpressionAttributeValues={":st":_s("error"),
                                     ":ec":_s(event.get("errorCode") or ""),
                                     ":em":_s(event.get("errorMessage") or ""),
                                     ":u":_n(now_ms)})
        ddb.delete_item(TableName=TABLE, Key={"PK":_s(pk), "SK":_s("Up#ACTIVE")})
        return {"ok": True}

    log.info("Ignored op=%s", op)
    return {"ignored": True, "op": op}