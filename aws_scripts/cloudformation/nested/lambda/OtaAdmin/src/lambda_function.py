import os
import time
import uuid
import json
import boto3
import botocore.exceptions

iot = boto3.client("iot")
s3 = boto3.client("s3")
sts = boto3.client("sts")

CAMERA_BUCKET = os.environ["CAMERA_BUCKET"]
OTA_SERVICE_ROLE_ARN = os.environ["OTA_SERVICE_ROLE_ARN"]
SIGNING_PROFILE_NAME = os.environ["SIGNING_PROFILE_NAME"]
UNSIGNED_PREFIX = os.environ.get("UNSIGNED_PREFIX", "firmware/unsigned/")
SIGNED_PREFIX = os.environ.get("SIGNED_PREFIX", "firmware/signed/")

def _args(event: dict) -> dict:
    # Supports AppSync direct resolver shape (event.arguments) and "raw args" shape.
    if isinstance(event, dict) and isinstance(event.get("arguments"), dict):
        return event["arguments"]
    return event or {}

def object_exists(bucket: str, key: str) -> bool:
    try:
        s3.head_object(Bucket=bucket, Key=key)
        return True
    except botocore.exceptions.ClientError as e:
        code = e.response.get("Error", {}).get("Code")
        if code in ("404", "NoSuchKey", "NotFound"):
            return False
        if code in ("403", "AccessDenied"):
            print(f"WARNING: no access to head signed key {key}; will re-sign")
            return False
        raise

def latest_version_id(bucket: str, key: str) -> str:
    head = s3.head_object(Bucket=bucket, Key=key)
    vid = head.get("VersionId")
    if vid and vid != "null":
        return vid

    resp = s3.list_object_versions(Bucket=bucket, Prefix=key, MaxKeys=50)
    versions = resp.get("Versions", [])
    candidates = [v for v in versions if v.get("Key") == key and v.get("IsLatest")]
    if candidates:
        return candidates[0]["VersionId"]

    raise RuntimeError(
        f"Cannot determine VersionId for s3://{bucket}/{key}. "
        "Ensure the object exists and bucket versioning is enabled."
    )

def get_ota_status(ota_update_id: str) -> dict:
    resp = iot.get_ota_update(otaUpdateId=ota_update_id)
    info = resp.get("otaUpdateInfo", {}) or {}
    return info

def lambda_handler(event, context):
    a = _args(event)

    # ---- Verbose context ----
    region = os.environ.get("AWS_REGION", "unknown")
    caller = sts.get_caller_identity()
    print("=== OTA Mutation Lambda invoked ===")
    print("aws_region:", region)
    print("caller_account:", caller.get("Account"))
    print("caller_arn:", caller.get("Arn"))
    print("env CAMERA_BUCKET:", CAMERA_BUCKET)
    print("env OTA_SERVICE_ROLE_ARN:", OTA_SERVICE_ROLE_ARN)
    print("env SIGNING_PROFILE_NAME:", SIGNING_PROFILE_NAME)
    print("raw_event_keys:", list(event.keys()) if isinstance(event, dict) else type(event))
    print("args:", json.dumps(a, default=str))

    thing_names = a.get("thingNames") or []
    if not isinstance(thing_names, list) or not thing_names:
        raise ValueError("thingNames must be a non-empty list")

    # Build the S3 key from camera model and firmware version
    s3_key = f"firmware/{a["cameraModel"]}/unsigned/{a["firmwareVersion"]}.bin"

    # Currently the API never sends fileNameOnDevice, this could be used in the future
    file_name_on_device = a.get("fileNameOnDevice", "firmware.bin")

    # Optional: allow the client to request polling for completion
    wait_for_create = bool(a.get("waitForOtaCreate", False))
    wait_seconds = int(a.get("waitSeconds", 30))
    poll_interval = float(a.get("pollIntervalSeconds", 2.0))

    account_id = caller["Account"]
    targets = [f"arn:aws:iot:{region}:{account_id}:thing/{name}" for name in thing_names]

    # Resolve S3 VersionId (important for versioned buckets)
    unsigned_vid = latest_version_id(CAMERA_BUCKET, s3_key)
    print("resolved_s3_version_id:", unsigned_vid)

    signed_prefix = f"firmware/{a["cameraModel"]}/signed/"
    signed_key = signed_prefix + f"{a["firmwareVersion"]}-{unsigned_vid}.bin"
    print("resolved signed_key:", signed_key);

    # Check if signed version already exists
    reuse_signed = object_exists(CAMERA_BUCKET, signed_key)

    # Unique OTA ID
    ota_id = a.get("otaUpdateId") or f"AFR_OTA-{int(time.time())}-{uuid.uuid4().hex[:8]}"

    try:
        if (reuse_signed):
            signed_vid = latest_version_id(CAMERA_BUCKET, signed_key)
            resp = iot.create_ota_update(
                otaUpdateId=ota_id,
                targets=targets,
                targetSelection="SNAPSHOT",
                protocols=["MQTT"],
                roleArn=OTA_SERVICE_ROLE_ARN,
                files=[
                    {
                        "fileName": file_name_on_device,
                        "fileLocation": {
                            "s3Location": {
                                "bucket": CAMERA_BUCKET,
                                "key": signed_key,
                                "version": signed_vid,
                            }
                        },
                    }
                ],
            )
        else:
            resp = iot.create_ota_update(
                otaUpdateId=ota_id,
                targets=targets,
                targetSelection="SNAPSHOT",
                protocols=["MQTT"],
                roleArn=OTA_SERVICE_ROLE_ARN,
                files=[
                    {
                        "fileName": file_name_on_device,
                        "fileLocation": {
                            "s3Location": {"bucket": CAMERA_BUCKET, "key": s3_key, "version": unsigned_vid}
                        },
                        "codeSigning": {
                            "startSigningJobParameter": {
                                "signingProfileName": SIGNING_PROFILE_NAME,
                                "destination": {
                                    "s3Destination": {"bucket": CAMERA_BUCKET, "prefix": signed_prefix}
                                },
                            }
                        },
                    }
                ],
            )

        print("create_ota_update_response:", json.dumps(resp, default=str))

    except botocore.exceptions.ClientError as e:
        # This is where you'd see PassRole / permission issues, bad params, etc.
        print("ERROR create_ota_update ClientError:", e.response)
        raise

    result = {
        "otaUpdateId": resp["otaUpdateId"],
        "targets": thing_names,
        "targetArns": targets,
        "reusedSigned": reuse_signed,
        "unsigned": {"bucket": CAMERA_BUCKET, "key": s3_key, "version": unsigned_vid},
        "signed":   {"bucket": CAMERA_BUCKET, "key": signed_key},
        "otaUpdateStatus": resp.get("otaUpdateStatus"),
    }

    # ---- Optional polling to surface CREATE_FAILED + errorInfo ----
    if wait_for_create:
        deadline = time.time() + max(1, wait_seconds)
        last = None
        while time.time() < deadline:
            info = get_ota_status(ota_id)
            last = info
            status = info.get("otaUpdateStatus")
            print("poll otaUpdateStatus:", status)
            if status in ("CREATE_COMPLETE", "CREATE_FAILED"):
                break
            time.sleep(poll_interval)

        if last:
            result["otaUpdateStatus"] = last.get("otaUpdateStatus", result["otaUpdateStatus"])
            # errorInfo is where AWS reports the real reason when it fails
            # (e.g., signer / S3 / role issues)
            result["errorInfo"] = last.get("errorInfo")
            result["awsIotJobId"] = last.get("awsIotJobId")
            result["otaUpdateArn"] = last.get("otaUpdateArn")

    return result
