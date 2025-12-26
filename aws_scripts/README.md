# AWS automatic deploy from scratch
This guide assumes the only thing currently created in AWS is the 
[root user](https://docs.aws.amazon.com/IAM/latest/UserGuide/id_root-user.html) and
nothing else, so you're starting _truly from scratch_. AWS recommends securing the
root user with MFA and creating an [admin role](https://docs.aws.amazon.com/accounts/latest/reference/getting-started-step4.html) 
to prevent costly mistakes. If you're managing an AWS organization follow the [quickstart guide from AWS](https://docs.aws.amazon.com/singlesignon/latest/userguide/quick-start-default-idc.html)
before continuing. If you're not managing an AWS organization you can follow
[this quickstart guide](https://docs.aws.amazon.com/IAM/latest/UserGuide/id_users_create.html) to create an admin user.

For AWS CLI V2, you can call `aws login` to configure your profile using your console
login. For AWS CLI V1, `aws configure` will promt you for the Access Key and
Secret Acess Key you created in IAM.

**Note: Creating a bootstrapping user:**
You can follow this guide using the admin role/user, but it's recommended that you
create a special bootsrapping user to keep permissions tight. A helper script is
provided in  [UserCreation/createBootstrapper.sh](./UserCreation/createBootstrapper.sh). Run it using
```bash
$ . UserCreation/createBootstrapper.sh
```
to keep the `BOOTSTRAP_PROFILE` variable in your environment. If you have your own
automations IAM user, you can set the `BOOTSTRAP_PROFILE` to the name of your
profile and the rest of the scripts provided here will work seamlessly.

## CloudFormation templates
The `cloudformation/` directory contains templates that replace the shell scripts:

- `storage-and-data.yaml` – creates the S3 bucket (versioned) and DynamoDB table plus the IoT Rule role used for DynamoDB actions.
- `iot-core.yaml` – provisions IoT Core fleet provisioning resources (policies, role, and provisioning template).
- `iot-lambda-and-rules.yaml` – deploys the UpdateOngoingRecording Lambda function and all IoT Topic Rules.
- `appsync.yaml` – defines the AppSync API, DynamoDB data source, resolvers, and functions.
- `bootstrap-user.yaml` – creates the bootstrap IAM user, managed policy (using the embedded `FullBootstrap.json` content), and access key. The template now inlines the JSON so you do not need to stage it in S3.

Everything done by the original shell scripts is represented in these templates; the only manual prerequisites are supplying environment-specific parameter values when you deploy each stack.

Deploy the stacks with `aws cloudformation deploy --template-file <file> --stack-name <name> --capabilities CAPABILITY_NAMED_IAM` and the parameters that match your environment (table name, bucket name, etc.).

## IoT Core provisioning
This project follows the [Provisioning by claim](https://docs.aws.amazon.com/iot/latest/developerguide/provision-wo-cert.html#claim-based)
strategy to create device certificates for cameras when they first connect. For
this, a provisioning template, a provisioner IAM Role and a provisioning policy are
needed. You can find this three files in the [IoTCore](IoTCore/) directory. Also,
after the cameras connect and a certificate is issued, an authenticated policy is
needed for normal operation. This policy is located at 
[IoTCore/cameraPolicy.json](IoTCore/cameraPolicy.json) and allows cameras to
subscribe and publish to relevant topics.

To automatically create and activate all the needed certificates, policies and 
templates, a helper script is provided at
[IoTCore/createIoT.sh](IoTCore/createIoT.sh). The `BOOTSTRAP_PROFILE` 
environment variable will be used to select the profile that will execute the AWS
commands, so be sure to set it accordingly (or leave it empty for the default 
profile).