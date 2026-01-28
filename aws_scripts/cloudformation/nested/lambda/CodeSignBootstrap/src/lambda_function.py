import json
import os
import subprocess
import tempfile
import traceback
import urllib.request

import boto3
import botocore.exceptions

# Clients are region-scoped (Lambda's AWS_REGION unless overridden)
acm = boto3.client("acm")
ssm = boto3.client("ssm")
signer = boto3.client("signer")
sts = boto3.client("sts")


def _log(msg, **kv):
    if kv:
        print(msg + " " + json.dumps(kv, default=str))
    else:
        print(msg)


def _send_cfn(event, context, status, physical_resource_id, data=None, reason=None):
    """
    Reliable CloudFormation custom resource response:
      - Content-Length must be BYTES, not character count.
      - Always best-effort send a response.
    """
    if data is None:
        data = {}

    body = {
        "Status": status,
        "Reason": reason or f"See CloudWatch logs: {context.log_stream_name}",
        "PhysicalResourceId": physical_resource_id,
        "StackId": event["StackId"],
        "RequestId": event["RequestId"],
        "LogicalResourceId": event["LogicalResourceId"],
        "NoEcho": False,
        "Data": data,
    }

    payload = json.dumps(body, separators=(",", ":")).encode("utf-8")

    req = urllib.request.Request(
        event["ResponseURL"],
        data=payload,
        headers={"content-type": "", "content-length": str(len(payload))},
        method="PUT",
    )

    with urllib.request.urlopen(req, timeout=10) as resp:
        resp.read()


def _is_client_error(e: Exception, code: str) -> bool:
    return isinstance(e, botocore.exceptions.ClientError) and e.response.get("Error", {}).get("Code") == code


def _acm_cert_exists(cert_arn: str) -> bool:
    try:
        acm.describe_certificate(CertificateArn=cert_arn)
        return True
    except botocore.exceptions.ClientError as e:
        if _is_client_error(e, "ResourceNotFoundException"):
            return False
        raise


def _generate_self_signed_codesign_cert(common_name: str):
    """
    Generates an ECDSA P-256 self-signed code-signing certificate using OpenSSL.
    """
    with tempfile.TemporaryDirectory() as d:
        key_path = os.path.join(d, "key.pem")
        cert_path = os.path.join(d, "cert.pem")
        cfg_path = os.path.join(d, "openssl.cnf")

        cfg = f"""[ req ]
distinguished_name = dn
prompt = no
x509_extensions = v3_req

[ dn ]
CN = {common_name}

[ v3_req ]
basicConstraints = CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = codeSigning
subjectKeyIdentifier = hash
"""
        with open(cfg_path, "w", encoding="utf-8") as f:
            f.write(cfg)

        subprocess.check_call(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", key_path])
        subprocess.check_call(["openssl", "req", "-new", "-x509", "-days", "3650", "-key", key_path, "-out", cert_path, "-config", cfg_path])

        with open(cert_path, "rb") as f:
            cert_pem = f.read()
        with open(key_path, "rb") as f:
            key_pem = f.read()

        return cert_pem, key_pem


def lambda_handler(event, context):
    props = event.get("ResourceProperties", {}) or {}
    req_type = event.get("RequestType", "Unknown")

    env = props.get("Environment", "unknown")
    parent = props.get("ParentStackName", "unknown")
    platform_id = props.get("SigningPlatformId", "AmazonFreeRTOS-Default")
    profile_name = props.get("ProfileName")
    cert_param = props.get("CertArnParam")
    certname_on_device = props.get("CertNameOnDevice", "/certs/codesign")
    _ = props.get("BootstrapTrigger")  # intentionally ignored; forces CFN Update when changed

    physical_id = f"{parent}-{env}-codesign-bootstrap"

    # Minimal identity + region breadcrumbs for debugging
    region = os.environ.get("AWS_REGION", "unknown")
    try:
        ident = sts.get_caller_identity()
    except Exception:
        ident = {}

    _log("codesign bootstrap invoked", requestType=req_type, region=region, account=ident.get("Account"), arn=ident.get("Arn"))
    _log("resource properties", **{k: props.get(k) for k in ["Environment", "ParentStackName", "SigningPlatformId", "ProfileName", "CertArnParam", "CertNameOnDevice", "BootstrapTrigger"]})

    try:
        if req_type == "Delete":
            _log("delete requested; retaining cert/profile")
            _send_cfn(event, context, "SUCCESS", physical_id, data={"Retained": True, "Region": region})
            return

        # Validate required inputs for Create/Update
        missing = [k for k in ("ProfileName", "CertArnParam") if not props.get(k)]
        if missing:
            raise ValueError(f"Missing required ResourceProperties: {', '.join(missing)}")

        # 1) Resolve or create ACM certificate (idempotent)
        cert_arn = None
        try:
            cert_arn = ssm.get_parameter(Name=cert_param)["Parameter"]["Value"]
            _log("ssm.get_parameter ok", certArn=cert_arn)
            if not _acm_cert_exists(cert_arn):
                _log("acm cert in ssm not found; will recreate", certArn=cert_arn)
                cert_arn = None
        except botocore.exceptions.ClientError as e:
            if _is_client_error(e, "ParameterNotFound"):
                _log("ssm parameter not found; will create", param=cert_param)
            else:
                raise

        if not cert_arn:
            cn = f"{parent}-codesigncert-{env}"
            _log("generating self-signed cert", commonName=cn)
            cert_pem, key_pem = _generate_self_signed_codesign_cert(cn)

            _log("importing certificate into ACM")
            resp = acm.import_certificate(
                Certificate=cert_pem,
                PrivateKey=key_pem,
                Tags=[
                    {"Key": "Name", "Value": cn},
                    {"Key": "ParentStackName", "Value": parent},
                    {"Key": "Environment", "Value": env},
                    {"Key": "Purpose", "Value": "CodeSigning"},
                ],
            )
            cert_arn = resp["CertificateArn"]
            _log("acm.import_certificate ok", certArn=cert_arn)

            ssm.put_parameter(Name=cert_param, Value=cert_arn, Type="String", Overwrite=True)
            _log("ssm.put_parameter ok", param=cert_param)

        # 2) Ensure signing profile exists AND has required signingParameters for IoT OTA
        profile = None
        try:
            profile = signer.get_signing_profile(profileName=profile_name)
            _log("signer.get_signing_profile ok (exists)", status=profile.get("status"))
        except botocore.exceptions.ClientError as e:
            if _is_client_error(e, "ResourceNotFoundException"):
                _log("signing profile not found; will create", profileName=profile_name)
            else:
                raise

        # Create or update if missing or certname mismatch
        need_put = False
        if not profile:
            need_put = True
        else:
            current_certname = (profile.get("signingParameters") or {}).get("certname")
            if current_certname != certname_on_device:
                _log("profile exists but certname differs; will create new version", current=current_certname, desired=certname_on_device)
                need_put = True

        put_resp = None
        if need_put:
            put_resp = signer.put_signing_profile(
                profileName=profile_name,
                platformId=platform_id,
                signingMaterial={"certificateArn": cert_arn},
                signingParameters={"certname": certname_on_device},
            )
            _log("signer.put_signing_profile ok", arn=put_resp.get("arn"), profileVersion=put_resp.get("profileVersion"))

        # Read-back verification: if this fails, fail the custom resource.
        profile = signer.get_signing_profile(profileName=profile_name)
        _log("signer.get_signing_profile verification ok", status=profile.get("status"), version=profile.get("profileVersion"))

        # Optional diagnostic list (non-fatal if denied)
        first_profiles = []
        try:
            lst = signer.list_signing_profiles(maxResults=20)
            first_profiles = [p.get("profileName") for p in lst.get("profiles", [])]
            _log("signer.list_signing_profiles ok", count=len(first_profiles))
        except botocore.exceptions.ClientError as e:
            _log("list_signing_profiles diagnostic skipped", errorCode=e.response.get("Error", {}).get("Code"))

        _send_cfn(
            event,
            context,
            "SUCCESS",
            physical_id,
            data={
                "Region": region,
                "Account": ident.get("Account"),
                "CertificateArn": cert_arn,
                "SigningProfileName": profile_name,
                "SigningPlatformId": platform_id,
                "CertNameOnDevice": certname_on_device,
                "SigningProfileArn": profile.get("arn"),
                "SigningProfileVersionArn": profile.get("profileVersionArn"),
                "SigningProfileStatus": profile.get("status"),
                "FirstProfilesInRegion": first_profiles,
            },
        )

    except Exception as e:
        _log("ERROR", message=str(e))
        _log(traceback.format_exc())
        reason = str(e)
        if len(reason) > 512:
            reason = reason[:512] + "..."
        try:
            _send_cfn(event, context, "FAILED", physical_id, data={"Region": region, "Account": ident.get("Account")}, reason=reason)
        except Exception:
            # If CFN response fails, re-raise so it's obvious in logs
            raise